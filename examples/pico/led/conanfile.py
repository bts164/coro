from conan import ConanFile
from conan.tools.cmake import cmake_layout, CMake, CMakeDeps, CMakeToolchain


class PicoLedRecipe(ConanFile):
    """Conan package for the WS2812 LED driver layer used by KitchenLED.

    Experimental — see doc/design/pico_led_conan_packaging.md. Validates
    whether Conan can coexist with the Pico SDK's own toolchain setup
    (pico_sdk_init()), which otherwise fights CMakeToolchain over who owns
    compiler/flag selection. Resolution: CMakeToolchain is still used (so
    CMakePresets.json still gets generated and the standard `CMake` build
    helper still works), but it is pared down via the blocks API to only
    the "find_paths" block (CMAKE_PREFIX_PATH/CMAKE_MODULE_PATH, needed for
    find_package()) — every block that would set CMAKE_SYSTEM_NAME or
    CMAKE_<LANG>_COMPILER is dropped, leaving pico_sdk_import.cmake/
    pico_sdk_init() as the sole owner of the toolchain.

    Depends on coro/0.1.0 built for settings.os == "baremetal" (see the
    root conanfile.py) — coro::pico / coro::pico_hal components, found via
    CMakeDeps + the same find_paths block, rather than compiling
    coro_pico/coro_pico_hal from source against a CORO_ROOT path. This is
    what makes the package relocatable: it no longer reaches outside its
    own exported sources into the live monorepo checkout.

    Scope: color.h/.cpp, led_driver.h/.cpp, pixel_buffer.h, ws2812.pio only
    — not effects.h/.cpp or effect_runner.h/.cpp (those depend on
    ws2812_proto/nanopb, unneeded by KitchenLED). The existing
    examples/pico/led/CMakeLists.txt (coro_pico_led, used by
    pico_ws2812_tcp) is untouched; this recipe builds its own narrower
    target via conan_build/CMakeLists.txt.
    """

    name = "pico_led"
    version = "0.1.0"
    package_type = "static-library"

    settings = "os", "compiler", "build_type", "arch"

    exports_sources = (
        "color.h", "color.cpp",
        "led_driver.h", "led_driver.cpp",
        "pixel_buffer.h",
        "ws2812.pio",
        "conan_build/CMakeLists.txt",
    )

    def requirements(self):
        self.requires("coro/0.1.0",
            transitive_headers=True,
            transitive_libs=True)

    def layout(self):
        cmake_layout(self)
        # Used only when this package is consumed in --editable mode (no
        # package() copy happens then) — package_info() below is what
        # applies after a real `conan create`. "led" headers/lib live one
        # level up from source_folder's perspective because source_folder
        # *is* the `led/` directory itself (see exports_sources: there's no
        # extra `led/` wrapper inside the export, since the recipe already
        # lives inside `led/`), but consumers #include "led/led_driver.h"
        # (matching the packaged include/led/ layout installed by
        # conan_build/CMakeLists.txt) — so the editable includedir must be
        # the parent of source_folder, not source_folder itself.
        self.cpp.source.includedirs = [".."]
        self.cpp.build.libdirs = ["."]

    def generate(self):
        # Deliberately NOT calling tc.blocks.enabled(...) here: which blocks
        # are active is controlled by the *profile's*
        # tools.cmake.cmaketoolchain:enabled_blocks conf (see
        # doc/design/pico_led_conan_packaging.md and
        # examples/pico/profiles/rp2040-arm-none-eabi.profile.example) —
        # Conan re-applies that conf inside tc.generate() itself, so calling
        # blocks.enabled() here too double-filters and throws ("Block 'X'
        # ... doesn't exist") once the conf's list includes anything this
        # call already removed. The conf must list "find_paths" (writes
        # CMAKE_PREFIX_PATH/CMAKE_MODULE_PATH for find_package()) and must
        # NOT list "generic_system" or "compilers" (those would fight
        # pico_sdk_init() over CMAKE_SYSTEM_NAME/CMAKE_<LANG>_COMPILER).
        tc = CMakeToolchain(self)
        tc.generate()

        CMakeDeps(self).generate()

    def build(self):
        cmake = CMake(self)
        cmake.configure(build_script_folder="conan_build")
        cmake.build()

    def package(self):
        cmake = CMake(self)
        cmake.install()

    def package_info(self):
        self.cpp_info.libs = ["pico_led"]
        self.cpp_info.includedirs = ["include"]
        self.cpp_info.requires = ["coro::pico", "coro::pico_hal"]
        self.cpp_info.set_property("cmake_target_name", "pico_led::pico_led")
