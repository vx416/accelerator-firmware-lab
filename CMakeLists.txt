cmake_minimum_required(VERSION 3.20)

project(accelerator_firmware_lab C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)

add_executable(fwctl tools/fwctl/fwctl.c)
target_include_directories(fwctl PRIVATE include)
target_compile_options(fwctl PRIVATE -Wall -Wextra -Wpedantic)

add_executable(afl-kernel-verifier tests/kernel_verifier.c)
target_include_directories(afl-kernel-verifier PRIVATE include)
target_compile_options(afl-kernel-verifier PRIVATE -Wall -Wextra -Wpedantic)

enable_testing()
