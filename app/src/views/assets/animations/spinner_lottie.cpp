#include "animations_register.h"

namespace eerie_leap::views::assets::animations {

// TODO: This data is for demonstration purposes only.
// Must be removed in production.

// A 120x120 spinner at 60 fps: an arc grows around a faint track, then shrinks from its tail
// while the layer turns once every two seconds.
const std::string_view ui_lottie_spinner_json = R"json({
  "v": "5.7.4", "fr": 60, "ip": 0, "op": 120, "w": 120, "h": 120, "nm": "spinner", "ddd": 0, "assets": [],
  "layers": [{
    "ddd": 0, "ind": 1, "ty": 4, "nm": "ring", "sr": 1, "ao": 0,
    "ip": 0, "op": 120, "st": 0, "bm": 0,
    "ks": {
      "o": { "a": 0, "k": 100 },
      "r": { "a": 1, "k": [
        { "t": 0, "s": [0], "i": { "x": [1], "y": [1] }, "o": { "x": [0], "y": [0] } },
        { "t": 120, "s": [360] }
      ] },
      "p": { "a": 0, "k": [60, 60, 0] },
      "a": { "a": 0, "k": [0, 0, 0] },
      "s": { "a": 0, "k": [100, 100, 100] }
    },
    "shapes": [
      {
        "ty": "gr", "nm": "arc",
        "it": [
          { "ty": "el", "nm": "path", "d": 1, "p": { "a": 0, "k": [0, 0] }, "s": { "a": 0, "k": [88, 88] } },
          { "ty": "st", "nm": "stroke", "c": { "a": 0, "k": [1, 1, 1, 1] }, "o": { "a": 0, "k": 100 },
            "w": { "a": 0, "k": 10 }, "lc": 2, "lj": 2 },
          { "ty": "tm", "nm": "trim", "m": 1, "o": { "a": 0, "k": 0 },
            "s": { "a": 1, "k": [
              { "t": 60, "s": [0], "i": { "x": [0.4], "y": [1] }, "o": { "x": [0.6], "y": [0] } },
              { "t": 120, "s": [100] }
            ] },
            "e": { "a": 1, "k": [
              { "t": 0, "s": [0], "i": { "x": [0.4], "y": [1] }, "o": { "x": [0.6], "y": [0] } },
              { "t": 60, "s": [100] }
            ] } },
          { "ty": "tr", "p": { "a": 0, "k": [0, 0] }, "a": { "a": 0, "k": [0, 0] },
            "s": { "a": 0, "k": [100, 100] }, "r": { "a": 0, "k": 0 }, "o": { "a": 0, "k": 100 } }
        ]
      },
      {
        "ty": "gr", "nm": "track",
        "it": [
          { "ty": "el", "nm": "path", "d": 1, "p": { "a": 0, "k": [0, 0] }, "s": { "a": 0, "k": [88, 88] } },
          { "ty": "st", "nm": "stroke", "c": { "a": 0, "k": [1, 1, 1, 1] }, "o": { "a": 0, "k": 25 },
            "w": { "a": 0, "k": 10 }, "lc": 2, "lj": 2 },
          { "ty": "tr", "p": { "a": 0, "k": [0, 0] }, "a": { "a": 0, "k": [0, 0] },
            "s": { "a": 0, "k": [100, 100] }, "r": { "a": 0, "k": 0 }, "o": { "a": 0, "k": 100 } }
        ]
      }
    ]
  }]
})json";

} // namespace eerie_leap::views::assets::animations
