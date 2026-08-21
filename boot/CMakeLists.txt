cmake_minimum_required(VERSION 3.22)

#
# This file is generated only once,
# and is not re-generated if converter is called multiple times.
#
# User is free to modify the file as much as necessary
#

# Setup compiler settings
set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS ON)


# Define the build type
if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE "Debug")
endif()

# Set the project name
set(CMAKE_PROJECT_NAME TopFlyingWing_Boot)

# Enable compile command to ease indexing with e.g. clangd
set(CMAKE_EXPORT_COMPILE_COMMANDS TRUE)

# Core project settings
project(${CMAKE_PROJECT_NAME})
message("Build type: " ${CMAKE_BUILD_TYPE})

set(ABOX_PRODUCT_CONFIG_DIR "${CMAKE_SOURCE_DIR}/../platform_config")
set(ABOX_PLATFORM_SCHEDULER "BAREMETAL")
set(TOP_FLYING_WING_BOOT_FW_VERSION "top-flying-wing-boot-2.2.0" CACHE STRING "Boot firmware version marker")
add_subdirectory("${CMAKE_SOURCE_DIR}/../platform" abox_platform_build)

# Enable CMake support for ASM and C languages
enable_language(C ASM)

# Create an executable object type
add_executable(${CMAKE_PROJECT_NAME})

# Add STM32CubeMX generated sources
add_subdirectory(cmake/stm32cubemx)

# Link directories setup
target_link_directories(${CMAKE_PROJECT_NAME} PRIVATE
    # Add user defined library search paths
)

# Add sources to executable
target_sources(${CMAKE_PROJECT_NAME} PRIVATE
    Core/User/Src/abox_product_port.c
    Core/User/Src/boot_dbg.c
    Core/User/Src/boot_descriptor.c
    Core/User/Src/boot_jump.c
    Core/User/Src/ota_core.c
    Core/User/Src/ota_info.c
    Core/User/Src/boot_state_compat.c
    Core/User/Src/fault_mailbox.c
)

# Add include paths
target_include_directories(${CMAKE_PROJECT_NAME} PRIVATE
    Core/User/Inc
)

# Add project symbols (macros)
target_compile_definitions(${CMAKE_PROJECT_NAME} PRIVATE
    # Add user defined symbols
)

target_compile_definitions(${CMAKE_PROJECT_NAME} PRIVATE
    TOP_FLYING_WING_BOOT_FW_VERSION="${TOP_FLYING_WING_BOOT_FW_VERSION}"
)

# Remove wrong libob.a library dependency when using cpp files
list(REMOVE_ITEM CMAKE_C_IMPLICIT_LINK_LIBRARIES ob)

# Add linked libraries
target_link_libraries(${CMAKE_PROJECT_NAME}
    stm32cubemx

    # Add user defined libraries
)

abox_platform_attach_boot_v2(${CMAKE_PROJECT_NAME})

add_custom_command(TARGET ${CMAKE_PROJECT_NAME} POST_BUILD
    COMMAND ${CMAKE_OBJCOPY} -O binary $<TARGET_FILE:${CMAKE_PROJECT_NAME}> ${CMAKE_PROJECT_NAME}.bin
)
