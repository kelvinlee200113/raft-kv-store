cmake_minimum_required(VERSION 3.14)
project(raft-kv)

# Generate compile_commands.json for language servers (clangd, etc.)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

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

# Find Google Test
find_package(GTest REQUIRED)
include_directories(${GTEST_INCLUDE_DIRS})

# Enable testing
enable_testing()

# Add tests
add_executable(voting_test
    tests/voting_test.cpp
    src/raft/raft.cpp
)

# Link with Google Test
target_link_libraries(voting_test GTest::gtest GTest::gtest_main)

# Register test with CTest
add_test(NAME VotingTest COMMAND voting_test)


