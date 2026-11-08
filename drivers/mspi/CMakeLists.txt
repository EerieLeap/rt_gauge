zephyr_library()
zephyr_library_sources_ifdef(CONFIG_MSPI_ESP32 mspi_esp32.c)

# This driver carries every display flush: -O2 although the board builds at -Og.
zephyr_library_compile_options(-O2)
