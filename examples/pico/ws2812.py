"""
ws2812.py — Python client for the pico_ws2812_tcp LED controller.

Usage:

    from ws2812 import LedController, Color, Rgb, Hsv, Hsl

    with LedController("192.168.1.42") as leds:
        leds.brightness(128)
        leds.set_leds(60)
        leds.fill(Rgb(255, 0, 0))
        leds.fill(Rgb(Hsv(170, 255, 255)))   # pure blue via HSV
        leds.fill(Rgb.hex("#ff8800"))
        leds.effect_comet(Rgb(0, 128, 255), speed=3.0, length=4.0, fade_rate=1.5)

Or manage the connection manually:

    leds = LedController("192.168.1.42")
    leds.connect()
    leds.clear()
    leds.disconnect()
"""

import math
import socket
import struct

import proto.ws2812_pb2 as pb


def _u8(v) -> int:
    return max(0, min(255, round(v)))


# ---------------------------------------------------------------------------
# Colour types
#
# All channels stored as int in [0, 255].  Hue convention: h=0 red,
# h=85 green, h=170 blue (full circle mapped to 0–255).
#
# Cross-type constructors mirror the C++ design:
#   Rgb(hsv)  — implicit in C++, explicit call in Python
#   Hsv(rgb)  — explicit in C++, explicit call in Python
#   Hsl(rgb)  — explicit in C++, explicit call in Python
#
# Hsv ↔ Hsl conversions route through Rgb internally, same as C++.
# ---------------------------------------------------------------------------

class Hsv:
    """Hue / saturation / value, each in [0, 255].
    h=0 red, h=85 green, h=170 blue."""

    __slots__ = ("h", "s", "v")

    def __init__(self, h_or_color, s=None, v=None):
        if isinstance(h_or_color, Rgb):
            self.h, self.s, self.v = _rgb_to_hsv(h_or_color.r, h_or_color.g, h_or_color.b)
        elif isinstance(h_or_color, Hsl):
            rgb = Rgb(h_or_color)
            self.h, self.s, self.v = _rgb_to_hsv(rgb.r, rgb.g, rgb.b)
        else:
            self.h = _u8(h_or_color)
            self.s = _u8(s)
            self.v = _u8(v)

    # Shortest-path hue interpolation.
    @staticmethod
    def lerp(a: "Hsv", b: "Hsv", t: float) -> "Hsv":
        if t <= 0.0: return Hsv(a.h, a.s, a.v)
        if t >= 1.0: return Hsv(b.h, b.s, b.v)
        dh = int(b.h) - int(a.h)
        if dh >  128: dh -= 256
        if dh < -128: dh += 256
        return Hsv(
            (int(a.h) + round(dh * t)) % 256,
            _u8(int(a.s) + round((int(b.s) - int(a.s)) * t)),
            _u8(int(a.v) + round((int(b.v) - int(a.v)) * t)),
        )

    # Hue wraps; s and v clamp.
    @staticmethod
    def add(a: "Hsv", b: "Hsv") -> "Hsv":
        return Hsv((a.h + b.h) % 256, min(255, a.s + b.s), min(255, a.v + b.v))

    def scaled(self, t: float) -> "Hsv":
        return Hsv(self.h, self.s, _u8(self.v * t))

    def dimmed(self, brightness: int) -> "Hsv":
        return Hsv(self.h, self.s, (self.v * _u8(brightness)) >> 8)

    def __eq__(self, other):
        if isinstance(other, Hsv):
            return self.h == other.h and self.s == other.s and self.v == other.v
        return NotImplemented

    def __repr__(self):
        return f"Hsv({self.h}, {self.s}, {self.v})"


class Hsl:
    """Hue / saturation / lightness, each in [0, 255].
    h=0 red, h=85 green, h=170 blue."""

    __slots__ = ("h", "s", "l")

    def __init__(self, h_or_color, s=None, l=None):
        if isinstance(h_or_color, Rgb):
            self.h, self.s, self.l = _rgb_to_hsl(h_or_color.r, h_or_color.g, h_or_color.b)
        elif isinstance(h_or_color, Hsv):
            rgb = Rgb(h_or_color)
            self.h, self.s, self.l = _rgb_to_hsl(rgb.r, rgb.g, rgb.b)
        else:
            self.h = _u8(h_or_color)
            self.s = _u8(s)
            self.l = _u8(l)

    # Shortest-path hue interpolation.
    @staticmethod
    def lerp(a: "Hsl", b: "Hsl", t: float) -> "Hsl":
        if t <= 0.0: return Hsl(a.h, a.s, a.l)
        if t >= 1.0: return Hsl(b.h, b.s, b.l)
        dh = int(b.h) - int(a.h)
        if dh >  128: dh -= 256
        if dh < -128: dh += 256
        return Hsl(
            (int(a.h) + round(dh * t)) % 256,
            _u8(int(a.s) + round((int(b.s) - int(a.s)) * t)),
            _u8(int(a.l) + round((int(b.l) - int(a.l)) * t)),
        )

    # Hue wraps; s and l clamp.
    @staticmethod
    def add(a: "Hsl", b: "Hsl") -> "Hsl":
        return Hsl((a.h + b.h) % 256, min(255, a.s + b.s), min(255, a.l + b.l))

    def scaled(self, t: float) -> "Hsl":
        return Hsl(self.h, self.s, _u8(self.l * t))

    def dimmed(self, brightness: int) -> "Hsl":
        return Hsl(self.h, self.s, (self.l * _u8(brightness)) >> 8)

    def __eq__(self, other):
        if isinstance(other, Hsl):
            return self.h == other.h and self.s == other.s and self.l == other.l
        return NotImplemented

    def __repr__(self):
        return f"Hsl({self.h}, {self.s}, {self.l})"


class Rgb:
    """Red / green / blue, each in [0, 255].
    Accepts Hsv or Hsl in place of r/g/b to convert from those spaces."""

    __slots__ = ("r", "g", "b")

    def __init__(self, r_or_color, g=None, b=None):
        if isinstance(r_or_color, Hsv):
            self.r, self.g, self.b = _hsv_to_rgb(r_or_color.h, r_or_color.s, r_or_color.v)
        elif isinstance(r_or_color, Hsl):
            self.r, self.g, self.b = _hsl_to_rgb(r_or_color.h, r_or_color.s, r_or_color.l)
        else:
            self.r = _u8(r_or_color)
            self.g = _u8(g)
            self.b = _u8(b)

    @staticmethod
    def black() -> "Rgb":
        return Rgb(0, 0, 0)

    @staticmethod
    def white() -> "Rgb":
        return Rgb(255, 255, 255)

    @staticmethod
    def hex(s: str) -> "Rgb":
        s = s.lstrip("#")
        if len(s) != 6:
            raise ValueError(f"expected 6 hex digits, got {s!r}")
        return Rgb(int(s[0:2], 16), int(s[2:4], 16), int(s[4:6], 16))

    @staticmethod
    def lerp(a: "Rgb", b: "Rgb", t: float) -> "Rgb":
        if t <= 0.0: return Rgb(a.r, a.g, a.b)
        if t >= 1.0: return Rgb(b.r, b.g, b.b)
        return Rgb(
            _u8(int(a.r) + round((int(b.r) - int(a.r)) * t)),
            _u8(int(a.g) + round((int(b.g) - int(a.g)) * t)),
            _u8(int(a.b) + round((int(b.b) - int(a.b)) * t)),
        )

    @staticmethod
    def add(a: "Rgb", b: "Rgb") -> "Rgb":
        return Rgb(min(255, a.r + b.r), min(255, a.g + b.g), min(255, a.b + b.b))

    def scaled(self, t: float) -> "Rgb":
        return Rgb.lerp(Rgb.black(), self, t)

    def dimmed(self, brightness: int) -> "Rgb":
        b16 = _u8(brightness)
        return Rgb((self.r * b16) >> 8, (self.g * b16) >> 8, (self.b * b16) >> 8)

    def to_hex(self) -> str:
        return f"#{self.r:02x}{self.g:02x}{self.b:02x}"

    def __eq__(self, other):
        if isinstance(other, Rgb):
            return self.r == other.r and self.g == other.g and self.b == other.b
        return NotImplemented

    def __repr__(self):
        return f"Rgb({self.r}, {self.g}, {self.b})"


Color = Rgb


# ---------------------------------------------------------------------------
# Conversion math (module-private)
# ---------------------------------------------------------------------------

def _hsv_to_rgb(h: int, s: int, v: int) -> tuple[int, int, int]:
    hf = h * (360.0 / 255.0)
    sf = s / 255.0
    vf = v / 255.0
    c  = vf * sf
    x  = c * (1.0 - abs(math.fmod(hf / 60.0, 2.0) - 1.0))
    m  = vf - c
    if   hf <  60: rf, gf, bf = c, x, 0.0
    elif hf < 120: rf, gf, bf = x, c, 0.0
    elif hf < 180: rf, gf, bf = 0.0, c, x
    elif hf < 240: rf, gf, bf = 0.0, x, c
    elif hf < 300: rf, gf, bf = x, 0.0, c
    else:          rf, gf, bf = c, 0.0, x
    return _u8((rf + m) * 255), _u8((gf + m) * 255), _u8((bf + m) * 255)


def _hsl_to_rgb(h: int, s: int, l: int) -> tuple[int, int, int]:
    hf = h * (360.0 / 255.0)
    sf = s / 255.0
    lf = l / 255.0
    c  = (1.0 - abs(2.0 * lf - 1.0)) * sf
    x  = c * (1.0 - abs(math.fmod(hf / 60.0, 2.0) - 1.0))
    m  = lf - c * 0.5
    if   hf <  60: rf, gf, bf = c, x, 0.0
    elif hf < 120: rf, gf, bf = x, c, 0.0
    elif hf < 180: rf, gf, bf = 0.0, c, x
    elif hf < 240: rf, gf, bf = 0.0, x, c
    elif hf < 300: rf, gf, bf = x, 0.0, c
    else:          rf, gf, bf = c, 0.0, x
    return _u8((rf + m) * 255), _u8((gf + m) * 255), _u8((bf + m) * 255)


def _rgb_to_hsv(r: int, g: int, b: int) -> tuple[int, int, int]:
    rf, gf, bf = r / 255.0, g / 255.0, b / 255.0
    cmax  = max(rf, gf, bf)
    cmin  = min(rf, gf, bf)
    delta = cmax - cmin
    h = 0.0
    if delta > 0.0:
        inv = 1.0 / delta
        if   cmax == rf: h = 60.0 * (math.fmod((gf - bf) * inv, 6.0))
        elif cmax == gf: h = 60.0 * ((bf - rf) * inv + 2.0)
        else:            h = 60.0 * ((rf - gf) * inv + 4.0)
        if h < 0.0: h += 360.0
    s = delta / cmax if cmax > 0.0 else 0.0
    return _u8(h * (255.0 / 360.0)) % 256, _u8(s * 255), _u8(cmax * 255)


def _rgb_to_hsl(r: int, g: int, b: int) -> tuple[int, int, int]:
    rf, gf, bf = r / 255.0, g / 255.0, b / 255.0
    cmax  = max(rf, gf, bf)
    cmin  = min(rf, gf, bf)
    delta = cmax - cmin
    lf    = (cmax + cmin) * 0.5
    h = 0.0
    s = 0.0
    if delta > 0.0:
        inv = 1.0 / delta
        if   cmax == rf: h = 60.0 * math.fmod((gf - bf) * inv, 6.0)
        elif cmax == gf: h = 60.0 * ((bf - rf) * inv + 2.0)
        else:            h = 60.0 * ((rf - gf) * inv + 4.0)
        if h < 0.0: h += 360.0
        s = delta / (1.0 - abs(2.0 * lf - 1.0))
    return _u8(h * (255.0 / 360.0)) % 256, _u8(s * 255), _u8(lf * 255)


# ---------------------------------------------------------------------------
# Controller
# ---------------------------------------------------------------------------

class LedControllerError(Exception):
    pass


class LedController:
    DEFAULT_PORT = 2812

    def __init__(self, host: str, port: int = DEFAULT_PORT, timeout: float = 5.0):
        self._host = host
        self._port = port
        self._timeout = timeout
        self._sock: socket.socket | None = None

    # ------------------------------------------------------------------
    # Connection management
    # ------------------------------------------------------------------

    def connect(self) -> "LedController":
        self._sock = socket.create_connection((self._host, self._port), timeout=self._timeout)
        return self

    def disconnect(self):
        if self._sock:
            try:
                self._sock.close()
            except OSError:
                pass
            self._sock = None

    def __enter__(self) -> "LedController":
        return self.connect()

    def __exit__(self, *_):
        self.disconnect()

    # ------------------------------------------------------------------
    # Manual pixel commands
    # ------------------------------------------------------------------

    def fill(self, color: Rgb) -> None:
        """Fill all active LEDs with a colour and show."""
        cmd = pb.Command()
        cmd.fill.r, cmd.fill.g, cmd.fill.b = color.r, color.g, color.b
        self._transact(cmd)

    def set_pixel(self, index: int, color: Rgb) -> None:
        """Set a single LED (0-based index) and show."""
        cmd = pb.Command()
        cmd.set.index = index
        cmd.set.color.r, cmd.set.color.g, cmd.set.color.b = color.r, color.g, color.b
        self._transact(cmd)

    def set_pixels(self, pixels: dict[int, Rgb]) -> None:
        """Set multiple LEDs in one message and show.

        Args:
            pixels: Mapping of LED index to Rgb.  Out-of-range indices are
                    silently ignored by the controller.
        """
        cmd = pb.Command()
        for index, color in pixels.items():
            px = cmd.set_pixels.pixels.add()
            px.index = index
            px.color.r, px.color.g, px.color.b = color.r, color.g, color.b
        self._transact(cmd)

    def clear(self) -> None:
        """Turn off all LEDs and show."""
        cmd = pb.Command()
        cmd.clear.CopyFrom(pb.Empty())
        self._transact(cmd)

    def show(self) -> None:
        """Push the current pixel buffer without modifying it."""
        cmd = pb.Command()
        cmd.show.CopyFrom(pb.Empty())
        self._transact(cmd)

    # ------------------------------------------------------------------
    # Control
    # ------------------------------------------------------------------

    def stop(self) -> None:
        """Stop any running effect."""
        cmd = pb.Command()
        cmd.stop.CopyFrom(pb.Empty())
        self._transact(cmd)

    def brightness(self, value: int) -> None:
        """Set global brightness (0–255)."""
        cmd = pb.Command()
        cmd.brightness = _u8(value)
        self._transact(cmd)

    def set_leds(self, count: int) -> None:
        """Set the active LED count (1–300)."""
        if not 1 <= count <= 300:
            raise ValueError(f"LED count must be 1–300, got {count}")
        cmd = pb.Command()
        cmd.set_leds = count
        self._transact(cmd)

    def get_leds(self) -> int:
        """Query the current active LED count."""
        cmd = pb.Command()
        cmd.get_leds.CopyFrom(pb.Empty())
        self._send_proto(cmd)
        resp = self._recv_response()
        which = resp.WhichOneof("result")
        if which == "led_count":
            return resp.led_count
        if which == "error":
            raise LedControllerError(f"controller error: {resp.error}")
        raise LedControllerError(f"unexpected response to get_leds: {resp}")

    # ------------------------------------------------------------------
    # Effects
    # ------------------------------------------------------------------

    def effect_fade(self, color: Rgb, rate: float = 1.0) -> None:
        """Fade all LEDs to a colour.

        Args:
            color: Target colour.
            rate:  Fade speed (0.1–10.0).
        """
        cmd = pb.Command()
        cmd.fade.color.r, cmd.fade.color.g, cmd.fade.color.b = color.r, color.g, color.b
        cmd.fade.rate = rate
        self._transact(cmd)

    def effect_marquee(self, color: Rgb, speed: float = 2.0) -> None:
        """Scrolling marquee.

        Args:
            color: Colour.
            speed: Scroll speed; negative reverses direction.
        """
        cmd = pb.Command()
        cmd.marquee.speed = speed
        cmd.marquee.color.r, cmd.marquee.color.g, cmd.marquee.color.b = color.r, color.g, color.b
        self._transact(cmd)

    def effect_comet(
        self,
        tail_color: Rgb,
        speed: float = 2.0,
        length: float = 3.0,
        fade_rate: float = 1.0,
        wrap: bool = False,
        head_color: Rgb | None = None,
    ) -> None:
        """Comet with a fading tail that bounces or wraps around the strip.

        Args:
            tail_color: Trail fade colour.
            speed:      Pixels per second.
            length:     Comet tail length in pixels.
            fade_rate:  How quickly the tail fades (0.1–10.0).
            wrap:       If True the comet wraps around instead of bouncing.
            head_color: Head colour; defaults to tail_color when omitted.
        """
        if head_color is None:
            head_color = tail_color
        cmd = pb.Command()
        cmd.comet.tail_color.r, cmd.comet.tail_color.g, cmd.comet.tail_color.b = tail_color.r, tail_color.g, tail_color.b
        cmd.comet.head_color.r, cmd.comet.head_color.g, cmd.comet.head_color.b = head_color.r, head_color.g, head_color.b
        cmd.comet.speed    = speed
        cmd.comet.length   = length
        cmd.comet.faderate = fade_rate
        cmd.comet.wrap     = wrap
        self._transact(cmd)

    def effect_stars(
        self,
        color: Rgb,
        spawn_rate: float = 1.0,
        fade_rate: float = 1.0,
    ) -> None:
        """Random twinkling stars.

        Args:
            color:      Colour.
            spawn_rate: How often new stars appear (0.1–10.0).
            fade_rate:  How quickly stars fade (0.1–10.0).
        """
        cmd = pb.Command()
        cmd.stars.color.r, cmd.stars.color.g, cmd.stars.color.b = color.r, color.g, color.b
        cmd.stars.spawnrate = spawn_rate
        cmd.stars.faderate = fade_rate
        self._transact(cmd)

    def effect_bounce(
        self,
        tail_color: Rgb,
        size: float = 2.0,
        gravity: float = 9.8,
        fade_rate: float = 1.0,
        head_color: Rgb | None = None,
    ) -> None:
        """Ball bouncing under gravity.

        Args:
            tail_color: Trail fade colour.
            size:       Ball size in pixels.
            gravity:    Gravitational acceleration (pixels/s²).
            fade_rate:  How quickly the trail fades.
            head_color: Ball colour; defaults to tail_color when omitted.
        """
        if head_color is None:
            head_color = tail_color
        cmd = pb.Command()
        cmd.bounce.tail_color.r, cmd.bounce.tail_color.g, cmd.bounce.tail_color.b = tail_color.r, tail_color.g, tail_color.b
        cmd.bounce.head_color.r, cmd.bounce.head_color.g, cmd.bounce.head_color.b = head_color.r, head_color.g, head_color.b
        cmd.bounce.size     = size
        cmd.bounce.gravity  = gravity
        cmd.bounce.faderate = fade_rate
        self._transact(cmd)

    def effect_particles(
        self,
        color: Rgb,
        spread: float = 0.5,
        cool: float = 1.0,
    ) -> None:
        """Particle fountain from the centre.

        Args:
            color:  Peak colour.
            spread: Lateral spread factor (0.1–3.0).
            cool:   Cooling rate — how quickly particles fade to black (0.1–5.0).
        """
        cmd = pb.Command()
        cmd.particles.color.r, cmd.particles.color.g, cmd.particles.color.b = color.r, color.g, color.b
        cmd.particles.spread = spread
        cmd.particles.cool = cool
        self._transact(cmd)

    def effect_waves(
        self,
        color1: Rgb,
        color2: Rgb,
        wavelength: float = 20.0,
        gravity: float = 9.8,
    ) -> None:
        """Sine/cosine wave whose phase origin bounces like a ball.

        Args:
            color1:     Colour at sin-wave peaks.
            color2:     Colour at cos-wave peaks (offset 90° from color1).
            wavelength: Wave period in LEDs (2–100).
            gravity:    Gravitational acceleration that drives the bouncing (1–30).
        """
        cmd = pb.Command()
        cmd.waves.color1.r, cmd.waves.color1.g, cmd.waves.color1.b = color1.r, color1.g, color1.b
        cmd.waves.color2.r, cmd.waves.color2.g, cmd.waves.color2.b = color2.r, color2.g, color2.b
        cmd.waves.wavelength = wavelength
        cmd.waves.gravity = gravity
        self._transact(cmd)

    def effect_spring(
        self,
        color1: Rgb,
        color2: Rgb,
        wavelength: float = 12.0,
        k: float = 8.0,
        damping: float = 0.0,
        equilibrium: float = 0.667,
    ) -> None:
        """Sine wave that compresses and stretches like a coil spring.

        Args:
            color1:      Colour at wave peaks.
            color2:      Colour at wave troughs.
            wavelength:  Initial wavelength in LEDs (determines number of visible cycles).
            k:           Spring constant — higher = faster oscillation.
            damping:     Damping coefficient (0 = no damping, oscillates forever).
            equilibrium: Natural length as a fraction of strip length (default ~0.667).
        """
        cmd = pb.Command()
        cmd.spring.color1.r, cmd.spring.color1.g, cmd.spring.color1.b = color1.r, color1.g, color1.b
        cmd.spring.color2.r, cmd.spring.color2.g, cmd.spring.color2.b = color2.r, color2.g, color2.b
        cmd.spring.wavelength = wavelength
        cmd.spring.k = k
        cmd.spring.damping = damping
        cmd.spring.equilibrium = equilibrium
        self._transact(cmd)

    def effect_plasma(
        self,
        speed: float = 1.0,
        brownian_theta: float = 0.0,
        brownian_sigma: float = 0.0,
    ) -> None:
        """Full-spectrum plasma — HSV rainbow sine-wave interference pattern.

        Speed drifts via an Ornstein-Uhlenbeck process when theta/sigma are non-zero.
        Stationary std-dev of speed ≈ sigma / sqrt(2 * theta).

        Args:
            speed:          Base speed multiplier (1.0 = default). Acts as the OU mean.
            brownian_theta: Mean-reversion rate — how quickly speed returns to base.
                            0 = pure Brownian walk (no pull); higher = tighter leash.
            brownian_sigma: Volatility — amplitude of random speed fluctuations.
                            0 = constant speed.
        """
        cmd = pb.Command()
        cmd.plasma.speed          = speed
        cmd.plasma.brownian_theta = brownian_theta
        cmd.plasma.brownian_sigma = brownian_sigma
        self._transact(cmd)

    # ------------------------------------------------------------------
    # Internal helpers
    # ------------------------------------------------------------------

    def _send_proto(self, cmd: pb.Command) -> None:
        if self._sock is None:
            raise LedControllerError("not connected — call connect() first")
        body = cmd.SerializeToString()
        self._sock.sendall(struct.pack("<I", len(body)) + body)

    def _recv_exact(self, n: int) -> bytes:
        buf = b""
        while len(buf) < n:
            chunk = self._sock.recv(n - len(buf))
            if not chunk:
                raise LedControllerError("connection closed by controller")
            buf += chunk
        return buf

    def _recv_response(self) -> pb.Response:
        length = struct.unpack("<I", self._recv_exact(4))[0]
        body = self._recv_exact(length)
        resp = pb.Response()
        resp.ParseFromString(body)
        return resp

    def _transact(self, cmd: pb.Command) -> None:
        """Send a command and raise LedControllerError on an error response."""
        self._send_proto(cmd)
        resp = self._recv_response()
        if resp.WhichOneof("result") == "error":
            raise LedControllerError(f"controller error: {resp.error}")
