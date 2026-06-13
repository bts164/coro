#pragma once
// Stub for pico/time.h — timer functions backed by steady_clock on Linux test builds.
// time_us_64() is declared here and defined in cyw43_arch_stub.cpp.
#include <cstdint>

uint64_t time_us_64();
