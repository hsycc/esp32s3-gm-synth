idf_component_register(SRCS "main.cpp"
    INCLUDE_DIRS "../../../app" "${V1_APP_FIXTURE_DIR}"
    REQUIRES firmware freertos mbedtls)
