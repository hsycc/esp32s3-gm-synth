cmake_minimum_required(VERSION 3.16)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

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
