//
// Created by zhanghu on 10/22/16.
//

#ifndef REDIS_CERBERUS_SETSEQUENCECOMMANDGROUP_H
#define REDIS_CERBERUS_SETSEQUENCECOMMANDGROUP_H

#include "utils/pointer.h"

namespace cerb {

    class SequenceCommandGroup;
    class Client;
    class Buffer;

    class SetSequenceCommandGroup : public SequenceCommandGroup {
    public:
        explicit SetSequenceCommandGroup(util::weak_pointer <Client> cli,
                                         std::shared_ptr <Buffer> buffer,
                                         std::shared_ptr <Buffer> key);

        virtual ~SetSequenceCommandGroup() {}

        void init_command_sequence();

        bool should_send_to_upstream_server_and_wait_response() const { return true; }

        bool is_sequence_group() const { return true; }

        std::shared_ptr <Buffer> key;
        std::shared_ptr <Buffer> buffer;
    };

}



#endif //REDIS_CERBERUS_SETSEQUENCECOMMANDGROUP_H
