# Pico TCP Echo Examples

Two examples showing `coro` running on a Raspberry Pi Pico W:

- **`pico_tcp_echo_server`** — binds to `0.0.0.0:8080`, accepts connections, and echoes back everything it receives. Handles multiple concurrent clients via `JoinSet`.
- **`pico_tcp_echo_client`** — connects to a running echo server, sends 5 messages, and prints the replies.

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

## Step 2 — Build the examples

From the `examples/pico` directory:

```bash
cd examples/pico
mkdir -p build && cd build

cmake .. \
    -DWIFI_SSID="your_network_name" \
    -DWIFI_PASSWORD="your_network_password"

make -j$(nproc) pico_tcp_echo_server
```

A successful build produces `pico_tcp_echo_server.uf2` in the build directory.

---

## Step 3 — Flash the firmware

1. Hold the **BOOTSEL** button on the Pico W while plugging it into USB.
2. Release BOOTSEL once it appears as a USB mass storage device (named `RPI-RP2`).
3. Copy the `.uf2` file to it:

```bash
cp build/pico_tcp_echo_server.uf2 /media/$USER/RPI-RP2/
```

The Pico reboots automatically and starts running the firmware.

---

## Step 4 — Monitor serial output

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

## Step 5 — Test with the host client

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

## Troubleshooting

| Symptom | Likely cause | Fix |
|---|---|---|
| CMake error: `PICO_SDK_PATH not set` | Environment variable missing | Run `export PICO_SDK_PATH=~/pico-sdk` |
| `fatal error: pico/cyw43_arch.h` | Building for wrong board | Run `export PICO_BOARD=pico_w` and wipe the build dir |
| `arm-none-eabi-gcc: not found` | Toolchain not installed | `sudo apt install gcc-arm-none-eabi` |
| Pico does not appear as mass storage | BOOTSEL not held during plug-in | Unplug, hold BOOTSEL, replug |
| Serial shows `WiFi connect failed` | Wrong SSID / password or 5 GHz network | Pico W supports 2.4 GHz only; rerun cmake with correct credentials |
| Client connects but gets no echo | Firewall on Pico's subnet | Not applicable — the Pico has no firewall; check the server serial log for accepted connections |
