idf_component_register(SRCS "main.c" INCLUDE_DIRS "." PRIV_REQUIRES esp_system xtensa)
# Record the cause of each probe's fault in RTC memory, then let IDF's own
# panic handler print and reboot as usual.
target_link_libraries(${COMPONENT_LIB} INTERFACE "-Wl,--wrap=esp_panic_handler")
