cmake_minimum_required(VERSION 3.16)
project(chat_server LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Writes build/compile_commands.json for clangd
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# Default to an optimized build unless -DCMAKE_BUILD_TYPE=Debug is given
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release)
endif()

find_package(Threads REQUIRED)

# ---- Core library: everything except main() ---------------------------------
add_library(chat_core
    src/util.cpp
    src/Epoll.cpp
    src/ClientSession.cpp
    src/UserDirectory.cpp
    src/ClientRegistry.cpp
    src/GroupRegistry.cpp
    src/CommandProcessor.cpp
    src/ChatServer.cpp
)
target_include_directories(chat_core PUBLIC include)
target_compile_options(chat_core PRIVATE -Wall -Wextra)
target_link_libraries(chat_core PUBLIC Threads::Threads)

# ---- Server executable ------------------------------------------------------
add_executable(chat_server src/main.cpp)
target_compile_options(chat_server PRIVATE -Wall -Wextra)
target_link_libraries(chat_server PRIVATE chat_core)

# ---- Optional targets: built only once their source files exist -------------
if(EXISTS "${CMAKE_SOURCE_DIR}/client/client.cpp")
    add_executable(chat_client client/client.cpp)
    target_compile_options(chat_client PRIVATE -Wall -Wextra)
    target_link_libraries(chat_client PRIVATE Threads::Threads)
endif()

if(EXISTS "${CMAKE_SOURCE_DIR}/tools/load_gen.cpp")
    add_executable(load_gen tools/load_gen.cpp)
    target_compile_options(load_gen PRIVATE -Wall -Wextra)
    target_link_libraries(load_gen PRIVATE Threads::Threads)
endif()

# ---- Tests (enable once tests/CMakeLists.txt exists) ------------------------
# enable_testing()
# add_subdirectory(tests)