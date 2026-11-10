#pragma once

// Display writes run on a thread of their own, Zephyr's flush thread or the frame writer, and never under the LVGL
// lock. A macro derived from Zephyr's own symbol, not a Kconfig symbol of the app: the test suites repeat the app's
// symbols in their own Kconfig files, and one they lack would silently pick the other path.
#if defined(CONFIG_LV_Z_FLUSH_THREAD) || defined(CONFIG_EERIE_LEAP_UI_FRAME_FLUSH)
    #define EERIE_LEAP_UI_ASYNC_FLUSH 1
#endif
