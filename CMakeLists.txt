cmake_minimum_required(VERSION 3.20)
project(easy-uds VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

add_library(easy-uds STATIC src/easy_uds.cpp)
target_include_directories(easy-uds PUBLIC include)
target_compile_options(easy-uds PRIVATE -Wall -Wextra -Wpedantic)

add_executable(server_example examples/server.cpp)
target_link_libraries(server_example easy-uds)

add_executable(client_example examples/client.cpp)
target_link_libraries(client_example easy-uds)

enable_testing()
add_executable(test_uds tests/test.cpp)
target_link_libraries(test_uds easy-uds)
add_test(NAME basic_test COMMAND test_uds)