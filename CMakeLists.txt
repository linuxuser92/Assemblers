cmake_minimum_required(VERSION 3.20)
project(BedrockMenu LANGUAGES CXX C)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

add_definitions(-DUNICODE -D_UNICODE -DNOMINMAX -DWIN32_LEAN_AND_MEAN)

set(DEPS_DIR "${CMAKE_CURRENT_SOURCE_DIR}/deps" CACHE PATH "Folder containing imgui and MinHook")
set(IMGUI_DIR "${DEPS_DIR}/imgui")
set(MINHOOK_DIR "${DEPS_DIR}/MinHook")

foreach(required
        "${IMGUI_DIR}/imgui.cpp"
        "${IMGUI_DIR}/backends/imgui_impl_dx12.cpp"
        "${MINHOOK_DIR}/src/Hook.c")
    if(NOT EXISTS "${required}")
        message(FATAL_ERROR "Missing dependency: ${required}\nSee README.md -> Dependencies")
    endif()
endforeach()

add_subdirectory("${MINHOOK_DIR}" "${CMAKE_BINARY_DIR}/minhook_build")

set(SOURCES
    src/main.cpp
    src/common.cpp
    src/config/Config.cpp
    src/utils/Memory.cpp
    src/sdk/Offsets.cpp
    src/sdk/Game.cpp
    src/render/Renderer.cpp
    src/render/D3D12Hook.cpp
    src/features/Features.cpp
    src/features/GameHook.cpp
    src/features/AutoDetect.cpp
    src/menu/Menu.cpp

    "${IMGUI_DIR}/imgui.cpp"
    "${IMGUI_DIR}/imgui_draw.cpp"
    "${IMGUI_DIR}/imgui_tables.cpp"
    "${IMGUI_DIR}/imgui_widgets.cpp"
    "${IMGUI_DIR}/backends/imgui_impl_dx12.cpp"
    "${IMGUI_DIR}/backends/imgui_impl_win32.cpp"
)

add_library(BedrockMenu SHARED ${SOURCES})
set_target_properties(BedrockMenu PROPERTIES
    PREFIX ""
    OUTPUT_NAME "BedrockMenu"
)

target_include_directories(BedrockMenu PRIVATE
    src
    src/config
    src/features
    src/menu
    src/render
    src/sdk
    src/utils
    "${IMGUI_DIR}"
    "${IMGUI_DIR}/backends"
    "${MINHOOK_DIR}/include"
    "${CMAKE_CURRENT_SOURCE_DIR}/third_party"
)

target_link_libraries(BedrockMenu PRIVATE
    minhook
    d3d12
    dxgi
    dxguid
    comctl32
    psapi
)

add_executable(BedrockInjector tools/Injector.cpp)
target_link_libraries(BedrockInjector PRIVATE psapi)

add_executable(BedrockKeyTest tools/KeyTest.cpp)

option(BUILD_TESTS "Build smoke host and unit tests" ON)
if(BUILD_TESTS)
    add_executable(SmokeHost tests/SmokeHost.cpp)

    add_executable(UnitTests tests/UnitTests.cpp src/common.cpp src/utils/Memory.cpp)
    target_include_directories(UnitTests PRIVATE src "${IMGUI_DIR}")
    target_link_libraries(UnitTests PRIVATE psapi)
endif()
