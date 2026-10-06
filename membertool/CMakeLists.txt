cmake_minimum_required(VERSION 3.16)
project(shadnet-toolbox LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)

if(MSVC)
    add_compile_options(/EHsc /utf-8)
endif()

find_package(Qt6 REQUIRED COMPONENTS Core Sql Widgets Network)

add_library(worlds_editor STATIC
    ../common/worlds/worldsconfig.cpp ../common/worlds/worldsconfig.h
    ../common/worlds/worldsstore.cpp ../common/worlds/worldsstore.h
    ../common/worlds/worldsbackend.h
    ../common/worlds/localworldsbackend.cpp ../common/worlds/localworldsbackend.h
    ../common/worlds/worldsdialog.cpp ../common/worlds/worldsdialog.h
)
target_include_directories(worlds_editor PUBLIC ../common/worlds)
target_link_libraries(worlds_editor PUBLIC Qt6::Core Qt6::Widgets Qt6::Network)

add_library(member_store STATIC
    member_store.cpp
    member_store.h
    ../src/database.cpp
    ../src/database.h
)
target_include_directories(member_store PUBLIC . ../src)
target_link_libraries(member_store PUBLIC Qt6::Core Qt6::Sql)

add_executable(shadnet-toolbox WIN32 main.cpp member_window.cpp member_window.h)
target_link_libraries(shadnet-toolbox PRIVATE member_store worlds_editor)

install(TARGETS shadnet-toolbox RUNTIME DESTINATION bin)
