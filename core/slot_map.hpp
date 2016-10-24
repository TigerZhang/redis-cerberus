#ifndef __CERBERUS_SLOT_MAP_HPP__
#define __CERBERUS_SLOT_MAP_HPP__

#include <set>
#include <string>

#include "common.hpp"
#include "utils/address.hpp"
//#include "server.hpp"

namespace cerb {

    class Server;
    class Proxy;

    typedef Server* ServerPtr;

    struct RedisNode {
        util::Address addr;
        std::string node_id;
        std::string master_id;
        std::set<std::pair<slot, slot>> slot_ranges;

        RedisNode(util::Address a, std::string nid)
            : addr(std::move(a))
            , node_id(std::move(nid))
        {}

        RedisNode(util::Address a, std::string nid, std::string mid)
            : addr(std::move(a))
            , node_id(std::move(nid))
            , master_id(std::move(mid))
        {}

        RedisNode(RedisNode&& rhs)
            : addr(std::move(rhs.addr))
            , node_id(std::move(rhs.node_id))
            , master_id(std::move(rhs.master_id))
            , slot_ranges(std::move(rhs.slot_ranges))
        {}

        RedisNode(RedisNode const& rhs)
            : addr(rhs.addr)
            , node_id(rhs.node_id)
            , master_id(rhs.master_id)
            , slot_ranges(rhs.slot_ranges)
        {}

        bool is_master() const
        {
            return master_id.empty();
        }
    };

    class SlotMap {
        ServerPtr _servers[CLUSTER_SLOT_COUNT];
    public:
        SlotMap();
        SlotMap(SlotMap const&) = delete;

        ServerPtr* begin()
        {
            return _servers;
        }

        ServerPtr* end()
        {
            return _servers + CLUSTER_SLOT_COUNT;
        }

        ServerPtr const* begin() const
        {
            return _servers;
        }

        ServerPtr const* end() const
        {
            return _servers + CLUSTER_SLOT_COUNT;
        }

        ServerPtr get_by_slot(slot s)
        {
            return _servers[s];
        }

        void set_by_slot(slot s, ServerPtr server)
        {
            _servers[s] = server;
        }

        void replace_map(std::vector<RedisNode> const& nodes, Proxy* proxy);
        void clear();
        ServerPtr random_addr() const;

        static void select_slave_if_possible(std::string host_beginning);
    };

    std::vector<RedisNode> parse_slot_map(std::string const& nodes_info,
                                          std::string const& default_host);
    void write_slot_map_cmd_to(int fd);

}

#endif /* __CERBERUS_SLOT_MAP_HPP__ */
