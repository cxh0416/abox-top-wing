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
set(CMAKE_PROJECT_NAME TopFlyingWing_App)

# Enable compile command to ease indexing with e.g. clangd
set(CMAKE_EXPORT_COMPILE_COMMANDS TRUE)

# Core project settings
project(${CMAKE_PROJECT_NAME})
message("Build type: " ${CMAKE_BUILD_TYPE})

set(ABOX_PRODUCT_CONFIG_DIR "${CMAKE_SOURCE_DIR}/../platform_config")
set(ABOX_PLATFORM_SCHEDULER "BAREMETAL")
add_subdirectory("${CMAKE_SOURCE_DIR}/../platform" abox_platform_build)

set(TOP_WING_CAN_BUS "2" CACHE STRING "Top flying wing CAN port (1 or 2)")
if(NOT TOP_WING_CAN_BUS MATCHES "^[12]$")
    message(FATAL_ERROR "TOP_WING_CAN_BUS must be 1 or 2")
endif()
set(TOP_FLYING_WING_APP_FW_VERSION "top-flying-wing-1.1.3" CACHE STRING "Top flying wing App firmware version")

# Enable CMake support for ASM and C languages
enable_language(C ASM)

# Create an executable object type
add_executable(${CMAKE_PROJECT_NAME})
set_property(TARGET ${CMAKE_PROJECT_NAME} APPEND PROPERTY LINK_DEPENDS
    "${CMAKE_SOURCE_DIR}/STM32F105xx_FLASH.ld"
)

# Add STM32CubeMX generated sources
add_subdirectory(cmake/stm32cubemx)

# Link directories setup
target_link_directories(${CMAKE_PROJECT_NAME} PRIVATE
    # Add user defined library search paths
)

# Add sources to executable
target_sources(${CMAKE_PROJECT_NAME} PRIVATE
    Core/User/Src/abox_product_port.c
    Core/User/Src/app_log.c
    Core/User/Src/board_config.c
    Core/User/Src/mqtt_app.c
    Core/User/Src/mqtt_app_cmd.c
    Core/User/Src/mqtt_app_hb.c
    Core/User/Src/mqtt_v4.c
    Core/User/Src/mqtt_v4_store.c
    Core/User/Src/mqtt_v4_persistence.c
    Core/User/Src/mqtt_runtime.c
    Core/User/Src/time_sync.c
    Core/User/Src/wing_enrollment.c
    "${CMAKE_SOURCE_DIR}/../platform/targets/stm32f105_ec800_v1/enrollment_identity_stm32f105.c"
    Core/User/Src/mqtt_core.c
    Core/User/Src/ota_info.c
    Core/User/Src/ota_service.c
    Core/User/Src/boot_state_compat.c
    Core/User/Src/time_base.c
    Core/User/Src/wing_protocol.c
    Core/User/Src/wing_state.c
    Core/User/Src/wing_controller.c
)

# Add include paths
target_include_directories(${CMAKE_PROJECT_NAME} PRIVATE
    Core/User/Inc
    "${CMAKE_SOURCE_DIR}/../platform/targets/stm32f105_ec800_v1/include"
)

# Add project symbols (macros)
target_compile_definitions(${CMAKE_PROJECT_NAME} PRIVATE
    TOP_FLYING_WING_BOOT_V2_APP=1
    TOP_WING_CAN_BUS=${TOP_WING_CAN_BUS}
    TOP_FLYING_WING_APP_FW_VERSION="${TOP_FLYING_WING_APP_FW_VERSION}"
)

# Remove wrong libob.a library dependency when using cpp files
list(REMOVE_ITEM CMAKE_C_IMPLICIT_LINK_LIBRARIES ob)

# Add linked libraries
target_link_libraries(${CMAKE_PROJECT_NAME}
    stm32cubemx
    abox::ec800
    abox::mqtt_digest
    abox::mqtt_runtime
    abox::mqtt_ec800
    abox::cloud_service
    abox::cloud_v4
    abox::mqtt_request
    abox::mqtt_idem_ufs
    abox::ec800_ufs_at
    abox::enrollment_ec800_http
    abox::mqtt_trial

    # Add user defined libraries
)

abox_platform_attach(${CMAKE_PROJECT_NAME})
abox_platform_attach_boot_v2_app(${CMAKE_PROJECT_NAME})
abox_platform_attach_standard_clock(${CMAKE_PROJECT_NAME})

add_custom_command(TARGET ${CMAKE_PROJECT_NAME} POST_BUILD
    COMMAND ${CMAKE_OBJCOPY} -O binary $<TARGET_FILE:${CMAKE_PROJECT_NAME}> ${CMAKE_PROJECT_NAME}.bin
)
