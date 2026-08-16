import os, sys
from conan import ConanFile
from conan.tools.cmake import CMakeToolchain, CMake, cmake_layout, CMakeDeps
from conan.tools.system.package_manager import Apt, Dnf, PacMan, Brew
from conan.tools.files import copy, update_conandata

# conan_version.py is a plain sibling module (not a Conan python_requires —
# see its own file comment for why), shared with test/conanfile.py so the
# two recipes can never resolve to mismatched coro versions.
sys.path.insert(0, os.path.dirname(__file__))
from conan_version import derive_coro_version

class CoroRecipe(ConanFile):
    name = "coro"
    package_type = "library"

    # coro is header/template-heavy: consumers compile against its headers
    # regardless of whether the library binary itself is statically embedded
    # or dynamically linked, so no patch release can be assumed ABI/API-safe
    # to drop in without a consumer rebuild. The embed-mode default
    # ("full_mode") already forces a rebuild on any change for the
    # static/header-embedding case; non_embed_mode's default ("minor_mode")
    # is the gap — it's what applies to the common desktop case (an
    # executable dynamically linking coro as a shared library, coro's
    # default_options), and it otherwise treats patch bumps as compatible
    # and skips the consumer rebuild. See README.md's Versioning and
    # Releases section.
    package_id_non_embed_mode = "patch_mode"

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
        "CMakeLists.txt", "cmake/**.cmake", "cmake/**.in",
    )
    # conan_version.py isn't a build source, but set_version() below needs to
    # import it again when this recipe re-runs against its own cache-exported
    # copy (see set_version()'s comment) — exports (unlike exports_sources)
    # copies it into the cache alongside conanfile.py for exactly that case.
    exports = "conan_version.py"

    # Derives the version from git instead of a hand-maintained string — see
    # README.md's "How the version is derived on non-tagged commits" and
    # conan_version.py (the actual derivation logic, shared with
    # test/conanfile.py). Requires at least one reachable vX.Y.Z tag (a
    # one-time bootstrap requirement) and enough git history to reach it; a
    # shallow clone with no tag in range fails loudly rather than silently
    # falling back to a guess.
    #
    # set_version() re-runs against the recipe's *cache-exported* copy (e.g.
    # when a consumer resolves "coro/<version>" from the cache later), and
    # that copy never has .git — exports_sources only copies source patterns,
    # never the repo metadata. export() runs right after set_version()'s
    # first (real-tree) success and persists the computed version into
    # conandata.yml's scm_version key, which — unlike exports_sources — Conan
    # always copies into the cache. derive_coro_version() checks for that key
    # up front (rather than trying git first and falling back on failure) to
    # skip an always-doomed `git describe` subprocess call on every later
    # cache-based recipe evaluation. This relies on the *tracked*
    # conandata.yml (source tree) never itself containing an scm_version key
    # — only export()'s write into the cache-exported copy should ever put
    # one there; don't add one by hand.
    def set_version(self):
        self.version = derive_coro_version(self, self.recipe_folder, self.conan_data)

    # Runs once, immediately after set_version() succeeds against the real
    # working tree (before the recipe is copied into the cache), so this is
    # the last point .git is guaranteed present. Persists the already-computed
    # version into conandata.yml — which does get copied into the cache
    # regardless of exports_sources — so later .git-less invocations of
    # set_version() (see its comment above) can recover it instead of failing.
    def export(self):
        update_conandata(self, {"scm_version": self.version})

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
            # <build_folder>/coro_pico_lwipopts/ and installed to
            # include/coro_pico_lwipopts/ by pico.cmake's install() rule.
            # For editable mode, CMakeDeps builds coro::pico from this
            # layout()'s cpp.build/cpp.source rather than from coro_pico's
            # real CMake target properties, so it needs its own includedirs
            # entry pointing at the build-folder path.
            self.cpp.build.components["pico"].includedirs = ["coro_pico_lwipopts"]
            # cpp.build.includedirs resolves against the build folder;
            # include/ lives in the source tree, so it belongs on cpp.source
            # instead. Must be set explicitly -- same gotcha package_info()
            # already documents: setting includedirs on a component at all
            # replaces the default ["include"] rather than adding to it.
            # Omitting this was a latent bug: any target linking coro::pico
            # without also linking coro::pico_hal (whose includedirs falls
            # back to the correct default, masking it) would fail to find
            # coro/**.h headers entirely.
            self.cpp.source.components["pico"].includedirs = ["include"]
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
        # Single source of truth for the version embedded in the built binary
        # (see README.md's "Embedding the version in the built binary") —
        # CMakeLists.txt generates include/coro/version.h from this rather
        # than trying to independently re-derive it via git itself.
        tc.cache_variables["CORO_VERSION"] = str(self.version)
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
            # lwipopts.h is installed to include/coro_pico_lwipopts/ by pico.cmake.
            # Must be listed explicitly because setting any includedirs replaces
            # the default ["include"], so both dirs are required.
            self.cpp_info.components["pico"].includedirs = ["include", "include/coro_pico_lwipopts"]

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
