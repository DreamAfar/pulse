// encode.cpp - Writes PNG, JPEG, BMP and GIF through WIC, and JPEG XL through
// libjxl; Convert ties reading, decoding, encoding and writing together.
#include "converter.h"
#include "wic.h"
#include <jxl/encode_cxx.h>
#include <jxl/thread_parallel_runner_cxx.h>
#include <algorithm>
#include <cstring>

namespace imgconv {
namespace {

Result Fail(std::string error) { return {false, false, std::move(error)}; }

// BGRA for WIC. Formats without alpha get the picture over the background.
std::vector<uint8_t> ToBgra(const Image& image, bool flatten, Background background) {
    std::vector<uint8_t> out(image.rgba.size());
    const unsigned bg = background == Background::White ? 255u : 0u;
    for (size_t i = 0; i < image.rgba.size(); i += 4) {
        const unsigned r = image.rgba[i], g = image.rgba[i + 1], b = image.rgba[i + 2], a = image.rgba[i + 3];
        if (flatten && a != 255) {
            out[i] = static_cast<uint8_t>((b * a + bg * (255 - a) + 127) / 255);
            out[i + 1] = static_cast<uint8_t>((g * a + bg * (255 - a) + 127) / 255);
            out[i + 2] = static_cast<uint8_t>((r * a + bg * (255 - a) + 127) / 255);
            out[i + 3] = 255;
        } else {
            out[i] = static_cast<uint8_t>(b);
            out[i + 1] = static_cast<uint8_t>(g);
            out[i + 2] = static_cast<uint8_t>(r);
            out[i + 3] = static_cast<uint8_t>(a);
        }
    }
    return out;
}

bool ReadStream(IStream* stream, std::vector<uint8_t>& out) {
    STATSTG stat{};
    if (FAILED(stream->Stat(&stat, STATFLAG_NONAME)) || stat.cbSize.HighPart || !stat.cbSize.LowPart) return false;
    LARGE_INTEGER start{};
    if (FAILED(stream->Seek(start, STREAM_SEEK_SET, nullptr))) return false;
    out.resize(stat.cbSize.LowPart);
    ULONG read = 0;
    return SUCCEEDED(stream->Read(out.data(), stat.cbSize.LowPart, &read)) && read == stat.cbSize.LowPart;
}

Result EncodeWic(const Image& image, const Options& options, std::vector<uint8_t>& out) {
    wic::Com com;
    auto factory = wic::Factory();
    if (!factory) return Fail("无法启动 Windows 图像组件 (WIC)");
    const bool alpha = image.HasAlpha();
    const bool gif = options.format == Format::Gif;
    const bool keeps_alpha = options.format == Format::Png || gif;
    auto pixels = ToBgra(image, !keeps_alpha, options.background);

    const GUID container = options.format == Format::Png ? GUID_ContainerFormatPng
                         : options.format == Format::Jpeg ? GUID_ContainerFormatJpeg
                         : options.format == Format::Bmp ? GUID_ContainerFormatBmp : GUID_ContainerFormatGif;
    const WICPixelFormatGUID wanted = gif ? GUID_WICPixelFormat8bppIndexed
                                    : (options.format == Format::Png && alpha) ? GUID_WICPixelFormat32bppBGRA
                                    : GUID_WICPixelFormat24bppBGR;
    const UINT stride = image.width * 4;
    ComPtr<IWICBitmap> bitmap;
    ComPtr<IStream> stream;
    ComPtr<IWICBitmapEncoder> encoder;
    ComPtr<IWICBitmapFrameEncode> frame;
    ComPtr<IPropertyBag2> properties;
    HRESULT hr = factory->CreateBitmapFromMemory(image.width, image.height, GUID_WICPixelFormat32bppBGRA, stride,
                                                 static_cast<UINT>(pixels.size()), pixels.data(), &bitmap);
    if (SUCCEEDED(hr)) hr = CreateStreamOnHGlobal(nullptr, TRUE, &stream);
    if (SUCCEEDED(hr)) hr = factory->CreateEncoder(container, nullptr, &encoder);
    if (SUCCEEDED(hr)) hr = encoder->Initialize(stream.Get(), WICBitmapEncoderNoCache);
    if (SUCCEEDED(hr)) hr = encoder->CreateNewFrame(&frame, &properties);
    if (SUCCEEDED(hr) && options.format == Format::Jpeg && properties) {
        PROPBAG2 option{};
        option.pstrName = const_cast<LPOLESTR>(L"ImageQuality");
        VARIANT value;
        VariantInit(&value);
        value.vt = VT_R4;
        value.fltVal = static_cast<float>(std::clamp(options.quality, 1, 100)) / 100.0f;
        properties->Write(1, &option, &value);
    }
    if (SUCCEEDED(hr)) hr = frame->Initialize(properties.Get());
    if (SUCCEEDED(hr)) hr = frame->SetSize(image.width, image.height);
    if (FAILED(hr)) return Fail("无法创建图片编码器");

    ComPtr<IWICPalette> palette;
    ComPtr<IWICFormatConverter> converter;
    if (FAILED(factory->CreateFormatConverter(&converter))) return Fail("无法创建像素格式转换器");
    if (gif) {
        // 256 colors at most; a transparent entry when the picture has alpha.
        hr = factory->CreatePalette(&palette);
        if (SUCCEEDED(hr)) hr = palette->InitializeFromBitmap(bitmap.Get(), 256, alpha ? TRUE : FALSE);
        if (SUCCEEDED(hr)) hr = converter->Initialize(bitmap.Get(), GUID_WICPixelFormat8bppIndexed,
                                                      WICBitmapDitherTypeErrorDiffusion, palette.Get(), 50.0,
                                                      WICBitmapPaletteTypeCustom);
        if (SUCCEEDED(hr)) hr = frame->SetPalette(palette.Get());
        if (FAILED(hr)) return Fail("无法生成 GIF 调色板");
        WICPixelFormatGUID format = GUID_WICPixelFormat8bppIndexed;
        if (FAILED(frame->SetPixelFormat(&format))) return Fail("GIF 编码器不接受索引色");
        if (alpha) {
            UINT count = 0;
            WICColor colors[256]{};
            ComPtr<IWICMetadataQueryWriter> writer;
            if (SUCCEEDED(palette->GetColors(256, colors, &count)) && SUCCEEDED(frame->GetMetadataQueryWriter(&writer))) {
                for (UINT i = 0; i < count; ++i) {
                    if ((colors[i] >> 24) != 0) continue;
                    PROPVARIANT flag;
                    PropVariantInit(&flag);
                    flag.vt = VT_BOOL;
                    flag.boolVal = VARIANT_TRUE;
                    PROPVARIANT index;
                    PropVariantInit(&index);
                    index.vt = VT_UI1;
                    index.bVal = static_cast<BYTE>(i);
                    writer->SetMetadataByName(L"/grctlext/TransparencyFlag", &flag);
                    writer->SetMetadataByName(L"/grctlext/TransparentColorIndex", &index);
                    break;
                }
            }
        }
    } else {
        WICPixelFormatGUID format = wanted;
        if (FAILED(frame->SetPixelFormat(&format))) return Fail("编码器不接受像素格式");
        if (FAILED(converter->Initialize(bitmap.Get(), format, WICBitmapDitherTypeNone, nullptr, 0.0,
                                         WICBitmapPaletteTypeCustom)))
            return Fail("无法转换像素格式");
        if (!image.icc.empty() && options.format != Format::Bmp) {
            ComPtr<IWICColorContext> context;
            if (SUCCEEDED(factory->CreateColorContext(&context)) &&
                SUCCEEDED(context->InitializeFromMemory(image.icc.data(), static_cast<UINT>(image.icc.size())))) {
                IWICColorContext* contexts[] = {context.Get()};
                frame->SetColorContexts(1, contexts);  // optional: some encoders refuse
            }
        }
    }
    hr = frame->WriteSource(converter.Get(), nullptr);
    if (SUCCEEDED(hr)) hr = frame->Commit();
    if (SUCCEEDED(hr)) hr = encoder->Commit();
    if (FAILED(hr) || !ReadStream(stream.Get(), out)) return Fail("图片编码失败");
    return {true, false, {}};
}

Result EncodeJxl(const Image& image, const Options& options, const std::vector<uint8_t>& exif,
                 std::vector<uint8_t>& out, bool allow_jpeg) {
    auto encoder = JxlEncoderMake(nullptr);
    auto runner = JxlThreadParallelRunnerMake(nullptr, std::min<size_t>(4, JxlThreadParallelRunnerDefaultNumWorkerThreads()));
    if (!encoder || !runner) return Fail("JPEG XL 编码器无法启动");
    JxlEncoder* enc = encoder.get();
    if (JxlEncoderSetParallelRunner(enc, JxlThreadParallelRunner, runner.get()) != JXL_ENC_SUCCESS)
        return Fail("JPEG XL 编码器无法启动");
    JxlEncoderFrameSettings* settings = JxlEncoderFrameSettingsCreate(enc, nullptr);
    JxlEncoderFrameSettingsSetOption(settings, JXL_ENC_FRAME_SETTING_EFFORT, 7);

    if (allow_jpeg && options.jxl_lossless && !image.jpeg.empty()) {
        // Lossless recompression of the original JPEG bytes: smaller and
        // reversible. The decoder applies the EXIF orientation at display.
        if (JxlEncoderStoreJPEGMetadata(enc, options.keep_exif ? JXL_TRUE : JXL_FALSE) != JXL_ENC_SUCCESS ||
            JxlEncoderAddJPEGFrame(settings, image.jpeg.data(), image.jpeg.size()) != JXL_ENC_SUCCESS)
            return EncodeJxl(image, options, exif, out, false);  // e.g. CMYK or arithmetic-coded JPEG
    } else {
        const bool alpha = image.HasAlpha();
        JxlBasicInfo info;
        JxlEncoderInitBasicInfo(&info);
        info.xsize = image.width;
        info.ysize = image.height;
        info.bits_per_sample = 8;
        info.num_color_channels = 3;
        info.alpha_bits = alpha ? 8 : 0;
        info.num_extra_channels = alpha ? 1 : 0;
        info.uses_original_profile = options.jxl_lossless ? JXL_TRUE : JXL_FALSE;
        if (JxlEncoderSetBasicInfo(enc, &info) != JXL_ENC_SUCCESS) return Fail("JPEG XL 不支持此图片尺寸");
        bool color = !image.icc.empty() &&
                     JxlEncoderSetICCProfile(enc, image.icc.data(), image.icc.size()) == JXL_ENC_SUCCESS;
        if (!color) {
            JxlColorEncoding srgb{};
            JxlColorEncodingSetToSRGB(&srgb, JXL_FALSE);
            if (JxlEncoderSetColorEncoding(enc, &srgb) != JXL_ENC_SUCCESS) return Fail("JPEG XL 色彩设置失败");
        }
        if (!exif.empty()) {
            std::vector<uint8_t> box{0, 0, 0, 0};  // offset to the TIFF header
            box.insert(box.end(), exif.begin(), exif.end());
            if (JxlEncoderUseBoxes(enc) == JXL_ENC_SUCCESS)
                JxlEncoderAddBox(enc, "Exif", box.data(), box.size(), JXL_FALSE);
        }
        if (options.jxl_lossless) {
            if (JxlEncoderSetFrameLossless(settings, JXL_TRUE) != JXL_ENC_SUCCESS) return Fail("JPEG XL 无损设置失败");
        } else {
            JxlEncoderSetFrameDistance(settings, JxlEncoderDistanceFromQuality(static_cast<float>(std::clamp(options.quality, 1, 100))));
        }
        std::vector<uint8_t> rgb;
        const uint8_t* pixels = image.rgba.data();
        size_t size = image.rgba.size();
        if (!alpha) {
            rgb.resize(static_cast<size_t>(image.width) * image.height * 3);
            for (size_t i = 0, o = 0; i < image.rgba.size(); i += 4, o += 3) std::memcpy(&rgb[o], &image.rgba[i], 3);
            pixels = rgb.data();
            size = rgb.size();
        }
        const JxlPixelFormat format{alpha ? 4u : 3u, JXL_TYPE_UINT8, JXL_NATIVE_ENDIAN, 0};
        if (JxlEncoderAddImageFrame(settings, &format, pixels, size) != JXL_ENC_SUCCESS) return Fail("JPEG XL 编码失败");
    }
    JxlEncoderCloseInput(enc);
    out.assign(1 << 16, 0);
    uint8_t* next = out.data();
    size_t available = out.size();
    for (;;) {
        const JxlEncoderStatus status = JxlEncoderProcessOutput(enc, &next, &available);
        if (status == JXL_ENC_SUCCESS) break;
        if (status != JXL_ENC_NEED_MORE_OUTPUT) return Fail("JPEG XL 编码失败");
        const size_t used = static_cast<size_t>(next - out.data());
        out.resize(out.size() * 2);
        next = out.data() + used;
        available = out.size() - used;
    }
    out.resize(static_cast<size_t>(next - out.data()));
    return {true, false, {}};
}

bool ReadAll(const std::wstring& path, std::vector<uint8_t>& out, std::string& error) {
    HANDLE file = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
                              FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
    if (file == INVALID_HANDLE_VALUE) { error = "无法打开文件"; return false; }
    LARGE_INTEGER size{};
    bool ok = GetFileSizeEx(file, &size) && size.QuadPart > 0;
    if (!ok) error = "文件为空";
    else if (size.QuadPart > (1ll << 30)) { ok = false; error = "文件超过 1 GB"; }
    if (ok) {
        out.resize(static_cast<size_t>(size.QuadPart));
        size_t done = 0;
        while (ok && done < out.size()) {
            DWORD read = 0;
            const DWORD chunk = static_cast<DWORD>(std::min<size_t>(out.size() - done, 1u << 24));
            ok = ::ReadFile(file, out.data() + done, chunk, &read, nullptr) && read;
            done += read;
        }
        if (!ok) error = "读取文件失败";
    }
    CloseHandle(file);
    return ok;
}

bool WriteNew(const std::wstring& path, const std::vector<uint8_t>& data) {
    HANDLE file = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return false;
    size_t done = 0;
    bool ok = true;
    while (ok && done < data.size()) {
        DWORD written = 0;
        const DWORD chunk = static_cast<DWORD>(std::min<size_t>(data.size() - done, 1u << 24));
        ok = ::WriteFile(file, data.data() + done, chunk, &written, nullptr) && written == chunk;
        done += written;
    }
    CloseHandle(file);
    if (!ok) DeleteFileW(path.c_str());
    return ok;
}

}  // namespace

const wchar_t* Extension(Format format) {
    switch (format) {
    case Format::Png: return L".png";
    case Format::Jpeg: return L".jpg";
    case Format::Bmp: return L".bmp";
    case Format::Gif: return L".gif";
    case Format::Jxl: return L".jxl";
    }
    return L".png";
}

Result Encode(const Image& image, const Options& options, std::vector<uint8_t>& out) {
    out.clear();
    std::vector<uint8_t> exif;
    if (options.keep_exif && options.format != Format::Bmp && options.format != Format::Gif) {
        exif = image.exif;
        if (image.orientation_applied) ExifResetOrientation(exif);
    }
    Result result = options.format == Format::Jxl ? EncodeJxl(image, options, exif, out, true)
                                                  : EncodeWic(image, options, out);
    if (result.ok && !exif.empty()) {
        if (options.format == Format::Jpeg) InsertJpegExif(out, exif);
        else if (options.format == Format::Png) InsertPngExif(out, exif);
    }
    return result;
}

Result Convert(const std::wstring& source, const std::wstring& output, const Options& options,
               const Cancelled& cancelled) {
    std::vector<uint8_t> bytes;
    std::string error;
    if (!ReadAll(source, bytes, error)) return Fail(error);
    if (cancelled && cancelled()) return {false, true, {}};
    Image image;
    Result result = Decode(bytes, image);
    bytes.clear();
    bytes.shrink_to_fit();
    if (!result.ok) return result;
    if (cancelled && cancelled()) return {false, true, {}};
    std::vector<uint8_t> encoded;
    result = Encode(image, options, encoded);
    if (!result.ok) return result;
    if (cancelled && cancelled()) return {false, true, {}};
    if (!WriteNew(output, encoded)) return Fail("无法写入输出文件");
    return {true, false, {}};
}

}  // namespace imgconv
