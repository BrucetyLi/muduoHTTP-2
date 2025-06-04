cmake_minimum_required(VERSION 3.0)
project(muduohttp)

set(EXECUTABLE_OUTPUT_PATH ${PROJECT_SOURCE_DIR}/bin)
include_directories(${PROJECT_SOURCE_DIR}/include)
aux_source_directory(${PROJECT_SOURCE_DIR}/src SRC_LIST)

add_executable(muduohttp main.cc ${SRC_LIST})
target_link_libraries(muduohttp muduo_net muduo_base pthread nghttp2 ssl crypto)
add_compile_options(-std=11 -W -g)