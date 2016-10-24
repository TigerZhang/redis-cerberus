#ifndef __CERBERUS_SERVER_HPP__
#define __CERBERUS_SERVER_HPP__

#include <map>
#include <set>

#include "proxy.hpp"
#include "buffer.hpp"
#include "connection.hpp"
#include "utils/pointer.h"
#include "utils/address.hpp"
#include "response.hpp"

namespace cerb {

    class Client;
    class DataCommand;
    class SingleCommandGroup;
    class Server;

//    typedef std::shared_ptr<Server> ServerPtr;
    typedef Server* ServerPtr;

    class Server
        : public ProxyConnection
    {
        Proxy* _proxy;
        Buffer _buffer;
        BufferSet _upstream_outgoing_buffers;

        std::vector<util::weak_pointer<DataCommand>> _incoming_commands;
        std::vector<util::weak_pointer<DataCommand>> _sent_commands;

        void _read_response();
        void _reconnect(util::Address const& addr, Proxy* p);
        void _handle_request(int events);

        Server()
            : ProxyConnection(-1)
            , _proxy(nullptr)
            , addr("", 0)
        {}

        ~Server() = default;

        static ServerPtr _alloc_server(util::Address const& addr, Proxy* p);
    public:
        typedef enum{CACHE, DB} Type;
        Type type;

        util::Address addr;
        std::set<ProxyConnection*> attached_long_connections;

        static void send_readonly_for_each_conn();
        static ServerPtr get_server(util::Address addr, Proxy* p);
        static std::map<util::Address, ServerPtr>::iterator addr_begin();
        static std::map<util::Address, ServerPtr>::iterator addr_end();

        void on_events(int events);
        void after_events(std::set<Connection*>&);
        std::string str() const;

        void on_error()
        {
            this->close_conn();
        }

        void close_conn();
        void receive_request(util::weak_pointer<DataCommand> cmd);
        void pop_client(Client* cli);
        std::vector<util::weak_pointer<DataCommand>> deliver_commands();

        void attach_long_connection(ProxyConnection* c)
        {
            this->attached_long_connections.insert(c);
            this->_proxy->incr_long_conn();
        }

        void detach_long_connection(ProxyConnection* c)
        {
            this->attached_long_connections.erase(c);
            this->_proxy->decr_long_conn();
        }

        bool need_try_to_promote_from_db(util::weak_pointer<DataCommand> command);

        void try_to_promote(util::weak_pointer<DataCommand> command, Server *server);
    };

}

#endif /* __CERBERUS_SERVER_HPP__ */
