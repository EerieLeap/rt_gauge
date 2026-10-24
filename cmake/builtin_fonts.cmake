# Embeds the built-in fonts (app/src/views/assets/fonts/builtin/) into the image as C byte lists that
# views/assets/fonts/builtin_fonts.cpp includes. Include it after find_package(Zephyr), when the target `app` exists.
# Without CONFIG_EERIE_LEAP_UI_BUILTIN_FONTS nothing is generated and the unit has no built-in fonts.
if(CONFIG_EERIE_LEAP_UI_BUILTIN_FONTS)
    set(BUILTIN_FONTS_DIR "${CMAKE_CURRENT_LIST_DIR}/../app/src/views/assets/fonts/builtin")

    foreach(BUILTIN_FONT IN ITEMS Rubik-Medium Montserrat-Medium Inconsolata-Bold)
        string(TOLOWER "${BUILTIN_FONT}" BUILTIN_FONT_ID)
        string(REPLACE "-" "_" BUILTIN_FONT_ID "${BUILTIN_FONT_ID}")

        generate_inc_file_for_target(app "${BUILTIN_FONTS_DIR}/${BUILTIN_FONT}.ttf"
            "${ZEPHYR_BINARY_DIR}/include/generated/builtin_font_${BUILTIN_FONT_ID}.inc")
    endforeach()
endif()
