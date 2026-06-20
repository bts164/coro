import re, os
from conan import ConanFile
from conan.tools.cmake import CMakeToolchain, CMake, cmake_layout, CMakeDeps
from conan.tools.system.package_manager import Apt, Dnf, PacMan, Brew

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
        "with_sanitize": ["none", "asan", "tsan"]
    }
    default_options = {
        "shared": True,
        "fPIC": True,
        "with_gperftools": True,
        "with_local_run_queue": True,
        "with_sanitize": "none"
    }
    exports_sources = "include/*.h","include/*.hpp", "src/*.cpp", "CMakeLists.txt", "cmake/*.cmake"

    def config_options(self):
        if self.settings.os == "Windows":
            self.options.rm_safe("fPIC")

        # Lets CORO_SANITIZE in the environment (e.g. via .envrc) set the
        # default for with_sanitize without passing -o on the conan CLI.
        # config_options() runs before run_configure_method() merges the
        # profile/CLI -o values onto self.options (methods.py), so an
        # explicit -o with_sanitize=... still overrides this. configure()
        # runs too late for this purpose — it executes after that merge, so
        # setting the option there would clobber an explicit -o instead of
        # yielding to it.
        sanitize = os.environ.get("CORO_SANITIZE", "none").strip().lower()
        if sanitize not in ("none", "asan", "tsan"):
            sanitize = "none"
        self.options.with_sanitize = sanitize

    def configure(self):
        if self.options.shared:
            self.options.rm_safe("fPIC")

    def system_requirements(self):
        Apt(self).install(["libcap-dev"])
        Dnf(self).install(["libcap-devel"])
        PacMan(self).install(["libcap"])

    def requirements(self):
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

    def generate(self):
        deps = CMakeDeps(self)
        deps.generate()
        tc = CMakeToolchain(self)
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

    def package_info(self):
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
