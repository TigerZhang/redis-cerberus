cmake_minimum_required(VERSION 3.3)
project(redis_cerberus)

include_directories(".")
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -O0 -ggdb3 -std=c++11 -Wall -Wextra -Wold-style-cast -Werror -DSEQ_COMMAND")

#if(MODE STREQUAL "debug")
#    set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -ggdb3 -DELPP_DISABLE_DEBUG_LOGS -DELPP_NO_DEFAULT_LOG_FILE")
#endif()

set(SOURCE_FILES
    backtracpp/demangle.cpp
    backtracpp/demangle.h
    backtracpp/sig-handler.cpp
    backtracpp/sig-handler.h
    backtracpp/trace.cpp
    backtracpp/trace.h
    core/acceptor.cpp
    core/acceptor.hpp
    core/buffer.cpp
    core/buffer.hpp
    core/client.cpp
    core/client.hpp
    core/command.cpp
    core/command.hpp
    core/concurrence.cpp
    core/concurrence.hpp
    core/connection.cpp
    core/connection.hpp
    core/fdutil.cpp
    core/fdutil.hpp
    core/globals.cpp
    core/globals.hpp
    core/message.cpp
    core/message.hpp
    core/proxy.cpp
    core/proxy.hpp
    core/response.cpp
    core/response.hpp
    core/server.cpp
    core/server.hpp
    core/slot_calc.cpp
    core/slot_calc.hpp
    core/slot_map.cpp
    core/slot_map.hpp
    core/stats.cpp
    core/stats.hpp
    core/subscription.cpp
    core/subscription.hpp
    cppformat/format.cc
    cppformat/format.h
    cppformat/posix.cc
    cppformat/posix.h
    except/exceptions.hpp
    syscalls/cio.h
    syscalls/fctl.h
    syscalls/poll.h
    utils/address.cpp
    utils/address.hpp
    utils/alg.hpp
    utils/easylogging++.h
    utils/logging.cpp
    utils/logging.hpp
    utils/pointer.cpp
    utils/pointer.h
    utils/random.cpp
    utils/random.hpp
    utils/string.cpp
    utils/string.h
    utils/typetraits.hpp
    common.hpp
    main.cpp)

find_package(Threads REQUIRED)
add_executable(redis_cerberus ${SOURCE_FILES})
target_link_libraries (redis_cerberus ${CMAKE_THREAD_LIBS_INIT})
