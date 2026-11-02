// clipboard_image.h - Encodes a clipboard DIB as PNG for 粘贴为图片文件.
#pragma once
#include <cstddef>
#include <cstdint>
#include <vector>

namespace pulse::ops {

// Where the pixels start in a .bmp holding this DIB (file header included).
// False for a truncated or malformed DIB, or a compression only printers use.
bool DibBmpPixelOffset(const uint8_t* dib, size_t size, uint32_t& offset);

// Any thread; initializes COM for its own use. Slow for a large image, so the
// UI thread hands this to a worker.
bool EncodeDibAsPng(const std::vector<uint8_t>& dib, std::vector<uint8_t>& png);

} // namespace pulse::ops
