// converter_test.cpp - Converts every fixture to every target format and reads
// the result back. Usage: image_convert_test <fixtures dir> <scratch dir>
#include "../src/converter.h"
#include "../src/convert_job.h"
#include <windows.h>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>

namespace fs = std::filesystem;
using namespace imgconv;

namespace {
int failures = 0;
void Check(bool ok, const std::string& what) {
    std::printf("[%s] %s\n", ok ? "PASS" : "FAIL", what.c_str());
    if (!ok) ++failures;
}

std::vector<uint8_t> ReadAll(const fs::path& path) {
    std::ifstream in(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(in), {}};
}

std::string Name(Format format) {
    const wchar_t* ext = Extension(format);
    return std::string(ext, ext + wcslen(ext));
}

struct Expected { std::string name; uint32_t width, height; bool alpha; };
}  // namespace

int main(int argc, char** argv) {
    SetConsoleOutputCP(CP_UTF8);
    if (argc < 3) { std::printf("usage: image_convert_test <fixtures> <scratch>\n"); return 2; }
    const fs::path fixtures = argv[1], scratch = argv[2];
    std::error_code error;
    fs::remove_all(scratch, error);
    fs::create_directories(scratch);

    // Orientation: a 3x2 image in every EXIF orientation.
    for (int o = 1; o <= 8; ++o) {
        Image image;
        image.width = 3; image.height = 2;
        image.rgba.assign(3 * 2 * 4, 0);
        image.rgba[0] = 255;  // top-left pixel is red
        ApplyOrientation(image, o);
        const bool swap = o >= 5;
        Check(image.width == (swap ? 2u : 3u) && image.height == (swap ? 3u : 2u), "orientation " + std::to_string(o) + " size");
    }
    {
        Image image;
        image.width = 3; image.height = 2;
        image.rgba.assign(24, 0);
        image.rgba[0] = 255;
        ApplyOrientation(image, 6);  // 90 CW: top-left moves to top-right
        Check(image.rgba[(0 * 2 + 1) * 4] == 255, "orientation 6 moves the top-left pixel to the top-right");
    }
    {
        std::vector<std::wstring> taken;
        const auto a = OutputName(L"C:\\x\\photo.heic", Format::Jpeg, taken);
        const auto b = OutputName(L"C:\\x\\photo.png", Format::Jpeg, taken);
        const auto c = OutputName(L"C:\\x\\PHOTO.webp", Format::Jpeg, taken);
        Check(a == L"photo.jpg" && b == L"photo (2).jpg" && c == L"PHOTO (3).jpg", "output names never collide inside one stage");
    }

    std::vector<Expected> expected;
    {
        std::ifstream list(fixtures / "expected.txt");
        std::string line;
        while (std::getline(list, line)) {
            std::istringstream fields(line);
            Expected e{};
            int alpha = 0;
            if (fields >> e.name >> e.width >> e.height >> alpha) { e.alpha = alpha != 0; expected.push_back(e); }
        }
    }
    Check(expected.size() >= 15, "fixture list loaded");

    for (const auto& e : expected) {
        const auto bytes = ReadAll(fixtures / e.name);
        Image image;
        const auto decoded = Decode(bytes, image);
        if (!e.width) {
            Check(!decoded.ok && !decoded.error.empty(), e.name + ": a broken file is refused with a reason");
            Options options;
            const auto result = Convert((fixtures / e.name).wstring(), (scratch / "broken.png").wstring(), options, {});
            Check(!result.ok && !fs::exists(scratch / "broken.png"), e.name + ": no output for a broken file");
            continue;
        }
        Check(decoded.ok, e.name + ": decodes" + (decoded.ok ? "" : " (" + decoded.error + ")"));
        if (!decoded.ok) continue;
        Check(image.width == e.width && image.height == e.height,
              e.name + ": upright size " + std::to_string(image.width) + "x" + std::to_string(image.height));
        Check(image.HasAlpha() == e.alpha, e.name + ": alpha " + (e.alpha ? "kept" : "absent"));

        for (int f = 0; f <= 4; ++f) {
            for (int variant = 0; variant < (f == 4 ? 2 : 1); ++variant) {
                Options options;
                options.format = static_cast<Format>(f);
                options.quality = 85;
                options.jxl_lossless = variant == 0;
                const std::string label = e.name + " -> " + Name(options.format) + (f == 4 ? (variant ? " lossy" : " lossless") : "");
                const fs::path output = scratch / (fs::path(e.name).stem().string() + "_" + std::to_string(f) + std::to_string(variant) +
                                                   Name(options.format));
                const auto result = Convert((fixtures / e.name).wstring(), output.wstring(), options, {});
                Check(result.ok, label + (result.ok ? "" : " (" + result.error + ")"));
                if (!result.ok) continue;
                const auto written = ReadAll(output);
                Image back;
                const auto reread = Decode(written, back);
                Check(reread.ok && back.width == e.width && back.height == e.height, label + ": output reads back upright");
                const bool keeps_alpha = options.format == Format::Png || options.format == Format::Gif || options.format == Format::Jxl;
                if (reread.ok) Check(back.HasAlpha() == (e.alpha && keeps_alpha), label + ": alpha as expected");
                const Kind kind = Detect(written.data(), written.size());
                const Kind wanted[] = {Kind::Png, Kind::Jpeg, Kind::Bmp, Kind::Gif, Kind::Jxl};
                Check(kind == wanted[f], label + ": real file format");
            }
        }
    }

    // EXIF survives, with the orientation reset because the pixels are upright.
    for (const Format format : {Format::Png, Format::Jpeg, Format::Jxl}) {
        Options options;
        options.format = format;
        options.jxl_lossless = false;
        const fs::path output = scratch / ("exif" + Name(format));
        const auto result = Convert((fixtures / "rotated.jpg").wstring(), output.wstring(), options, {});
        Image back;
        const bool read = result.ok && Decode(ReadAll(output), back).ok;
        Check(read && !back.exif.empty() && ExifOrientation(back.exif) == 1, "EXIF kept with orientation 1 in " + Name(format));
        options.keep_exif = false;
        const fs::path stripped = scratch / ("noexif" + Name(format));
        const bool ok = Convert((fixtures / "rotated.jpg").wstring(), stripped.wstring(), options, {}).ok;
        Image plain;
        Check(ok && Decode(ReadAll(stripped), plain).ok && plain.exif.empty(), "EXIF removed on request in " + Name(format));
    }
    {
        // Lossless JPEG XL from a JPEG keeps the original JPEG inside (smaller than pixels).
        Options options;
        options.format = Format::Jxl;
        const fs::path output = scratch / "recompressed.jxl";
        const bool ok = Convert((fixtures / "rotated.jpg").wstring(), output.wstring(), options, {}).ok;
        Image back;
        Check(ok && Decode(ReadAll(output), back).ok && back.width == 24 && back.height == 40,
              "JPEG -> lossless JPEG XL recompression keeps the orientation");
    }
    {
        Options options;
        const fs::path output = scratch / "exists.png";
        std::ofstream(output) << "x";
        Check(!Convert((fixtures / "rgba.png").wstring(), output.wstring(), options, {}).ok, "an existing output is never replaced");
        const auto cancelled = Convert((fixtures / "rgba.png").wstring(), (scratch / "cancel.png").wstring(), options, [] { return true; });
        Check(!cancelled.ok && cancelled.cancelled && !fs::exists(scratch / "cancel.png"), "cancellation writes nothing");
    }

    std::printf("%s: %d failure(s)\n", failures ? "FAILED" : "OK", failures);
    return failures ? 1 : 0;
}
