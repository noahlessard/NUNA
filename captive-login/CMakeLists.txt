cmake_minimum_required(VERSION 3.15)
project(captive-login)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -Wall -Wextra")

# Prefer static libraries
set(CMAKE_FIND_LIBRARY_SUFFIXES ".a")
set(BUILD_SHARED_LIBS OFF)

# Add the source directory to include paths so config.h can be found
include_directories(${CMAKE_SOURCE_DIR})

find_package(CURL REQUIRED)

add_executable(captive-login src/main.cpp)

target_link_libraries(captive-login CURL::libcurl ssl crypto z pthread dl)
target_include_directories(captive-login PRIVATE ${CURL_INCLUDE_DIRS})

# Link everything statically
target_link_options(captive-login PRIVATE -static)