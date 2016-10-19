#include "response.hpp"
#include "command.hpp"
#include "proxy.hpp"
#include "message.hpp"
#include "utils/string.h"
#include "utils/address.hpp"
#include "utils/logging.hpp"

using namespace cerb;

std::string const Response::NIL_STR("$-1\r\n");
Buffer const Response::NIL(NIL_STR);

namespace {

    class NormalResponse
        : public Response
    {
    public:
        Buffer response_buffer;
        bool error;

        NormalResponse(Buffer r, bool e)
            : response_buffer(std::move(r))
            , error(e)
        {}

        void forward_response(util::weak_pointer<DataCommand> cmd, util::weak_pointer<Proxy>)
        {
            cmd->receive_response(std::move(this->response_buffer), error);
        }

        Buffer const& get_buffer() const
        {
            return response_buffer;
        }

        bool is_not_found() const {
            return response_buffer.same_as_string("$-1\r\n");
        }
    };

    class RetryMovedAskResponse
        : public Response
    {
        static Buffer const dump;
    public:
        void forward_response(util::weak_pointer<DataCommand> cmd, util::weak_pointer<Proxy> p)
        {
            p->retry_move_ask_command_later(cmd);
        }

        Buffer const& get_buffer() const
        {
            return dump;
        }

        bool server_moved() const
        {
            return true;
        }
    };
    Buffer const RetryMovedAskResponse::dump("$ RETRY MOVED OR ASK $");

    class ServerResponseSplitter
        : public cerb::msg::MessageSplitterBase<
            Buffer::iterator, ServerResponseSplitter>
    {
        typedef Buffer::iterator Iterator;
        typedef cerb::msg::MessageSplitterBase<Iterator, ServerResponseSplitter> BaseType;

        std::string _last_error;

        void _push_retry_rsp()
        {
            this->responses.push_back(util::make_unique_ptr(new RetryMovedAskResponse));
        }

        void _push_normal_rsp(Iterator begin, Iterator end)
        {
            this->responses.push_back(util::make_unique_ptr(
                    new NormalResponse(Buffer(begin, end), !this->_last_error.empty())));
        }

        void _push_rsp(Iterator i)
        {
            if (!_last_error.empty()) {
                if (util::stristartswith(_last_error, "MOVED") ||
                    util::stristartswith(_last_error, "ASK") ||
                    util::stristartswith(_last_error, "CLUSTERDOWN"))
                {
                    LOG(DEBUG) << "Retry due to " << _last_error;
                    return this->_push_retry_rsp();
                }
            }
            this->_push_normal_rsp(this->_split_points.back(), i);
        }
    public:
        std::vector<util::unique_pointer<Response>> responses;

        explicit ServerResponseSplitter(Iterator i)
            : BaseType(i)
        {}

        void on_split_point(Iterator next)
        {
            this->_push_rsp(next);
            this->_last_error.clear();
        }

        void on_error(Iterator begin, Iterator end)
        {
            this->_last_error = std::string(begin, end);
        }
    };

}

std::vector<util::unique_pointer<Response>> cerb::split_server_response(Buffer& buffer)
{
    ServerResponseSplitter r(msg::split_by(
        buffer.begin(), buffer.end(), ServerResponseSplitter(buffer.begin())));
    if (r.finished()) {
        buffer.clear();
    } else {
        buffer.truncate_from_begin(r.interrupt_point());
    }
    return std::move(r.responses);
}
