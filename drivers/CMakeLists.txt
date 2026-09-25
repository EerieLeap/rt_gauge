# Out-of-tree drivers for existing driver classes
add_subdirectory_ifdef(CONFIG_MSPI mspi)
add_subdirectory_ifdef(CONFIG_DISPLAY display)
add_subdirectory_ifdef(CONFIG_INPUT_CST92XX input)
