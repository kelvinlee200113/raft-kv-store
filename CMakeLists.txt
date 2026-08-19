cmake_minimum_required(VERSION 3.20)
project(raft-kv VERSION 0.1.0 LANGUAGES CXX)

if(POLICY CMP0167)
  cmake_policy(SET CMP0167 NEW)
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

find_package(Boost REQUIRED COMPONENTS system)
find_package(Threads REQUIRED)
find_package(msgpack-cxx REQUIRED)

set(RAFT_SANITIZER "" CACHE STRING
    "Enable a sanitizer: address, undefined, or thread")
set_property(CACHE RAFT_SANITIZER PROPERTY STRINGS
             "" address undefined thread)
option(RAFT_ENABLE_COVERAGE "Enable LLVM source coverage" OFF)

if(RAFT_SANITIZER AND RAFT_ENABLE_COVERAGE)
  message(FATAL_ERROR "Sanitizers and coverage require separate builds")
endif()

add_library(raft_instrumentation INTERFACE)

if(RAFT_SANITIZER)
  if(NOT CMAKE_CXX_COMPILER_ID MATCHES "^(AppleClang|Clang|GNU)$")
    message(FATAL_ERROR "RAFT_SANITIZER requires Clang or GCC")
  endif()
  if(NOT RAFT_SANITIZER MATCHES "^(address|undefined|thread)$")
    message(FATAL_ERROR "Unknown RAFT_SANITIZER=${RAFT_SANITIZER}")
  endif()

  target_compile_options(raft_instrumentation INTERFACE
      -fsanitize=${RAFT_SANITIZER}
      -fno-omit-frame-pointer)
  target_link_options(raft_instrumentation INTERFACE
      -fsanitize=${RAFT_SANITIZER})
endif()

if(RAFT_ENABLE_COVERAGE)
  if(NOT CMAKE_CXX_COMPILER_ID MATCHES "^(AppleClang|Clang)$")
    message(FATAL_ERROR "RAFT_ENABLE_COVERAGE requires Clang")
  endif()

  target_compile_options(raft_instrumentation INTERFACE
      -O0
      -g
      -fprofile-instr-generate
      -fcoverage-mapping)
  target_link_options(raft_instrumentation INTERFACE
      -fprofile-instr-generate)
endif()

add_library(raft_lib STATIC
    src/common/bytebuffer.cpp
    src/raft/raft.cpp
    src/server/kv_store.cpp
    src/server/resp_codec.cpp
    src/server/resp_server.cpp
    src/transport/peer.cpp
    src/transport/server.cpp
    src/wal/wal.cpp
)
target_include_directories(raft_lib PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/src")
target_link_libraries(raft_lib PUBLIC
    raft_instrumentation
    Boost::system
    Threads::Threads
    msgpack-cxx)
target_compile_options(raft_lib PRIVATE
    $<$<CXX_COMPILER_ID:AppleClang,Clang,GNU>:-Wall;-Wextra;-Wpedantic>
)

add_executable(raft-kv src/main.cpp src/app/node_runtime.cpp)
target_link_libraries(raft-kv PRIVATE raft_lib)
target_compile_options(raft-kv PRIVATE
    $<$<CXX_COMPILER_ID:AppleClang,Clang,GNU>:-Wall;-Wextra;-Wpedantic>
)

include(CTest)
if(BUILD_TESTING)
  find_package(GTest REQUIRED)

  function(add_raft_test target source)
    add_executable(${target} ${source})
    target_link_libraries(${target} PRIVATE raft_lib GTest::gtest_main)
    add_test(NAME ${target} COMMAND ${target})
  endfunction()

  add_raft_test(voting_test tests/voting_test.cpp)
  add_raft_test(replication_test tests/replication_test.cpp)
  add_raft_test(kv_store_test tests/kv_store_test.cpp)
  add_raft_test(transport_proto_test tests/transport_proto_test.cpp)
  add_raft_test(network_test tests/network_test.cpp)
  add_raft_test(read_index_test tests/read_index_test.cpp)
  add_raft_test(async_apply_test tests/async_apply_test.cpp)
  add_raft_test(wal_test tests/wal_test.cpp)
  add_raft_test(snapshot_test tests/snapshot_test.cpp)
  add_raft_test(resp_codec_test tests/resp_codec_test.cpp)
  add_raft_test(resp_server_test tests/resp_server_test.cpp)
endif()
