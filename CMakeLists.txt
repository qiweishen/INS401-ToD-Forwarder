cmake_minimum_required(VERSION 3.16)
project(tod_forwarder LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

find_package(Threads REQUIRED)

add_executable(tod_forwarder
    src/main.cpp
    src/ethernet_socket.cpp
    src/ins401_discover.cpp
    src/ins401_receiver.cpp
    src/rs232_sender.cpp
)

target_include_directories(tod_forwarder PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_link_libraries(tod_forwarder PRIVATE
    Threads::Threads
)
