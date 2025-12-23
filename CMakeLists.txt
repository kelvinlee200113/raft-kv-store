cmake_minimum_required(VERSION 3.14)
project(raft-kv)

# Use C++17 standard
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Include directories
include_directories(src)

# Add the main executable
add_executable(raft-kv
    src/main.cpp
    src/raft/raft.cpp
)


