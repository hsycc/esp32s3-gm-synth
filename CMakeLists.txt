cmake_minimum_required(VERSION 3.16)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Explicit build-wide ABI opt-in for qualification, never enabled by defaults.
# This compiles the optional Voice path; it does not provision any loop plan.
option(GM_SYNTH_BUILD_VOICE_LOOP "Compile experimental Voice loop execution" OFF)
option(GM_SYNTH_BUILD_LOOP_STARTUP_QUAL "Build isolated startup lifecycle qualification" OFF)
if(GM_SYNTH_BUILD_LOOP_STARTUP_QUAL)
    if(NOT EXISTS "${GM_SYNTH_STARTUP_GENERATED}/loop_startup_manifest.h" OR
       NOT EXISTS "${GM_SYNTH_STARTUP_GENERATED}/bank.sf2")
        message(FATAL_ERROR "Run scripts/build_v1_loop_startup.sh in a fresh dedicated build directory")
    endif()
    # ESP-IDF's separate early requirement-scan CMake process does not inherit
    # custom cache options. Export normalized build-local mode/path to that
    # child, overriding any caller environment in BOTH on and off builds.
    set(ENV{GM_SYNTH_STARTUP_EARLY_MODE} "1")
    set(ENV{GM_SYNTH_STARTUP_EARLY_GENERATED} "${GM_SYNTH_STARTUP_GENERATED}")
    add_compile_definitions(GM_SYNTH_LOOP_STARTUP_QUALIFICATION=1)
else()
    set(ENV{GM_SYNTH_STARTUP_EARLY_MODE} "0")
    set(ENV{GM_SYNTH_STARTUP_EARLY_GENERATED} "")
endif()
if(GM_SYNTH_BUILD_VOICE_LOOP OR GM_SYNTH_BUILD_LOOP_STARTUP_QUAL)
    add_compile_definitions(GM_SYNTH_EXPERIMENTAL_VOICE_LOOP=1)
endif()

set(EXTRA_COMPONENT_DIRS "${CMAKE_CURRENT_LIST_DIR}/firmware")

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(esp32s3_gm_synth)

# QEMU-only integration fixture. Production/default builds never create or flash
# this partition. The CI script stages one pinned real SF2 as test.sf2 before
# configuring the dedicated QEMU build directory.
if(CONFIG_GM_SYNTH_QEMU_FLASH_SF2_TEST)
    set(QEMU_SF2_DATA_DIR "${CMAKE_CURRENT_LIST_DIR}/.qemu_sf2_data")
    if(NOT EXISTS "${QEMU_SF2_DATA_DIR}/test.sf2")
        message(FATAL_ERROR
            "GM_SYNTH_QEMU_FLASH_SF2_TEST requires ${QEMU_SF2_DATA_DIR}/test.sf2; "
            "run scripts/run_qemu_sf2_ci.sh or stage the fixture manually")
    endif()

    fatfs_create_rawflash_image(qemu_sf2 "${QEMU_SF2_DATA_DIR}" FLASH_IN_PROJECT ONE_FAT)
endif()

if(GM_SYNTH_BUILD_LOOP_STARTUP_QUAL)
    if(CONFIG_GM_SYNTH_QEMU_FLASH_SF2_TEST OR CONFIG_GM_SYNTH_I2S_QUAL_ENABLE)
        message(FATAL_ERROR "Do not combine startup qualification with old storage/I2S profiles")
    endif()
    esp_partition_register_target(loop_bank "${GM_SYNTH_STARTUP_GENERATED}/bank.sf2" FLASH_IN_PROJECT)
endif()
