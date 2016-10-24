//
// Created by zhanghu on 10/22/16.
//

#include <utils/easylogging++.h>
#include "client.hpp"
#include "SetSequenceCommandGroup.h"

namespace cerb {
    SetSequenceCommandGroup::SetSequenceCommandGroup(
            util::weak_pointer <Client> cli,
            std::shared_ptr <Buffer> buffer,
            std::shared_ptr <Buffer>) :
    SequenceCommandGroup(cli, buffer)
    {
        // Init command sequence
        init_command_sequence();
    }

    void SetSequenceCommandGroup::init_command_sequence() {
        LOG(DEBUG) << "SetSequenceCommandGroup::init_command_sequence _origin_command " << _origin_command->command_buffer->to_string();

        std::shared_ptr<CommandStateMachine>
                SetCommandCache =std::make_shared<CommandStateMachine>(),
                SetCommandDb = std::make_shared<CommandStateMachine>(),
                ReturnResult = std::make_shared<CommandStateMachine>(),
                ReturnFail = std::make_shared<CommandStateMachine>();

        ReturnFail->return_now = true;
        ReturnResult->return_now = true;

        SetCommandCache->make_command = [this](std::shared_ptr<DataCommand> ,
                                          std::shared_ptr<CommandStateMachine> ,
                                          std::shared_ptr<Buffer> ) {
            return this->_origin_command; };
        SetCommandCache->target_server = _cache;
        SetCommandCache->next_state_machine[CommandStateMachine::SUCC] = SetCommandDb;
        SetCommandCache->next_state_machine[CommandStateMachine::FAIL] = ReturnFail;


        SetCommandDb->make_command = [this](std::shared_ptr<DataCommand> ,
                                               std::shared_ptr<CommandStateMachine> ,
                                               std::shared_ptr<Buffer> ) {
            return this->_origin_command; };
        SetCommandDb->target_server = _db;
        SetCommandDb->next_state_machine[CommandStateMachine::SUCC] = ReturnResult;
        SetCommandDb->next_state_machine[CommandStateMachine::FAIL] = ReturnFail;

        current_state = SetCommandCache;
    }
}
