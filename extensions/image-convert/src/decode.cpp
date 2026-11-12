// decode.cpp - Reads every supported source format into one RGBA image.
// The format comes from the file's first bytes, never from its extension.
#include "converter.h"
#include "wic.h"
#include <jxl/decode_cxx.h>
#include <jxl/thread_parallel_runner_cxx.h>
#include <libheif/heif.h>
#include <webp/demux.h>
#include <algorithm>
#include <cstring>
#include <mutex>

#define STBI_ONLY_PSD
#define STBI_NO_STDIO
#define STB_IMAGE_IMPLEMENTATION
#pragma warning(push, 0)
#include <stb_image.h>
#pragma warning(pop)

namespace imgconv {
namespace {

// Larger pictures would need gigabytes of RGBA memory.
constexpr uint64_t kMaxPixels = 200ull * 1000 * 1000;

Result Fail(std::string error) { return {false, false, std::move(error)}; }

Result CheckSize(uint64_t width, uint64_t height) {
    if (!width || !height) return Fail("图片尺寸无效");
    if (width * height > kMaxPixels) return Fail("图片超过 2 亿像素，无法转换");
    return {true, false, {}};
}

bool HasBox(const uint8_t* data, size_t size, const char* brands[], size_t count) {
    if (size < 12 || std::memcmp(data + 4, "ftyp", 4) != 0) return false;
    const size_t box = std::min<size_t>(size, static_cast<size_t>(data[0]) << 24 | data[1] << 16 | data[2] << 8 | data[3]);
    for (size_t at = 8; at + 4 <= box; at += 4) {
        if (at == 12) continue;  // minor version
        for (size_t i = 0; i < count; ++i)
            if (std::memcmp(data + at, brands[i], 4) == 0) return true;
    }
    return false;
}

// ---- WIC: PNG, JPEG, GIF, BMP, TIFF, ICO --------------------------------
Result DecodeWic(const std::vector<uint8_t>& bytes, Kind kind, Image& image) {
    wic::Com com;
    auto factory = wic::Factory();
    if (!factory) return Fail("无法启动 Windows 图像组件 (WIC)");
    ComPtr<IWICStream> stream;
    ComPtr<IWICBitmapDecoder> decoder;
    HRESULT hr = factory->CreateStream(&stream);
    if (SUCCEEDED(hr)) hr = stream->InitializeFromMemory(const_cast<BYTE*>(bytes.data()), static_cast<DWORD>(bytes.size()));
    if (SUCCEEDED(hr)) hr = factory->CreateDecoderFromStream(stream.Get(), nullptr, WICDecodeMetadataCacheOnDemand, &decoder);
    if (FAILED(hr)) return Fail("文件已损坏，或 Windows 无法读取此格式");
    UINT frames = 0, chosen = 0;
    decoder->GetFrameCount(&frames);
    if (!frames) return Fail("文件中没有图像");
    if (kind == Kind::Ico) {  // the largest icon size
        UINT best = 0;
        for (UINT i = 0; i < frames; ++i) {
            ComPtr<IWICBitmapFrameDecode> frame;
            UINT w = 0, h = 0;
            if (SUCCEEDED(decoder->GetFrame(i, &frame)) && SUCCEEDED(frame->GetSize(&w, &h)) && w * h > best) {
                best = w * h;
                chosen = i;
            }
        }
    }
    ComPtr<IWICBitmapFrameDecode> frame;
    UINT width = 0, height = 0;
    if (FAILED(decoder->GetFrame(chosen, &frame)) || FAILED(frame->GetSize(&width, &height)))
        return Fail("无法读取图像帧");
    if (auto check = CheckSize(width, height); !check.ok) return check;

    image.icc = wic::ReadIcc(factory.Get(), frame.Get());
    int orientation = 1;
    if (kind == Kind::Jpeg) {
        image.exif = JpegExif(bytes.data(), bytes.size());
        orientation = ExifOrientation(image.exif);
        image.jpeg = bytes;
    } else if (kind == Kind::Png) {
        image.exif = PngExif(bytes.data(), bytes.size());
        orientation = ExifOrientation(image.exif);
    } else if (kind == Kind::Tiff) {
        orientation = wic::ReadTiffOrientation(frame.Get());
    }

    image.width = width;
    image.height = height;
    image.rgba.resize(static_cast<size_t>(width) * height * 4);
    if (!wic::CopyRgba(factory.Get(), frame.Get(), width, height, image.rgba)) return Fail("无法转换图像像素");
    ApplyOrientation(image, orientation);
    return {true, false, {}};
}

// ---- WebP (still and animated: the first frame) -------------------------
Result DecodeWebp(const std::vector<uint8_t>& bytes, Image& image) {
    WebPData data{bytes.data(), bytes.size()};
    WebPAnimDecoderOptions options;
    if (!WebPAnimDecoderOptionsInit(&options)) return Fail("WebP 解码器版本不匹配");
    options.color_mode = MODE_RGBA;
    options.use_threads = 0;
    WebPAnimDecoder* decoder = WebPAnimDecoderNew(&data, &options);
    if (!decoder) return Fail("WebP 文件已损坏");
    WebPAnimInfo info{};
    uint8_t* pixels = nullptr;
    int timestamp = 0;
    Result result{true, false, {}};
    if (!WebPAnimDecoderGetInfo(decoder, &info) || !(result = CheckSize(info.canvas_width, info.canvas_height)).ok) {
        if (result.ok) result = Fail("WebP 文件已损坏");
    } else if (!WebPAnimDecoderGetNext(decoder, &pixels, &timestamp) || !pixels) {
        result = Fail("WebP 图像无法解码");
    } else {
        image.width = info.canvas_width;
        image.height = info.canvas_height;
        image.rgba.assign(pixels, pixels + static_cast<size_t>(image.width) * image.height * 4);
        const WebPDemuxer* demux = WebPAnimDecoderGetDemuxer(decoder);
        WebPChunkIterator chunk;
        if (WebPDemuxGetChunk(demux, "ICCP", 1, &chunk)) {
            image.icc.assign(chunk.chunk.bytes, chunk.chunk.bytes + chunk.chunk.size);
            WebPDemuxReleaseChunkIterator(&chunk);
        }
        if (WebPDemuxGetChunk(demux, "EXIF", 1, &chunk)) {
            const uint8_t* p = chunk.chunk.bytes;
            size_t n = chunk.chunk.size;
            if (n > 6 && std::memcmp(p, "Exif\0\0", 6) == 0) { p += 6; n -= 6; }
            image.exif.assign(p, p + n);
            WebPDemuxReleaseChunkIterator(&chunk);
        }
    }
    WebPAnimDecoderDelete(decoder);
    return result;
}

// ---- HEIC / AVIF through libheif ----------------------------------------
Result DecodeHeif(const std::vector<uint8_t>& bytes, Image& image) {
    static std::once_flag once;
    std::call_once(once, [] { heif_init(nullptr); });
    heif_context* context = heif_context_alloc();
    heif_image_handle* handle = nullptr;
    heif_image* decoded = nullptr;
    heif_decoding_options* options = heif_decoding_options_alloc();
    Result result{true, false, {}};
    heif_error error = heif_context_read_from_memory_without_copy(context, bytes.data(), bytes.size(), nullptr);
    if (error.code == heif_error_Ok) error = heif_context_get_primary_image_handle(context, &handle);
    if (error.code != heif_error_Ok) {
        result = Fail(std::string("HEIC/AVIF 文件无法读取：") + (error.message ? error.message : ""));
    } else if (!(result = CheckSize(heif_image_handle_get_width(handle), heif_image_handle_get_height(handle))).ok) {
    } else {
        // Transformations (rotation, mirror, crop) are applied by default.
        error = heif_decode_image(handle, &decoded, heif_colorspace_RGB, heif_chroma_interleaved_RGBA, options);
        if (error.code != heif_error_Ok) {
            result = Fail(std::string("HEIC/AVIF 图像无法解码：") + (error.message ? error.message : ""));
        } else {
            int stride = 0;
            const uint8_t* plane = heif_image_get_plane_readonly(decoded, heif_channel_interleaved, &stride);
            const int width = heif_image_get_width(decoded, heif_channel_interleaved);
            const int height = heif_image_get_height(decoded, heif_channel_interleaved);
            if (!plane || width <= 0 || height <= 0 || stride < width * 4) {
                result = Fail("HEIC/AVIF 图像格式不受支持");
            } else {
                image.width = static_cast<uint32_t>(width);
                image.height = static_cast<uint32_t>(height);
                image.rgba.resize(static_cast<size_t>(width) * height * 4);
                for (int y = 0; y < height; ++y)
                    std::memcpy(&image.rgba[static_cast<size_t>(y) * width * 4], plane + static_cast<size_t>(y) * stride,
                                static_cast<size_t>(width) * 4);
                image.orientation_applied = true;
                const auto profile = heif_image_handle_get_color_profile_type(handle);
                if (profile == heif_color_profile_type_prof || profile == heif_color_profile_type_rICC) {
                    image.icc.resize(heif_image_handle_get_raw_color_profile_size(handle));
                    if (image.icc.empty() ||
                        heif_image_handle_get_raw_color_profile(handle, image.icc.data()).code != heif_error_Ok)
                        image.icc.clear();
                }
                heif_item_id id = 0;
                if (heif_image_handle_get_list_of_metadata_block_IDs(handle, "Exif", &id, 1) == 1) {
                    std::vector<uint8_t> block(heif_image_handle_get_metadata_size(handle, id));
                    if (block.size() > 4 && heif_image_handle_get_metadata(handle, id, block.data()).code == heif_error_Ok) {
                        // A 4-byte big-endian offset to the TIFF header comes first.
                        const size_t skip = 4 + (static_cast<size_t>(block[0]) << 24 | block[1] << 16 | block[2] << 8 | block[3]);
                        if (skip < block.size()) image.exif.assign(block.begin() + static_cast<std::ptrdiff_t>(skip), block.end());
                    }
                }
            }
        }
    }
    if (decoded) heif_image_release(decoded);
    heif_decoding_options_free(options);
    if (handle) heif_image_handle_release(handle);
    heif_context_free(context);
    return result;
}

// ---- JPEG XL (the first frame) ------------------------------------------
Result DecodeJxl(const std::vector<uint8_t>& bytes, Image& image) {
    auto decoder = JxlDecoderMake(nullptr);
    auto runner = JxlThreadParallelRunnerMake(nullptr, std::min<size_t>(4, JxlThreadParallelRunnerDefaultNumWorkerThreads()));
    if (!decoder || !runner) return Fail("JPEG XL 解码器无法启动");
    JxlDecoder* dec = decoder.get();
    if (JxlDecoderSetParallelRunner(dec, JxlThreadParallelRunner, runner.get()) != JXL_DEC_SUCCESS ||
        JxlDecoderSubscribeEvents(dec, JXL_DEC_BASIC_INFO | JXL_DEC_COLOR_ENCODING | JXL_DEC_FULL_IMAGE | JXL_DEC_BOX) != JXL_DEC_SUCCESS ||
        JxlDecoderSetDecompressBoxes(dec, JXL_TRUE) != JXL_DEC_SUCCESS)
        return Fail("JPEG XL 解码器无法启动");
    JxlDecoderSetInput(dec, bytes.data(), bytes.size());
    JxlDecoderCloseInput(dec);
    const JxlPixelFormat format{4, JXL_TYPE_UINT8, JXL_NATIVE_ENDIAN, 0};
    JxlBasicInfo info{};
    std::vector<uint8_t> box;
    bool reading_exif = false;
    const auto finish_box = [&] {
        if (!reading_exif) return;
        box.resize(box.size() - JxlDecoderReleaseBoxBuffer(dec));
        // The Exif box starts with a 4-byte offset to the TIFF header.
        if (box.size() > 4) {
            const size_t skip = 4 + (static_cast<size_t>(box[0]) << 24 | box[1] << 16 | box[2] << 8 | box[3]);
            if (skip < box.size()) image.exif.assign(box.begin() + static_cast<std::ptrdiff_t>(skip), box.end());
        }
        reading_exif = false;
    };
    for (;;) {
        const JxlDecoderStatus status = JxlDecoderProcessInput(dec);
        if (status != JXL_DEC_BOX_NEED_MORE_OUTPUT) finish_box();
        switch (status) {
        case JXL_DEC_ERROR: return Fail("JPEG XL 文件已损坏");
        case JXL_DEC_NEED_MORE_INPUT: return Fail("JPEG XL 文件不完整");
        case JXL_DEC_BASIC_INFO: {
            if (JxlDecoderGetBasicInfo(dec, &info) != JXL_DEC_SUCCESS) return Fail("JPEG XL 文件已损坏");
            if (auto check = CheckSize(info.xsize, info.ysize); !check.ok) return check;
            const bool swap = info.orientation >= JXL_ORIENT_TRANSPOSE;  // applied by the decoder
            image.width = swap ? info.ysize : info.xsize;
            image.height = swap ? info.xsize : info.ysize;
            break;
        }
        case JXL_DEC_COLOR_ENCODING: {
            size_t size = 0;
            if (JxlDecoderGetICCProfileSize(dec, JXL_COLOR_PROFILE_TARGET_DATA, &size) == JXL_DEC_SUCCESS && size) {
                image.icc.resize(size);
                if (JxlDecoderGetColorAsICCProfile(dec, JXL_COLOR_PROFILE_TARGET_DATA, image.icc.data(), size) != JXL_DEC_SUCCESS)
                    image.icc.clear();
            }
            break;
        }
        case JXL_DEC_NEED_IMAGE_OUT_BUFFER: {
            size_t size = 0;
            if (JxlDecoderImageOutBufferSize(dec, &format, &size) != JXL_DEC_SUCCESS ||
                size != static_cast<size_t>(image.width) * image.height * 4)
                return Fail("JPEG XL 图像尺寸异常");
            image.rgba.resize(size);
            if (JxlDecoderSetImageOutBuffer(dec, &format, image.rgba.data(), size) != JXL_DEC_SUCCESS)
                return Fail("JPEG XL 图像无法解码");
            break;
        }
        case JXL_DEC_BOX: {
            JxlBoxType type{};
            if (JxlDecoderGetBoxType(dec, type, JXL_TRUE) == JXL_DEC_SUCCESS && std::memcmp(type, "Exif", 4) == 0) {
                box.assign(65536, 0);
                if (JxlDecoderSetBoxBuffer(dec, box.data(), box.size()) == JXL_DEC_SUCCESS) reading_exif = true;
            }
            break;
        }
        case JXL_DEC_BOX_NEED_MORE_OUTPUT: {
            const size_t used = box.size() - JxlDecoderReleaseBoxBuffer(dec);
            if (box.size() >= (16u << 20)) { reading_exif = false; box.clear(); break; }
            box.resize(box.size() * 2);
            // The first `used` bytes are kept; the decoder continues after them.
            if (JxlDecoderSetBoxBuffer(dec, box.data() + used, box.size() - used) != JXL_DEC_SUCCESS) {
                reading_exif = false;
                box.clear();
            }
            break;
        }
        case JXL_DEC_FULL_IMAGE:  // first frame done: animations stop here
        case JXL_DEC_SUCCESS:
            if (image.rgba.empty()) return Fail("JPEG XL 文件中没有图像");
            image.orientation_applied = true;
            return {true, false, {}};
        default:
            break;
        }
    }
}

// ---- PSD: the saved composite image -------------------------------------
Result DecodePsd(const std::vector<uint8_t>& bytes, Image& image) {
    if (bytes.size() > 0x7FFFFFFF) return Fail("PSD 文件过大");
    int width = 0, height = 0, channels = 0;
    // stb_image reads the "maximize compatibility" composite: RGB or gray, 8/16-bit.
    stbi_uc* pixels = stbi_load_from_memory(bytes.data(), static_cast<int>(bytes.size()), &width, &height, &channels, 4);
    if (!pixels) {
        return Fail("PSD 无法读取：只支持带合成图的 RGB/灰度 PSD"
                    "（在 Photoshop 中保存时勾选“最大兼容”）");
    }
    Result result = CheckSize(static_cast<uint64_t>(width), static_cast<uint64_t>(height));
    if (result.ok) {
        image.width = static_cast<uint32_t>(width);
        image.height = static_cast<uint32_t>(height);
        image.rgba.assign(pixels, pixels + static_cast<size_t>(width) * height * 4);
    }
    stbi_image_free(pixels);
    return result;
}

}  // namespace

bool Image::HasAlpha() const {
    for (size_t i = 3; i < rgba.size(); i += 4)
        if (rgba[i] != 255) return true;
    return false;
}

Kind Detect(const uint8_t* d, size_t n) {
    if (n >= 8 && std::memcmp(d, "\x89PNG\r\n\x1a\n", 8) == 0) return Kind::Png;
    if (n >= 3 && d[0] == 0xFF && d[1] == 0xD8 && d[2] == 0xFF) return Kind::Jpeg;
    if (n >= 6 && (std::memcmp(d, "GIF87a", 6) == 0 || std::memcmp(d, "GIF89a", 6) == 0)) return Kind::Gif;
    if (n >= 2 && d[0] == 'B' && d[1] == 'M') return Kind::Bmp;
    if (n >= 4 && (std::memcmp(d, "II*\0", 4) == 0 || std::memcmp(d, "MM\0*", 4) == 0)) return Kind::Tiff;
    if (n >= 4 && d[0] == 0 && d[1] == 0 && d[2] == 1 && d[3] == 0) return Kind::Ico;
    if (n >= 12 && std::memcmp(d, "RIFF", 4) == 0 && std::memcmp(d + 8, "WEBP", 4) == 0) return Kind::Webp;
    if (n >= 2 && d[0] == 0xFF && d[1] == 0x0A) return Kind::Jxl;
    if (n >= 12 && std::memcmp(d, "\0\0\0\x0CJXL \r\n\x87\n", 12) == 0) return Kind::Jxl;
    if (n >= 4 && std::memcmp(d, "8BPS", 4) == 0) return Kind::Psd;
    static const char* heif[] = {"heic", "heix", "heim", "heis", "hevc", "hevx", "mif1", "msf1", "avif", "avis", "mif2"};
    if (HasBox(d, n, heif, std::size(heif))) return Kind::Heif;
    return Kind::Unknown;
}

Result Decode(const std::vector<uint8_t>& bytes, Image& image) {
    image = {};
    switch (Detect(bytes.data(), bytes.size())) {
    case Kind::Png: return DecodeWic(bytes, Kind::Png, image);
    case Kind::Jpeg: return DecodeWic(bytes, Kind::Jpeg, image);
    case Kind::Gif: return DecodeWic(bytes, Kind::Gif, image);
    case Kind::Bmp: return DecodeWic(bytes, Kind::Bmp, image);
    case Kind::Tiff: return DecodeWic(bytes, Kind::Tiff, image);
    case Kind::Ico: return DecodeWic(bytes, Kind::Ico, image);
    case Kind::Webp: return DecodeWebp(bytes, image);
    case Kind::Heif: return DecodeHeif(bytes, image);
    case Kind::Jxl: return DecodeJxl(bytes, image);
    case Kind::Psd: return DecodePsd(bytes, image);
    default: return Fail("不是受支持的图片格式");
    }
}

}  // namespace imgconv
