cmake_minimum_required(VERSION 3.16)
project(shadnet VERSION 1.0.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)

# === CRITICAL FIX: Set runtime library consistently ===
if(MSVC)
    cmake_policy(SET CMP0091 NEW)  # Enable runtime library selection
    # Use static runtime for consistency with your protobuf/abseil
    set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
    
    # Fix exception handling and UTF-8 warnings
    add_compile_options(/EHsc /utf-8)
endif()

find_package(Qt6 REQUIRED COMPONENTS Core Network Sql Concurrent)

add_subdirectory(externals)

set(PROTO_OUT "${CMAKE_CURRENT_BINARY_DIR}/proto_gen")
file(MAKE_DIRECTORY "${PROTO_OUT}")

add_custom_command(
    OUTPUT
        "${PROTO_OUT}/score_messages.pb.cc"
        "${PROTO_OUT}/score_messages.pb.h"
    COMMAND $<TARGET_FILE:protoc>
        "--proto_path=${CMAKE_CURRENT_SOURCE_DIR}/src"
        "--cpp_out=${PROTO_OUT}"
        "${CMAKE_CURRENT_SOURCE_DIR}/src/score_messages.proto"
    DEPENDS
        "${CMAKE_CURRENT_SOURCE_DIR}/src/score_messages.proto"
        protoc
    COMMENT "Generating score_messages protobuf sources"
    VERBATIM
)

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
    src/score_types.h
    src/score_db.h
    src/score_db.cpp
    src/score_cache.cpp
    src/score_cache.h
    src/proto_utils.h
    "${PROTO_OUT}/score_messages.pb.cc"
)

add_executable(shadnet ${SOURCES})

target_include_directories(shadnet PRIVATE
    src
    "${PROTO_OUT}"                                    
    "${CMAKE_CURRENT_SOURCE_DIR}/externals/protobuf/src"
)

target_link_libraries(shadnet PRIVATE
    Qt6::Core
    Qt6::Network
    Qt6::Sql
    Qt6::Concurrent
    libprotobuf
)

# Optional: Add this to see what runtime libraries are being used
if(MSVC)
    target_link_options(shadnet PRIVATE /VERBOSE:LIB)
endif()

install(TARGETS shadnet RUNTIME DESTINATION bin)