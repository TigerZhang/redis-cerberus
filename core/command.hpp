#ifndef __CERBERUS_COMMAND_HPP__
#define __CERBERUS_COMMAND_HPP__

#include <memory>
#include <set>
#include <vector>
#include <map>

#include "utils/pointer.h"
#include "buffer.hpp"
#include "response.hpp"
#include "client.hpp"

namespace cerb {

    class Proxy;
    class Server;
    class Client;
    class CommandGroup;

    class Command {
    public:
        typedef enum { UNKOWN_COMMAND, DUMP_COMMAND, RESTORE_COMMAND } CommandType;
        std::shared_ptr<Buffer> buffer;
        util::weak_pointer<CommandGroup> const group;
        std::pair<Buffer::iterator, Buffer::iterator> command_name_pos;
        std::pair<Buffer::iterator, Buffer::iterator> command_key_pos;

        virtual ~Command() = default;

        virtual Server* select_server(Proxy* proxy) = 0;
        virtual void receive_response(Buffer rsp, bool error);

        void responsed();

        Command(Buffer b, util::weak_pointer<CommandGroup> g)
            : buffer(new Buffer(std::move(b)))
            , group(g)
        , handle_response(nullptr)
        {}

        Command(Buffer b, std::shared_ptr<CommandGroup> g)
                : buffer(new Buffer(std::move(b)))
                , group(g)
                , handle_response(nullptr)
        {}

        Command(std::shared_ptr<Buffer> b, std::shared_ptr<CommandGroup> g)
                : buffer(b)
                , group(g)
                , handle_response(nullptr)
        {}

        Command(std::shared_ptr<Buffer> b, util::weak_pointer<CommandGroup> g)
                : buffer(b)
                , group(g)
                , handle_response(nullptr)
        {}

        explicit Command(std::shared_ptr<CommandGroup> g)
            : buffer(new Buffer)
            , group(g)
        , handle_response(nullptr)
        {}

        Command(Command const&) = delete;

        static void allow_write_commands();

        std::function<void (Command* command, Server* server, void* context)>
                handle_response;
    };

    class DataCommand
        : public Command
    {
    public:
        DataCommand(Buffer b, util::weak_pointer<CommandGroup> g)
            : Command(std::move(b), g),
              origin_command(nullptr),
              origin_server(nullptr),
              commandType(Command::UNKOWN_COMMAND)
        {}

        DataCommand(std::shared_ptr<Buffer> b, util::weak_pointer<CommandGroup> g)
                : Command(b, g),
                  origin_command(nullptr),
                  origin_server(nullptr),
                  commandType(Command::UNKOWN_COMMAND)
        {}

        DataCommand(std::shared_ptr<Buffer> b, std::shared_ptr<CommandGroup> g)
                : Command(b, g),
                  origin_command(nullptr),
                  origin_server(nullptr),
                  commandType(Command::UNKOWN_COMMAND)
        {}

        explicit DataCommand(std::shared_ptr<CommandGroup> g)
            : Command(g),
              origin_command(nullptr),
              origin_server(nullptr),
              commandType(Command::UNKOWN_COMMAND)
        {}

        Time sent_time;
        Time resp_time;

        Interval remote_cost() const
        {
            return resp_time - sent_time;
        }

        DataCommand* origin_command; // for promote command. FIXME: should be shared_ptr?
        Server* origin_server; // FIXME: should be shared_ptr?

        Command::CommandType commandType;
        std::shared_ptr<std::string> key;
    };

    class CommandGroup {
    public:
        util::weak_pointer<Client> const client;

        explicit CommandGroup(util::weak_pointer<Client> cli)
            : client(cli)
        {}

        CommandGroup(CommandGroup const&) = delete;
        virtual ~CommandGroup() = default;

        virtual bool long_connection() const
        {
            return false;
        }

        virtual void deliver_client(Proxy*) {}
        virtual bool should_send_to_upstream_server_and_wait_response() const = 0;
        virtual void select_server_and_push_command_to_it(Proxy *proxy) = 0;
        virtual void enqueue_response_to_client(BufferSet &b) = 0;
        virtual int total_buffer_size() const = 0;
        virtual void receive_response(std::shared_ptr<Buffer> ptr) = 0;
        virtual void collect_stats(Proxy*) const {}
        virtual bool is_sequence_group() const { return false; }
    };

    /***
  +--------+                       +---------------+                         +----------------------+
  |        |  Received a command   |               |  Create a CommandGroup  | StateMachine for     |
  | Client | +-------------------> | Parse command | +---------------------> | handling             |
  |        |                       |               |                         | CommandSequence      |
  +--------+                       +---------------+       ^                 +----------------------+
                                                           |
                                                           |
                                                           +
                                                  Create a CommandSequence here

              CommandSequence: GET: GET(cache) -> DUMP(db) -> RESTORE(cache) -> GET(cache)




CommandGroup: A Command from client like MSET will be splitted to multiple SET commands
That's why CommnadGroup.

     ***/
    class CommandStateMachine {
    public:
        enum ResponseResult { SUCC, FAIL, NOT_FOUND, ZERO, BIG_THAN_ZERO } ;

        CommandStateMachine() :
                command(nullptr)
                , target_server(nullptr) {}

        std::shared_ptr<DataCommand> command;
        std::function<std::shared_ptr<DataCommand>
                (std::shared_ptr<DataCommand> origin_command,
                std::shared_ptr<CommandStateMachine> previous_command,
                std::shared_ptr<Buffer> previous_response)>
                make_command;
        std::function<CommandStateMachine::ResponseResult
                (std::shared_ptr<DataCommand> command, std::shared_ptr<Response> response,
                 std::shared_ptr<Buffer> previous_response)>
                response_handler;
        std::map<CommandStateMachine::ResponseResult, std::shared_ptr<CommandStateMachine>> next_state_machine;
        Server *target_server;

        bool return_now = false;
    };

    class SequenceCommandGroup : public CommandGroup {
    public:
        typedef std::function<CommandStateMachine::ResponseResult
                (std::shared_ptr<DataCommand> command, std::shared_ptr<Response> response,
                 std::shared_ptr<Buffer> previous_response)> ResponseHandler;
        typedef std::tuple<std::shared_ptr<DataCommand>, std::shared_ptr<Server>,
                ResponseHandler> CommandAndHandler;
        typedef std::vector<std::shared_ptr<CommandAndHandler>> CommnadAndHandlers;
        enum {COMMAND, SERVER, HANDLER};

        explicit SequenceCommandGroup(util::weak_pointer<Client> cli, std::shared_ptr<Buffer> buffer);

        virtual ~SequenceCommandGroup() {};

        bool long_connection() const
        {
            return false;
        }
        void deliver_client(Proxy*) {}
        bool should_send_to_upstream_server_and_wait_response() const { return false; }
        void select_server_and_push_command_to_it(Proxy *proxy) { proxy = proxy; }
        void enqueue_response_to_client(BufferSet &b);
        int total_buffer_size() const { return 0; }
        void receive_response(std::shared_ptr<Buffer> ptr);
        void collect_stats(Proxy*) const {}

        ResponseHandler _handle_response_of_get_command;
        std::shared_ptr<DataCommand> _origin_command;
        std::shared_ptr<CommandStateMachine> current_state;

        std::shared_ptr<DataCommand> origin_command;
        std::shared_ptr<CommandStateMachine> previous_command;
        std::shared_ptr<Buffer> previous_response;

        std::vector<std::shared_ptr<CommandStateMachine>> command_state_machines;

        void send_currnet_command();

    protected:
        CommnadAndHandlers _commands;
        CommnadAndHandlers::iterator _next_command;
        Server *_cache, *_db;
        Proxy* _proxy;
    };

    class GetSequenceCommandGroup : public SequenceCommandGroup {
    public:
        explicit GetSequenceCommandGroup(util::weak_pointer<Client> cli,
                                         std::shared_ptr<Buffer> buffer,
        std::shared_ptr<std::string> key);
        virtual ~GetSequenceCommandGroup() {}

        void init_command_sequence();

        void _handle_response_of_read_from_cache_take_one(
                std::shared_ptr<DataCommand>, std::shared_ptr<Response>);

        std::shared_ptr<DataCommand> _dump_command;
        ResponseHandler _handle_response_of_dump_from_db;

        std::shared_ptr<DataCommand> _restore_command;
        ResponseHandler _handle_response_of_restore_to_cache;

        ResponseHandler _handle_response_of_read_from_cache_take_two;

        std::shared_ptr<DataCommand> make_dump_command(std::shared_ptr<std::string> key);

        std::shared_ptr<DataCommand> make_restore_command(std::shared_ptr<std::string> key,
                                                          std::shared_ptr<Buffer> dump_response);

        bool should_send_to_upstream_server_and_wait_response() const { return true; }
        bool is_sequence_group() const { return true; }

        std::shared_ptr<std::string> key;
        std::shared_ptr<Buffer> buffer;
    };

    class OneSlotCommand
            : public DataCommand
    {
        slot const key_slot;
    public:
        OneSlotCommand(Buffer b, util::weak_pointer<CommandGroup> g, slot ks);
        OneSlotCommand(std::shared_ptr<Buffer> b, util::weak_pointer<CommandGroup> g, slot ks);
        OneSlotCommand(std::shared_ptr<Buffer> b, std::shared_ptr<CommandGroup> g, slot ks);
        OneSlotCommand(Buffer b, std::shared_ptr<CommandGroup> g, slot ks);

        Server* select_server(Proxy* proxy);
    };

    void split_client_command(Buffer& buffer, util::weak_pointer<Client> cli);

}

#endif /* __CERBERUS_COMMAND_HPP__ */
