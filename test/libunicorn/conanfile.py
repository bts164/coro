from conan import ConanFile
from conan.tools.cmake import CMake, CMakeToolchain, cmake_layout
from conan.tools.scm import Git


class LibUnicornConan(ConanFile):
    # Vendors a pinned unicorn-engine/unicorn release for coro's own test
    # suite -- see doc/design/fiber.md's "Testing strategy": the ARMv6-M/
    # CORO_PICO fiber backend is unit-tested by emulating Cortex-M0+'s
    # MSP/PSP stack-pointer banking under Unicorn rather than on real
    # hardware. Not a general-purpose Conan package for unicorn -- just
    # enough to build and install libunicorn locally for that.
    name = "libunicorn"
    version = "2.1.4"
    package_type = "library"

    settings = "os", "compiler", "build_type", "arch"
    options = {
        "shared": [True, False],
        "fPIC": [True, False],
    }
    default_options = {
        "shared": False,
        "fPIC": True,
    }

    # Upstream tag this recipe is pinned to, confirmed to exist at
    # https://github.com/unicorn-engine/unicorn/tags -- bump deliberately
    # (and re-check UNICORN_ARCH/UNICORN_BUILD_TESTS/UNICORN_INSTALL below
    # still match root CMakeLists.txt at the new tag) rather than tracking
    # a moving branch.
    _git_url = "https://github.com/unicorn-engine/unicorn.git"
    _git_tag = "2.1.4"

    def config_options(self):
        if self.settings.os == "Windows":
            self.options.rm_safe("fPIC")

    def configure(self):
        if self.options.shared:
            self.options.rm_safe("fPIC")

    def layout(self):
        cmake_layout(self)

    def source(self):
        git = Git(self)
        git.clone(url=self._git_url, target=".")
        git.checkout(self._git_tag)

    def generate(self):
        tc = CMakeToolchain(self)
        # Only build the backend coro's Pico/ARMv6-M fiber tests need --
        # cuts build time substantially versus unicorn's default of building
        # every architecture (x86;arm;aarch64;riscv;mips;sparc;m68k;ppc;s390x;tricore).
        tc.cache_variables["UNICORN_ARCH"] = "arm"
        tc.cache_variables["UNICORN_BUILD_TESTS"] = False
        tc.cache_variables["UNICORN_INSTALL"] = True
        tc.cache_variables["BUILD_SHARED_LIBS"] = bool(self.options.shared)
        tc.generate()

    def build(self):
        cmake = CMake(self)
        cmake.configure()
        cmake.build()

    def package(self):
        cmake = CMake(self)
        cmake.install()

    def package_info(self):
        self.cpp_info.libs = ["unicorn"]
        self.cpp_info.set_property("cmake_target_name", "unicorn::unicorn")
        self.cpp_info.set_property("pkg_config_name", "unicorn")
