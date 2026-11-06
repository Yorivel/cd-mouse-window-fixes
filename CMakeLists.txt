cmake_minimum_required(VERSION 3.24)
project(MouseWindowFixes LANGUAGES C CXX RC)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

include(FetchContent)
FetchContent_Declare(minhook
    GIT_REPOSITORY https://github.com/TsudaKageyu/minhook.git
    GIT_TAG v1.3.4
    GIT_SHALLOW TRUE)
FetchContent_MakeAvailable(minhook)

add_library(MouseWindowFixes SHARED src/mouse_window_fixes.cpp src/log.cpp src/mouse_window_fixes.rc)
target_link_libraries(MouseWindowFixes PRIVATE minhook)
target_include_directories(MouseWindowFixes PRIVATE src)
target_compile_definitions(MouseWindowFixes PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX UNICODE _UNICODE)
target_compile_options(MouseWindowFixes PRIVATE /W4 /permissive- /utf-8)
set_target_properties(MouseWindowFixes PROPERTIES SUFFIX ".asi")
