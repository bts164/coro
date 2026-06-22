import os

from conan import ConanFile
from conan.tools.cmake import cmake_layout, CMake, CMakeDeps, CMakeToolchain
from conan.tools.files import copy


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

    Scope: color.h/.cpp, led_driver.h/.cpp, pixel_buffer.h, ws2812.pio, plus
    effects.h/.cpp + effect_runner.h/.cpp and the ws2812.proto/.options they
    depend on (KitchenLED's Plasma animation calls led::plasma_effect()
    directly — see kitchen_led.md). Pulling those in means conan_build's
    build() step needs the same nanopb codegen as examples/pico/CMakeLists.txt
    (FetchContent of the nanopb runtime + an `nanopb_generator` on PATH —
    `pip install -r examples/pico/requirements.txt`). The existing
    examples/pico/led/CMakeLists.txt (coro_pico_led, used by
    pico_ws2812_tcp) is untouched; this recipe builds its own target via
    conan_build/CMakeLists.txt.
    """

    name = "pico_led"
    version = "0.1.0"
    package_type = "static-library"

    settings = "os", "compiler", "build_type", "arch"

    def export_sources(self):
        # exports_sources (a plain attribute) can only reach files at or
        # below the recipe folder. ws2812.proto/.options live one level up,
        # in examples/pico/proto/ (shared with the non-Conan
        # examples/pico/CMakeLists.txt build) — export_sources() is the
        # documented escape hatch for grabbing files outside the recipe
        # folder while keeping the package relocatable (no live-checkout
        # path baked into conan_build/CMakeLists.txt).
        for pattern in (
            "color.h", "color.cpp",
            "led_driver.h", "led_driver.cpp",
            "pixel_buffer.h",
            "ws2812.pio",
            "effects.h", "effects.cpp",
            "effect_runner.h", "effect_runner.cpp",
            "conan_build/CMakeLists.txt",
        ):
            copy(self, pattern, self.recipe_folder, self.export_sources_folder)
        # Lands at <export_sources_folder>/proto, i.e. led/proto — see
        # conan_build/CMakeLists.txt's PROTO_SRC_DIR.
        copy(self, "*", os.path.join(self.recipe_folder, os.pardir, "proto"),
             os.path.join(self.export_sources_folder, "proto"))

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
        # ws2812.pb.h (nanopb-generated, included bare as "ws2812.pb.h" by
        # effects.h) and nanopb's own runtime header (pb.h) are generated/
        # fetched into the build folder, not source — see
        # conan_build/CMakeLists.txt's PROTO_OUT_DIR / NANOPB_DIR, both
        # relative to CMAKE_CURRENT_BINARY_DIR (== self.build_folder here).
        # Only exercised in editable mode; package_info() takes over for a
        # real `conan create`.
        self.cpp.build.includedirs = ["proto", "_deps/nanopb-src"]

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
