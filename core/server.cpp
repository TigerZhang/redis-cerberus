#include <map>
#include <cppformat/format.h>

#include "command.hpp"
#include "server.hpp"
#include "client.hpp"
#include "proxy.hpp"
#include "response.hpp"
#include "except/exceptions.hpp"
#include "utils/alg.hpp"
#include "utils/logging.hpp"
#include "syscalls/poll.h"
#include "syscalls/fctl.h"

using namespace cerb;

void Server::on_events(int events)
{
    if (this->closed()) {
        return;
    }
    if (poll::event_is_hup(events)) {
        return this->close_conn();
    }
    if (poll::event_is_read(events)) {
        try {
            this->_recv_from();
        } catch (BadRedisMessage& e) {
            LOG(ERROR) << "Receive bad message from server " << this->str()
                       << " because: " << e.what()
                       << " dump buffer (before close): "
                       << this->_buffer.to_string();
            return this->close_conn();
        }
    }
    this->_push_to_buffer_set();
    if (poll::event_is_write(events)) {
        this->_outgoing_buffers.writev(this->fd);
    }
    if (this->_outgoing_buffers.empty()) {
        this->_proxy->set_conn_poll_ro(this);
    } else {
        this->_proxy->set_conn_poll_rw(this);
    }
}

void Server::_push_to_buffer_set()
{
    auto now = Clock::now();
    for (util::sref<DataCommand> c: this->_incoming_commands) {
        this->_sent_commands.push_back(c);
        this->_outgoing_buffers.append(c->buffer);
        c->sent_time = now;
    }
    this->_incoming_commands.clear();
}

void Server::_recv_from()
{
    int n = this->_buffer.read(this->fd);
    if (n == 0) {
        throw ConnectionHungUp();
    }
    LOG(DEBUG) << "Read " << this->str() << " buffer size " << this->_buffer.size();
    auto responses(split_server_response(this->_buffer));
    if (responses.size() > this->_sent_commands.size()) {
        LOG(ERROR) << "+Error on split, expected size: " << this->_sent_commands.size()
                   << " actual: " << responses.size() << " dump buffer:";
        for (util::sptr<Response> const& rsp: responses) {
            LOG(ERROR) << "::: " << rsp->get_buffer().to_string();
        }
        LOG(ERROR) << "Rest buffer: " << this->_buffer.to_string();
        return this->close_conn();
    }
    LOG(DEBUG) << "+responses size: " << responses.size();
    LOG(DEBUG) << "+rest buffer: " << this->_buffer.size();
    auto cmd_it = this->_sent_commands.begin();
    auto now = Clock::now();
    for (util::sptr<Response>& rsp: responses) {
        util::sref<DataCommand> c = *cmd_it++;
        if (c.not_nul()) {
            if (c->origin_command) {
                // it's internal cache to db command

                // e.g.: when a key missed in cache,
                // try to promote it from db (dump/restore)
//                rsp->rsp_to(c->origin_command, util::mkref(*this->_proxy));
//                c->resp_time = now;
//                c->origin_command->resp_time = now;

                // if the command is dump and it's forwarded command
                // if (c->command_name = COMMAND_DUMP) {
                // }
                // send restore to original server
                std::string respone_str = rsp->get_buffer().to_string();
                // TODO: better implementation for checking if the response is "key not found"
                if (c->commandType == Command::DUMP_COMMAND) {
                    if (rsp->is_not_found()) {
                        rsp->rsp_to(util::mkref(*c->origin_command),
                                    util::mkref(*this->_proxy));
                        c->origin_command->resp_time = now;
                        c->resp_time = now;
                    } else if (c->origin_server) {
                        slot s = 0;
                        const std::string RESTORE_CMD = "*4\r\n"
                                "$7\r\nRESTORE\r\n"
                                "${}\r\n{}\r\n"
                                "$1\r\n0\r\n"
                                "{}";

                        // FIXME: better implementation of constructing a string
                        std::string key(c->origin_command->command_key_pos.first,
                            c->origin_command->command_key_pos.second);
//                        for (Buffer::iterator i = c->command_key_pos.first; i <= c->command_key_pos.second; i++) {
//                            key += static_cast<char>(*i);
//                        };
                        LOG(DEBUG) << "Try to restore to origin server. " << key;
                        Buffer restore_command_buffer(
                                fmt::format(RESTORE_CMD, key.size(), key, respone_str));
                        OneSlotCommand* restore_command = new OneSlotCommand(
                                std::move(restore_command_buffer),
                                c->group, s);
                        restore_command->origin_command = c.get();
                        restore_command->commandType = Command::RESTORE_COMMAND;
                        c->origin_server->push_client_command(util::mkref(*restore_command));
                        _proxy->set_conn_poll_rw(c->origin_server);
                    }
                } else if (c->commandType == Command::RESTORE_COMMAND) {
                    DataCommand* dump_command = c->origin_command;
                    DataCommand* origin_command = dump_command->origin_command;
                    dump_command->origin_server->push_client_command(util::mkref(*origin_command));
                    _proxy->set_conn_poll_rw(dump_command->origin_server);
                }

            } else {
                if (rsp->is_not_found() && need_try_to_promote_from_db(c)) {
                    try_to_promote(c, this);
//                Response& response_from_db =
//                response_from_db.rsp_to(c, util::mkref(*this->_proxy));
                } else {
                    rsp->rsp_to(c, util::mkref(*this->_proxy));
                    c->resp_time = now;
                }
            }
        }
    }
    this->_sent_commands.erase(this->_sent_commands.begin(), cmd_it);
}

void Server::push_client_command(util::sref<DataCommand> cmd)
{
    _incoming_commands.push_back(cmd);
    cmd->group->client->add_peer(this);
}

void Server::pop_client(Client* cli)
{
    util::erase_if(
        this->_incoming_commands,
        [&](util::sref<DataCommand> cmd)
        {
            return cmd->group->client.is(cli);
        });
    for (util::sref<DataCommand>& cmd: this->_sent_commands) {
        if (cmd.not_nul() && cmd->group->client.is(cli)) {
            cmd.reset();
        }
    }
}

std::vector<util::sref<DataCommand>> Server::deliver_commands()
{
    util::erase_if(
        this->_sent_commands,
        [](util::sref<DataCommand> cmd)
        {
            return cmd.nul();
        });
    _incoming_commands.insert(_incoming_commands.end(), _sent_commands.begin(),
                     _sent_commands.end());
    return std::move(_incoming_commands);
}

// TODO: using smart pointer
static thread_local std::map<util::Address, Server*> servers_map;
static thread_local std::vector<Server*> servers_pool;

static void remove_entry(Server* server)
{
    ::servers_map.erase(server->addr);
    ::servers_pool.push_back(server);
}

void Server::after_events(std::set<Connection*>&)
{
    if (this->closed()) {
        this->_proxy->update_slot_map();
    }
}

std::string Server::str() const
{
    return fmt::format("Server({}@{})[{}]", this->fd,
                       static_cast<void const*>(this), this->addr.str());
}

void Server::close_conn()
{
    if (!this->closed()) {
        LOG(INFO) << "Close " << this->str();
        this->close();
        this->_buffer.clear();
        this->_outgoing_buffers.clear();

        for (util::sref<DataCommand> c: this->_incoming_commands) {
            this->_proxy->retry_move_ask_command_later(c);
        }
        this->_incoming_commands.clear();

        for (util::sref<DataCommand> c: this->_sent_commands) {
            if (c.nul()) {
                continue;
            }
            this->_proxy->retry_move_ask_command_later(c);
        }
        this->_sent_commands.clear();

        for (ProxyConnection* conn: this->attached_long_connections) {
            this->_proxy->inactivate_long_conn(conn);
        }
        this->attached_long_connections.clear();

        ::remove_entry(this);
    }
}

std::map<util::Address, Server*>::iterator Server::addr_begin()
{
    return ::servers_map.begin();
}

std::map<util::Address, Server*>::iterator Server::addr_end()
{
    return ::servers_map.end();
}

static std::function<void(int, std::vector<util::sref<DataCommand>>&)> on_server_connected(
    [](int, std::vector<util::sref<DataCommand>>&) {});

void Server::_reconnect(util::Address const& addr, Proxy* p)
{
    this->fd = fctl::new_stream_socket();
    this->_proxy = p;
    this->addr = addr;

    fctl::set_nonblocking(this->fd);
    fctl::connect_fd(addr.host, addr.port, this->fd);
    LOG(INFO) << "Open " << this->str();
    p->poll_add_rw(this);
    ::on_server_connected(this->fd, this->_sent_commands);
}

Server* Server::_alloc_server(util::Address const& addr, Proxy* p)
{
    if (servers_pool.empty()) {
        for (int i = 0; i < 8; ++i) {
            servers_pool.push_back(new Server);
            LOG(DEBUG) << "Allocate Server: " << servers_pool.back();
        }
    }
    Server* s = servers_pool.back();
    try {
        s->_reconnect(addr, p);
    } catch (IOErrorBase& e) {
        LOG(ERROR) << "Fail to open server " << s->str() << " because " << e.what();
        s->close_conn();
    }
    servers_pool.pop_back();
    return s;
}

Server* Server::get_server(util::Address addr, Proxy* p)
{
    auto i = servers_map.find(addr);
    if (i == servers_map.end() || i->second->closed()) {
        Server* s = Server::_alloc_server(addr, p);
        servers_map.insert(std::make_pair(std::move(addr), s));
        return s;
    }
    return i->second;
}

static std::string const READONLY_CMD("READONLY\r\n");

void Server::send_readonly_for_each_conn()
{
    ::on_server_connected =
        [](int fd, std::vector<util::sref<DataCommand>>& cmds)
        {
            flush_string(fd, READONLY_CMD);
            cmds.push_back(util::sref<DataCommand>(nullptr));
        };
}

bool Server::need_try_to_promote_from_db(util::sref<DataCommand> command) {
    if (this->type == Type::DB) {
        return false;
    }

//    command->command_name_pos
    LOG(DEBUG) << "command name pos begin: "
               << *command->command_name_pos.first;
    if (*command->command_name_pos.first == 'g') {
        if (*(command->command_name_pos.first+1) == 'e') {
            if (*(command->command_name_pos.first+2) == 't') {
                return true;
            }
        }
    }
    return false;
}

const std::string DUMP_CMD = "*2\r\n$4\r\nDUMP\r\n${}\r\n{}\r\n";

// FIXME: if there are multiple clients try to access the same key simultaneously,
// there will be multiple promotion issued.
// TODO: it's better to make sure the promotion just happened one time.
void Server::try_to_promote(util::sref<DataCommand> command, Server *server) {
    Server* db = _proxy->get_db();

    std::string key(command->command_key_pos.first,
                  command->command_key_pos.second);
    LOG(DEBUG) << "Try to promote " << key;

    Buffer dump_command_buffer(fmt::format(DUMP_CMD, key.size(), key));
    if (db) {
        slot s = 0;
        OneSlotCommand* dump_command =new OneSlotCommand(
                std::move(dump_command_buffer),
                command->group, s);
        dump_command->origin_command = command.get();
        dump_command->origin_server = server;
        dump_command->commandType = Command::DUMP_COMMAND;
        db->push_client_command(util::mkref(*dump_command));
        _proxy->set_conn_poll_rw(db);
    }
}