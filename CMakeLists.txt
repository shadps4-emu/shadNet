cmake_minimum_required(VERSION 3.16)
project(shadnet VERSION 1.0.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)

find_package(Qt6 REQUIRED COMPONENTS Core Network Sql Concurrent)

set(SOURCES
    src/main.cpp
    src/config.cpp
    src/config.h
    src/server.cpp
    src/server.h
    src/client_session.cpp
    src/client_session.h
    src/protocol.h
    src/stream_extractor.h
    src/database.cpp
    src/database.h
    src/cmd_account.cpp
    src/cmd_friend.cpp
)

add_executable(shadnet ${SOURCES})

target_link_libraries(shadnet PRIVATE
    Qt6::Core
    Qt6::Network
    Qt6::Sql
    Qt6::Concurrent
)

target_include_directories(shadnet PRIVATE src)

install(TARGETS shadnet RUNTIME DESTINATION bin)
