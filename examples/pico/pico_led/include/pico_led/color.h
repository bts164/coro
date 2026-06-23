#pragma once
#include <cstdint>
#include <algorithm>

namespace led {

struct Rgb;

// ---------------------------------------------------------------------------
// Hsv — hue/saturation/value, all channels in [0, 255]
// h=0 red, h=85 green, h=170 blue
// ---------------------------------------------------------------------------
struct Hsv {
    uint8_t h{}, s{}, v{};

    constexpr Hsv() noexcept = default;
    constexpr Hsv(uint8_t h, uint8_t s, uint8_t v) noexcept : h(h), s(s), v(v) {}
    explicit Hsv(Rgb c) noexcept;

    // Shortest-path hue interpolation — takes the short way around the colour wheel.
    static constexpr Hsv lerp(Hsv a, Hsv b, float t) noexcept {
        if (t <= 0.0f) return a;
        if (t >= 1.0f) return b;
        int dh = static_cast<int>(b.h) - static_cast<int>(a.h);
        if (dh >  128) dh -= 256;
        if (dh < -128) dh += 256;
        return {
            static_cast<uint8_t>(static_cast<int>(a.h) + static_cast<int>(static_cast<float>(dh) * t)),
            static_cast<uint8_t>(static_cast<int>(a.s) + static_cast<int>(static_cast<float>(static_cast<int>(b.s) - static_cast<int>(a.s)) * t)),
            static_cast<uint8_t>(static_cast<int>(a.v) + static_cast<int>(static_cast<float>(static_cast<int>(b.v) - static_cast<int>(a.v)) * t)),
        };
    }

    // Hue wraps (uint8 overflow); s and v clamp.
    static constexpr Hsv add(Hsv a, Hsv b) noexcept {
        return {
            static_cast<uint8_t>(a.h + b.h),
            static_cast<uint8_t>(std::min(255, static_cast<int>(a.s) + static_cast<int>(b.s))),
            static_cast<uint8_t>(std::min(255, static_cast<int>(a.v) + static_cast<int>(b.v))),
        };
    }

    // Scale value channel; hue and saturation unchanged.
    constexpr Hsv scaled(float t) const noexcept {
        return {h, s, static_cast<uint8_t>(static_cast<float>(v) * t)};
    }

    constexpr Hsv dimmed(uint8_t brightness) const noexcept {
        return {h, s, static_cast<uint8_t>((static_cast<uint16_t>(v) * brightness) >> 8)};
    }

    constexpr bool operator==(const Hsv&) const noexcept = default;
};

// ---------------------------------------------------------------------------
// Hsl — hue/saturation/lightness, all channels in [0, 255]
// h=0 red, h=85 green, h=170 blue
// ---------------------------------------------------------------------------
struct Hsl {
    uint8_t h{}, s{}, l{};

    constexpr Hsl() noexcept = default;
    constexpr Hsl(uint8_t h, uint8_t s, uint8_t l) noexcept : h(h), s(s), l(l) {}
    explicit Hsl(Rgb c) noexcept;

    // Shortest-path hue interpolation.
    static constexpr Hsl lerp(Hsl a, Hsl b, float t) noexcept {
        if (t <= 0.0f) return a;
        if (t >= 1.0f) return b;
        int dh = static_cast<int>(b.h) - static_cast<int>(a.h);
        if (dh >  128) dh -= 256;
        if (dh < -128) dh += 256;
        return {
            static_cast<uint8_t>(static_cast<int>(a.h) + static_cast<int>(static_cast<float>(dh) * t)),
            static_cast<uint8_t>(static_cast<int>(a.s) + static_cast<int>(static_cast<float>(static_cast<int>(b.s) - static_cast<int>(a.s)) * t)),
            static_cast<uint8_t>(static_cast<int>(a.l) + static_cast<int>(static_cast<float>(static_cast<int>(b.l) - static_cast<int>(a.l)) * t)),
        };
    }

    // Hue wraps (uint8 overflow); s and l clamp.
    static constexpr Hsl add(Hsl a, Hsl b) noexcept {
        return {
            static_cast<uint8_t>(a.h + b.h),
            static_cast<uint8_t>(std::min(255, static_cast<int>(a.s) + static_cast<int>(b.s))),
            static_cast<uint8_t>(std::min(255, static_cast<int>(a.l) + static_cast<int>(b.l))),
        };
    }

    // Scale lightness channel; hue and saturation unchanged.
    constexpr Hsl scaled(float t) const noexcept {
        return {h, s, static_cast<uint8_t>(static_cast<float>(l) * t)};
    }

    constexpr Hsl dimmed(uint8_t brightness) const noexcept {
        return {h, s, static_cast<uint8_t>((static_cast<uint16_t>(l) * brightness) >> 8)};
    }

    constexpr bool operator==(const Hsl&) const noexcept = default;
};

// ---------------------------------------------------------------------------
// Rgb — red/green/blue, all channels in [0, 255]
//
// Implicit conversions from Hsv and Hsl mean either type can be passed
// wherever Rgb/Color is expected; the compiler inserts the conversion.
// ---------------------------------------------------------------------------
struct Rgb {
    uint8_t r{}, g{}, b{};

    constexpr Rgb() noexcept = default;
    constexpr Rgb(uint8_t r, uint8_t g, uint8_t b) noexcept : r(r), g(g), b(b) {}
    Rgb(Hsv c) noexcept;
    Rgb(Hsl c) noexcept;

    static constexpr Rgb black() noexcept { return {}; }
    static constexpr Rgb white() noexcept { return {255, 255, 255}; }

    static constexpr Rgb lerp(Rgb a, Rgb b, float t) noexcept {
        if (t <= 0.0f) return a;
        if (t >= 1.0f) return b;
        return {
            static_cast<uint8_t>(static_cast<int>(a.r) + static_cast<int>(static_cast<float>(static_cast<int>(b.r) - static_cast<int>(a.r)) * t)),
            static_cast<uint8_t>(static_cast<int>(a.g) + static_cast<int>(static_cast<float>(static_cast<int>(b.g) - static_cast<int>(a.g)) * t)),
            static_cast<uint8_t>(static_cast<int>(a.b) + static_cast<int>(static_cast<float>(static_cast<int>(b.b) - static_cast<int>(a.b)) * t)),
        };
    }

    static constexpr Rgb add(Rgb a, Rgb b) noexcept {
        return {
            static_cast<uint8_t>(std::min(255, static_cast<int>(a.r) + static_cast<int>(b.r))),
            static_cast<uint8_t>(std::min(255, static_cast<int>(a.g) + static_cast<int>(b.g))),
            static_cast<uint8_t>(std::min(255, static_cast<int>(a.b) + static_cast<int>(b.b))),
        };
    }

    constexpr Rgb scaled(float t) const noexcept { return lerp(black(), *this, t); }

    constexpr Rgb dimmed(uint8_t brightness) const noexcept {
        uint16_t b16 = brightness;
        return {
            static_cast<uint8_t>((r * b16) >> 8),
            static_cast<uint8_t>((g * b16) >> 8),
            static_cast<uint8_t>((b * b16) >> 8),
        };
    }

    constexpr uint32_t to_grb_word() const noexcept {
        return (static_cast<uint32_t>(g) << 24)
             | (static_cast<uint32_t>(r) << 16)
             | (static_cast<uint32_t>(b) <<  8);
    }

    constexpr bool operator==(const Rgb&) const noexcept = default;
};

using Color = Rgb;

} // namespace led
