#ifndef __CERBERUS_CLIENT_HPP__
#define __CERBERUS_CLIENT_HPP__

#include <vector>

#include "command.hpp"
#include "connection.hpp"

namespace cerb {

    class Proxy;
    class Server;
    class CommandGroup;
    class Command;

    class Client
        : public ProxyConnection
    {
        void _write_response();
        void _read_request();

        Proxy* const _proxy;
    public:
        Proxy * get_proxy();

    private:
        std::set<Server*> _peers;
        std::vector<std::shared_ptr<CommandGroup>> _parsed_groups;
        std::vector<std::shared_ptr<CommandGroup>> _awaiting_groups;
        std::vector<std::shared_ptr<CommandGroup>> _ready_groups;
        int _count_of_requests_waiting_response_from_upstream_server;
        Buffer _buffer;
        BufferSet _downstream_outgoing_buffers;

        void _forward_request();
        void _write_outgoing_responses_to_client();
        void _do_write_response(); // and move command from awaiting group to ready group
    public:
        Client(int fd, Proxy* p);
        ~Client();

        void on_events(int events);
        void after_events(std::set<Connection*>&);
        std::string str() const;

        void handle_response();
        void add_peer(Server* svr);
        void reactivate(util::weak_pointer<Command> cmd);
        void push_command(std::shared_ptr<CommandGroup> g);
    };

}

#endif /* __CERBERUS_CLIENT_HPP__ */
