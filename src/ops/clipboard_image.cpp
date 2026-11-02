// clipboard_image.cpp - see clipboard_image.h.
#include "clipboard_image.h"
#include <windows.h>
#include <objidl.h>
#include <wincodec.h>
#include <wrl/client.h>
#include <cstring>

using Microsoft::WRL::ComPtr;

namespace pulse::ops {

bool DibBmpPixelOffset(const uint8_t* dib, size_t size, uint32_t& offset) {
    BITMAPINFOHEADER header{};
    if (!dib || size < sizeof(header)) return false;
    std::memcpy(&header, dib, sizeof(header));
    if (header.biSize < sizeof(header) || header.biSize > size) return false;
    if (header.biWidth <= 0 || header.biHeight == 0 || header.biPlanes != 1) return false;
    const uint32_t bits = header.biBitCount;
    if (bits != 1 && bits != 4 && bits != 8 && bits != 16 && bits != 24 && bits != 32) return false;

    constexpr DWORD kAlphaBitfields = 6;  // BI_ALPHABITFIELDS
    uint64_t masks = 0;
    if (header.biCompression == BI_BITFIELDS || header.biCompression == kAlphaBitfields) {
        if (bits != 16 && bits != 32) return false;
        // Masks follow only the short header; a longer one holds them inside.
        if (header.biSize == sizeof(header))
            masks = header.biCompression == BI_BITFIELDS ? 12 : 16;
    } else if (header.biCompression != BI_RGB) {
        return false;
    }
    uint64_t colors = header.biClrUsed;
    if (colors == 0 && bits <= 8) colors = 1ull << bits;

    const uint64_t pixels_at = uint64_t{header.biSize} + masks + colors * 4;
    const uint64_t height = header.biHeight < 0 ? -int64_t{header.biHeight} : header.biHeight;
    const uint64_t stride = ((uint64_t{static_cast<uint32_t>(header.biWidth)} * bits + 31) / 32) * 4;
    // width and height fit 31 bits and stride is under 2^35: no overflow here
    // for any image that could also fit `size`.
    if (height > (uint64_t{1} << 28) || stride * height > size || pixels_at + stride * height > size) return false;
    const uint64_t file_offset = sizeof(BITMAPFILEHEADER) + pixels_at;
    if (file_offset > 0xFFFFFFFFull) return false;
    offset = static_cast<uint32_t>(file_offset);
    return true;
}

bool EncodeDibAsPng(const std::vector<uint8_t>& dib, std::vector<uint8_t>& png) {
    png.clear();
    uint32_t pixel_offset = 0;
    if (!DibBmpPixelOffset(dib.data(), dib.size(), pixel_offset)) return false;
    const uint64_t bmp_size = uint64_t{sizeof(BITMAPFILEHEADER)} + dib.size();
    if (bmp_size > 0xFFFFFFFFull) return false;

    // WIC's BMP decoder deals with row order, masks and palettes, and reads
    // 32-bit BI_RGB as opaque: a DIB's alpha byte is usually just zero.
    BITMAPFILEHEADER file{};
    file.bfType = 0x4D42;  // "BM"
    file.bfSize = static_cast<DWORD>(bmp_size);
    file.bfOffBits = pixel_offset;
    std::vector<uint8_t> bmp(static_cast<size_t>(bmp_size));
    std::memcpy(bmp.data(), &file, sizeof(file));
    std::memcpy(bmp.data() + sizeof(file), dib.data(), dib.size());

    const HRESULT com = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    bool ok = false;
    {
        ComPtr<IWICImagingFactory> wic;
        ComPtr<IWICStream> input;
        ComPtr<IWICBitmapDecoder> decoder;
        ComPtr<IWICBitmapFrameDecode> source;
        ComPtr<IStream> output;
        ComPtr<IWICBitmapEncoder> encoder;
        ComPtr<IWICBitmapFrameEncode> frame;
        HRESULT hr = CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&wic));
        if (SUCCEEDED(hr)) hr = wic->CreateStream(&input);
        if (SUCCEEDED(hr)) hr = input->InitializeFromMemory(bmp.data(), static_cast<DWORD>(bmp.size()));
        if (SUCCEEDED(hr)) hr = wic->CreateDecoderFromStream(input.Get(), nullptr, WICDecodeMetadataCacheOnDemand, &decoder);
        if (SUCCEEDED(hr)) hr = decoder->GetFrame(0, &source);
        if (SUCCEEDED(hr)) hr = CreateStreamOnHGlobal(nullptr, TRUE, &output);
        if (SUCCEEDED(hr)) hr = wic->CreateEncoder(GUID_ContainerFormatPng, nullptr, &encoder);
        if (SUCCEEDED(hr)) hr = encoder->Initialize(output.Get(), WICBitmapEncoderNoCache);
        if (SUCCEEDED(hr)) hr = encoder->CreateNewFrame(&frame, nullptr);
        if (SUCCEEDED(hr)) hr = frame->Initialize(nullptr);
        if (SUCCEEDED(hr)) hr = frame->WriteSource(source.Get(), nullptr);
        if (SUCCEEDED(hr)) hr = frame->Commit();
        if (SUCCEEDED(hr)) hr = encoder->Commit();
        STATSTG stat{};
        if (SUCCEEDED(hr)) hr = output->Stat(&stat, STATFLAG_NONAME);
        if (SUCCEEDED(hr) && stat.cbSize.QuadPart > 0 && stat.cbSize.HighPart == 0) {
            LARGE_INTEGER start{};
            hr = output->Seek(start, STREAM_SEEK_SET, nullptr);
            png.resize(stat.cbSize.LowPart);
            ULONG read = 0;
            if (SUCCEEDED(hr)) hr = output->Read(png.data(), stat.cbSize.LowPart, &read);
            ok = SUCCEEDED(hr) && read == stat.cbSize.LowPart;
        }
    }
    if (SUCCEEDED(com)) CoUninitialize();
    if (!ok) png.clear();
    return ok;
}

} // namespace pulse::ops
