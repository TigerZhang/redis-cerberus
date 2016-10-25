Major Processes
===============

Listener
========

```
Acceptor
  \
 on_events
   \
  _proxy->new_client
```

Data flow
=========

```
Client                                                     Server
  \                                                           /
on_events                                               on_events
   \                                                         /
_read_request                                      _read_response
    \                                                       /
::split_client_command                      split_server_response
     \                                                     /
_forward_request                                 forward_response
      \                                                   /
SequenceCommandGroup->send_current_command       forward_response
       \                                                 /
Server::receive_request             DataCommand::receive_response


Client                                                         Server
  \                                                               /
_write_response                                       _handle_request
   \                                                             /
_do_write_response                  _upstream_outgoing_buffers.append
    \                                                           /
_write_outgoing_responses_to_client _upstream_outgoing_buffers.writev
```

Command Altering
================
...
