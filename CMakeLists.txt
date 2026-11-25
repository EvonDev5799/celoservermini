cmake_minimum_required(VERSION 3.10)
project(newsflow CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()
add_compile_options(-Wall -Wextra -Wpedantic)

add_executable(news_server src/server.cpp)
add_executable(news_client src/client.cpp)
add_executable(unit_tests tests/unit_tests.cpp)

enable_testing()
add_test(NAME unit COMMAND unit_tests)
add_test(NAME demo COMMAND ${CMAKE_SOURCE_DIR}/demo.sh ${CMAKE_BINARY_DIR})
