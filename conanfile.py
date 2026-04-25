import re, os
from conan import ConanFile
from conan.tools.cmake import CMakeToolchain, CMake, cmake_layout, CMakeDeps

class CoroRecipe(ConanFile):
    name = "coro"
    version = "0.1.0"
    package_type = "library"

    # Binary configuration
    settings = "os", "compiler", "build_type", "arch"
    options = {
        "shared": [True, False],
        "fPIC": [True, False],
        "with_gperftools": [True, False]
    }
    default_options = {
        "shared": True,
        "fPIC": True,
        "with_gperftools": True
    }
    exports_sources = "include/*.h","include/*.hpp", "src/*.cpp", "CMakeLists.txt"
    
    def config_options(self):
        if self.settings.os == "Windows":
            self.options.rm_safe("fPIC")

    def configure(self):
        if self.options.shared:
            self.options.rm_safe("fPIC")

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
        if self.options.with_gperftools:
            self.cpp_info.requires.append("gperftools::gperftools")
        self.cpp_info.requires.append("libuv::libuv")
        self.cpp_info.requires.append("libwebsockets::libwebsockets")
        
        self.cpp_info.set_property("cmake_find_package", "coro")
        self.cpp_info.set_property("cmake_find_package_multi", "coro")
        self.cpp_info.set_property("pkg_config", "coro")
        self.cpp_info.set_property("cmake_target_name", "coro::coro")
