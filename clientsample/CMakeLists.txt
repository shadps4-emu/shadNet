cmake_minimum_required(VERSION 3.16)
project(shadnet-sample VERSION 1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

add_executable(shadnet-sample
    main.cpp
    client.cpp
    connection.cpp
)

target_include_directories(shadnet-sample PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})

if(WIN32)
    target_link_libraries(shadnet-sample PRIVATE ws2_32)
    target_compile_definitions(shadnet-sample PRIVATE
        _WIN32_WINNT=0x0601  
        WIN32_LEAN_AND_MEAN
        _CRT_SECURE_NO_WARNINGS
    )
endif()

if(MSVC)
    target_compile_options(shadnet-sample PRIVATE /W3 /utf-8)
else()
    target_compile_options(shadnet-sample PRIVATE -Wall -Wextra)
endif()
