#include <cppformat/format.h>

#include "acceptor.hpp"
#include "except/exceptions.hpp"
#include "syscalls/fctl.h"
#include "syscalls/poll.h"

using namespace cerb;

Acceptor::Acceptor(util::weak_pointer<Proxy> p, int listen_port)
    : Connection(fctl::new_stream_socket())
    , _proxy(p)
{
    fctl::set_nonblocking(this->fd);
    fctl::bind_to(this->fd, listen_port);
    p->poll_add_ro(this);
}

void Acceptor::on_events(int)
{
    int cfd;
    while ((cfd = cio::accept(this->fd)) > 0)
    {
        fctl::set_nonblocking(cfd);
        fctl::set_tcpnodelay(cfd);
        this->_proxy->new_client(cfd);
    }
    if (cfd == -1) {
        if (errno != EAGAIN && errno != ECONNABORTED
            && errno != EPROTO && errno != EINTR)
        {
            throw SocketAcceptError(errno);
        }
    }
}

std::string Acceptor::str() const
{
    return fmt::format("Acceptor({}@{})", this->fd, static_cast<void const*>(this));
}
