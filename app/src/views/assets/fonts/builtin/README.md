# Built-in fonts

TrueType fonts compiled into flash and served as read-only files of the `fonts` asset kind
(`CONFIG_EERIE_LEAP_UI_BUILTIN_FONTS`, `views/assets/fonts/builtin_fonts.cpp`). Each is licensed under the SIL
Open Font License 1.1; the license texts sit next to the files.

| File | Font | Source |
| --- | --- | --- |
| `Rubik-Medium.ttf` | Rubik Medium | https://github.com/googlefonts/rubik |
| `Montserrat-Medium.ttf` | Montserrat Medium | https://github.com/JulietaUla/Montserrat |
| `Inconsolata-Bold.ttf` | Inconsolata Bold | https://github.com/googlefonts/Inconsolata |

The files are Latin-1 subsets of the upstream fonts, made with fontTools:

```sh
pyftsubset FONT.ttf --unicodes=U+0020-007E,U+00A0-00FF --layout-features=kern --no-hinting --output-file=OUT.ttf
```

The names are the ones clients and saved configurations use (`FONT_FILE_PATH`): never rename them.
