#pragma once
#include "color.h"
#include <vector>
#include <cstdint>

namespace led {

// A frame of pixels stored as packed GRB words ready for DMA transfer.
// Use Color::to_grb_word() to convert a Color when writing, and the
// static helpers below for whole-buffer operations.
using PixelBuffer = std::vector<uint32_t>;

inline PixelBuffer& fill(PixelBuffer& buf, Color c) {
    uint32_t word = c.to_grb_word();
    for (auto& px : buf) px = word;
    return buf;
}

inline PixelBuffer& clear(PixelBuffer& buf) {
    for (auto& px : buf) px = 0u;
    return buf;
}

} // namespace led
