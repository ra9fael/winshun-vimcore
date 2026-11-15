# vimcore — a small modal (vim-style) editing core in portable C++20.
#
# Hard rule: nothing platform-specific may be required to build or test this
# library. No Qt, no Win32, no OS headers. Hosts adapt their own text widgets
# to vimcore::Document.
cmake_minimum_required(VERSION 3.24)

project(vimcore LANGUAGES CXX VERSION 0.2.0)

add_library(vimcore STATIC
    src/editor.cpp
)
target_include_directories(vimcore
    PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include
)
target_compile_features(vimcore PUBLIC cxx_std_20)
if(MSVC)
    target_compile_options(vimcore PRIVATE /W4 /permissive- /utf-8)
else()
    target_compile_options(vimcore PRIVATE -Wall -Wextra -Wpedantic)
endif()

option(VIMCORE_BUILD_TESTS "Build the vimcore behaviour tests" ON)

if(VIMCORE_BUILD_TESTS)
    enable_testing()
    add_executable(vimcore_tests
        tests/test_vimcore.cpp
    )
    target_include_directories(vimcore_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include)
    target_link_libraries(vimcore_tests PRIVATE vimcore)
    target_compile_features(vimcore_tests PRIVATE cxx_std_20)
    if(MSVC)
        target_compile_options(vimcore_tests PRIVATE /W4 /utf-8)
    endif()
    add_test(NAME vimcore_tests COMMAND vimcore_tests)

    add_executable(vimcore_conformance
        tests/test_conformance.cpp
    )
    target_include_directories(vimcore_conformance PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include)
    target_link_libraries(vimcore_conformance PRIVATE vimcore)
    target_compile_features(vimcore_conformance PRIVATE cxx_std_20)
    if(MSVC)
        target_compile_options(vimcore_conformance PRIVATE /W4 /utf-8)
    endif()
    add_test(NAME vimcore_conformance COMMAND vimcore_conformance)
endif()
