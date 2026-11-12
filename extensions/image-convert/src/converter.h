// converter.h - Decodes one picture and writes it in another format.
// Thread-safe: every call owns its decoders and encoders. Call from any worker
// thread; WIC needs COM, which Convert initializes for its own thread.
#pragma once
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace imgconv {

enum class Format { Png = 0, Jpeg = 1, Bmp = 2, Gif = 3, Jxl = 4 };
enum class Background { White = 0, Black = 1 };

struct Options {
    Format format = Format::Png;
    int quality = 90;            // JPEG and lossy JPEG XL, 1..100
    bool jxl_lossless = true;    // a JPEG source is recompressed losslessly
    Background background = Background::White;  // JPEG / BMP have no alpha
    bool keep_exif = true;       // PNG / JPEG / JPEG XL carry the source EXIF
};

// Straight (not premultiplied) RGBA, 8 bits per channel, top row first.
struct Image {
    uint32_t width = 0, height = 0;
    std::vector<uint8_t> rgba;
    std::vector<uint8_t> icc;     // source ICC profile, empty when unknown
    std::vector<uint8_t> exif;    // TIFF-structured EXIF ("II*\0" / "MM\0*")
    std::vector<uint8_t> jpeg;    // original bytes when the source is a JPEG
    bool orientation_applied = false;  // pixels are upright; EXIF must say 1
    bool HasAlpha() const;
};

enum class Kind { Unknown, Png, Jpeg, Gif, Bmp, Tiff, Ico, Webp, Heif, Jxl, Psd };
Kind Detect(const uint8_t* data, size_t size);

struct Result {
    bool ok = false;
    bool cancelled = false;
    std::string error;  // UTF-8, for the task's error text
};

using Cancelled = std::function<bool()>;

// Reads `source`, writes `output` (created new; never replaced).
Result Convert(const std::wstring& source, const std::wstring& output, const Options& options,
               const Cancelled& cancelled);

// For tests and the converter itself.
Result Decode(const std::vector<uint8_t>& bytes, Image& image);
Result Encode(const Image& image, const Options& options, std::vector<uint8_t>& out);

const wchar_t* Extension(Format format);  // ".png", ...

// EXIF helpers (exif.cpp).
int ExifOrientation(const std::vector<uint8_t>& tiff);
void ExifResetOrientation(std::vector<uint8_t>& tiff);
std::vector<uint8_t> JpegExif(const uint8_t* data, size_t size);
std::vector<uint8_t> PngExif(const uint8_t* data, size_t size);
bool InsertJpegExif(std::vector<uint8_t>& jpeg, const std::vector<uint8_t>& tiff);
bool InsertPngExif(std::vector<uint8_t>& png, const std::vector<uint8_t>& tiff);
void ApplyOrientation(Image& image, int orientation);

}  // namespace imgconv
