# Tests

The unit tests for coro, written with GoogleTest. This folder is its own Conan project:
it consumes the coro package like any other user of the library, and builds either test
programs for the development machine or firmware images that run the same tests on a
Raspberry Pi Pico.

- [How the tests are organised](#how-the-tests-are-organised)
- [Running on the development machine](#running-on-the-development-machine)
- [Running on a Pico](#running-on-a-pico)
- [Adding or moving a test](#adding-or-moving-a-test)
- [Troubleshooting](#troubleshooting)

## How the tests are organised

Every test file is named once, in [tests.cmake](tests.cmake). Its line says which builds
the file belongs to:

```cmake
coro_test(sync/test_oneshot.cpp          DESKTOP PICO_STUB ON_TARGET sync)
coro_test(runtime/test_io_driver.cpp     DESKTOP)
coro_test(io/test_tcp_stream.cpp         DESKTOP HOST_LINK coro_lwip_tcp ON_TARGET net)
```

| Keyword | What it builds | Runs on |
|---|---|---|
| `DESKTOP` | One program per file, linked with the desktop library. Typed tests run against all three executors | Development machine |
| `PICO_STUB` | `test_pico_suite`: one program holding all such files, built from the Pico configuration of the library (`CORO_PICO`, current-thread executor only) with the Pico SDK calls stubbed out | Development machine |
| `HOST_LINK <lib>...` | One program per file, linked with the named libraries instead of the desktop library. For Pico code tested against stubbed hardware or lwIP built for the host. Named `<file>_pico` when the file is also `DESKTOP` | Development machine |
| `ON_TARGET <image>` | The firmware image `coro_target_<image>`. Files naming the same image share it | The board, or an [emulator](#running-in-the-emulator) |

A file usually has several keywords: most of the `sync/` tests run on the desktop, in the
stub suite, and on the board. The socket tests run against the desktop sockets
(`test_tcp_stream`), against lwIP built for the host (`test_tcp_stream_pico`), and on the
board.

Which tests *inside* a file apply to a platform is decided in the source, with the
preprocessor. Today that is `#ifndef CORO_PICO` around the desktop-only tests, and in
the socket tests `CORO_TCP_BACKEND_LWIP` / `CORO_UDP_BACKEND_LWIP`.

### Layout

| Path | Contents |
|---|---|
| `tests.cmake` | The list of test files |
| `CMakeLists.txt` | The one build; `CORO_PLATFORM` (set by the recipe) selects desktop or Pico |
| `conanfile.py` | The recipe. A desktop profile builds the host programs, a bare-metal profile the firmware |
| `cmake/coro_test.cmake` | `coro_test()`, the function `tests.cmake` calls |
| `cmake/host_pico_support.cmake` | Libraries behind `PICO_STUB` and `HOST_LINK`: the stubbed Pico core and HAL, lwIP for the host |
| `cmake/on_target_pico.cmake` | GoogleTest for the board, and the firmware images |
| `detail/`, `runtime/`, `task/`, `sync/`, `io/`, `test_*.cpp` | Tests, in folders that mirror `include/coro/` |
| `pico/` | Tests of Pico-only code, the SDK stubs (`pico/stub/`), and the host lwIP configuration (`pico/lwip_host/`) |
| `stress/` | `test_skynet` and its benchmark script |
| `target/pico/main.cpp` | The program that runs the tests on the board |
| `target/run_on_target.py` | Flashes the board, starts the tests, and reports the result. With `--emulator`, runs the images in the emulator instead |
| `target/emulator/` | The RP2040 emulator's launcher and its pinned npm dependency |
| `executor_traits.h` | The executor list that typed tests are instantiated over, and `SingleThreadRuntime` for the tests that are not typed |
| `isr_trigger.h` | `IsrTrigger`, which calls a function from a timer interrupt on the board and from a thread on the host |
| `net_runtime.h` | `NetRuntime`, the runtime for tests that use sockets |
| `libunicorn/` | Conan recipe for the ARM emulator used by `test_switch_context_pico` |

## Running on the development machine

Needs `arm-none-eabi-gcc` on the `PATH` (`sudo apt-get install gcc-arm-none-eabi`): one
test cross-assembles the Pico context-switch code and runs it in an emulator.

```bash
# From the repository root: put coro and the emulator recipe in the Conan cache.
conan export .
conan export test/libunicorn

cd test
conan build . --build=missing
cd build/Release && ctest
```

One test program, or one test in it:

```bash
ctest -R test_oneshot
./test_oneshot --gtest_filter='OneshotTest/*.SenderIsMovable'
```

For a sanitizer build, set `CORO_SANITIZE=asan`, `tsan` or `ubsan` in the environment
before those commands. It applies to both the library and the tests; see
[BUILD.md](../BUILD.md).

## Running on a Pico

The same recipe builds firmware when the host profile is a bare-metal one
(`os=baremetal`). [examples/pico/profiles](../examples/pico/profiles) has an example
profile; `rp2040_pico_w` below stands for yours.

### One-time setup

- The Pico SDK, with `PICO_SDK_PATH` set in the profile's `[buildenv]`.
- `picotool` on the `PATH`.
- pyserial for the Python that runs the script: `pip install pyserial`. The package
  named `serial` is a different library; see [Troubleshooting](#troubleshooting).
- Permission to open the board's USB device, on Linux through picotool's udev rules:

    ```bash
    sudo cp <picotool source>/udev/60-picotool.rules /etc/udev/rules.d/
    sudo udevadm control --reload
    ```

    Then unplug the board and plug it back in. Your user must be in the `plugdev` group.

### Build and run

```bash
# From the repository root.
conan export .

cd test
conan build . -pr:h rp2040_pico_w -pr:b default --build=missing

# Flash and run every image in turn.
./target/run_on_target.py build/baremetal/Release/coro_target_*.uf2
```

`--build=missing` builds coro itself for the board the first time. The firmware goes to
`build/baremetal/Release`, apart from the desktop build in `build/Release`.

The board does not need to be in BOOTSEL mode. The script asks picotool to restart a
running board into the bootloader, load the image and start it. It then runs the tests
over the USB serial port, prints their output, and moves on to the next image.

```bash
./target/run_on_target.py build/baremetal/Release/coro_target_sync.uf2   # one image
./target/run_on_target.py --filter 'OneshotTest/*'    # what is on the board, no flashing
./target/run_on_target.py --port /dev/ttyACM1 ...     # more than one Pico connected
```

Exit status: 0 if every test passed, 1 if a test failed, 2 if a run did not finish
because the board hung, crashed or never answered.

A plain Pico is enough for everything in the firmware today. The networking tests use
lwIP's loopback interface and never start the radio.

### Running in the emulator

The same images run in an emulated RP2040 ([rp2040js](https://github.com/wokwi/rp2040js)),
with no board attached:

```bash
./target/run_on_target.py --emulator build/baremetal/Release/coro_target_*.uf2
```

It needs Node.js 18 or later (`node` and `npm` on the `PATH`), and neither picotool nor
pyserial. The first run installs the emulator into `target/emulator/node_modules` and
downloads the RP2040 boot ROM from Raspberry Pi's
[pico-bootrom-rp2040](https://github.com/raspberrypi/pico-bootrom-rp2040) releases into
`target/emulator/.cache`, so it needs network access once. With `--work-dir <dir>` both go
in `<dir>` instead, for a source tree that cannot be written to; the emulator's runner
is copied there too, and that copy is the one that runs. `--filter`, the timeouts and the exit
status are the same as on a board. An image takes about a second.

!!! danger "WARNING: the emulator does not validate the Pico build"
    It is a quick check that catches the easier bugs on a machine with no board, such as
    a CI runner: the firmware no longer boots, a test fails on a 32-bit ARM core, the
    image runs out of heap. Timing differs from the board, and a peripheral behaves only
    as well as the emulator models it. A pass here says the board is worth testing, not
    that it works. Run the suite on the hardware before a release.

### Memory checks in the firmware

ASan and TSan do not exist for a microcontroller: use them on the desktop build, where
`test_pico_suite` runs the Pico configuration of the library under both. The firmware has
two checks of its own:

- **A stack guard, always on.** A write below the main stack is a hard fault instead of
  silent corruption. Only the board enforces it; the emulator has no memory protection
  unit. Fiber stacks are not covered.
- **UBSan, on request.** Set `CORO_SANITIZE=ubsan` (or pass `-o with_sanitize=ubsan`)
  when building. The library and the tests are then compiled with UBSan in trap mode:
  undefined behaviour stops the firmware where it happens. It works on the board and in
  the emulator.

  ```bash
  CORO_SANITIZE=ubsan conan build . -pr:h rp2040_pico_w -pr:b default --build=missing
  ./target/run_on_target.py --emulator build/baremetal/Release/coro_target_*.uf2
  ```

Either check ends a run the same way: the script reports the test that was running and
exits with status 2. There is no message from the firmware itself. The emulator prints
the program counter, which `arm-none-eabi-addr2line -e coro_target_<image>.elf <pc>`
turns into a source line.

### Why several images

One image cannot hold the whole suite on an RP2040: each registered test costs about
340 bytes of RAM before any test runs, which limits an image to roughly 325 tests. The
name after `ON_TARGET` chooses the image, and a new name creates a new image. The
measurements and the design are in
[doc/design/on_target_tests.md](../doc/design/on_target_tests.md).

## Adding or moving a test

- **A new test file:** add one `coro_test()` line to [tests.cmake](tests.cmake).
- **Run an existing file on the board:** add `ON_TARGET <image>` to its line. Guard any
  test in it that needs threads, a multi-threaded executor, a filesystem or process
  exit with `#ifndef CORO_PICO`. For the runtime use `CurrentThreadTraits` (typed
  tests) or `SingleThreadRuntime` from `executor_traits.h`, or `NetRuntime` from
  `net_runtime.h` if the test uses sockets: on the board the default runtime polls a
  Wi-Fi driver that the tests never initialise. For a call from an interrupt handler
  use `IsrTrigger` from `isr_trigger.h`, not a thread.
- **Run an existing file in the stub suite:** add `PICO_STUB`. It needs the same guards.
- **Either of those puts the file in one program with others.** Put its helper types
  and functions in an anonymous namespace, or two files that both define a
  `MockWaker` break the one-definition rule without a diagnostic. A test suite name
  shared by two files must be used with the same macro (`TEST`, `TEST_F` or
  `TYPED_TEST`) in both, and with distinct test names.
- **A new image:** use a new name after `ON_TARGET`. Nothing else is needed.

After the last test of a run, `run_on_target.py` prints how much heap the image used.
If the peak is close to the total, move files to another image.

## Troubleshooting

| Symptom | Cause and fix |
|---|---|
| `ModuleNotFoundError: No module named 'serial.tools'` | The Python environment has the package `serial`, not `pyserial`; both import as `serial`. `pip uninstall serial`, then `pip install pyserial` |
| picotool: `appears to have a USB serial connection, but picotool was unable to connect` | The udev rules cover the board only in BOOTSEL mode (product ID `0003`), not while it runs firmware (`000a`). Install picotool's current `60-picotool.rules` and remove any older rule file, as in [One-time setup](#one-time-setup) |
| picotool: `No accessible RP-series devices in BOOTSEL mode were found`, with nothing else | The firmware on the board has no USB serial port, or has crashed. Hold BOOTSEL while plugging the board in, once; the test firmware it then loads can be restarted from the host |
| `error: no serial port found` | No Pico, or more than one, is connected. Pass `--port` |
| `*** PANIC ***  Out of memory`, then `the device stopped during: <test>` | The image ran out of heap. The script prints how much was free when that test started. Move files to another image, or make the test allocate less |
| `the device never announced CORO_TEST_READY` | The firmware did not reach `main()`, or another program has the serial port open |
| `--emulator`: `run_rp2040: the firmware stopped at a breakpoint` | The firmware panicked or failed an assert. The panic message is the last line of output before it |
| `--emulator`: `run_rp2040: [<component>] ...`, then `the emulator exited with status 3` | The firmware did something the emulator does not model or rejects, such as reading an unmapped address. Run `node target/emulator/run_rp2040.mjs --verbose --bootrom target/emulator/.cache/b1.elf <image>.uf2` to see the emulator's warnings as well, and check the same image on a board |
| `the device stopped during: <test>` with no panic message, or in the emulator `stopped at a breakpoint or trap` | A hard fault: a stack overflow caught by the stack guard, or undefined behaviour in a `with_sanitize=ubsan` build. See [Memory checks in the firmware](#memory-checks-in-the-firmware) |
| `--emulator`: `no output for 30s` | The emulated firmware hung. An image normally finishes in about a second, so a slow machine is an unlikely cause; `--idle-timeout` rules it out |
