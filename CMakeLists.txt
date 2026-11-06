cmake_minimum_required(VERSION 3.24)
project(BorderlessLock LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

include(FetchContent)
FetchContent_Declare(minhook
    GIT_REPOSITORY https://github.com/TsudaKageyu/minhook.git
    GIT_TAG v1.3.4
    GIT_SHALLOW TRUE)
FetchContent_MakeAvailable(minhook)

add_library(BorderlessLock SHARED src/borderless_lock.cpp src/log.cpp)
target_link_libraries(BorderlessLock PRIVATE minhook)
target_include_directories(BorderlessLock PRIVATE src)
target_compile_definitions(BorderlessLock PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX UNICODE _UNICODE)
target_compile_options(BorderlessLock PRIVATE /W4 /permissive- /utf-8)
set_target_properties(BorderlessLock PROPERTIES SUFFIX ".asi")
