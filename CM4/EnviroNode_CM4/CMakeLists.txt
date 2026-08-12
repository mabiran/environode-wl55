# Set the minimum version of CMake that can be used
cmake_minimum_required(VERSION 3.20)

# Enable compile command to ease indexing with e.g. clangd
set(CMAKE_EXPORT_COMPILE_COMMANDS TRUE)

# Project definition
project(EnviroNode_CM4)

# Enable CMake support for ASM and C/CPP languages
enable_language(C CXX ASM)

set(BUILD_UNIT_0_NAME EnviroNode_CM4)

# Create an executable object type
add_executable(${BUILD_UNIT_0_NAME})

# STM32CubeIDE for Visual Studio Code managed CMake file
include(cmake/vscode_generated.cmake)

# Compiler options
target_compile_options(${BUILD_UNIT_0_NAME} PRIVATE
  # User defined compiler options
)

# Linker options
target_link_options(${BUILD_UNIT_0_NAME} PRIVATE
  # User defined linker options
  # newlib-nano ships an integer-only printf; force-link the float variant so
  # "%f" works in snprintf (sensor readings on `nucleo sensors` / `info`). The auto-generated
  # flags pass "-u _printf_float" as ONE quoted token, which the linker ignores;
  # SHELL: emits it as two proper arguments (-u _printf_float).
  "SHELL:-u _printf_float"
)

# Add sources to executable/library
target_sources(${BUILD_UNIT_0_NAME} PRIVATE
  # User defined sources
)

# FatFs is a vendored third-party library we never step through in a debugger,
# and at the Debug profile's -O0 it costs ~15 KB against a ~13 KB budget.
# Building just this file size-optimised keeps the image comfortably inside
# FLASH (118K) while the application keeps full debug fidelity.
set_source_files_properties("Middlewares/FatFs/ff.c"
  PROPERTIES COMPILE_OPTIONS "-Os")

# Add include directories
target_include_directories(${BUILD_UNIT_0_NAME} PRIVATE
  # User defined include directories
  "Core/WL55JC1/Inc"   # non-standard header location for this project
)
