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
    directly — see kitchen_led.md). Pulling those in means build() needs
    nanopb codegen, via experimental/nanopb (see that recipe) instead of
    FetchContent + a pip-installed generator: the runtime is a regular
    requires() (transitive — KitchenLED links pb.h types through pico_led's
    public headers without knowing about nanopb itself), the generator is a
    build_requirements() tool_requires() (not transitive — a consumer that
    wants to generate its own .proto files must tool_requires("nanopb/...")
    itself).

    examples/pico is Conan-only — there is no in-tree add_subdirectory()
    build of this package; ws2812_tcp/ (the consumer that needs effects.h/
    effect_runner.h) finds it via find_package(pico_led CONFIG REQUIRED),
    same as KitchenLED does.
    """

    name = "pico_led"
    version = "0.1.0"
    package_type = "static-library"

    settings = "os", "compiler", "build_type", "arch"

    def export_sources(self):
        # CMakeLists.txt, include/, src/, and proto/ all live directly under
        # this package's own recipe folder, so the plain exports_sources
        # attribute would work just as well — using copy() here only to
        # keep the pattern list explicit and self-documenting.
        for pattern in (
            "CMakeLists.txt", "include/**.h", "src/**.cpp",
            "src/**.pio", "proto/**.proto", "proto/**.options"
        ):
            copy(self, pattern, self.recipe_folder, self.export_sources_folder)

    def requirements(self):
        self.requires("coro/0.1.0",
            transitive_headers=True,
            transitive_libs=True)
        # Transitive: pico_led's public headers (effects.h -> ws2812.pb.h ->
        # pb.h) need the nanopb runtime visible to whatever links pico_led,
        # e.g. KitchenLED — without that consumer needing to know nanopb is
        # involved at all.
        self.requires("nanopb/0.4.9.1",
            transitive_headers=True,
            transitive_libs=True)

    def build_requirements(self):
        # Not transitive — tool_requires() never propagates to consumers.
        # Only this package's own ws2812.proto -> ws2812.pb.c/.h codegen
        # needs the generator. A consumer (e.g. KitchenLED) that wants to
        # generate its own .proto files must tool_requires("nanopb/...")
        # itself.
        self.tool_requires("nanopb/0.4.9.1")

    def layout(self):
        cmake_layout(self)
        # Used only when this package is consumed in --editable mode (no
        # package() copy happens then) — package_info() below is what
        # applies after a real `conan create`. cmake_layout()'s default
        # self.cpp.source.includedirs = ["include"] already matches this
        # package's own physical layout (include/pico_led/led_driver.h), so
        # consumers' #include "pico_led/led_driver.h" resolves with no
        # override needed here — unlike before this package's headers/
        # sources were reorganized into include/ and src/ subdirectories.

        # ws2812.pb.h (nanopb-generated, included bare as "ws2812.pb.h" by
        # effects.h) is generated into the build folder, not source — see
        # CMakeLists.txt's PROTO_OUT_DIR, relative to
        # CMAKE_CURRENT_BINARY_DIR (== self.build_folder here). pb.h itself
        # now comes from the nanopb dependency's own includedirs (CMakeDeps),
        # not a path here. Only exercised in editable mode; package_info()
        # takes over for a real `conan create`.
        self.cpp.build.includedirs = ["proto"]

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
        cmake.configure()
        cmake.build()

    def package(self):
        cmake = CMake(self)
        cmake.install()

    def package_info(self):
        self.cpp_info.libs = ["pico_led"]
        self.cpp_info.includedirs = ["include"]
        self.cpp_info.requires = ["coro::pico", "coro::pico_hal", "nanopb::nanopb"]
        self.cpp_info.set_property("cmake_target_name", "pico_led::pico_led")
