#include <algorithm>
#include <cppformat/format.h>

#include "message.hpp"
#include "command.hpp"
#include "proxy.hpp"
#include "client.hpp"
#include "server.hpp"
#include "subscription.hpp"
#include "stats.hpp"
#include "slot_calc.hpp"
#include "globals.hpp"
#include "except/exceptions.hpp"
#include "utils/logging.hpp"
#include "utils/random.hpp"
#include "utils/string.h"

using namespace cerb;

namespace {

    std::string const RSP_OK_STR("+OK\r\n");
    std::shared_ptr<Buffer> const RSP_OK(new Buffer(RSP_OK_STR));

    Server* select_server_for(Proxy* proxy, DataCommand* cmd, slot key_slot)
    {
        Server* svr = proxy->get_server_by_slot(key_slot);
        if (svr == nullptr) {
            LOG(DEBUG) << "Cluster slot not covered " << key_slot;
            proxy->retry_move_ask_command_later(util::make_weak_pointer(*cmd));
            return nullptr;
        }
        svr->receive_request(util::make_weak_pointer(*cmd));
        return svr;
    }

    class MultiStepsCommand
        : public DataCommand
    {
    public:
        slot current_key_slot;
        std::function<void(Buffer, bool)> on_rsp;

        MultiStepsCommand(util::weak_pointer<CommandGroup> group, slot s,
                          std::function<void(Buffer, bool)> r)
            : DataCommand(group)
            , current_key_slot(s)
            , on_rsp(std::move(r))
        {}

        Server* select_server(Proxy* proxy)
        {
            return ::select_server_for(proxy, this, this->current_key_slot);
        }

        void receive_response(Buffer& rsp, bool error)
        {
            on_rsp(std::move(rsp), error);
        }
    };

    class DirectCommandGroup
        : public CommandGroup
    {
        class DirectCommand
            : public Command
        {
        public:
            DirectCommand(Buffer b, util::weak_pointer<CommandGroup> g)
                : Command(std::move(b), g)
            {}

            Server* select_server(Proxy*)
            {
                return nullptr;
            }
        };
    public:
        util::unique_pointer<DirectCommand> command;

        DirectCommandGroup(util::weak_pointer<Client> client, Buffer b)
            : CommandGroup(client)
            , command(new DirectCommand(std::move(b), util::make_weak_pointer(*this)))
        {}

        DirectCommandGroup(util::weak_pointer<Client> client, char const* r)
            : DirectCommandGroup(client, Buffer(r))
        {}

        DirectCommandGroup(util::weak_pointer<Client> client, std::string r)
            : DirectCommandGroup(client, Buffer(r))
        {}

        bool should_send_to_upstream_server_and_wait_response() const
        {
            return false;
        }

        void select_server_and_push_command_to_it(Proxy *) {}

        void enqueue_response_to_client(BufferSet& b)
        {
            b.append(command->buffer);
        }

        int total_buffer_size() const
        {
            return command->buffer->size();
        }

        void receive_response() {}
    };

    class StatsCommandGroup
        : public CommandGroup
    {
    protected:
        explicit StatsCommandGroup(util::weak_pointer<Client> cli)
            : CommandGroup(cli)
            , creation(Clock::now())
            , complete(false)
        {}

        Time const creation;
        bool complete;

        bool should_send_to_upstream_server_and_wait_response() const
        {
            return true;
        }

        void collect_stats(Proxy* p) const
        {
            p->stat_proccessed(Clock::now() - this->creation,
                               this->avg_commands_remote_cost());
        }

        virtual Interval avg_commands_remote_cost() const = 0;
    };

    class SingleCommandGroup
        : public StatsCommandGroup
    {
    public:
        util::unique_pointer<DataCommand> command;

        explicit SingleCommandGroup(util::weak_pointer<Client> cli)
            : StatsCommandGroup(cli)
            , command(nullptr)
        {}

        SingleCommandGroup(util::weak_pointer<Client> cli, Buffer b, slot ks)
            : StatsCommandGroup(cli)
            , command(new OneSlotCommand(std::move(b), util::make_weak_pointer(*this), ks))
        {}

        void receive_response()
        {
            this->client->handle_response();
            this->complete = true;
        }

        void enqueue_response_to_client(BufferSet& b)
        {
            b.append(command->buffer);
        }

        int total_buffer_size() const
        {
            return command->buffer->size();
        }

        void select_server_and_push_command_to_it(Proxy *proxy)
        {
            command->select_server(proxy);
        }

        Interval avg_commands_remote_cost() const
        {
            return command->remote_cost();
        }
    };

    class MultipleCommandsGroup
        : public StatsCommandGroup
    {
    public:
        std::shared_ptr<Buffer> arr_payload;
        std::vector<util::unique_pointer<DataCommand>> commands;
        int awaiting_count;

        explicit MultipleCommandsGroup(util::weak_pointer<Client> c)
            : StatsCommandGroup(c)
            , arr_payload(new Buffer)
            , awaiting_count(0)
        {}

        void append_command(util::unique_pointer<DataCommand> c)
        {
            awaiting_count += 1;
            commands.push_back(std::move(c));
        }

        void receive_response()
        {
            if (--this->awaiting_count == 0) {
                this->arr_payload->swap(Buffer(
                    fmt::format("*{}\r\n", this->commands.size())));
                this->client->handle_response();
                this->complete = true;
            }
        }

        void enqueue_response_to_client(BufferSet& b)
        {
            b.append(this->arr_payload);
            for (auto const& c: this->commands) {
                b.append(c->buffer);
            }
        }

        int total_buffer_size() const
        {
            int i = this->arr_payload->size();
            for (auto const& c: this->commands) {
                i += c->buffer->size();
            }
            return i;
        }

        void select_server_and_push_command_to_it(Proxy *proxy)
        {
            for (auto& c: this->commands) {
                c->select_server(proxy);
            }
        }

        Interval avg_commands_remote_cost() const
        {
            if (this->commands.empty()) {
                return Interval(0);
            }
            return std::accumulate(
                this->commands.begin(), this->commands.end(), Interval(0),
                [](Interval a, util::unique_pointer<DataCommand> const& c)
                {
                    return a + c->remote_cost();
                }) / this->commands.size();
        }
    };

    class LongCommandGroup
        : public CommandGroup
    {
    public:
        LongCommandGroup(util::weak_pointer<Client> client)
            : CommandGroup(client)
        {}

        bool long_connection() const
        {
            return true;
        }

        bool should_send_to_upstream_server_and_wait_response() const
        {
            return false;
        }

        int total_buffer_size() const
        {
            return 0;
        }

        void select_server_and_push_command_to_it(Proxy *) {}
        void enqueue_response_to_client(BufferSet&) {}
        void receive_response() {}
    };

    std::string stats_string()
    {
        std::string s(stats_all());
        return '+' + s + "\r\n";
    }

    void notify_each_thread_update_slot_map()
    {
        for (auto& t: cerb_global::all_threads) {
            t.get_proxy()->update_slot_map();
        }
    }

    class SpecialCommandParser {
    public:
        virtual void on_str(Buffer::iterator begin, Buffer::iterator end) = 0;
        virtual ~SpecialCommandParser() {}

        virtual util::unique_pointer<CommandGroup> spawn_commands(
            util::weak_pointer<Client> c, Buffer::iterator end) = 0;

        SpecialCommandParser() = default;
        SpecialCommandParser(SpecialCommandParser const&) = delete;
    };

    class PingCommandParser
        : public SpecialCommandParser
    {
        std::string msg;
    public:
        PingCommandParser() = default;

        util::unique_pointer<CommandGroup> spawn_commands(util::weak_pointer<Client> c, Buffer::iterator)
        {
            if (this->msg.empty()) {
                return util::make_unique_ptr(new DirectCommandGroup(c, "+PONG\r\n"));
            }
            return util::make_unique_ptr(new DirectCommandGroup(c, fmt::format(
                    "${}\r\n{}\r\n", this->msg.size(), this->msg)));
        }

        void on_str(Buffer::iterator begin, Buffer::iterator end)
        {
            this->msg = std::string(begin, end);
        }
    };

    class ProxyStatsCommandParser
        : public SpecialCommandParser
    {
    public:
        ProxyStatsCommandParser() = default;

        util::unique_pointer<CommandGroup> spawn_commands(
            util::weak_pointer<Client> c, Buffer::iterator)
        {
            return util::make_unique_ptr(new DirectCommandGroup(c, stats_string()));
        }

        void on_str(Buffer::iterator, Buffer::iterator) {}
    };

    class UpdateSlotMapCommandParser
        : public SpecialCommandParser
    {
    public:
        UpdateSlotMapCommandParser() = default;

        util::unique_pointer<CommandGroup> spawn_commands(util::weak_pointer<Client> c, Buffer::iterator)
        {
            ::notify_each_thread_update_slot_map();
            return util::make_unique_ptr(new DirectCommandGroup(c, RSP_OK_STR));
        }

        void on_str(Buffer::iterator, Buffer::iterator) {}
    };

    class SetRemotesCommandParser
        : public SpecialCommandParser
    {
        std::set<util::Address> remotes;
        std::string last_host;
        bool current_is_host;
        bool bad;
    public:
        SetRemotesCommandParser()
            : current_is_host(true)
            , bad(false)
        {}

        util::unique_pointer<CommandGroup> spawn_commands(util::weak_pointer<Client> c, Buffer::iterator)
        {
            if (this->bad) {
                return util::make_unique_ptr(new DirectCommandGroup(
                        c, "-ERR invalid port number\r\n"));
            }
            if (this->remotes.empty() || !this->current_is_host) {
                return util::make_unique_ptr(new DirectCommandGroup(
                        c, "-ERR wrong number of arguments for 'SETREMOTES' command\r\n"));
            }
            cerb_global::set_remotes(std::move(this->remotes));
            ::notify_each_thread_update_slot_map();
            return util::make_unique_ptr(new DirectCommandGroup(c, RSP_OK_STR));
        }

        void on_str(Buffer::iterator begin, Buffer::iterator end)
        {
            if (this->bad) {
                return;
            }
            if (current_is_host) {
                this->last_host = std::string(begin, end);
            } else {
                this->remotes.insert(util::Address(std::move(this->last_host),
                                                   util::atoi(std::string(begin, end))));
            }
            this->current_is_host = !this->current_is_host;
        }
    };

    class EachKeyCommandParser
        : public SpecialCommandParser
    {
        std::string const command_name;
        std::vector<Buffer::iterator> keys_split_points;
        std::vector<slot> keys_slots;

        virtual Buffer command_header() const = 0;

        virtual util::unique_pointer<MultipleCommandsGroup> makeGroup(util::weak_pointer<Client> c) const
        {
            return util::make_unique_ptr(new MultipleCommandsGroup(c));
        }
    public:
        EachKeyCommandParser(Buffer::iterator arg_begin, std::string cmd)
            : command_name(std::move(cmd))
        {
            keys_split_points.push_back(arg_begin);
        }

        void on_str(Buffer::iterator begin, Buffer::iterator end)
        {
            KeySlotCalc slot_calc;
            for (; begin != end; ++begin) {
                slot_calc.next_byte(*begin);
            }
            this->keys_slots.push_back(slot_calc.get_slot());
            this->keys_split_points.push_back(end + msg::LENGTH_OF_CR_LF);
        }

        util::unique_pointer<CommandGroup> spawn_commands(
            util::weak_pointer<Client> c, Buffer::iterator)
        {
            if (keys_slots.empty()) {
                return util::make_unique_ptr(new DirectCommandGroup(
                        c, "-ERR wrong number of arguments for '" + this->command_name + "' command\r\n"));
            }
            util::unique_pointer<MultipleCommandsGroup> g(this->makeGroup(c));
            for (unsigned i = 0; i < keys_slots.size(); ++i) {
                Buffer b(command_header());
                b.append_from(this->keys_split_points[i], this->keys_split_points[i + 1]);
                g->append_command(util::make_unique_ptr(
                        new OneSlotCommand(std::move(b), *g, this->keys_slots[i])));
            }
            return std::move(g);
        }
    };

    class MGetCommandParser
        : public EachKeyCommandParser
    {
        Buffer command_header() const
        {
            return Buffer("*2\r\n$3\r\nGET\r\n");
        }
    public:
        explicit MGetCommandParser(Buffer::iterator arg_begin)
            : EachKeyCommandParser(arg_begin, "mget")
        {}
    };

    class DelCommandParser
        : public EachKeyCommandParser
    {
        class DelCommandGroup
            : public MultipleCommandsGroup
        {
        public:
            explicit DelCommandGroup(util::weak_pointer<Client> c)
                : MultipleCommandsGroup(c)
            {}

            void enqueue_response_to_client(BufferSet& b)
            {
                cerb::rint count = 0;
                for (auto const& c: this->commands) {
                    count += std::find(c->buffer->begin(), c->buffer->end(), '1') == c->buffer->end() ? 0 : 1;
                }
                this->arr_payload->swap(Buffer(":" + util::str(count) + "\r\n"));
                b.append(this->arr_payload);
            }

            int total_buffer_size() const
            {
                return RSP_OK->size();
            }
        };

        util::unique_pointer<MultipleCommandsGroup> makeGroup(util::weak_pointer<Client> c) const
        {
            return util::make_unique_ptr(new DelCommandGroup(c));
        }

        Buffer command_header() const
        {
            return Buffer("*2\r\n$3\r\nDEL\r\n");
        }
    public:
        explicit DelCommandParser(Buffer::iterator arg_begin)
            : EachKeyCommandParser(arg_begin, "del")
        {}
    };

    class MSetCommandParser
        : public SpecialCommandParser
    {
        class MSetCommandGroup
            : public MultipleCommandsGroup
        {
        public:
            explicit MSetCommandGroup(util::weak_pointer<Client> c)
                : MultipleCommandsGroup(c)
            {}

            void enqueue_response_to_client(BufferSet& b)
            {
                b.append(RSP_OK);
            }

            int total_buffer_size() const
            {
                return RSP_OK->size();
            }
        };

        std::vector<Buffer::iterator> kv_split_points;
        std::vector<slot> keys_slots;
        bool current_is_key;
    public:
        explicit MSetCommandParser(Buffer::iterator arg_begin)
            : current_is_key(true)
        {
            kv_split_points.push_back(arg_begin);
        }

        void on_str(Buffer::iterator begin, Buffer::iterator end)
        {
            if (this->current_is_key) {
                KeySlotCalc slot_calc;
                for (; begin != end; ++begin) {
                    slot_calc.next_byte(*begin);
                }
                this->keys_slots.push_back(slot_calc.get_slot());
            }
            this->current_is_key = !this->current_is_key;
            this->kv_split_points.push_back(end + msg::LENGTH_OF_CR_LF);
        }

        util::unique_pointer<CommandGroup> spawn_commands(
            util::weak_pointer<Client> c, Buffer::iterator)
        {
            if (keys_slots.empty() || !current_is_key) {
                return util::make_unique_ptr(new DirectCommandGroup(
                        c, "-ERR wrong number of arguments for 'mset' command\r\n"));
            }
            util::unique_pointer<MSetCommandGroup> g(new MSetCommandGroup(c));
            for (unsigned i = 0; i < keys_slots.size(); ++i) {
                Buffer b("*3\r\n$3\r\nSET\r\n");
                b.append_from(kv_split_points[i * 2], kv_split_points[i * 2 + 2]);
                g->append_command(util::make_unique_ptr(new OneSlotCommand(
                        std::move(b), *g, keys_slots[i])));
            }
            return std::move(g);
        }
    };

    class RenameCommandParser
        : public SpecialCommandParser
    {
        class RenameCommand
            : public MultiStepsCommand
        {
            Buffer old_key;
            Buffer new_key;
            slot old_key_slot;
            slot new_key_slot;
        public:
            RenameCommand(Buffer old_key, Buffer new_key, slot old_key_slot,
                          slot new_key_slot, util::weak_pointer<CommandGroup> group)
                : MultiStepsCommand(group, old_key_slot,
                                    [&](Buffer r, bool e)
                                    {
                                        return this->rsp_get(std::move(r), e);
                                    })
                , old_key(std::move(old_key))
                , new_key(std::move(new_key))
                , old_key_slot(old_key_slot)
                , new_key_slot(new_key_slot)
            {
                this->buffer->swap(Buffer("*2\r\n$3\r\nGET\r\n"));
                this->buffer->append_from(this->old_key.begin(), this->old_key.end());
            }

            void rsp_get(Buffer rsp, bool error)
            {
                if (error) {
                    this->buffer->swap(rsp);
                    return this->responsed();
                }
                if (rsp.same_as_string("$-1\r\n")) {
                    this->buffer->swap(Buffer("-ERR no such key\r\n"));
                    return this->responsed();
                }
                this->buffer->swap(Buffer("*3\r\n$3\r\nSET\r\n"));
                this->buffer->append_from(new_key.begin(), new_key.end());
                this->buffer->append_from(rsp.begin(), rsp.end());
                this->current_key_slot = new_key_slot;
                this->on_rsp =
                    [this](Buffer rsp, bool error)
                    {
                        if (error) {
                            this->buffer->swap(rsp);
                            return this->responsed();
                        }
                        this->rsp_set();
                    };
                this->group->client->reactivate(util::make_weak_pointer(*this));
            }

            void rsp_set()
            {
                this->buffer->swap(Buffer("*2\r\n$3\r\nDEL\r\n"));
                this->buffer->append_from(old_key.begin(), old_key.end());
                this->current_key_slot = old_key_slot;
                this->on_rsp =
                    [this](Buffer, bool)
                    {
                        this->buffer->swap(Buffer(RSP_OK_STR));
                        this->responsed();
                    };
                this->group->client->reactivate(util::make_weak_pointer(*this));
            }
        };

        Buffer::iterator command_begin;
        std::vector<Buffer::iterator> split_points;
        KeySlotCalc key_slot[2];
        int slot_index;
        bool bad;
    public:
        RenameCommandParser(Buffer::iterator cmd_begin,
                            Buffer::iterator arg_begin)
            : command_begin(cmd_begin)
            , slot_index(0)
            , bad(false)
        {
            split_points.push_back(arg_begin);
        }

        void on_str(Buffer::iterator begin, Buffer::iterator end)
        {
            if (this->slot_index == 2) {
                this->bad = true;
                return;
            }
            for (; begin != end; ++begin) {
                this->key_slot[this->slot_index].next_byte(*begin);
            }
            this->split_points.push_back(end + msg::LENGTH_OF_CR_LF);
            ++this->slot_index;
        }

        util::unique_pointer<CommandGroup> spawn_commands(
            util::weak_pointer<Client> c, Buffer::iterator)
        {
            if (slot_index != 2 || this->bad) {
                return util::make_unique_ptr(new DirectCommandGroup(
                        c, "-ERR wrong number of arguments for 'rename' command\r\n"));
            }
            slot src_slot = key_slot[0].get_slot();
            slot dst_slot = key_slot[1].get_slot();
            LOG(DEBUG) << "#Rename slots: " << src_slot << " - " << dst_slot;
            if (src_slot == dst_slot) {
                return util::make_unique_ptr(new SingleCommandGroup(
                        c, Buffer(command_begin, split_points[2]), src_slot));
            }
            util::unique_pointer<SingleCommandGroup> g(new SingleCommandGroup(c));
            g->command = util::make_unique_ptr(new RenameCommand(
                    Buffer(split_points[0], split_points[1]),
                    Buffer(split_points[1], split_points[2]),
                    src_slot, dst_slot, *g));
            return std::move(g);
        }
    };

    class SubscribeCommandParser
        : public SpecialCommandParser
    {
        class Subscribe
            : public LongCommandGroup
        {
            Buffer buffer;
        public:
            Subscribe(util::weak_pointer<Client> client, Buffer b)
                : LongCommandGroup(client)
                , buffer(std::move(b))
            {}

            void deliver_client(Proxy* p)
            {
                Server* s = p->random_addr();
                if (s == nullptr) {
                    return this->client->close();
                }
                new Subscription(p, this->client->fd, s, std::move(buffer));
                LOG(DEBUG) << "Convert " << this->client->str() << " as subscription";
                this->client->fd = -1;
            }
        };

        Buffer::iterator begin;
        bool no_arg;
    public:
        void on_str(Buffer::iterator, Buffer::iterator)
        {
            this->no_arg = false;
        }

        explicit SubscribeCommandParser(Buffer::iterator begin)
            : begin(begin)
            , no_arg(true)
        {}

        util::unique_pointer<CommandGroup> spawn_commands(
            util::weak_pointer<Client> c, Buffer::iterator end)
        {
            if (this->no_arg) {
                return util::make_unique_ptr(new DirectCommandGroup(
                        c, "-ERR wrong number of arguments for 'subscribe' command\r\n"));
            }
            return util::make_unique_ptr(new Subscribe(c, Buffer(this->begin, end)));
        }
    };

    class BlockedListPopParser
        : public SpecialCommandParser
    {
        class BlockedPop
            : public LongCommandGroup
        {
            Buffer buffer;
            slot key_slot;
        public:
            BlockedPop(util::weak_pointer<Client> client, Buffer b, slot s)
                : LongCommandGroup(client)
                , buffer(std::move(b))
                , key_slot(s)
            {}

            void deliver_client(Proxy* p)
            {
                Server* s = p->get_server_by_slot(this->key_slot);
                if (s == nullptr) {
                    return this->client->close();
                }
                new BlockedListPop(p, this->client->fd, s, std::move(this->buffer));
                LOG(DEBUG) << "Convert " << this->client->str() << " as blocked pop";
                this->client->fd = -1;
            }
        };

        Buffer::iterator cmd_begin;
        KeySlotCalc slot_calc;
        int args_count;
    public:
        void on_str(Buffer::iterator begin, Buffer::iterator end)
        {
            for (; begin != end; ++begin) {
                this->slot_calc.next_byte(*begin);
            }
            ++this->args_count;
        }

        explicit BlockedListPopParser(Buffer::iterator begin)
            : cmd_begin(begin)
            , args_count(0)
        {}

        util::unique_pointer<CommandGroup> spawn_commands(
            util::weak_pointer<Client> c, Buffer::iterator end)
        {
            if (this->args_count != 2) {
                return util::make_unique_ptr(new DirectCommandGroup(
                        c, "-ERR BLPOP/BRPOP takes exactly 2 arguments KEY TIMEOUT in proxy\r\n"));
            }
            return util::make_unique_ptr(new BlockedPop(c, Buffer(this->cmd_begin, end),
                                                        this->slot_calc.get_slot()));
        }
    };

    class EvalCommandParser
        : public SpecialCommandParser
    {
        Buffer::iterator cmd_begin;
        KeySlotCalc slot_calc;
        int arg_count;
        int key_count;
    public:
        void on_str(Buffer::iterator begin, Buffer::iterator end)
        {
            switch (this->arg_count++) {
                case 0:
                    return;
                case 1:
                    this->key_count = util::atoi(std::string(begin, end));
                    return;
                case 2:
                    for (; begin != end; ++begin) {
                        this->slot_calc.next_byte(*begin);
                    }
                    return;
                default:
                    return;
            }
        }

        explicit EvalCommandParser(Buffer::iterator begin)
            : cmd_begin(begin)
            , arg_count(0)
            , key_count(0)
        {}

        util::unique_pointer<CommandGroup> spawn_commands(
            util::weak_pointer<Client> c, Buffer::iterator end)
        {
            if (this->arg_count < 3 || this->key_count != 1) {
                return util::make_unique_ptr(new DirectCommandGroup(
                        c, "-ERR wrong number of arguments for 'eval' command\r\n"));
            }
            return util::make_unique_ptr(new SingleCommandGroup(
                    c, Buffer(this->cmd_begin, end), this->slot_calc.get_slot()));
        }
    };

    class PublishCommandParser
        : public SpecialCommandParser
    {
        Buffer::iterator begin;
        int arg_count;
    public:
        void on_str(Buffer::iterator, Buffer::iterator)
        {
            ++this->arg_count;
        }

        explicit PublishCommandParser(Buffer::iterator begin)
            : begin(begin)
            , arg_count(0)
        {}

        util::unique_pointer<CommandGroup> spawn_commands(
            util::weak_pointer<Client> c, Buffer::iterator end)
        {
            if (this->arg_count != 2) {
                return util::make_unique_ptr(new DirectCommandGroup(
                        c, "-ERR wrong number of arguments for 'publish' command\r\n"));
            }
            return util::make_unique_ptr(new SingleCommandGroup(
                    c, Buffer(this->begin, end), util::randint(0, CLUSTER_SLOT_COUNT)));
        }
    };

    class KeysInSlotParser
        : public SpecialCommandParser
    {
        Buffer::iterator _arg_start;
        int _arg_count;
        slot _slot;
    public:
        void on_str(Buffer::iterator begin, Buffer::iterator end)
        {
            if (this->_arg_count == 0) {
                this->_slot = util::atoi(std::string(begin, end));
            }
            ++this->_arg_count;
        }

        explicit KeysInSlotParser(Buffer::iterator arg_start)
            : _arg_start(arg_start)
            , _arg_count(0)
            , _slot(0)
        {}

        util::unique_pointer<CommandGroup> spawn_commands(
            util::weak_pointer<Client> c, Buffer::iterator end)
        {
            if (this->_arg_count != 2 || this->_slot >= CLUSTER_SLOT_COUNT) {
                return util::make_unique_ptr(new DirectCommandGroup(
                        c, "-ERR wrong arguments for 'keysinslot' command\r\n"));
            }
            Buffer buffer("*4\r\n$7\r\nCLUSTER\r\n$13\r\nGETKEYSINSLOT\r\n");
            buffer.append_from(this->_arg_start, end);
            return util::make_unique_ptr(new SingleCommandGroup(c, std::move(buffer), this->_slot));
        }
    };

    std::map<std::string, std::function<util::unique_pointer<SpecialCommandParser>(
        Buffer::iterator, Buffer::iterator)>> SPECIAL_RSP(
    {
        {"PING",
            [](Buffer::iterator, Buffer::iterator)
            {
                return util::make_unique_ptr(new PingCommandParser);
            }},
        {"INFO",
            [](Buffer::iterator, Buffer::iterator)
            {
                return util::make_unique_ptr(new ProxyStatsCommandParser);
            }},
        {"PROXY",
            [](Buffer::iterator, Buffer::iterator)
            {
                return util::make_unique_ptr(new ProxyStatsCommandParser);
            }},
        {"UPDATESLOTMAP",
            [](Buffer::iterator, Buffer::iterator)
            {
                return util::make_unique_ptr(new UpdateSlotMapCommandParser);
            }},
        {"SETREMOTES",
            [](Buffer::iterator, Buffer::iterator)
            {
                return util::make_unique_ptr(new SetRemotesCommandParser);
            }},
        {"MGET",
            [](Buffer::iterator, Buffer::iterator arg_start)
            {
                return util::make_unique_ptr(new MGetCommandParser(arg_start));
            }},
        {"SUBSCRIBE",
            [](Buffer::iterator command_begin, Buffer::iterator)
            {
                return util::make_unique_ptr(new SubscribeCommandParser(command_begin));
            }},
        {"PSUBSCRIBE",
            [](Buffer::iterator command_begin, Buffer::iterator)
            {
                return util::make_unique_ptr(new SubscribeCommandParser(command_begin));
            }},
    });

    std::set<std::string> STD_COMMANDS({
        "DUMP", "EXISTS", "TTL", "PTTL", "TYPE",
        "GET", "BITCOUNT", "GETBIT", "GETRANGE", "STRLEN",
        "HGET", "HGETALL", "HKEYS", "HVALS", "HLEN", "HEXISTS", "HMGET", "HSCAN",
        "LINDEX", "LLEN", "LRANGE",
        "SCARD", "SISMEMBER", "SRANDMEMBER", "SMEMBERS", "SSCAN",

        "ZCARD", "ZSCAN", "ZCOUNT", "ZLEXCOUNT", "ZRANGE",
        "ZRANGEBYLEX", "ZREVRANGEBYLEX", "ZRANGEBYSCORE", "ZRANK",
        "ZREVRANGE", "ZREVRANGEBYSCORE", "ZREVRANK", "ZSCORE",
    });

    class ClientCommandSplitter
        : public cerb::msg::MessageSplitterBase<
            Buffer::iterator, ClientCommandSplitter>
    {
        typedef Buffer::iterator Iterator;
        typedef cerb::msg::MessageSplitterBase<Iterator, ClientCommandSplitter> BaseType;

        std::function<void(ClientCommandSplitter&, Iterator, Iterator)> _on_str;

        static void on_string_nop(ClientCommandSplitter&, Iterator, Iterator) {}

        static void on_command_name(ClientCommandSplitter &s, Iterator begin, Iterator end)
        {
            s.command_name_pos.first = begin;
            s.command_name_pos.second = end;
            s.select_command_parser(begin, end);
        }

        static void on_command_key(ClientCommandSplitter& s, Iterator begin, Iterator end)
        {
            s.command_key_pos.first = begin;
            s.command_key_pos.second = end;
            s.key = std::move(std::make_shared<std::string>(begin, end));
            s.last_command_is_bad = false;
            s._on_str = ClientCommandSplitter::on_string_nop;
            std::for_each(begin, end, [&](byte b) { s.slot_calc.next_byte(b); });
        }

        static void special_parser_on_str(ClientCommandSplitter& s, Iterator begin, Iterator end)
        {
            s.special_parser->on_str(begin, end);
        }
    public:
        Iterator last_command_begin;
        KeySlotCalc slot_calc;
        bool last_command_is_bad;
        util::unique_pointer<SpecialCommandParser> special_parser;
        util::weak_pointer<Client> client;
        std::pair<Iterator, Iterator> command_name_pos;
        std::pair<Iterator, Iterator> command_key_pos;
        std::shared_ptr<std::string> key;

        void on_string(Iterator begin, Iterator end)
        {
            this->_on_str(*this, begin, end);
        }

        ClientCommandSplitter(Iterator i, util::weak_pointer<Client> cli)
            : BaseType(i)
            , _on_str(ClientCommandSplitter::on_command_name)
            , last_command_begin(i)
            , last_command_is_bad(false)
            , special_parser(nullptr)
            , client(cli)
        {}

        ClientCommandSplitter(ClientCommandSplitter&& rhs)
            : BaseType(std::move(rhs))
            , _on_str(std::move(rhs._on_str))
            , last_command_begin(rhs.last_command_begin)
            , slot_calc(std::move(rhs.slot_calc))
            , last_command_is_bad(rhs.last_command_is_bad)
            , special_parser(std::move(rhs.special_parser))
            , client(rhs.client)
        {}

        bool handle_standard_key_command(std::string const& command)
        {
            auto i = STD_COMMANDS.find(command);
            if (i == STD_COMMANDS.end()) {
                return false;
            }
            this->last_command_is_bad = true;
            this->_on_str = ClientCommandSplitter::on_command_key;
            return true;
        }

        void select_command_parser(Iterator begin, Iterator end)
        {
            std::string cmd;
            std::for_each(begin, end, [&](byte b) { cmd += std::toupper(b); });
            if (this->handle_standard_key_command(cmd)) {
                return;
            }
            auto sfi = SPECIAL_RSP.find(cmd);
            if (sfi != SPECIAL_RSP.end()) {
                this->special_parser = sfi->second(last_command_begin, end + msg::LENGTH_OF_CR_LF);
                this->_on_str = ClientCommandSplitter::special_parser_on_str;
                return;
            }
            this->last_command_is_bad = true;
            this->_on_str = ClientCommandSplitter::on_string_nop;
        }

        void on_split_point(Iterator i)
        {
            this->_on_str = ClientCommandSplitter::on_command_name;
            if (this->last_command_is_bad) {
                this->client->push_command(util::make_unique_ptr(new DirectCommandGroup(
                        client, "-ERR Unknown command or command key not specified\r\n")));
            } else if (this->special_parser.nul()) {
                std::unique_ptr<Buffer> buffer(new Buffer(this->last_command_begin, i));
                LOG(DEBUG) << "on_split_point " << buffer->to_string();
                util::unique_pointer<GetSequenceCommandGroup> command_group
                        = util::make_unique_ptr(
                        new GetSequenceCommandGroup(client, std::move(buffer), std::move(this->key)));
//                command_group->_origin_command->command_name_pos = command_name_pos;
//                command_group->_origin_command->command_key_pos = command_key_pos;
                LOG(DEBUG) << "on_split_point key " << this->key.get()->data();
//                auto command_group = util::make_unique_ptr(new SingleCommandGroup(
//                        client, Buffer(this->last_command_begin, i), this->slot_calc.get_slot()));
//                command_group->command->command_name_pos = command_name_pos;
//                command_group->command->command_key_pos = command_key_pos;
                this->client->push_command(std::move(command_group));
            } else {
                this->client->push_command(this->special_parser->spawn_commands(this->client, i));
                this->special_parser.reset();
            }
            this->last_command_begin = i;
            this->slot_calc.reset();
            this->last_command_is_bad = false;
        }

        void on_array(cerb::rint size)
        {
            /*
             * Redis server will reset a request of more than 1M args.
             * See also
             * https://github.com/antirez/redis/blob/3.0/src/networking.c#L1001
             */
            if (size > 1024 * 1024) {
                throw BadRedisMessage("Request is too large");
            }
            if (!this->_nested_array_element_count.empty()) {
                throw BadRedisMessage("Invalid nested array as client command");
            }
            if (size == 0) {
                return;
            }
        }
    };

}

void Command::receive_response(Buffer rsp, bool)
{
    this->buffer->swap(rsp);
//    this->responsed();
    this->group->receive_response();
}

void Command::responsed()
{
    this->group->receive_response();
}

void cerb::split_client_command(Buffer& buffer, util::weak_pointer<Client> cli)
{
    ClientCommandSplitter c(cerb::msg::split_by(
        buffer.begin(), buffer.end(), ClientCommandSplitter(
            buffer.begin(), cli)));
    if (c.finished()) {
        buffer.clear();
    } else {
        buffer.truncate_from_begin(c.interrupt_point());
    }
}

void Command::allow_write_commands()
{
    static std::set<std::string> const WRITE_COMMANDS({
        "EXPIRE", "EXPIREAT", "TTL", "PEXPIRE", "PEXPIREAT", "PERSIST", "RESTORE",

        "SET", "SETNX", "GETSET", "SETEX", "PSETEX", "SETBIT", "APPEND",
        "SETRANGE", "INCR", "DECR", "INCRBY", "DECRBY", "INCRBYFLOAT",

        "HSET", "HSETNX", "HDEL", "HINCRBY", "HINCRBYFLOAT", "HMSET",

        "LINSERT", "LPOP", "RPOP", "LPUSH", "LPUSHX",
        "RPUSH", "RPUSHX", "LREM", "LSET", "LTRIM", "SORT",

        "SADD", "SPOP", "SREM",

        "ZADD", "ZREM", "ZINCRBY", "ZREMRANGEBYLEX", "ZREMRANGEBYRANK", "ZREMRANGEBYSCORE",
    });
    for (std::string const& c: WRITE_COMMANDS) {
        STD_COMMANDS.insert(c);
    }
    static std::map<std::string, std::function<util::unique_pointer<SpecialCommandParser>(
        Buffer::iterator, Buffer::iterator)>> const SPECIAL_WRITE_COMMAND(
    {
        {"DEL",
            [](Buffer::iterator, Buffer::iterator arg_start)
            {
                return util::make_unique_ptr(new DelCommandParser(arg_start));
            }},
        {"MSET",
            [](Buffer::iterator, Buffer::iterator arg_start)
            {
                return util::make_unique_ptr(new MSetCommandParser(arg_start));
            }},
        {"RENAME",
            [](Buffer::iterator command_begin, Buffer::iterator arg_start)
            {
                return util::make_unique_ptr(new RenameCommandParser(
                        command_begin, arg_start));
            }},
        {"PUBLISH",
            [](Buffer::iterator command_begin, Buffer::iterator)
            {
                return util::make_unique_ptr(new PublishCommandParser(command_begin));
            }},
        {"KEYSINSLOT",
            [](Buffer::iterator, Buffer::iterator arg_start)
            {
                return util::make_unique_ptr(new KeysInSlotParser(arg_start));
            }},
        {"BLPOP",
            [](Buffer::iterator command_begin, Buffer::iterator)
            {
                return util::make_unique_ptr(new BlockedListPopParser(command_begin));
            }},
        {"BRPOP",
            [](Buffer::iterator command_begin, Buffer::iterator)
            {
                return util::make_unique_ptr(new BlockedListPopParser(command_begin));
            }},
        {"EVAL",
            [](Buffer::iterator command_begin, Buffer::iterator)
            {
                return util::make_unique_ptr(new EvalCommandParser(command_begin));
            }},
    });
    for (auto const& c: SPECIAL_WRITE_COMMAND) {
        SPECIAL_RSP.insert(c);
    }
}

namespace cerb {
    OneSlotCommand::OneSlotCommand(Buffer b, util::weak_pointer<CommandGroup> g, slot ks)
            : DataCommand(std::move(b), g)
            , key_slot(ks)
    {
        LOG(DEBUG) << "-Keyslot = " << this->key_slot;
    }

    OneSlotCommand::OneSlotCommand(std::unique_ptr<Buffer> b, util::weak_pointer<CommandGroup> g, slot ks)
            : DataCommand(std::move(b), g)
            , key_slot(ks)
    {
        LOG(DEBUG) << "-Keyslot = " << this->key_slot;
    }

    Server* OneSlotCommand::select_server(Proxy* proxy)
    {
        return ::select_server_for(proxy, this, this->key_slot);
    }

    SequenceCommandGroup::SequenceCommandGroup(util::weak_pointer<Client> cli,
                                               std::unique_ptr<Buffer> buffer)
            : CommandGroup(cli)
    , _origin_command(std::make_shared<OneSlotCommand>(std::move(buffer), util::weak_pointer<CommandGroup>(this), 0))
    {
        _proxy = cli->get_proxy();
    }

    void SequenceCommandGroup::send_currnet_command() {
        Server* server = current_state->target_server;
        if (server == nullptr) {
            return;
        }
        current_state->command = current_state->make_command(
                        origin_command, previous_command, previous_response);
        server->receive_request(util::weak_pointer<DataCommand>(
                current_state->command.get()));
        client->get_proxy()->set_conn_poll_rw(server);
    }

    void SequenceCommandGroup::receive_response() {
        auto response = current_state->command->buffer;
        // check type of response
        // determine next state
        previous_command = current_state;
        previous_response = response;
        current_state = current_state->next_state_machine[CommandStateMachine::NOT_FOUND];
        send_currnet_command();
    }

    GetSequenceCommandGroup::GetSequenceCommandGroup(
            util::weak_pointer<Client> cli,
            std::unique_ptr<Buffer> buffer,
    std::shared_ptr<std::string> key)
            : key(std::move(key))
            ,SequenceCommandGroup(cli, std::move(buffer))
    {
        _cache = client->get_proxy()->get_cache();
        _db = client->get_proxy()->get_db();

        // Init command sequence
        init_command_sequence();
        _next_command = _commands.begin();

//        LOG(DEBUG) << "GetSequenceCommandGroup " << buffer->to_string();
    }

    void GetSequenceCommandGroup::init_command_sequence() {
//        std::string key(_origin_command->command_key_pos.first,
//                        _origin_command->command_key_pos.second);
//        std::shared_ptr<std::string> key = key;

        LOG(DEBUG) << "GetSequenceCommandGroup::init_command_sequence _origin_command " << _origin_command->buffer->to_string();
        LOG(DEBUG) << "GetSequenceCommandGroup::init_command_sequence [" << *key << "]";
        /*
         Send GET command
         handler = get_command_take_one_handle_response
        response = GetCommand->handle_response()
         if (response == NOT_FOUND)
             DumpCommand
         */
        /*
         * current_status, response_result = handler(response)
         * current_status[response_result] -> command_should_be_issued, new handler, new status
         */
        /*
        GET_command:
         response_result = response_handler(response): SUCC -> RETUREN, NOT_FOUND -> DUMP_command, FAIL -> RETURN_FAIL
         DUMP_command:
         response_result = response_handler(response): SUCC -> RESTORE_command, NOT_FOUND -> RETURN, FAIL -> RETURN_FAIL
         */

        std::shared_ptr<CommandStateMachine>
                GetCommand =std::make_shared<CommandStateMachine>(),
                DumpCommand = std::make_shared<CommandStateMachine>(),
                RestoreCommand = std::make_shared<CommandStateMachine>(),
                GetCommand2 = std::make_shared<CommandStateMachine>(),
                ReturnResult = std::make_shared<CommandStateMachine>(),
                ReturnFail = std::make_shared<CommandStateMachine>();

        GetCommand->make_command = [this](std::shared_ptr<DataCommand> origin_command,
                                          std::shared_ptr<CommandStateMachine> previous_command,
                                          std::shared_ptr<Buffer> previous_response) {
            return this->_origin_command; };
        GetCommand->target_server = _cache;
        GetCommand->response_handler = _handle_response_of_get_command;
        GetCommand->next_state_machine[CommandStateMachine::SUCC] = ReturnResult;
        GetCommand->next_state_machine[CommandStateMachine::NOT_FOUND] = DumpCommand;
        GetCommand->next_state_machine[CommandStateMachine::FAIL] = ReturnFail;

        DumpCommand->make_command = [this](std::shared_ptr<DataCommand> origin_command,
                                                std::shared_ptr<CommandStateMachine> previous_command,
                                                std::shared_ptr<Buffer> previous_response) {
            return this->make_dump_command(this->key); };
        DumpCommand->target_server = _db;
        DumpCommand->response_handler = _handle_response_of_dump_from_db;
        DumpCommand->next_state_machine[CommandStateMachine::SUCC] = RestoreCommand;
        DumpCommand->next_state_machine[CommandStateMachine::NOT_FOUND] = ReturnResult;
        DumpCommand->next_state_machine[CommandStateMachine::FAIL] = ReturnFail;

        RestoreCommand->make_command = [this](std::shared_ptr<DataCommand> origin_command,
                                             std::shared_ptr<CommandStateMachine> previous_command,
                                             std::shared_ptr<Buffer> previous_response) {
            return this->make_restore_command(this->key, previous_response);
        };
        RestoreCommand->target_server = _cache;
        RestoreCommand->response_handler = _handle_response_of_restore_to_cache;
        RestoreCommand->next_state_machine[CommandStateMachine::SUCC] = GetCommand2;
        RestoreCommand->next_state_machine[CommandStateMachine::FAIL] = ReturnFail;

        GetCommand2->make_command = [this](std::shared_ptr<DataCommand> origin_command,
                                           std::shared_ptr<CommandStateMachine> previous_command,
                                           std::shared_ptr<Buffer> previous_response) {
            return this->_origin_command; };
        GetCommand2->target_server = _db;
        GetCommand2->response_handler = _handle_response_of_get_command;
        GetCommand2->next_state_machine[CommandStateMachine::SUCC] = ReturnResult;
        GetCommand2->next_state_machine[CommandStateMachine::NOT_FOUND] = ReturnResult;
        GetCommand2->next_state_machine[CommandStateMachine::FAIL] = ReturnFail;

        current_state = GetCommand;
        /*
        // 1. GET command (to cache)
        _commands.push_back(std::make_shared<CommandAndHandler>(
                std::make_tuple(_origin_command, _cache, _handle_response_of_read_from_cache_take_one)
        ));

        _dump_command = make_dump_command(key);

        // 2. if GET command failed: nil, send DUMP (to db)
        // if get succeed, return response to client
        _commands.push_back(std::make_shared<CommandAndHandler>(
                std::make_tuple(_dump_command, _db, _handle_response_of_dump_from_db)
        ));

        _restore_command = make_restore_command(key);

        // 3. if DUMP command failed: nil, return nil to client
        // if DUMP succeed, send RESTORE command (to cache)
        _commands.push_back(std::make_shared<CommandAndHandler>(
                std::make_tuple(_restore_command, _cache, _handle_response_of_restore_to_cache)
        ));

        // 4. if RESTORE command failed, return error to client
        // if RESTORE succeed, send GET command (to cache)
        _commands.push_back(std::make_shared<CommandAndHandler>(
                std::make_tuple(_origin_command, _cache, _handle_response_of_read_from_cache_take_two)
        ));

        // 5. if GET command (take 2) failed, return error to client
        // if GET command (take 2) succeed, send response to client
         */
    }

    void
    GetSequenceCommandGroup::_handle_response_of_read_from_cache_take_one(
            std::shared_ptr<DataCommand> command,
            std::shared_ptr<Response> response) {
        if (response->is_not_found()) {
            _dump_command = make_dump_command(_origin_command->key);
            // read failed: not found, next step: dump(db)
            _db->receive_request(
                    util::make_weak_pointer(*_dump_command.get()));
            _proxy->set_conn_poll_rw(_db);
        }
    }

    std::shared_ptr<DataCommand>
    GetSequenceCommandGroup::make_dump_command(std::shared_ptr<std::string> key) {
        const std::string DUMP_CMD = "*2\r\n$4\r\nDUMP\r\n${}\r\n{}\r\n";
        return std::make_shared<OneSlotCommand>(
                std::unique_ptr<Buffer>(new Buffer(fmt::format(DUMP_CMD, key->size(), *key))),
                util::weak_pointer<CommandGroup>(this), 0);
    }

    std::shared_ptr<DataCommand>
    GetSequenceCommandGroup::make_restore_command(
            std::shared_ptr<std::string> key,
            std::shared_ptr<Buffer> dump_response) {
        const std::string RESTORE_CMD = "*4\r\n"
                "$7\r\nRESTORE\r\n"
                "${}\r\n{}\r\n"
                "$1\r\n0\r\n"
                "{}";

        return std::make_shared<OneSlotCommand>(
                Buffer(fmt::format(RESTORE_CMD,
                                   key->size(), key,
                                   dump_response->to_string())),
                util::weak_pointer<CommandGroup>(this), 0);
    }
}
