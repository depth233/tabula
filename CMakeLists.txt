cmake_minimum_required(VERSION 3.20)
project(tabula_vulkan_triangle LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

add_executable(tabula_vulkan_triangle WIN32
  main.cpp
)

if (WIN32)
  target_compile_definitions(tabula_vulkan_triangle PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX)
endif()

# Prefer Vulkan SDK (LunarG). If FindVulkan fails, fall back to VULKAN_SDK env.
find_package(Vulkan QUIET)

if (NOT Vulkan_FOUND)
  if (DEFINED ENV{VULKAN_SDK})
    set(_VULKAN_SDK "$ENV{VULKAN_SDK}")
    find_path(Vulkan_INCLUDE_DIR
      NAMES vulkan/vulkan.h
      PATHS "${_VULKAN_SDK}/Include"
      NO_DEFAULT_PATH
    )
    find_library(Vulkan_LIBRARY
      NAMES vulkan-1
      PATHS "${_VULKAN_SDK}/Lib" "${_VULKAN_SDK}/Lib32"
      NO_DEFAULT_PATH
    )
  endif()

  if (Vulkan_INCLUDE_DIR AND Vulkan_LIBRARY)
    add_library(Vulkan::Vulkan UNKNOWN IMPORTED)
    set_target_properties(Vulkan::Vulkan PROPERTIES
      IMPORTED_LOCATION "${Vulkan_LIBRARY}"
      INTERFACE_INCLUDE_DIRECTORIES "${Vulkan_INCLUDE_DIR}"
    )
    set(Vulkan_FOUND TRUE)
  endif()
endif()

if (NOT Vulkan_FOUND)
  message(FATAL_ERROR
    "找不到 Vulkan。请安装 LunarG Vulkan SDK，然后重新打开终端/IDE，确保存在环境变量 VULKAN_SDK。\n"
    "也可以手动指定：-DVulkan_INCLUDE_DIR=... -DVulkan_LIBRARY=...\n"
    "下载： https://vulkan.lunarg.com/sdk/home"
  )
endif()

target_link_libraries(tabula_vulkan_triangle PRIVATE Vulkan::Vulkan)

if (DEFINED ENV{VULKAN_SDK})
  set(_VULKAN_SDK "$ENV{VULKAN_SDK}")
endif()

# Build shaders with dxc (HLSL -> SPIR-V). Vulkan SDK includes dxc.exe.
find_program(DXC_EXECUTABLE
  NAMES dxc dxc.exe
  PATHS "${_VULKAN_SDK}/Bin" "${_VULKAN_SDK}/Bin32"
  NO_DEFAULT_PATH
)

if (NOT DXC_EXECUTABLE)
  message(FATAL_ERROR
    "找不到 dxc（HLSL 编译器）。请确认已安装 Vulkan SDK，并设置环境变量 VULKAN_SDK。\n"
    "如果你用 scoop 安装，请在构建前执行：$env:VULKAN_SDK=(scoop prefix vulkan)"
  )
endif()

set(SHADER_SRC_DIR "${CMAKE_SOURCE_DIR}/shaders")
set(SHADER_OUT_DIR "${CMAKE_BINARY_DIR}/shaders")
file(MAKE_DIRECTORY "${SHADER_OUT_DIR}")

set(VERT_SRC "${SHADER_SRC_DIR}/triangle.vert.hlsl")
set(FRAG_SRC "${SHADER_SRC_DIR}/triangle.frag.hlsl")
set(VERT_SPV "${SHADER_OUT_DIR}/triangle.vert.spv")
set(FRAG_SPV "${SHADER_OUT_DIR}/triangle.frag.spv")

add_custom_command(
  OUTPUT "${VERT_SPV}"
  COMMAND "${DXC_EXECUTABLE}" -nologo -spirv -T vs_6_0 -E main -fspv-target-env=vulkan1.0 -Fo "${VERT_SPV}" "${VERT_SRC}"
  DEPENDS "${VERT_SRC}"
  VERBATIM
)

add_custom_command(
  OUTPUT "${FRAG_SPV}"
  COMMAND "${DXC_EXECUTABLE}" -nologo -spirv -T ps_6_0 -E main -fspv-target-env=vulkan1.0 -Fo "${FRAG_SPV}" "${FRAG_SRC}"
  DEPENDS "${FRAG_SRC}"
  VERBATIM
)

add_custom_target(tabula_shaders ALL DEPENDS "${VERT_SPV}" "${FRAG_SPV}")
add_dependencies(tabula_vulkan_triangle tabula_shaders)

add_custom_command(TARGET tabula_vulkan_triangle POST_BUILD
  COMMAND ${CMAKE_COMMAND} -E make_directory "$<TARGET_FILE_DIR:tabula_vulkan_triangle>/shaders"
  COMMAND ${CMAKE_COMMAND} -E copy_if_different "${VERT_SPV}" "$<TARGET_FILE_DIR:tabula_vulkan_triangle>/shaders/triangle.vert.spv"
  COMMAND ${CMAKE_COMMAND} -E copy_if_different "${FRAG_SPV}" "$<TARGET_FILE_DIR:tabula_vulkan_triangle>/shaders/triangle.frag.spv"
  VERBATIM
)

if (MSVC)
  target_compile_options(tabula_vulkan_triangle PRIVATE /W4 /permissive-)
else()
  target_compile_options(tabula_vulkan_triangle PRIVATE -Wall -Wextra -Wpedantic)
endif()

