cmake_minimum_required(VERSION 3.20.0)

project(CustomMemory)

if (CMAKE_SYSTEM_NAME MATCHES "Linux")
message(STATUS "Curren OS: ${CMAKE_SYSTEM_NAME}")
else()
message(FATAL_ERROR "Curren OS: ${CMAKE_SYSTEM_NAME}, Just Support OS Linux")
endif (CMAKE_SYSTEM_NAME MATCHES "Linux")

add_subdirectory(libs/googletest)

include_directories(include)

aux_source_directory(src SRC_LIST)

add_executable(${PROJECT_NAME} ${SRC_LIST})
target_link_libraries(${PROJECT_NAME} gtest_main)
