set(main_requires firmware esp_psram)

# Only the dedicated QEMU SF2-in-flash integration build needs FATFS. Keep the
# production/default bring-up dependency surface unchanged.
if(CONFIG_GM_SYNTH_QEMU_FLASH_SF2_TEST)
    list(APPEND main_requires fatfs)
endif()

idf_component_register(
    SRCS "app_main.cpp"
    INCLUDE_DIRS "."
    REQUIRES ${main_requires}
)
