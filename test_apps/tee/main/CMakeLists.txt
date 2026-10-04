idf_component_register(SRCS "tee_main.c"
                       INCLUDE_DIRS "."
                       REQUIRES esp_tee tee_sec_storage)
