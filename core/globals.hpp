#ifndef __CERBERUS_GLOBALS_HPP__
#define __CERBERUS_GLOBALS_HPP__

#include <set>
#include <vector>

#include "common.hpp"
#include "concurrence.hpp"
#include "utils/pointer.h"
#include "utils/address.hpp"

namespace cerb_global {

    extern std::vector<cerb::ListenThread> all_threads;
    extern thread_local cerb::msize_t allocated_buffer;

    void set_remotes(std::set<util::Address> remotes);
    std::set<util::Address>& get_remotes();

    void set_cluster_req_full_cov(bool c);
    bool cluster_req_full_cov();

    void set_cluster_ok(bool ok);
    bool cluster_ok();

    void set_cache(util::Address cache);
    util::Address& get_cache();

    void set_db(util::Address db);
    util::Address& get_db();

    extern bool stopped;
}

#endif /* __CERBERUS_GLOBALS_HPP__ */
