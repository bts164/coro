import re, os
from conan import ConanFile
from conan.tools.cmake import CMakeToolchain, CMake, cmake_layout, CMakeDeps
from conan.tools.system.package_manager import Apt, Dnf, PacMan, Brew

class CoroRecipe(ConanFile):
    name = "coro_unit_tests"
    version = "0.1.0"
    package_type = "library"

    # Binary configuration
    settings = "os", "compiler", "build_type", "arch"
    options = {
        "shared": [True, False],
        "fPIC": [True, False],
        "with_gperftools": [True, False],
        "with_local_run_queue": [True, False],
        "with_sanitize": ["none", "asan", "tsan"]
    }
    default_options = {
        "shared": True,
        "fPIC": True,
        "with_gperftools": True,
        "with_local_run_queue": True,
        "with_sanitize": "none"
    }

    def config_options(self):
        if self.settings.os == "Windows":
            self.options.rm_safe("fPIC")

        # Mirrors conanfile.py: lets CORO_SANITIZE in the environment set the
        # default so the test binaries are built with the same sanitizer as
        # the coro package they link against (required — ASan/TSan must be
        # applied to the whole binary, not just one side of the link).
        # config_options() runs before run_configure_method() merges the
        # profile/CLI -o values onto self.options, so an explicit
        # -o with_sanitize=... still overrides this; configure() runs too
        # late for this purpose (after that merge).
        sanitize = os.environ.get("CORO_SANITIZE", "none").strip().lower()
        if sanitize not in ("none", "asan", "tsan"):
            sanitize = "none"
        self.options.with_sanitize = sanitize

    def configure(self):
        if self.options.shared:
            self.options.rm_safe("fPIC")

    def requirements(self):
        self.requires("coro/[0.1.0]", options={
            "with_gperftools": self.options.with_gperftools,
            "with_local_run_queue": self.options.with_local_run_queue,
            "with_sanitize": self.options.with_sanitize
        })
        self.requires("gtest/[>=1.14.0 <2]")

    def layout(self):
        cmake_layout(self)

    def generate(self):
        deps = CMakeDeps(self)
        deps.generate()
        tc = CMakeToolchain(self)
        tc.cache_variables["WITH_SANITIZE"] = str(self.options.with_sanitize)
        tc.generate()

    def build(self):
        cmake = CMake(self)
        cmake.configure()
        cmake.build()