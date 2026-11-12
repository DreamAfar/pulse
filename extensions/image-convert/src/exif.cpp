// exif.cpp - The few EXIF operations the converter needs: read and reset the
// orientation tag, and move the EXIF block between JPEG and PNG containers.
#include "converter.h"
#include <algorithm>
#include <array>
#include <cstring>

namespace imgconv {
namespace {

struct Tiff {
    const uint8_t* data;
    size_t size;
    bool little;
    uint16_t U16(size_t at) const {
        return little ? static_cast<uint16_t>(data[at] | data[at + 1] << 8)
                      : static_cast<uint16_t>(data[at] << 8 | data[at + 1]);
    }
    uint32_t U32(size_t at) const {
        return little ? static_cast<uint32_t>(data[at] | data[at + 1] << 8 | data[at + 2] << 16 | data[at + 3] << 24)
                      : static_cast<uint32_t>(data[at] << 24 | data[at + 1] << 16 | data[at + 2] << 8 | data[at + 3]);
    }
};

// Offset of the IFD0 orientation entry's value, or 0.
size_t OrientationValue(const uint8_t* data, size_t size, bool& little) {
    if (size < 8) return 0;
    if (data[0] == 'I' && data[1] == 'I' && data[2] == 42 && data[3] == 0) little = true;
    else if (data[0] == 'M' && data[1] == 'M' && data[2] == 0 && data[3] == 42) little = false;
    else return 0;
    const Tiff tiff{data, size, little};
    const uint32_t ifd = tiff.U32(4);
    if (ifd < 8 || ifd > size - 2) return 0;
    const uint16_t count = tiff.U16(ifd);
    if (static_cast<size_t>(ifd) + 2 + static_cast<size_t>(count) * 12 > size) return 0;
    for (uint16_t i = 0; i < count; ++i) {
        const size_t entry = ifd + 2 + static_cast<size_t>(i) * 12;
        if (tiff.U16(entry) == 0x0112 && tiff.U16(entry + 2) == 3 && tiff.U32(entry + 4) == 1) return entry + 8;
    }
    return 0;
}

std::array<uint32_t, 256> CrcTable() {
    std::array<uint32_t, 256> table{};
    for (uint32_t n = 0; n < 256; ++n) {
        uint32_t c = n;
        for (int k = 0; k < 8; ++k) c = (c & 1) ? 0xEDB88320u ^ (c >> 1) : c >> 1;
        table[n] = c;
    }
    return table;
}

uint32_t Crc(const uint8_t* data, size_t size) {
    static const auto table = CrcTable();
    uint32_t c = 0xFFFFFFFFu;
    for (size_t i = 0; i < size; ++i) c = table[(c ^ data[i]) & 0xFF] ^ (c >> 8);
    return c ^ 0xFFFFFFFFu;
}

void PutU32(std::vector<uint8_t>& out, uint32_t value) {
    out.push_back(static_cast<uint8_t>(value >> 24)); out.push_back(static_cast<uint8_t>(value >> 16));
    out.push_back(static_cast<uint8_t>(value >> 8)); out.push_back(static_cast<uint8_t>(value));
}

uint32_t GetU32(const uint8_t* p) {
    return static_cast<uint32_t>(p[0]) << 24 | static_cast<uint32_t>(p[1]) << 16 | static_cast<uint32_t>(p[2]) << 8 | p[3];
}

}  // namespace

int ExifOrientation(const std::vector<uint8_t>& tiff) {
    bool little = true;
    const size_t at = OrientationValue(tiff.data(), tiff.size(), little);
    if (!at) return 1;
    const int value = Tiff{tiff.data(), tiff.size(), little}.U16(at);
    return value >= 1 && value <= 8 ? value : 1;
}

void ExifResetOrientation(std::vector<uint8_t>& tiff) {
    bool little = true;
    const size_t at = OrientationValue(tiff.data(), tiff.size(), little);
    if (!at) return;
    tiff[at] = little ? 1 : 0;
    tiff[at + 1] = little ? 0 : 1;
}

std::vector<uint8_t> JpegExif(const uint8_t* data, size_t size) {
    if (size < 4 || data[0] != 0xFF || data[1] != 0xD8) return {};
    size_t at = 2;
    while (at + 4 <= size) {
        if (data[at] != 0xFF) return {};
        const uint8_t marker = data[at + 1];
        if (marker == 0xFF) { ++at; continue; }
        if (marker == 0xDA || marker == 0xD9) return {};  // scan data: no more headers
        const size_t length = static_cast<size_t>(data[at + 2]) << 8 | data[at + 3];
        if (length < 2 || at + 2 + length > size) return {};
        if (marker == 0xE1 && length >= 8 && std::memcmp(data + at + 4, "Exif\0\0", 6) == 0)
            return {data + at + 10, data + at + 2 + length};
        at += 2 + length;
    }
    return {};
}

std::vector<uint8_t> PngExif(const uint8_t* data, size_t size) {
    size_t at = 8;
    while (at + 12 <= size) {
        const uint32_t length = GetU32(data + at);
        if (length > size - at - 12) return {};
        if (std::memcmp(data + at + 4, "eXIf", 4) == 0) return {data + at + 8, data + at + 8 + length};
        if (std::memcmp(data + at + 4, "IDAT", 4) == 0) return {};
        at += 12 + length;
    }
    return {};
}

bool InsertJpegExif(std::vector<uint8_t>& jpeg, const std::vector<uint8_t>& tiff) {
    if (tiff.empty() || tiff.size() > 65533 - 8 || jpeg.size() < 4 || jpeg[0] != 0xFF || jpeg[1] != 0xD8) return false;
    // After SOI and a JFIF APP0, as cameras write it.
    size_t at = 2;
    if (jpeg.size() >= 6 && jpeg[2] == 0xFF && jpeg[3] == 0xE0) {
        const size_t length = static_cast<size_t>(jpeg[4]) << 8 | jpeg[5];
        if (2 + 2 + length <= jpeg.size()) at = 4 + length;
    }
    const size_t length = 2 + 6 + tiff.size();
    std::vector<uint8_t> segment{0xFF, 0xE1, static_cast<uint8_t>(length >> 8), static_cast<uint8_t>(length),
                                 'E', 'x', 'i', 'f', 0, 0};
    segment.insert(segment.end(), tiff.begin(), tiff.end());
    jpeg.insert(jpeg.begin() + static_cast<std::ptrdiff_t>(at), segment.begin(), segment.end());
    return true;
}

bool InsertPngExif(std::vector<uint8_t>& png, const std::vector<uint8_t>& tiff) {
    if (tiff.empty() || tiff.size() > 0x7FFFFFFF || png.size() < 8) return false;
    size_t at = 8;
    while (at + 12 <= png.size()) {
        const uint32_t length = GetU32(png.data() + at);
        if (length > png.size() - at - 12) return false;
        if (std::memcmp(png.data() + at + 4, "IDAT", 4) == 0) {
            std::vector<uint8_t> chunk;
            PutU32(chunk, static_cast<uint32_t>(tiff.size()));
            chunk.insert(chunk.end(), {'e', 'X', 'I', 'f'});
            chunk.insert(chunk.end(), tiff.begin(), tiff.end());
            PutU32(chunk, Crc(chunk.data() + 4, chunk.size() - 4));
            png.insert(png.begin() + static_cast<std::ptrdiff_t>(at), chunk.begin(), chunk.end());
            return true;
        }
        at += 12 + length;
    }
    return false;
}

void ApplyOrientation(Image& image, int orientation) {
    if (orientation < 2 || orientation > 8 || image.rgba.empty()) return;
    const uint32_t w = image.width, h = image.height;
    const bool swap = orientation >= 5;
    const uint32_t ow = swap ? h : w, oh = swap ? w : h;
    std::vector<uint8_t> out(image.rgba.size());
    for (uint32_t y = 0; y < h; ++y) {
        for (uint32_t x = 0; x < w; ++x) {
            uint32_t dx = x, dy = y;
            switch (orientation) {
            case 2: dx = w - 1 - x; dy = y; break;              // mirror horizontal
            case 3: dx = w - 1 - x; dy = h - 1 - y; break;      // rotate 180
            case 4: dx = x; dy = h - 1 - y; break;              // mirror vertical
            case 5: dx = y; dy = x; break;                      // transpose
            case 6: dx = h - 1 - y; dy = x; break;              // rotate 90 CW
            case 7: dx = h - 1 - y; dy = w - 1 - x; break;      // transverse
            case 8: dx = y; dy = w - 1 - x; break;              // rotate 90 CCW
            default: break;
            }
            std::memcpy(&out[(static_cast<size_t>(dy) * ow + dx) * 4], &image.rgba[(static_cast<size_t>(y) * w + x) * 4], 4);
        }
    }
    image.rgba = std::move(out);
    image.width = ow;
    image.height = oh;
    image.orientation_applied = true;
}

}  // namespace imgconv
