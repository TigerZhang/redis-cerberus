#ifndef __CERBERUS_ACCEPTOR_HPP__
#define __CERBERUS_ACCEPTOR_HPP__

#include "connection.hpp"
#include "proxy.hpp"

namespace cerb {

    class Acceptor
        : public Connection
    {
        util::weak_pointer<Proxy> const _proxy;
    public:
        Acceptor(util::weak_pointer<Proxy> p, int listen_port);

        void on_events(int);
        void on_error() {}
        std::string str() const;
    };

}

#endif /* __CERBERUS_ACCEPTOR_HPP__ */
