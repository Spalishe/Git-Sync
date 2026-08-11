cmake_minimum_required(VERSION 3.10)

set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
project(git-sync LANGUAGES C CXX)

set(CMAKE_C_STANDARD 17)
set(CMAKE_C_STANDARD_REQUIRED True)

file(GLOB SOURCES_C "src/*.c")
file(GLOB SOURCES_CPP "src/*.cpp")

add_executable(git-sync ${SOURCES_C} ${SOURCES_CPP})

target_compile_options(git-sync PRIVATE -O3 -march=native)

target_link_libraries(git-sync PRIVATE m)

find_package(PkgConfig REQUIRED)
pkg_check_modules(WAYLAND_CLIENT REQUIRED wayland-client)
pkg_check_modules(CAIRO REQUIRED cairo)
pkg_check_modules(CURL REQUIRED IMPORTED_TARGET libcurl)

target_link_libraries(git-sync PRIVATE ${WAYLAND_CLIENT_LIBRARIES} ${CAIRO_LIBRARIES} ${CURL_LIBRARIES})
target_include_directories(git-sync PRIVATE ${WAYLAND_CLIENT_INCLUDE_DIRS} ${CAIRO_INCLUDE_DIRS} ${CURL_INCLUDE_DIRS})

