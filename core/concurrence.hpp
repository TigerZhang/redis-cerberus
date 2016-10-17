#ifndef __CERBERUS_CONCURRENCE_HPP__
#define __CERBERUS_CONCURRENCE_HPP__

#include <thread>

#include "common.hpp"
#include "proxy.hpp"
#include "utils/pointer.h"

namespace cerb {

    class ListenThread {
        int const _listen_port;
        util::unique_pointer<Proxy> _proxy;
        util::unique_pointer<std::thread> _thread;
        msize_t const* _mem_buffer_stat;
    public:
        explicit ListenThread(int listen_port);
        ListenThread(ListenThread const&) = delete;

        ListenThread(ListenThread&& rhs)
            : _listen_port(rhs._listen_port)
            , _proxy(std::move(rhs._proxy))
            , _thread(std::move(rhs._thread))
            , _mem_buffer_stat(rhs._mem_buffer_stat)
        {}

        void run();
        void join();

        util::weak_pointer<Proxy const> get_proxy() const
        {
            return *_proxy;
        }

        util::weak_pointer<Proxy> get_proxy()
        {
            return *_proxy;
        }

        msize_t buffer_allocated() const
        {
            return *_mem_buffer_stat;
        }
    };

}

#endif /* __CERBERUS_CONCURRENCE_HPP__ */
