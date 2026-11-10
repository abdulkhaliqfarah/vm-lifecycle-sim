cmake_minimum_required(VERSION 3.20)
project(vm_lifecycle_sim CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()

add_compile_options(-Wall -Wextra -Wpedantic)

add_library(vmsim src/simulator.cpp)
target_include_directories(vmsim PUBLIC include)

add_executable(sim tools/sim.cpp)
target_link_libraries(sim PRIVATE vmsim)

add_executable(check tools/check.cpp)
target_link_libraries(check PRIVATE vmsim)

enable_testing()
add_executable(test_simulator tests/test_simulator.cpp)
target_link_libraries(test_simulator PRIVATE vmsim)
add_test(NAME simulator_scenarios COMMAND test_simulator)
