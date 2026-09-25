zephyr_library()
zephyr_library_sources_ifdef(CONFIG_MSPI_ESP32 mspi_esp32.c)
