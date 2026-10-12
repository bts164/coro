import os, sys
from conan import ConanFile
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
        "gtest_from_conan": False,
    }

    # Mirrors conanfile.py's _env_bool/config_options: lets CORO_<OPTION> in
    # the environment set a boolean option's default without -o on the CLI.
    # with_gperftools/with_local_run_queue are mirrored from the same env vars
    # as the coro package's own conanfile.py, so one .envrc drives both
    # recipes identically.
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
            self.requires(f"coro/{self._coro_dependency_version()}")
            if self.options.gtest_from_conan:
                self.requires("gtest/[>=1.14.0 <2]",
                              options={"disable_pthreads": True})
            return
        self.requires(f"coro/{self._coro_dependency_version()}", options={
            "with_gperftools": self.options.with_gperftools,
            "with_local_run_queue": self.options.with_local_run_queue
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
        tc.generate()

    def build(self):
        cmake = CMake(self)
        cmake.configure()
        cmake.build()