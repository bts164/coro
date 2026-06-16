#!/usr/bin/env python3
"""
ws2812_gui.py — PyQt6 desktop GUI for the pico_ws2812_tcp LED controller.

Install dependencies:
    pip install -r requirements.txt
    python gen_proto.py   # generates proto/ws2812_pb2.py

Usage:
    python ws2812_gui.py [--host 192.168.x.y] [--port 2812]
"""

import struct
import sys
import argparse

from PyQt6.QtWidgets import (
    QApplication, QMainWindow, QWidget, QVBoxLayout, QHBoxLayout,
    QFormLayout, QGroupBox, QTabWidget, QLabel, QLineEdit, QPushButton,
    QSlider, QSpinBox, QComboBox, QTextEdit, QStackedWidget,
    QSizePolicy, QFrame, QCheckBox,
)
from PyQt6.QtCore import Qt, QByteArray, QSettings
from PyQt6.QtGui import QColor, QPalette, QFont
from PyQt6.QtNetwork import QTcpSocket, QAbstractSocket
from PyQt6.QtWidgets import QColorDialog

import proto.ws2812_pb2 as pb


# ---------------------------------------------------------------------------
# Helper widgets
# ---------------------------------------------------------------------------

class ColorButton(QPushButton):
    """Button that displays its current colour and opens a picker on click."""

    def __init__(self, color: QColor = QColor(255, 0, 0), parent=None):
        super().__init__(parent)
        self.setFixedWidth(80)
        self.set_color(color)
        self.clicked.connect(self._pick)

    def set_color(self, color: QColor):
        self._color = color
        self.setStyleSheet(
            f"QPushButton {{ background-color: {color.name()}; "
            f"border: 2px solid #888; border-radius: 4px; }}"
        )

    def color(self) -> QColor:
        return self._color

    def _pick(self):
        c = QColorDialog.getColor(self._color, self, "Pick Colour")
        if c.isValid():
            self.set_color(c)


class FloatSlider(QWidget):
    """Horizontal slider for a float range with an inline value label.

    Provides value() / setValue() so it can be used as a drop-in for
    QDoubleSpinBox in make_command() calls.
    """

    def __init__(self, lo: float, hi: float, default: float,
                 decimals: int = 1, steps: int = 200):
        super().__init__()
        self._lo       = lo
        self._hi       = hi
        self._steps    = steps
        self._decimals = decimals

        row = QHBoxLayout(self)
        row.setContentsMargins(0, 0, 0, 0)
        row.setSpacing(6)

        self._slider = QSlider(Qt.Orientation.Horizontal)
        self._slider.setRange(0, steps)
        row.addWidget(self._slider)

        self._label = QLabel()
        self._label.setMinimumWidth(46)
        self._label.setAlignment(
            Qt.AlignmentFlag.AlignRight | Qt.AlignmentFlag.AlignVCenter)
        row.addWidget(self._label)

        self._slider.valueChanged.connect(lambda _: self._refresh())
        self.setValue(default)

    def _refresh(self):
        self._label.setText(f"{self.value():.{self._decimals}f}")

    def setValue(self, v: float):
        frac = (v - self._lo) / (self._hi - self._lo)
        self._slider.setValue(round(max(0.0, min(1.0, frac)) * self._steps))

    def value(self) -> float:
        return self._lo + (self._slider.value() / self._steps) * (self._hi - self._lo)


class IntSlider(QWidget):
    """Horizontal slider for an integer range with an inline value label."""

    def __init__(self, lo: int, hi: int, default: int):
        super().__init__()
        row = QHBoxLayout(self)
        row.setContentsMargins(0, 0, 0, 0)
        row.setSpacing(6)

        self._slider = QSlider(Qt.Orientation.Horizontal)
        self._slider.setRange(lo, hi)
        row.addWidget(self._slider)

        self._label = QLabel()
        self._label.setMinimumWidth(46)
        self._label.setAlignment(
            Qt.AlignmentFlag.AlignRight | Qt.AlignmentFlag.AlignVCenter)
        row.addWidget(self._label)

        self._slider.valueChanged.connect(lambda v: self._label.setText(str(v)))
        self.setValue(default)

    def setValue(self, v: int):
        self._slider.setValue(v)

    def value(self) -> int:
        return self._slider.value()


def _ispin(lo: int, hi: int, default: int) -> QSpinBox:
    w = QSpinBox()
    w.setRange(lo, hi)
    w.setValue(default)
    return w


# ---------------------------------------------------------------------------
# Per-effect parameter panels
# ---------------------------------------------------------------------------

class FadeParams(QWidget):
    def __init__(self):
        super().__init__()
        form = QFormLayout(self)
        self.color = ColorButton(QColor(0, 100, 255))
        self.rate  = FloatSlider(0.1, 10.0, 1.0)
        form.addRow("Color:", self.color)
        form.addRow("Rate:",  self.rate)

    def make_command(self) -> pb.Command:
        cmd = pb.Command()
        c = self.color.color()
        cmd.fade.color.r, cmd.fade.color.g, cmd.fade.color.b = c.red(), c.green(), c.blue()
        cmd.fade.rate = self.rate.value()
        return cmd


class MarqueeParams(QWidget):
    def __init__(self):
        super().__init__()
        form = QFormLayout(self)
        self.color = ColorButton(QColor(0, 100, 255))
        self.speed = FloatSlider(-10.0, 10.0, 2.0)
        form.addRow("Color:", self.color)
        form.addRow("Speed:", self.speed)
        form.addRow(QLabel("(negative = reverse direction)"))

    def make_command(self) -> pb.Command:
        cmd = pb.Command()
        c = self.color.color()
        cmd.marquee.color.r, cmd.marquee.color.g, cmd.marquee.color.b = c.red(), c.green(), c.blue()
        cmd.marquee.speed = self.speed.value()
        return cmd


class CometParams(QWidget):
    def __init__(self):
        super().__init__()
        form = QFormLayout(self)
        self.head_color = ColorButton(_hsv(50))   # warm yellow tip
        self.tail_color = ColorButton(_hsv(214))  # blue trail (0,100,255)
        self.speed      = FloatSlider(0.1, 50.0, 2.0)
        self.length     = IntSlider(1, 20, 3)
        self.faderate   = FloatSlider(0.1, 10.0, 1.0)
        self.wrap       = QCheckBox()
        form.addRow("Head color:", self.head_color)
        form.addRow("Tail color:", self.tail_color)
        form.addRow("Speed:",      self.speed)
        form.addRow("Length:",     self.length)
        form.addRow("Fade rate:",  self.faderate)
        form.addRow("Wrap:",       self.wrap)

    def make_command(self) -> pb.Command:
        cmd = pb.Command()
        tc = self.tail_color.color()
        cmd.comet.tail_color.r, cmd.comet.tail_color.g, cmd.comet.tail_color.b = tc.red(), tc.green(), tc.blue()
        hc = self.head_color.color()
        cmd.comet.head_color.r, cmd.comet.head_color.g, cmd.comet.head_color.b = hc.red(), hc.green(), hc.blue()
        cmd.comet.speed    = self.speed.value()
        cmd.comet.length   = self.length.value()
        cmd.comet.faderate = self.faderate.value()
        cmd.comet.wrap     = self.wrap.isChecked()
        return cmd


class StarsParams(QWidget):
    def __init__(self):
        super().__init__()
        form = QFormLayout(self)
        self.color     = ColorButton(_hsv(50))   # warm yellow — like real stars
        self.spawnrate = FloatSlider(0.1, 10.0, 1.0)
        self.faderate  = FloatSlider(0.1, 10.0, 1.0)
        form.addRow("Color:",      self.color)
        form.addRow("Spawn rate:", self.spawnrate)
        form.addRow("Fade rate:",  self.faderate)

    def make_command(self) -> pb.Command:
        cmd = pb.Command()
        c = self.color.color()
        cmd.stars.color.r, cmd.stars.color.g, cmd.stars.color.b = c.red(), c.green(), c.blue()
        cmd.stars.spawnrate = self.spawnrate.value()
        cmd.stars.faderate  = self.faderate.value()
        return cmd


class BounceParams(QWidget):
    def __init__(self):
        super().__init__()
        form = QFormLayout(self)
        self.head_color = ColorButton(_hsv(120))  # green ball
        self.tail_color = ColorButton(_hsv(214))  # blue trail (0,100,255)
        self.size       = IntSlider(1, 10, 2)
        self.gravity    = FloatSlider(1.0, 30.0, 9.8)
        self.faderate   = FloatSlider(0.1, 10.0, 1.0)
        form.addRow("Head color:", self.head_color)
        form.addRow("Tail color:", self.tail_color)
        form.addRow("Ball size:",  self.size)
        form.addRow("Gravity:",    self.gravity)
        form.addRow("Fade rate:",  self.faderate)

    def make_command(self) -> pb.Command:
        cmd = pb.Command()
        tc = self.tail_color.color()
        cmd.bounce.tail_color.r, cmd.bounce.tail_color.g, cmd.bounce.tail_color.b = tc.red(), tc.green(), tc.blue()
        hc = self.head_color.color()
        cmd.bounce.head_color.r, cmd.bounce.head_color.g, cmd.bounce.head_color.b = hc.red(), hc.green(), hc.blue()
        cmd.bounce.size     = self.size.value()
        cmd.bounce.gravity  = self.gravity.value()
        cmd.bounce.faderate = self.faderate.value()
        return cmd


class ParticlesParams(QWidget):
    def __init__(self):
        super().__init__()
        form = QFormLayout(self)
        self.color  = ColorButton(QColor(255, 80, 0))
        self.spread = FloatSlider(0.1, 3.0, 0.5, decimals=2)
        self.cool   = FloatSlider(0.1, 5.0, 1.0)
        form.addRow("Color:",         self.color)
        form.addRow("Spread factor:", self.spread)
        form.addRow("Cooling rate:",  self.cool)
        form.addRow(QLabel("(source placed at centre LED)"))

    def make_command(self) -> pb.Command:
        cmd = pb.Command()
        c = self.color.color()
        cmd.particles.color.r, cmd.particles.color.g, cmd.particles.color.b = c.red(), c.green(), c.blue()
        cmd.particles.spread = self.spread.value()
        cmd.particles.cool   = self.cool.value()
        return cmd


class WavesParams(QWidget):
    def __init__(self):
        super().__init__()
        form = QFormLayout(self)
        self.color1     = ColorButton(QColor(0, 100, 255))
        self.color2     = ColorButton(QColor(255, 0, 128))
        self.wavelength = IntSlider(2, 100, 20)
        self.gravity    = FloatSlider(1.0, 30.0, 9.8)
        form.addRow("Color 1:",    self.color1)
        form.addRow("Color 2:",    self.color2)
        form.addRow("Wavelength:", self.wavelength)
        form.addRow("Gravity:",    self.gravity)

    def make_command(self) -> pb.Command:
        cmd = pb.Command()
        c1 = self.color1.color()
        cmd.waves.color1.r, cmd.waves.color1.g, cmd.waves.color1.b = c1.red(), c1.green(), c1.blue()
        c2 = self.color2.color()
        cmd.waves.color2.r, cmd.waves.color2.g, cmd.waves.color2.b = c2.red(), c2.green(), c2.blue()
        cmd.waves.wavelength = self.wavelength.value()
        cmd.waves.gravity    = self.gravity.value()
        return cmd


class PlasmaParams(QWidget):
    def __init__(self):
        super().__init__()
        form = QFormLayout(self)
        self.speed   = FloatSlider(0.1, 5.0, 1.0)
        self.theta   = FloatSlider(0.0, 5.0, 0.0, decimals=2)
        self.sigma   = FloatSlider(0.0, 3.0, 0.0, decimals=2)
        form.addRow("Speed:",            self.speed)
        form.addRow("Brownian θ (pull):", self.theta)
        form.addRow("Brownian σ (noise):", self.sigma)

    def make_command(self) -> pb.Command:
        cmd = pb.Command()
        cmd.plasma.speed          = self.speed.value()
        cmd.plasma.brownian_theta = self.theta.value()
        cmd.plasma.brownian_sigma = self.sigma.value()
        return cmd


class SpringParams(QWidget):
    def __init__(self):
        super().__init__()
        form = QFormLayout(self)
        self.color1      = ColorButton(QColor(0, 100, 255))
        self.color2      = ColorButton(QColor(0, 200, 255))
        self.wavelength  = IntSlider(2, 60, 12)
        self.k           = FloatSlider(0.5, 50.0, 8.0)
        self.damping     = FloatSlider(0.0, 10.0, 0.0)
        self.equilibrium = FloatSlider(0.1, 1.0, 0.667, decimals=3, steps=900)
        form.addRow("Color 1:",     self.color1)
        form.addRow("Color 2:",     self.color2)
        form.addRow("Wavelength:",  self.wavelength)
        form.addRow("Spring k:",    self.k)
        form.addRow("Damping:",     self.damping)
        form.addRow("Equilibrium:", self.equilibrium)

    def make_command(self) -> pb.Command:
        cmd = pb.Command()
        c1 = self.color1.color()
        cmd.spring.color1.r, cmd.spring.color1.g, cmd.spring.color1.b = c1.red(), c1.green(), c1.blue()
        c2 = self.color2.color()
        cmd.spring.color2.r, cmd.spring.color2.g, cmd.spring.color2.b = c2.red(), c2.green(), c2.blue()
        cmd.spring.wavelength  = self.wavelength.value()
        cmd.spring.k           = self.k.value()
        cmd.spring.damping     = self.damping.value()
        cmd.spring.equilibrium = self.equilibrium.value()
        return cmd


# ---------------------------------------------------------------------------
# Colour palette — 16 slots loaded into QColorDialog's custom-colour row.
# User edits persist via QSettings; these are the factory defaults.
# ---------------------------------------------------------------------------

def _hsv(h: int, s: int = 255, v: int = 255) -> QColor:
    """Convenience: build a QColor from HSV with h in [0, 359]."""
    return QColor.fromHsv(h, s, v)


# 16 landmark hues at full saturation — used as the factory custom-colour row.
_PALETTE_DEFAULTS = [
    _hsv(  0),   # red
    _hsv( 20),   # orange-red
    _hsv( 35),   # orange
    _hsv( 60),   # yellow
    _hsv( 90),   # chartreuse
    _hsv(120),   # green
    _hsv(150),   # spring green
    _hsv(180),   # cyan
    _hsv(200),   # deep sky blue
    _hsv(225),   # azure
    _hsv(240),   # blue
    _hsv(270),   # violet
    _hsv(300),   # magenta
    _hsv(330),   # rose
    _hsv(  0, s=0, v=255),  # white  (the only non-saturated slot)
    _hsv(  0, s=0, v=180),  # light grey
]

# ---------------------------------------------------------------------------
# Main window
# ---------------------------------------------------------------------------

EFFECTS = ["Fade", "Marquee", "Comet", "Stars", "Bounce", "Particles", "Waves", "Spring", "Plasma"]


class MainWindow(QMainWindow):
    def __init__(self, host: str, port: int):
        super().__init__()
        self.setWindowTitle("WS2812 LED Controller")
        self.resize(520, 640)

        self._querying_leds = False
        self._rbuf = b""
        self._socket = QTcpSocket(self)
        self._socket.connected.connect(self._on_connected)
        self._socket.disconnected.connect(self._on_disconnected)
        self._socket.errorOccurred.connect(self._on_error)
        self._socket.readyRead.connect(self._on_ready_read)

        root = QWidget()
        self.setCentralWidget(root)
        layout = QVBoxLayout(root)
        layout.setSpacing(6)

        layout.addWidget(self._make_connection_bar(host, port))
        layout.addWidget(self._make_tabs())
        layout.addWidget(self._make_log())

        #self._load_palette()

    # -----------------------------------------------------------------------
    # Colour palette persistence
    # -----------------------------------------------------------------------

    def _load_palette(self):
        # Replace the 48 built-in standard-color swatches with a full-saturation
        # rainbow so that the most prominent part of the picker shows LED-ready hues.
        for i in range(48):
            h = round(i * 360 / 48) % 360
            QColorDialog.setStandardColor(i, QColor.fromHsv(h, 255, 255))

        settings = QSettings("ws2812", "led_controller")
        for i, default in enumerate(_PALETTE_DEFAULTS):
            saved = settings.value(f"palette/{i}")
            QColorDialog.setCustomColor(i, QColor(saved) if saved else default)

    def _save_palette(self):
        settings = QSettings("ws2812", "led_controller")
        for i in range(len(_PALETTE_DEFAULTS)):
            settings.setValue(f"palette/{i}", QColorDialog.customColor(i).name())

    def closeEvent(self, event):
        self._save_palette()
        super().closeEvent(event)

    # -----------------------------------------------------------------------
    # Connection bar
    # -----------------------------------------------------------------------

    def _make_connection_bar(self, host: str, port: int) -> QWidget:
        bar = QGroupBox("Connection")
        h = QHBoxLayout(bar)

        h.addWidget(QLabel("Host:"))
        self._host_edit = QLineEdit(host)
        self._host_edit.setFixedWidth(140)
        h.addWidget(self._host_edit)

        h.addWidget(QLabel("Port:"))
        self._port_edit = QLineEdit(str(port))
        self._port_edit.setFixedWidth(60)
        h.addWidget(self._port_edit)

        self._connect_btn = QPushButton("Connect")
        self._connect_btn.clicked.connect(self._toggle_connection)
        h.addWidget(self._connect_btn)

        self._status_label = QLabel("Disconnected")
        self._status_label.setStyleSheet("color: #c00; font-weight: bold;")
        h.addWidget(self._status_label)
        h.addStretch()
        return bar

    # -----------------------------------------------------------------------
    # Tabs
    # -----------------------------------------------------------------------

    def _make_tabs(self) -> QTabWidget:
        tabs = QTabWidget()
        tabs.addTab(self._make_effects_tab(), "Effects")
        tabs.addTab(self._make_manual_tab(),  "Manual")
        return tabs

    def _make_effects_tab(self) -> QWidget:
        w = QWidget()
        v = QVBoxLayout(w)

        # Effect selector
        h = QHBoxLayout()
        h.addWidget(QLabel("Effect:"))
        self._effect_combo = QComboBox()
        self._effect_combo.addItems(EFFECTS)
        self._effect_combo.currentIndexChanged.connect(self._on_effect_changed)
        h.addWidget(self._effect_combo)
        h.addStretch()
        v.addLayout(h)

        # Per-effect parameter panels (stacked)
        self._effect_panels = {
            "Fade":      FadeParams(),
            "Marquee":   MarqueeParams(),
            "Comet":     CometParams(),
            "Stars":     StarsParams(),
            "Bounce":    BounceParams(),
            "Particles": ParticlesParams(),
            "Waves":     WavesParams(),
            "Spring":    SpringParams(),
            "Plasma":    PlasmaParams(),
        }
        self._params_stack = QStackedWidget()
        for name in EFFECTS:
            self._params_stack.addWidget(self._effect_panels[name])
        v.addWidget(self._params_stack)

        # Apply / Stop buttons
        btn_row = QHBoxLayout()
        btn_row.addStretch()
        apply_btn = QPushButton("▶  Apply Effect")
        apply_btn.setFixedWidth(140)
        apply_btn.clicked.connect(self._apply_effect)
        btn_row.addWidget(apply_btn)
        stop_btn = QPushButton("■  Stop")
        stop_btn.setFixedWidth(90)
        stop_btn.clicked.connect(self._do_stop)
        btn_row.addWidget(stop_btn)
        v.addLayout(btn_row)
        v.addStretch()
        return w

    def _make_manual_tab(self) -> QWidget:
        w = QWidget()
        v = QVBoxLayout(w)

        # LED strip size
        strip_box = QGroupBox("LED Strip")
        sh = QHBoxLayout(strip_box)
        sh.addWidget(QLabel("LED count:"))
        self._led_count = _ispin(1, 300, 8)
        self._led_count.setFixedWidth(70)
        sh.addWidget(self._led_count)
        set_count_btn = QPushButton("Set")
        set_count_btn.setFixedWidth(50)
        set_count_btn.clicked.connect(self._do_set_leds)
        sh.addWidget(set_count_btn)
        sh.addStretch()
        v.addWidget(strip_box)

        # Brightness
        bright_box = QGroupBox("Brightness")
        bh = QHBoxLayout(bright_box)
        self._brightness_slider = QSlider(Qt.Orientation.Horizontal)
        self._brightness_slider.setRange(0, 255)
        self._brightness_slider.setValue(255)
        self._brightness_label = QLabel("255")
        self._brightness_slider.valueChanged.connect(
            lambda val: self._brightness_label.setText(str(val)))
        bh.addWidget(self._brightness_slider)
        bh.addWidget(self._brightness_label)
        set_btn = QPushButton("Set")
        set_btn.setFixedWidth(50)
        set_btn.clicked.connect(self._do_brightness)
        bh.addWidget(set_btn)
        v.addWidget(bright_box)

        # Fill
        fill_box = QGroupBox("Fill All LEDs")
        fh = QHBoxLayout(fill_box)
        self._fill_color = ColorButton(QColor(255, 64, 0))
        fh.addWidget(QLabel("Colour:"))
        fh.addWidget(self._fill_color)
        fill_btn = QPushButton("Fill")
        fill_btn.clicked.connect(self._do_fill)
        fh.addWidget(fill_btn)
        clear_btn = QPushButton("Clear")
        clear_btn.clicked.connect(self._do_clear)
        fh.addWidget(clear_btn)
        fh.addStretch()
        v.addWidget(fill_box)

        # Set individual LED
        set_box = QGroupBox("Set Individual LED")
        sh2 = QHBoxLayout(set_box)
        sh2.addWidget(QLabel("Index:"))
        self._led_index = _ispin(0, 255, 0)
        self._led_index.setFixedWidth(60)
        sh2.addWidget(self._led_index)
        sh2.addWidget(QLabel("Colour:"))
        self._set_color = ColorButton(QColor(0, 255, 0))
        sh2.addWidget(self._set_color)
        set_led_btn = QPushButton("Set")
        set_led_btn.clicked.connect(self._do_set)
        sh2.addWidget(set_led_btn)
        sh2.addStretch()
        v.addWidget(set_box)

        show_btn = QPushButton("Show (push buffer)")
        show_btn.clicked.connect(self._do_show)
        v.addWidget(show_btn)
        v.addStretch()
        return w

    def _make_log(self) -> QWidget:
        box = QGroupBox("Log")
        v = QVBoxLayout(box)
        self._log = QTextEdit()
        self._log.setReadOnly(True)
        self._log.setFont(QFont("Monospace", 9))
        self._log.setMaximumHeight(160)
        v.addWidget(self._log)
        clear_btn = QPushButton("Clear log")
        clear_btn.setFixedWidth(80)
        clear_btn.clicked.connect(self._log.clear)
        h = QHBoxLayout()
        h.addStretch()
        h.addWidget(clear_btn)
        v.addLayout(h)
        return box

    # -----------------------------------------------------------------------
    # Effect tab helpers
    # -----------------------------------------------------------------------

    def _on_effect_changed(self, index: int):
        self._params_stack.setCurrentIndex(index)

    def _apply_effect(self):
        name = self._effect_combo.currentText()
        panel = self._effect_panels[name]
        self._send_cmd(panel.make_command())

    # -----------------------------------------------------------------------
    # Manual tab command helpers
    # -----------------------------------------------------------------------

    def _do_fill(self):
        c = self._fill_color.color()
        cmd = pb.Command()
        cmd.fill.r, cmd.fill.g, cmd.fill.b = c.red(), c.green(), c.blue()
        self._send_cmd(cmd)

    def _do_clear(self):
        cmd = pb.Command()
        cmd.clear.CopyFrom(pb.Empty())
        self._send_cmd(cmd)

    def _do_set(self):
        i = self._led_index.value()
        c = self._set_color.color()
        cmd = pb.Command()
        cmd.set.index = i
        cmd.set.color.r, cmd.set.color.g, cmd.set.color.b = c.red(), c.green(), c.blue()
        self._send_cmd(cmd)

    def _do_show(self):
        cmd = pb.Command()
        cmd.show.CopyFrom(pb.Empty())
        self._send_cmd(cmd)

    def _do_stop(self):
        cmd = pb.Command()
        cmd.stop.CopyFrom(pb.Empty())
        self._send_cmd(cmd)

    def _do_set_leds(self):
        cmd = pb.Command()
        cmd.set_leds = self._led_count.value()
        self._send_cmd(cmd)

    def _do_brightness(self):
        cmd = pb.Command()
        cmd.brightness = self._brightness_slider.value()
        self._send_cmd(cmd)

    # -----------------------------------------------------------------------
    # Connection management
    # -----------------------------------------------------------------------

    def _toggle_connection(self):
        state = self._socket.state()
        if state == QAbstractSocket.SocketState.UnconnectedState:
            host = self._host_edit.text().strip()
            try:
                port = int(self._port_edit.text())
            except ValueError:
                self._log_line("err: invalid port")
                return
            self._status_label.setText("Connecting…")
            self._status_label.setStyleSheet("color: #a80; font-weight: bold;")
            self._connect_btn.setEnabled(False)
            self._socket.connectToHost(host, port)
        else:
            self._socket.disconnectFromHost()

    def _on_connected(self):
        self._status_label.setText("Connected")
        self._status_label.setStyleSheet("color: #080; font-weight: bold;")
        self._connect_btn.setText("Disconnect")
        self._connect_btn.setEnabled(True)
        self._rbuf = b""
        self._log_line(f"-- connected to {self._socket.peerAddress().toString()}:{self._socket.peerPort()}")
        # Query the current LED count from the controller.
        self._querying_leds = True
        cmd = pb.Command()
        cmd.get_leds.CopyFrom(pb.Empty())
        self._send_cmd(cmd, log=False)

    def _on_disconnected(self):
        self._querying_leds = False
        self._rbuf = b""
        self._status_label.setText("Disconnected")
        self._status_label.setStyleSheet("color: #c00; font-weight: bold;")
        self._connect_btn.setText("Connect")
        self._connect_btn.setEnabled(True)
        self._log_line("-- disconnected")

    def _on_error(self, error):
        self._log_line(f"err: {self._socket.errorString()}")
        self._connect_btn.setEnabled(True)
        self._status_label.setText("Error")
        self._status_label.setStyleSheet("color: #c00; font-weight: bold;")

    def _on_ready_read(self):
        self._rbuf += bytes(self._socket.readAll())
        while len(self._rbuf) >= 4:
            length = struct.unpack_from("<I", self._rbuf)[0]
            if len(self._rbuf) < 4 + length:
                break
            body = self._rbuf[4:4 + length]
            self._rbuf = self._rbuf[4 + length:]
            resp = pb.Response()
            resp.ParseFromString(body)
            self._handle_response(resp)

    def _handle_response(self, resp: pb.Response):
        which = resp.WhichOneof("result")
        if which == "led_count":
            if self._querying_leds:
                self._led_count.setValue(resp.led_count)
                self._querying_leds = False
            else:
                self._log_line(f"< leds {resp.led_count}")
        elif which == "error":
            self._log_line(f"< err: {resp.error}")
        else:
            self._log_line("< ok")

    # -----------------------------------------------------------------------
    # Sending
    # -----------------------------------------------------------------------

    def _send_cmd(self, cmd: pb.Command, *, log: bool = True):
        if self._socket.state() != QAbstractSocket.SocketState.ConnectedState:
            self._log_line("err: not connected")
            return
        body = cmd.SerializeToString()
        frame = struct.pack("<I", len(body)) + body
        self._socket.write(QByteArray(frame))
        if log:
            self._log_line(f"> {cmd.WhichOneof('cmd')}")

    def _log_line(self, text: str):
        self._log.append(text)
        sb = self._log.verticalScrollBar()
        sb.setValue(sb.maximum())


# ---------------------------------------------------------------------------
# Entry point
# ---------------------------------------------------------------------------

def main():
    parser = argparse.ArgumentParser(description="WS2812 LED GUI")
    parser.add_argument("--host", default="pico-leds.local")
    parser.add_argument("--port", type=int, default=2812)
    args = parser.parse_args()

    app = QApplication(sys.argv)
    app.setApplicationName("WS2812 LED Controller")

    win = MainWindow(args.host, args.port)
    win.show()
    sys.exit(app.exec())


if __name__ == "__main__":
    main()
