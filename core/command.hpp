#ifndef __CERBERUS_COMMAND_HPP__
#define __CERBERUS_COMMAND_HPP__

#include <set>
#include <vector>

#include "utils/pointer.h"
#include "buffer.hpp"

namespace cerb {

    class Proxy;
    class Client;
    class Server;
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
        virtual void on_remote_responsed(Buffer rsp, bool error);

        void responsed();

        Command(Buffer b, util::weak_pointer<CommandGroup> g)
            : buffer(new Buffer(std::move(b)))
            , group(g)
        , handle_response(nullptr)
        {}

        explicit Command(util::weak_pointer<CommandGroup> g)
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

        explicit DataCommand(util::weak_pointer<CommandGroup> g)
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
        virtual bool wait_remote() const = 0;
        virtual void select_server_and_push_command_to_it(Proxy *proxy) = 0;
        virtual void append_buffer_to(BufferSet& b) = 0;
        virtual int total_buffer_size() const = 0;
        virtual void command_responsed() = 0;
        virtual void collect_stats(Proxy*) const {}
    };

    class OneSlotCommand
            : public DataCommand
    {
        slot const key_slot;
    public:
        OneSlotCommand(Buffer b, util::weak_pointer<CommandGroup> g, slot ks);
        Server* select_server(Proxy* proxy);
    };

    void split_client_command(Buffer& buffer, util::weak_pointer<Client> cli);

}

#endif /* __CERBERUS_COMMAND_HPP__ */
