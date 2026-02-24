cmake_minimum_required(VERSION 3.16)
project(tod_forwarder LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if (NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE "Release" CACHE STRING "Choose the type of build." FORCE)
    set_property(CACHE CMAKE_BUILD_TYPE PROPERTY STRINGS "Debug" "Release" "MinSizeRel" "RelWithDebInfo")
    message(STATUS "Setting build type to '${CMAKE_BUILD_TYPE}' as none was specified.")
endif ()

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

# Serial loopback test (requires USB-to-DB9 adapter with pin 2-3 loopback)
add_executable(test_serial_loopback
    tests/test_serial_loopback.cpp
    src/rs232_sender.cpp
)

target_include_directories(test_serial_loopback PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_link_libraries(test_serial_loopback PRIVATE
    Threads::Threads
)
