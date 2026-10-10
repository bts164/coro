import os, sys
from conan import ConanFile
from conan.errors import ConanInvalidConfiguration
from conan.tools.cmake import CMakeToolchain, CMake, cmake_layout, CMakeDeps
from conan.tools.system.package_manager import Apt, Dnf, PacMan, Brew

# conan_version.py lives at the repo root (one level up from test/), and is
# a plain sibling module rather than a Conan python_requires -- see its own
# file comment for why. Shared with the root conanfile.py so this recipe's
# `coro/<version>` requirement (see requirements() below) can never drift
# out of sync with what building the repo's own conanfile.py right now
# would actually produce.
sys.path.insert(0, os.path.join(os.path.dirname(__file__), ".."))
from conan_version import derive_coro_version, load_conandata

class CoroRecipe(ConanFile):
    name = "coro_unit_tests"
    package_type = "library"

    # coro_unit_tests isn't an independently published package -- it's a
    # thin, disposable wrapper that only exists to build/run this checkout's
    # tests -- but it still lived with a hand-hardcoded "0.1.0" here, the
    # same kind of stale-pin problem requirements() used to have with
    # `coro/[*]` (see below). Deriving it the same way keeps both recipes
    # describing "this same checkout" consistently instead of leaving a
    # second easily-forgotten hardcoded version behind.
    def set_version(self):
        coro_root = os.path.join(self.recipe_folder, "..")
        self.version = derive_coro_version(self, coro_root, load_conandata(coro_root))

    # Binary configuration
    settings = "os", "compiler", "build_type", "arch"
    options = {
        "shared": [True, False],
        "fPIC": [True, False],
        "with_gperftools": [True, False],
        "with_local_run_queue": [True, False],
        "with_sanitize": ["none", "asan", "tsan", "ubsan"],
        # Bare metal only (settings.os == "baremetal"); removed otherwise. The
        # on-target firmware compiles GoogleTest from source by default
        # (cmake/on_target_pico.cmake). True takes it from the Conan package
        # instead, which has not been made to work yet -- see
        # doc/design/on_target_tests.md.
        "gtest_from_conan": [True, False],
    }
    default_options = {
        "shared": True,
        "fPIC": True,
        "with_gperftools": True,
        "with_local_run_queue": True,
        "with_sanitize": "none",
        "gtest_from_conan": False,
    }

    # Mirrors conanfile.py's _env_bool/config_options: lets CORO_<OPTION> in
    # the environment set a boolean option's default without -o on the CLI.
    # with_gperftools/with_local_run_queue/with_sanitize are mirrored from
    # the same env vars as the coro package's own conanfile.py so the test
    # binaries stay consistent with whatever coro package they link against
    # (required for with_sanitize — see below; the others are just
    # convenience so one .envrc drives both recipes identically).
    @staticmethod
    def _env_bool(name, current):
        val = os.environ.get(name)
        if val is None:
            return current
        return val.strip().lower() in ("1", "true", "yes", "on")

    def config_options(self):
        if self.settings.os == "Windows":
            self.options.rm_safe("fPIC")

        # baremetal (Pico): builds the on-target firmware images instead of the
        # desktop executables. Mirrors the root conanfile.py -- always static,
        # and none of the desktop-only options apply.
        if self.settings.os == "baremetal":
            self.package_type = "static-library"
            self.options.rm_safe("shared")
            self.options.rm_safe("fPIC")
            self.options.rm_safe("with_gperftools")
        else:
            self.options.rm_safe("gtest_from_conan")
            self.options.shared = self._env_bool("CORO_SHARED", self.options.shared)
            self.options.with_gperftools = self._env_bool(
                "CORO_WITH_GPERFTOOLS", self.options.with_gperftools)
            self.options.with_local_run_queue = self._env_bool(
                "CORO_WITH_LOCAL_RUN_QUEUE", self.options.with_local_run_queue)

        # CORO_SANITIZE in the environment sets the default so the test
        # binaries are built with the same sanitizer as the coro package
        # they link against (required — ASan/TSan must be applied to the
        # whole binary, not just one side of the link). config_options()
        # runs before run_configure_method() merges the profile/CLI -o
        # values onto self.options, so an explicit -o with_sanitize=... still
        # overrides this; configure() runs too late for this purpose (after
        # that merge).
        # baremetal has only ubsan (cmake/Sanitize.cmake). An asan or tsan
        # left in the environment for the desktop builds means none there,
        # so one .envrc serves both; an explicit -o is rejected in validate().
        sanitize = os.environ.get("CORO_SANITIZE", "none").strip().lower()
        allowed = (("none", "ubsan") if self.settings.os == "baremetal"
                   else ("none", "asan", "tsan", "ubsan"))
        if sanitize not in allowed:
            sanitize = "none"
        self.options.with_sanitize = sanitize

    def validate(self):
        if (self.settings.os == "baremetal"
                and str(self.options.with_sanitize) in ("asan", "tsan")):
            raise ConanInvalidConfiguration(
                "with_sanitize on baremetal must be none or ubsan: ASan and "
                "TSan need a runtime a microcontroller does not have")

    def configure(self):
        if self.options.get_safe("shared"):
            self.options.rm_safe("fPIC")

    # Pins the coro/ dependency to exactly the version this same checkout's
    # root conanfile.py would produce right now -- see conan_version.py's
    # file comment. That guarantees the test binaries always link against a
    # coro package built from this tree, not merely *some* coro package
    # that happens to already be in the cache (which is what `coro/[*]` was
    # silently doing before, and how the tests ended up quietly built
    # against a stale coro once the version stopped being a fixed "0.1.0").
    #
    # CORO_TEST_VERSION overrides the pin when set, for deliberately running
    # this same test suite against a different, already-built coro version
    # (e.g. bisecting a regression against an older release) without editing
    # this file. Deliberately doesn't affect self.version (set in
    # set_version() above) -- "what coro build to test against" and "what
    # checkout is this recipe itself" are different questions; only the
    # former should move under the override.
    def _coro_dependency_version(self):
        return os.environ.get("CORO_TEST_VERSION") or self.version

    def requirements(self):
        if self.settings.os == "baremetal":
            self.requires(f"coro/{self._coro_dependency_version()}", options={
                "with_sanitize": self.options.with_sanitize
            })
            if self.options.gtest_from_conan:
                self.requires("gtest/[>=1.14.0 <2]",
                              options={"disable_pthreads": True})
            return
        self.requires(f"coro/{self._coro_dependency_version()}", options={
            "with_gperftools": self.options.with_gperftools,
            "with_local_run_queue": self.options.with_local_run_queue,
            "with_sanitize": self.options.with_sanitize
        })
        self.requires("gtest/[>=1.14.0 <2]")
        self.requires("libunicorn/2.1.4")

    def layout(self):
        # Keeps the firmware build apart from the desktop one, which would
        # otherwise share build/Release and the "conan-release" preset name:
        # this gives build/baremetal/Release and "conan-baremetal-release".
        if self.settings.os == "baremetal":
            self.folders.build_folder_vars = ["settings.os"]
        cmake_layout(self)

    def generate(self):
        deps = CMakeDeps(self)
        deps.generate()
        tc = CMakeToolchain(self)
        # The toolchain for baremetal is owned by pico_sdk_init(), not Conan --
        # see the root conanfile.py's generate().
        if self.settings.os == "baremetal":
            tc.cache_variables["CORO_PLATFORM"] = "pico"
            tc.cache_variables["CORO_TEST_GTEST_FROM_CONAN"] = bool(
                self.options.gtest_from_conan)
        tc.cache_variables["WITH_SANITIZE"] = str(self.options.with_sanitize)
        tc.generate()

    def build(self):
        cmake = CMake(self)
        cmake.configure()
        cmake.build()