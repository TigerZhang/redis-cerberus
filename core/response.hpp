#ifndef __CERBERUS_RESPONSE_HPP__
#define __CERBERUS_RESPONSE_HPP__

#include <vector>

#include "utils/pointer.h"
#include "buffer.hpp"

namespace cerb {

    class DataCommand;
    class Proxy;

    class Response {
    public:
        Response() {}
        virtual ~Response() {}
        Response(Response const&) = delete;

        virtual void rsp_to(util::weak_pointer<DataCommand> c, util::weak_pointer<Proxy> p) = 0;
        virtual Buffer const& get_buffer() const = 0;
        virtual bool server_moved() const { return false; }
        virtual bool is_not_found() const { return false; }

        static std::string const NIL_STR;
        static Buffer const NIL;
    };

    std::vector<util::unique_pointer<Response>> split_server_response(Buffer& buffer);

}

#endif /* __CERBERUS_RESPONSE_HPP__ */
