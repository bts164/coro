#include <cmath>
#include "pico_led/color.h"

namespace led {

// ---------------------------------------------------------------------------
// Rgb ← Hsv  (implicit conversion)
// ---------------------------------------------------------------------------

Rgb::Rgb(Hsv c) noexcept {
    float hf = c.h * (360.0f / 255.0f);
    float sf = c.s * (1.0f / 255.0f);
    float vf = c.v * (1.0f / 255.0f);

    float ch = vf * sf;
    float x  = ch * (1.0f - std::fabs(std::fmod(hf * (1.0f / 60.0f), 2.0f) - 1.0f));
    float m  = vf - ch;

    float rf{}, gf{}, bf{};
    if      (hf <  60.0f) { rf = ch; gf = x;  bf = 0;  }
    else if (hf < 120.0f) { rf = x;  gf = ch; bf = 0;  }
    else if (hf < 180.0f) { rf = 0;  gf = ch; bf = x;  }
    else if (hf < 240.0f) { rf = 0;  gf = x;  bf = ch; }
    else if (hf < 300.0f) { rf = x;  gf = 0;  bf = ch; }
    else                  { rf = ch; gf = 0;  bf = x;  }

    r = static_cast<uint8_t>((rf + m) * 255.0f);
    g = static_cast<uint8_t>((gf + m) * 255.0f);
    b = static_cast<uint8_t>((bf + m) * 255.0f);
}

// ---------------------------------------------------------------------------
// Rgb ← Hsl  (implicit conversion)
// ---------------------------------------------------------------------------

Rgb::Rgb(Hsl c) noexcept {
    float hf = c.h * (360.0f / 255.0f);
    float sf = c.s * (1.0f / 255.0f);
    float lf = c.l * (1.0f / 255.0f);

    float ch = (1.0f - std::fabs(2.0f * lf - 1.0f)) * sf;
    float x  = ch * (1.0f - std::fabs(std::fmod(hf * (1.0f / 60.0f), 2.0f) - 1.0f));
    float m  = lf - ch * 0.5f;

    float rf{}, gf{}, bf{};
    if      (hf <  60.0f) { rf = ch; gf = x;  bf = 0;  }
    else if (hf < 120.0f) { rf = x;  gf = ch; bf = 0;  }
    else if (hf < 180.0f) { rf = 0;  gf = ch; bf = x;  }
    else if (hf < 240.0f) { rf = 0;  gf = x;  bf = ch; }
    else if (hf < 300.0f) { rf = x;  gf = 0;  bf = ch; }
    else                  { rf = ch; gf = 0;  bf = x;  }

    r = static_cast<uint8_t>((rf + m) * 255.0f);
    g = static_cast<uint8_t>((gf + m) * 255.0f);
    b = static_cast<uint8_t>((bf + m) * 255.0f);
}

// ---------------------------------------------------------------------------
// Hsv ← Rgb  (explicit conversion)
// ---------------------------------------------------------------------------

Hsv::Hsv(Rgb c) noexcept {
    float rf = c.r * (1.0f / 255.0f);
    float gf = c.g * (1.0f / 255.0f);
    float bf = c.b * (1.0f / 255.0f);

    float cmax  = std::fmax(rf, std::fmax(gf, bf));
    float cmin  = std::fmin(rf, std::fmin(gf, bf));
    float delta = cmax - cmin;

    float hf = 0.0f;
    if (delta > 0.0f) {
        float inv_delta = 1.0f / delta;
        if      (cmax == rf) hf = 60.0f * std::fmod((gf - bf) * inv_delta, 6.0f);
        else if (cmax == gf) hf = 60.0f * ((bf - rf) * inv_delta + 2.0f);
        else                 hf = 60.0f * ((rf - gf) * inv_delta + 4.0f);
        if (hf < 0.0f) hf += 360.0f;
    }

    float sf = (cmax > 0.0f) ? delta / cmax : 0.0f;

    h = static_cast<uint8_t>(hf * (255.0f / 360.0f) + 0.5f);
    s = static_cast<uint8_t>(sf * 255.0f + 0.5f);
    v = static_cast<uint8_t>(cmax * 255.0f + 0.5f);
}

// ---------------------------------------------------------------------------
// Hsl ← Rgb  (explicit conversion)
// ---------------------------------------------------------------------------

Hsl::Hsl(Rgb c) noexcept {
    float rf = c.r * (1.0f / 255.0f);
    float gf = c.g * (1.0f / 255.0f);
    float bf = c.b * (1.0f / 255.0f);

    float cmax  = std::fmax(rf, std::fmax(gf, bf));
    float cmin  = std::fmin(rf, std::fmin(gf, bf));
    float delta = cmax - cmin;
    float lf    = (cmax + cmin) * 0.5f;

    float hf = 0.0f;
    float sf = 0.0f;
    if (delta > 0.0f) {
        float inv_delta = 1.0f / delta;
        if      (cmax == rf) hf = 60.0f * std::fmod((gf - bf) * inv_delta, 6.0f);
        else if (cmax == gf) hf = 60.0f * ((bf - rf) * inv_delta + 2.0f);
        else                 hf = 60.0f * ((rf - gf) * inv_delta + 4.0f);
        if (hf < 0.0f) hf += 360.0f;
        sf = delta / (1.0f - std::fabs(2.0f * lf - 1.0f));
    }

    h = static_cast<uint8_t>(hf * (255.0f / 360.0f) + 0.5f);
    s = static_cast<uint8_t>(sf * 255.0f + 0.5f);
    l = static_cast<uint8_t>(lf * 255.0f + 0.5f);
}

} // namespace led
