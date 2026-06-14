# Pico W Examples

Three examples showing `coro` running on a Raspberry Pi Pico W:

- **`pico_tcp_echo_server`** — binds to `0.0.0.0:8080`, accepts connections, and echoes back everything it receives. Handles multiple concurrent clients via `JoinSet`.
- **`pico_tcp_echo_client`** — connects to a running echo server, sends 5 messages, and prints the replies.
- **`pico_ws2812_tcp`** — WS2812B LED controller. Accepts TCP connections on port 2812 and drives LEDs with fill/set/clear commands and animated effects. Comes with a PyQt6 desktop GUI (`ws2812_gui.py`).

---

## Prerequisites

### Hardware

- Raspberry Pi Pico W (the **W** variant with CYW43 WiFi chip)
- USB Micro-B cable

### System packages (Ubuntu / Debian)

```bash
sudo apt update
sudo apt install -y \
    cmake \
    gcc-arm-none-eabi \
    libnewlib-arm-none-eabi \
    libstdc++-arm-none-eabi-newlib \
    git \
    python3
```

---

## Step 1 — Download the Pico SDK

Clone the SDK and its submodules into a location of your choice:

```bash
git clone https://github.com/raspberrypi/pico-sdk.git ~/pico-sdk
cd ~/pico-sdk
git submodule update --init
```

Set the environment variables so CMake can find the SDK and target the correct board. Add these to your `~/.bashrc` or `~/.zshrc` to make them permanent:

```bash
export PICO_SDK_PATH=$HOME/pico-sdk
export PICO_BOARD=pico_w
```

Then reload your shell or run `source ~/.bashrc`.

!!! warning "PICO_BOARD must be `pico_w`"
    The default board is `pico` (the original, no WiFi). Without `PICO_BOARD=pico_w` the CYW43 WiFi headers are not included and the build will fail with a missing `pico/cyw43_arch.h` error.

---

## Step 2 — Set your WiFi credentials

Copy the credentials template and fill in your network name and password:

```bash
cd examples/pico
cp wifi_credentials.h.example wifi_credentials.h
$EDITOR wifi_credentials.h   # set WIFI_SSID and WIFI_PASSWORD
```

`wifi_credentials.h` is listed in `.gitignore` so it will never be accidentally committed.

!!! warning "Never pass credentials on the cmake command line"
    Putting `-DWIFI_SSID=...` on the cmake command line bakes the values into
    `CMakeCache.txt` and `compile_commands.json`, where they can leak in build logs
    or editor tooling. The `wifi_credentials.h` approach keeps them out of the build
    system entirely.

---

## Step 3 — Build the examples

```bash
cd examples/pico
mkdir -p build && cd build
cmake ..
make -j$(nproc) pico_tcp_echo_server
```

A successful build produces `pico_tcp_echo_server.uf2` in the build directory.

---

## Step 4 — Flash the firmware

1. Hold the **BOOTSEL** button on the Pico W while plugging it into USB.
2. Release BOOTSEL once it appears as a USB mass storage device (named `RPI-RP2`).
3. Copy the `.uf2` file to it:

```bash
cp build/pico_tcp_echo_server.uf2 /media/$USER/RPI-RP2/
```

The Pico reboots automatically and starts running the firmware.

---

## Step 5 — Monitor serial output

The firmware prints its IP address and status over USB serial. Open a terminal:

```bash
# Find the device — usually /dev/ttyACM0
ls /dev/ttyACM*

# Monitor with screen (Ctrl-A Ctrl-\ to exit)
screen /dev/ttyACM0 115200

# Or with minicom
minicom -b 115200 -D /dev/ttyACM0
```

Expected output once WiFi connects:

```
Connecting to WiFi 'your_network_name'...
WiFi connected. Starting echo server on port 8080...
[0ms] pico_tcp_echo_server.cpp:84 -1 - Binding to 0.0.0.0:8080...
[312ms] pico_tcp_echo_server.cpp:86 -1 - Listening on 0.0.0.0:8080
```

Note the IP address assigned by your router — you need it for the next step. If the firmware does not print an IP, check your WiFi credentials and that the Pico W is in range.

!!! tip "Finding the Pico's IP without serial"
    If you do not have a serial terminal handy, check your router's DHCP client list for a device named `PicoW` or similar.

---

## Step 6 — Test with the host client

Build and run `tcp_echo_client` from the main coro build (it uses the libuv-based executor on Linux — no lwIP needed on the host side):

```bash
# From the coro repo root, inside your regular build directory:
make -j$(nproc) tcp_echo_client

# Connect to the Pico — replace 192.168.1.42 with the actual IP
./tcp_echo_client 192.168.1.42 8080 "hello pico"
```

You will see 10 client connections open, each sending 5 messages with randomised delays, and the Pico serial output logging each echo.

Alternatively, use `netcat` for a quick sanity check:

```bash
echo "hello" | nc 192.168.1.42 8080
```

---

## WS2812B LED controller (`pico_ws2812_tcp`)

### Build and flash

```bash
cd examples/pico/build
make -j$(nproc) pico_ws2812_tcp
cp pico_ws2812_tcp.uf2 /media/$USER/RPI-RP2/
```

### Desktop GUI

Install Python dependencies (once):

```bash
cd examples/pico
pip install -r requirements.txt
```

Launch the GUI (replace the IP with your Pico's address):

```bash
python ws2812_gui.py --host 192.168.1.42 --port 2812
```

The GUI has two tabs:

- **Effects** — pick an effect (Fade, Marquee, Comet, Stars, Bounce, Particles), choose a colour, tune the parameters with the spin-boxes, and click **Apply Effect**. Click **Stop** to return to manual mode.
- **Manual** — set brightness, fill all LEDs with a colour, set individual pixels, or push the current buffer with Show.

### TCP command reference

Commands are newline-terminated text. Each returns `ok` or `err: <reason>`.

```
fill <r> <g> <b>
set  <i> <r> <g> <b>
clear
show
stop
brightness <0-255>
effect fade      <r> <g> <b> <rate>
effect marquee   <speed> <r> <g> <b>
effect comet     <r> <g> <b> <speed> <length> <faderate>
effect stars     <r> <g> <b> <spawnrate> <faderate>
effect bounce    <r> <g> <b> <size> <gravity> <faderate>
effect particles <r> <g> <b> <spread> <cool>
```

Quick test with netcat:

```bash
echo "effect fade 255 0 64 1.5" | nc 192.168.1.42 2812
```

---

## Troubleshooting

| Symptom | Likely cause | Fix |
|---|---|---|
| CMake error: `PICO_SDK_PATH not set` | Environment variable missing | Run `export PICO_SDK_PATH=~/pico-sdk` |
| `fatal error: pico/cyw43_arch.h` | Building for wrong board | Run `export PICO_BOARD=pico_w` and wipe the build dir |
| `arm-none-eabi-gcc: not found` | Toolchain not installed | `sudo apt install gcc-arm-none-eabi` |
| Pico does not appear as mass storage | BOOTSEL not held during plug-in | Unplug, hold BOOTSEL, replug |
| Serial shows `WiFi connect failed` | Wrong SSID / password or 5 GHz network | Pico W supports 2.4 GHz only; fix `wifi_credentials.h` and rebuild |
| Client connects but gets no echo | Firewall on Pico's subnet | Not applicable — the Pico has no firewall; check the server serial log for accepted connections |
