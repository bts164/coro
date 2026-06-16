#include "effects.h"
#include <chrono>
#include <cmath>
#include <numbers>
#include <random>
#include <vector>
#include <algorithm>
#include <memory>

namespace led {

// Convert a proto Color to led::Color.
static Color from_proto(const ::Color& c) noexcept {
    return Color(
        static_cast<uint8_t>(std::min<uint32_t>(c.r, 255u)),
        static_cast<uint8_t>(std::min<uint32_t>(c.g, 255u)),
        static_cast<uint8_t>(std::min<uint32_t>(c.b, 255u)));
}

// Return v if non-zero, else fallback.
static float def(float v, float fallback) noexcept { return v != 0.0f ? v : fallback; }

// Decrement each channel of c toward black by value (PicoLed::fadePixelValue toward RGB(0,0,0)).
static void fade_pixel_value(Color& c, uint8_t value) noexcept {
    c.r = c.r > value ? c.r - value : 0;
    c.g = c.g > value ? c.g - value : 0;
    c.b = c.b > value ? c.b - value : 0;
}

// Sub-pixel accurate line draw onto a Color buffer.
// Equivalent to PicoLed::fadeLine(color, first, count, 1.0) in MODE_SET:
//   full pixels  → set to color
//   edge pixels  → lerp(existing, color, coverage_fraction)
static void fade_line_onto(std::vector<Color>& buf, Color color, float first, float count) {
    if (first < 0.0f) { count += first; first = 0.0f; }
    float last = first + count;
    float n    = static_cast<float>(buf.size());
    if (last > n) last = n;
    if (last <= first) return;

    int iN           = static_cast<int>(buf.size());
    float mid_start  = std::ceil(first);
    float mid_end    = std::floor(last);

    if (last <= mid_start) {
        // Entire span within one pixel
        int i = static_cast<int>(first);
        if (i >= 0 && i < iN)
            buf[i] = Color::lerp(buf[i], color, last - first);
    } else {
        // Partial first pixel
        int i0 = static_cast<int>(first);
        if (first < mid_start && i0 >= 0 && i0 < iN)
            buf[i0] = Color::lerp(buf[i0], color, mid_start - first);
        // Full middle pixels
        for (int i = static_cast<int>(mid_start); i < static_cast<int>(mid_end); ++i)
            if (i >= 0 && i < iN) buf[i] = color;
        // Partial last pixel
        int iL = static_cast<int>(mid_end);
        if (last > mid_end && iL >= 0 && iL < iN)
            buf[iL] = Color::lerp(buf[iL], color, last - mid_end);
    }
}

// ---------------------------------------------------------------------------
// fade
// ---------------------------------------------------------------------------

coro::CoroStream<PixelBuffer> fade_effect(
        std::shared_ptr<LedDriver> driver, FadeEffect params) {
    Color color   = from_proto(params.color);
    float rate_hz = def(params.rate, 1.0f);

    PixelBuffer frame;
    float phase = 0.0f;

    for (;;) {
        frame.resize(driver->led_count());
        float brightness = 0.5f * (1.0f + std::sin(phase));
        uint32_t word = Color::lerp(Color::black(), color, brightness).to_grb_word();
        for (auto& px : frame) px = word;

        auto yield_time = std::chrono::steady_clock::now();
        co_yield frame;
        float dt = std::chrono::duration<float>(std::chrono::steady_clock::now() - yield_time).count();

        phase += rate_hz * 2.0f * std::numbers::pi_v<float> * dt;
    }
}

// ---------------------------------------------------------------------------
// marquee
// ---------------------------------------------------------------------------

coro::CoroStream<PixelBuffer> marquee_effect(
        std::shared_ptr<LedDriver> driver, MarqueeEffect params) {
    Color color = from_proto(params.color);
    float speed = def(params.speed, 8.0f);

    uint32_t on_word  = color.to_grb_word();
    uint32_t off_word = 0u;

    PixelBuffer frame;
    float offset = 0.0f;

    for (;;) {
        std::size_t n = driver->led_count();
        frame.resize(n);

        for (std::size_t i = 0; i < n; ++i) {
            float pos = std::fmod(
                static_cast<float>(i) - offset + static_cast<float>(n) * 6.0f, 6.0f);
            frame[i] = (pos < 3.0f) ? on_word : off_word;
        }

        auto yield_time = std::chrono::steady_clock::now();
        co_yield frame;
        float dt = std::chrono::duration<float>(std::chrono::steady_clock::now() - yield_time).count();

        offset += speed * dt;
    }
}

// ---------------------------------------------------------------------------
// comet
// ---------------------------------------------------------------------------

coro::CoroStream<PixelBuffer> comet_effect(
        std::shared_ptr<LedDriver> driver, CometEffect params) {
    Color tail_color = from_proto(params.tail_color);
    Color head_color = from_proto(params.head_color);
    // Default head colour to tail colour when not explicitly set.
    if (head_color == Color::black()) head_color = tail_color;
    float speed     = def(params.speed,    15.0f);
    float length    = def(params.length,    4.0f);
    float fade_rate = def(params.faderate,  0.5f);
    bool  wrap      = params.wrap;

    // fade_interval: seconds per integer fade step (PicoLed: 1000/255/fadeRate ms)
    float fade_interval = 1.0f / 255.0f / fade_rate;

    std::vector<Color> trail;
    std::vector<Color> display;   // trail copy with head overridden in head_color
    PixelBuffer frame;
    float pos      = 0.0f;
    float vel      = speed;
    auto fade_last = std::chrono::steady_clock::now();
    std::mt19937 rng(42);

    for (;;) {
        std::size_t n = driver->led_count();
        float fn      = static_cast<float>(n);
        trail.resize(n, Color::black());
        frame.assign(n, 0u);

        // Fade tail: stochastic per-pixel fade toward black (Comet::fadePixels).
        // Only fires when enough time has accumulated for a non-trivial fade step (> 12),
        // which suppresses per-frame micro-flicker.
        auto fade_now  = std::chrono::steady_clock::now();
        float time_gone = std::chrono::duration<float>(fade_now - fade_last).count();
        auto fade_value = static_cast<uint8_t>(time_gone / fade_interval);
        if (fade_value > 12) {
            std::uniform_int_distribution<int> coin(0, 1);
            for (auto& c : trail)
                if (coin(rng)) fade_pixel_value(c, fade_value);
            fade_last = fade_now;
        }

        // Paint tail_color onto the persistent trail so future fades decay the
        // tail colour.  Then build a display copy with the head overridden in
        // head_color — same pattern as the Python led_sim generator.
        fade_line_onto(trail, tail_color, pos, length);
        if (wrap)
            fade_line_onto(trail, tail_color, pos - fn, length);

        display = trail;
        fade_line_onto(display, head_color, pos, length);
        if (wrap)
            fade_line_onto(display, head_color, pos - fn, length);

        for (std::size_t i = 0; i < n; ++i)
            frame[i] = display[i].to_grb_word();

        auto yield_time = std::chrono::steady_clock::now();
        co_yield frame;
        float dt = std::chrono::duration<float>(std::chrono::steady_clock::now() - yield_time).count();

        pos += vel * dt;
        if (wrap) {
            // Keep pos in [0, n) so the shifted copy above always covers the seam.
            pos = std::fmod(pos + fn, fn);
        } else {
            float top = fn - 1.0f;
            if (pos > top - 1.0f)        { vel *= -1.0f; pos = top - 1.0f; }
            else if (pos + length < 1.0f) { vel *= -1.0f; pos = 1.0f - length; }
        }
    }
}

// ---------------------------------------------------------------------------
// stars
// ---------------------------------------------------------------------------

coro::CoroStream<PixelBuffer> stars_effect(
        std::shared_ptr<LedDriver> driver, StarsEffect params) {
    Color color      = from_proto(params.color);
    float spawn_rate = def(params.spawnrate, 8.0f);
    float fade_rate  = def(params.faderate,  1.5f);

    struct Star { std::size_t pos; float brightness; };

    PixelBuffer frame;
    std::vector<Star> stars;
    stars.reserve(32);
    std::mt19937 rng(42);

    for (;;) {
        std::size_t n = driver->led_count();
        frame.assign(n, 0u);

        for (const auto& s : stars)
            if (s.pos < frame.size())
                frame[s.pos] = Color::lerp(Color::black(), color, s.brightness).to_grb_word();

        auto yield_time = std::chrono::steady_clock::now();
        co_yield frame;
        float dt = std::chrono::duration<float>(std::chrono::steady_clock::now() - yield_time).count();

        for (auto& s : stars)
            s.brightness -= fade_rate * dt;
        stars.erase(std::remove_if(stars.begin(), stars.end(),
            [](const Star& s) { return s.brightness <= 0.0f; }), stars.end());

        float expected = spawn_rate * dt;
        std::uniform_real_distribution<float> prob(0.0f, 1.0f);
        std::uniform_int_distribution<std::size_t> pos_dist(0, n > 0 ? n - 1 : 0);
        while (prob(rng) < expected) {
            stars.push_back({pos_dist(rng), 1.0f});
            expected -= 1.0f;
        }
    }
}

// ---------------------------------------------------------------------------
// bounce
// ---------------------------------------------------------------------------

coro::CoroStream<PixelBuffer> bounce_effect(
        std::shared_ptr<LedDriver> driver, BounceEffect params) {
    Color tail_color = from_proto(params.tail_color);
    Color head_color = from_proto(params.head_color);
    if (head_color == Color::black()) head_color = tail_color;
    float length    = def(params.size,      2.0f);
    float gravity   = def(params.gravity,  40.0f);
    float fade_rate = def(params.faderate,  0.5f);

    float fade_interval = 1.0f / 255.0f / fade_rate;

    std::vector<Color> trail;
    std::vector<Color> display;
    PixelBuffer frame;
    float n0  = static_cast<float>(driver->led_count());
    float pos = 0.0f;
    float vel = std::sqrt(2.0f * gravity * n0);   // PicoLed uses numLeds (not numLeds-1)
    auto fade_last = std::chrono::steady_clock::now();

    for (;;) {
        std::size_t n = driver->led_count();
        trail.resize(n, Color::black());
        frame.assign(n, 0u);

        // Fade tail: apply to ALL pixels (Fade::fadePixels, not the comet override).
        // No randomness, no > 12 threshold.
        auto fade_now   = std::chrono::steady_clock::now();
        float time_gone = std::chrono::duration<float>(fade_now - fade_last).count();
        auto fade_value = static_cast<uint8_t>(time_gone / fade_interval);
        if (fade_value >= 1) {
            for (auto& c : trail) fade_pixel_value(c, fade_value);
            fade_last = fade_now;
        }

        // Paint tail_color onto trail for future fade, then build display copy
        // with the ball position overridden in head_color.
        fade_line_onto(trail, tail_color, pos, length);

        display = trail;
        fade_line_onto(display, head_color, pos, length);

        for (std::size_t i = 0; i < n; ++i)
            frame[i] = display[i].to_grb_word();

        auto yield_time = std::chrono::steady_clock::now();
        co_yield frame;
        float dt = std::chrono::duration<float>(std::chrono::steady_clock::now() - yield_time).count();

        // PicoLed integration order: move first, then apply gravity.
        float ceiling = static_cast<float>(n > 0 ? n - 1 : 0);
        float base_speed = std::sqrt(2.0f * gravity * static_cast<float>(n));
        pos += vel * dt;
        vel -= gravity * dt;
        if (pos > ceiling) {
            // Elastic ceiling reflection (corrects overshoot).
            pos = 2.0f * ceiling - pos;
            vel *= -1.0f;
        }
        if (pos < 0.0f) {
            pos *= -1.0f;
            vel *= -0.9f;   // 10% energy loss per floor bounce (PicoLed)
            if (std::abs(vel) < 0.4f * base_speed) {
                pos = 0.0f;
                vel = base_speed;
            }
        }
    }
}

// ---------------------------------------------------------------------------
// particles
// ---------------------------------------------------------------------------

coro::CoroStream<PixelBuffer> particles_effect(
        std::shared_ptr<LedDriver> driver, ParticlesEffect params) {
    Color color  = from_proto(params.color);
    float spread = def(params.spread, 12.0f);
    float cool   = def(params.cool,    1.2f);

    struct Particle { float pos; float vel; float brightness; };

    PixelBuffer frame;
    std::vector<Particle> particles;
    particles.reserve(64);
    std::mt19937 rng(99);

    for (;;) {
        std::size_t n = driver->led_count();
        float centre = static_cast<float>(n) / 2.0f;
        frame.assign(n, 0u);

        for (const auto& p : particles) {
            int i = static_cast<int>(p.pos);
            if (i >= 0 && i < static_cast<int>(n)) {
                auto idx = static_cast<std::size_t>(i);
                // Additive blend: unpack existing word, add new contribution, repack.
                uint32_t existing = frame[idx];
                Color ex{
                    static_cast<uint8_t>((existing >> 16) & 0xFF),
                    static_cast<uint8_t>((existing >> 24) & 0xFF),
                    static_cast<uint8_t>((existing >>  8) & 0xFF),
                };
                frame[idx] = Color::add(ex, Color::lerp(Color::black(), color, p.brightness))
                                 .to_grb_word();
            }
        }

        auto yield_time = std::chrono::steady_clock::now();
        co_yield frame;
        float dt = std::chrono::duration<float>(std::chrono::steady_clock::now() - yield_time).count();

        for (auto& p : particles) {
            p.pos        += p.vel * dt;
            p.brightness -= cool  * dt;
        }
        particles.erase(std::remove_if(particles.begin(), particles.end(),
            [&](const Particle& p) {
                return p.brightness <= 0.0f
                    || p.pos < -1.0f
                    || p.pos > static_cast<float>(n);
            }), particles.end());

        std::uniform_real_distribution<float> vel_dist(spread * 0.5f, spread * 1.5f);
        float v = vel_dist(rng);
        particles.push_back({centre,  v, 1.0f});
        particles.push_back({centre, -v, 1.0f});
    }
}

// ---------------------------------------------------------------------------
// waves
// ---------------------------------------------------------------------------

coro::CoroStream<PixelBuffer> waves_effect(
        std::shared_ptr<LedDriver> driver, WavesEffect params) {
    Color color1     = from_proto(params.color1);
    Color color2     = from_proto(params.color2);
    float wavelength = def(params.wavelength, 12.0f);
    float gravity    = def(params.gravity,    25.0f);

    PixelBuffer frame;
    float phase = 0.0f;
    float vel   = 0.0f;

    for (;;) {
        std::size_t n = driver->led_count();
        frame.resize(n);

        float k = 2.0f * std::numbers::pi_v<float> / wavelength;
        for (std::size_t i = 0; i < n; ++i) {
            float x = k * (static_cast<float>(i) - phase);
            float s = 0.5f + 0.5f * std::sin(x);
            frame[i] = Color::lerp(color1, color2,  s).to_grb_word();
        }

        auto yield_time = std::chrono::steady_clock::now();
        co_yield frame;
        float dt = std::chrono::duration<float>(std::chrono::steady_clock::now() - yield_time).count();

        vel   += gravity * dt;
        phase += vel     * dt;
        float top = static_cast<float>(n);
        if (phase < 0.0f) { phase = 0.0f; vel =  std::abs(vel); }
        if (phase > top)  { phase = top;  vel = -std::abs(vel); }
    }
}

// ---------------------------------------------------------------------------
// spring
// ---------------------------------------------------------------------------

coro::CoroStream<PixelBuffer> spring_effect(
        std::shared_ptr<LedDriver> driver, SpringEffect params) {
    Color color1      = from_proto(params.color1);
    Color color2      = from_proto(params.color2);
    float wavelength  = def(params.wavelength,  12.0f);
    float spring_k    = def(params.k,            8.0f);
    float damping     = params.damping / 20.0;  // 0 = no damping
    float equil_frac  = def(params.equilibrium, 2.0f / 3.0f);

    PixelBuffer frame;

    // Number of wave cycles is fixed at the initial strip length / wavelength.
    // As the spring compresses the wavelength scales proportionally, so the
    // cycles squeeze and stretch — the slinky/coil-spring visual.
    std::size_t n0      = driver->led_count();
    float n_cycles      = static_cast<float>(n0) / wavelength;
    float natural       = static_cast<float>(n0) * equil_frac;
    float extent        = static_cast<float>(n0);   // start fully extended
    float extent_vel    = 0.0f;

    // Capture initial PE so each kick restores the same energy budget.
    float pe0 = 0.5f * spring_k * (extent - natural) * (extent - natural);

    // Random kick timer and colour-phase toggle.
    std::mt19937 rng(std::random_device{}());
    std::uniform_real_distribution<float> kick_dist(0.2f, 5.0f);
    float kick_timer = kick_dist(rng);
    float phi        = 0.0f;   // 0 or π — flips color1/color2 on each kick

    for (;;) {
        std::size_t n = driver->led_count();
        frame.assign(n, 0u);

        // Render wave scaled to current extent, centred on the strip.
        float centre = static_cast<float>(n - 1) * 0.5f;
        float left   = centre - extent * 0.5f;
        float right  = centre + extent * 0.5f;

        for (std::size_t i = 0; i < n; ++i) {
            float fi = static_cast<float>(i);
            if (fi < left || fi > right) continue;
            float t     = (fi - left) / extent;   // [0, 1] within current extent
            float angle = 2.0f * std::numbers::pi_v<float> * n_cycles * t;
            float s     = 0.5f + 0.5f * std::cos(angle + phi);
            frame[i] = Color::lerp(color1, color2, s).to_grb_word();
        }

        auto yield_time = std::chrono::steady_clock::now();
        co_yield frame;
        float dt = std::chrono::duration<float>(std::chrono::steady_clock::now() - yield_time).count();

        // Hooke's law with optional viscous damping.
        float force  = -spring_k * (extent - natural) - damping * extent_vel;
        extent_vel  += force * dt;
        extent      += extent_vel * dt;
        extent       = std::max(1.0f, std::min(static_cast<float>(n), extent));

        // Periodic kick: restore initial energy and flip the colour phase.
        kick_timer -= dt;
        if (kick_timer <= 0.0f) {
            kick_timer = kick_dist(rng);
            float pe     = 0.5f * spring_k * (extent - natural) * (extent - natural);
            float ke     = pe0 - pe;
            float sign   = (extent > natural) ? -1.0f : 1.0f;
            extent_vel   = sign * std::sqrt(std::max(0.0f, 2.0f * ke));
            phi          = (phi == 0.0f) ? std::numbers::pi_v<float> : 0.0f;
        }
    }
}

// ---------------------------------------------------------------------------
// plasma
// ---------------------------------------------------------------------------

coro::CoroStream<PixelBuffer> plasma_effect(
        std::shared_ptr<LedDriver> driver, PlasmaEffect params) {
    float speed   = def(params.speed,          1.0f);
    float theta   = def(params.brownian_theta, 0.0f);  // mean-reversion rate (OU θ)
    float sigma   = def(params.brownian_sigma, 0.0f);  // volatility (OU σ)

    // Three sine waves with incommensurable spatial & temporal frequencies.
    // Negative temporal frequency makes the middle wave travel in the opposite
    // direction, producing organic interference.
    struct Wave { float k; float w; };
    static constexpr Wave waves[] = {
        {1.5f,  1.10f},
        {3.7f, -0.73f},
        {0.8f,  1.87f},
    };

    std::mt19937 rng(std::random_device{}());
    std::normal_distribution<float> randn(0.0f, 1.0f);

    PixelBuffer frame;
    float t              = 0.0f;   // phase accumulator driven by effective_speed
    float effective_speed = speed;  // Ornstein-Uhlenbeck state; starts at base speed

    for (;;) {
        std::size_t n = driver->led_count();
        frame.resize(n);
        float fn = static_cast<float>(n > 1 ? n - 1 : 1);

        for (std::size_t i = 0; i < n; ++i) {
            float x = static_cast<float>(i) / fn;   // normalised [0, 1]
            float v = 0.0f;
            for (const auto& w : waves)
                v += std::sin(2.0f * std::numbers::pi_v<float> * w.k * x + w.w * t);
            v /= 3.0f;   // [-1, 1]

            auto hue = static_cast<uint8_t>(static_cast<int>((v + 1.0f) * 0.5f * 255.0f) & 0xFF);
            frame[i] = Color(Hsv(hue, 255, 255)).to_grb_word();
        }

        auto yield_time = std::chrono::steady_clock::now();
        co_yield frame;
        float dt = std::chrono::duration<float>(std::chrono::steady_clock::now() - yield_time).count();

        // Ornstein-Uhlenbeck update: dv = θ(μ - v)dt + σ√dt · N(0,1)
        effective_speed += theta * (speed - effective_speed) * dt
                         + sigma * std::sqrt(dt) * randn(rng);
        t += dt * effective_speed;
    }
}

} // namespace led
