import re, os
from conan import ConanFile
from conan.tools.cmake import CMakeToolchain, CMake, cmake_layout, CMakeDeps
from conan.tools.system.package_manager import Apt, Dnf, PacMan, Brew
from conan.tools.files import copy

class CoroRecipe(ConanFile):
    name = "coro"
    version = "0.1.0"
    package_type = "library"

    # Binary configuration
    settings = "os", "compiler", "build_type", "arch"
    options = {
        "shared": [True, False],
        "fPIC": [True, False],
        "with_gperftools": [True, False],
        "with_local_run_queue": [True, False],
        "with_sanitize": ["none", "asan", "tsan"],
        # Pico-only (settings.os == "baremetal"); ignored/removed otherwise.
        # Drives both whether coro_pico_mqtt is built and whether the
        # bundled default lwipopts.h defines LWIP_MQTT — see
        # doc/design/lwip_config.md. Participates in the package ID like any
        # other option, so an app needing MQTT gets a distinctly-cached
        # binary from one that doesn't.
        "with_mqtt": [True, False],
    }
    default_options = {
        "shared": True,
        "fPIC": True,
        "with_gperftools": True,
        "with_local_run_queue": True,
        "with_sanitize": "none",
        "with_mqtt": False,
    }
    exports_sources = (
        "include/**.h", "include/**.hpp",
        "src/**.cpp", "src/**.h", "src/**.in",
        "CMakeLists.txt", "cmake/**.cmake",
    )

    # Lets CORO_<OPTION> in the environment (e.g. via .envrc) set a boolean
    # option's default without passing -o on the conan CLI. config_options()
    # runs before run_configure_method() merges the profile/CLI -o values
    # onto self.options (methods.py), so an explicit -o ...=... still
    # overrides this. configure() runs too late for this purpose — it
    # executes after that merge, so setting the option there would clobber
    # an explicit -o instead of yielding to it.
    @staticmethod
    def _env_bool(name, current):
        val = os.environ.get(name)
        if val is None:
            return current
        return val.strip().lower() in ("1", "true", "yes", "on")

    def config_options(self):
        if self.settings.os == "Windows":
            self.options.rm_safe("fPIC")

        # baremetal (Pico) targets: always static (no "shared" option to
        # decide otherwise), no PIC, and the desktop I/O stack
        # (libuv/gperftools/libwebsockets) doesn't exist for them.
        if self.settings.os == "baremetal":
            self.package_type = "static-library"
            self.options.rm_safe("shared")
            self.options.rm_safe("fPIC")
            self.options.rm_safe("with_gperftools")
        else:
            self.options.rm_safe("with_mqtt")
            self.options.shared = self._env_bool("CORO_SHARED", self.options.shared)
            self.options.with_gperftools = self._env_bool(
                "CORO_WITH_GPERFTOOLS", self.options.with_gperftools)
            self.options.with_local_run_queue = self._env_bool(
                "CORO_WITH_LOCAL_RUN_QUEUE", self.options.with_local_run_queue)

        sanitize = os.environ.get("CORO_SANITIZE", "none").strip().lower()
        if sanitize not in ("none", "asan", "tsan"):
            sanitize = "none"
        self.options.with_sanitize = sanitize

    def configure(self):
        if self.settings.os != "baremetal" and self.options.shared:
            self.options.rm_safe("fPIC")

    def system_requirements(self):
        if self.settings.os == "baremetal":
            return
        Apt(self).install(["libcap-dev"])
        Dnf(self).install(["libcap-devel"])
        PacMan(self).install(["libcap"])

    def requirements(self):
        if self.settings.os == "baremetal":
            return
        if self.options.with_gperftools:
            self.requires("gperftools/2.17.2",
                transitive_headers = True,
                transitive_libs = True)
        self.requires("libuv/1.47.0",
            transitive_headers = True,
            transitive_libs = True)
        self.requires("libwebsockets/[>=4.3.5 <5]",
            options={"with_libuv": True},
            transitive_headers = True,
            transitive_libs = True)

    def layout(self):
        cmake_layout(self)
        # cmake_layout() only patches the root self.cpp.build (libdirs=["."]),
        # not per-component build dirs. Since package_info() for baremetal
        # is components-only (no root cpp_info.libs), an editable consumer's
        # CMakeDeps falls back to each component's *packaged* lib dir
        # (.../lib), which doesn't exist in editable mode (package()/install()
        # never runs). Mirror the component set from package_info() here so
        # `conan editable add` resolves coro_pico/coro_pico_hal/coro_pico_mqtt
        # straight out of the build folder.
        if self.settings.os == "baremetal":
            self.cpp.build.components["pico"].libdirs = ["."]
            self.cpp.build.components["pico_hal"].libdirs = ["."]
            if self.options.with_mqtt:
                self.cpp.build.components["pico_mqtt"].libdirs = ["."]
            # lwipopts.h is generated by pico.cmake into
            # <build_folder>/coro_pico_lwipopts (pico.cmake is include()d
            # directly from this repo's top-level CMakeLists.txt, so
            # CMAKE_CURRENT_BINARY_DIR there is self.build_folder) and is
            # never installed by package() — a real (non-editable) package
            # consumer hits the same "lwipopts.h not found" gap and isn't
            # covered by this fix. For editable mode, CMakeDeps builds
            # coro::pico purely from this layout()'s cpp.build/cpp.source
            # settings rather than from coro_pico's real CMake target
            # properties, so it needs its own includedirs entry here even
            # though coro_pico's own target_include_directories() now
            # exposes the same directory PUBLIC (see pico.cmake) for
            # in-tree consumers.
            self.cpp.build.components["pico"].includedirs = ["coro_pico_lwipopts"]
            # cmake_build_modules path (see package_info()) — lives in the
            # recipe source tree, not the build tree, so it's declared under
            # cpp.source (resolved against self.source_folder, which for an
            # editable package is this live checkout) rather than cpp.build.
            # CMakeDeps only reads build_modules off the root cpp_info, not
            # per-component (verified against the generated *-config.cmake:
            # only `<pkg>_BUILD_MODULES_PATHS_RELEASE`, no per-component
            # variant) — must be set at the top level, not on a component.
            self.cpp.source.build_modules = [
                "cmake/pico_hal_build_module.cmake",
                "cmake/pico_mqtt_build_module.cmake",
            ]

    def generate(self):
        deps = CMakeDeps(self)
        deps.generate()
        tc = CMakeToolchain(self)
        # Pico (baremetal): the toolchain itself is owned by pico_sdk_init(),
        # not Conan — see doc/design/pico_led_conan_packaging.md. The host
        # profile is responsible for paring CMakeToolchain's blocks down via
        # tools.cmake.cmaketoolchain:enabled_blocks (find_paths + user_toolchain)
        # and for [buildenv] PICO_SDK_PATH/PICO_BOARD/PICO_PLATFORM. Nothing
        # extra needed here for that part.
        if self.settings.os == "baremetal":
            tc.cache_variables["CORO_PLATFORM"] = "pico"
            tc.cache_variables["CORO_PICO_WITH_MQTT"] = bool(self.options.with_mqtt)
        else:
            tc.cache_variables["WITH_GPERFTOOLS"] = self.options.with_gperftools
            tc.cache_variables["CORO_USE_LOCAL_RUN_QUEUE"] = self.options.with_local_run_queue
            tc.cache_variables["WITH_SANITIZE"] = str(self.options.with_sanitize)
        tc.generate()

    def build(self):
        cmake = CMake(self)
        cmake.configure()
        cmake.build()

    def package(self):
        cmake = CMake(self)
        cmake.install()
        if self.settings.os == "baremetal":
            # cmake.install() (governed by the root CMakeLists.txt's
            # install() calls) only copies built artifacts/headers, not
            # this build module — it isn't a build product, so it needs its
            # own explicit copy into the package folder (mirrored by
            # cpp.source.build_modules in layout() for the editable-package
            # case, where package() never runs).
            copy(self, "pico_hal_build_module.cmake",
                 os.path.join(self.source_folder, "cmake"),
                 os.path.join(self.package_folder, "cmake"))
            copy(self, "pico_mqtt_build_module.cmake",
                 os.path.join(self.source_folder, "cmake"),
                 os.path.join(self.package_folder, "cmake"))

    def package_info(self):
        if self.settings.os == "baremetal":
            self.cpp_info.components["pico"].libs = ["coro_pico"]
            self.cpp_info.components["pico"].defines = ["CORO_PICO", "CORO_TCP_BACKEND_LWIP"]
            self.cpp_info.components["pico"].set_property("cmake_target_name", "coro::pico")

            self.cpp_info.components["pico_hal"].libs = ["coro_pico_hal"]
            self.cpp_info.components["pico_hal"].requires = ["pico"]
            self.cpp_info.components["pico_hal"].set_property("cmake_target_name", "coro::pico_hal")
            # See cmake/pico_hal_build_module.cmake and
            # cmake/pico_mqtt_build_module.cmake: re-link raw Pico SDK
            # targets (hardware_dma/hardware_irq onto coro::pico_hal;
            # pico_stdlib/pico_cyw43_arch_lwip_poll onto coro::pico_mqtt)
            # after CMakeDeps declares those targets, since raw Pico SDK
            # targets can't be expressed as a normal cpp_info dependency.
            # CMakeDeps only reads build_modules off the root cpp_info —
            # there's no per-component variant in the generated
            # *-config.cmake — so this must be set here, not on the
            # individual components.
            self.cpp_info.set_property(
                "cmake_build_modules", [
                    "cmake/pico_hal_build_module.cmake",
                    "cmake/pico_mqtt_build_module.cmake",
                ])

            if self.options.with_mqtt:
                self.cpp_info.components["pico_mqtt"].libs = ["coro_pico_mqtt"]
                self.cpp_info.components["pico_mqtt"].requires = ["pico"]
                self.cpp_info.components["pico_mqtt"].set_property("cmake_target_name", "coro::pico_mqtt")
            return

        self.cpp_info.libs = ["coro"]
        self.cpp_info.system_libs = ["cap"]
        if self.options.with_gperftools:
            self.cpp_info.requires.append("gperftools::gperftools")
        if self.options.with_local_run_queue:
            self.cpp_info.defines.append("CORO_USE_LOCAL_RUN_QUEUE")
        self.cpp_info.requires.append("libuv::libuv")
        self.cpp_info.requires.append("libwebsockets::libwebsockets")

        self.cpp_info.set_property("cmake_find_package", "coro")
        self.cpp_info.set_property("cmake_find_package_multi", "coro")
        self.cpp_info.set_property("pkg_config", "coro")
        self.cpp_info.set_property("cmake_target_name", "coro::coro")
