// wic.cpp - see wic.h.
#include "wic.h"

namespace imgconv::wic {

ComPtr<IWICImagingFactory> Factory() {
    ComPtr<IWICImagingFactory> factory;
    if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&factory))))
        factory.Reset();
    return factory;
}

std::vector<uint8_t> ReadIcc(IWICImagingFactory* factory, IWICBitmapFrameDecode* frame) {
    UINT count = 0;
    if (FAILED(frame->GetColorContexts(0, nullptr, &count)) || !count || count > 16) return {};
    std::vector<ComPtr<IWICColorContext>> owned(count);
    std::vector<IWICColorContext*> contexts(count);
    for (UINT i = 0; i < count; ++i) {
        if (FAILED(factory->CreateColorContext(&owned[i]))) return {};
        contexts[i] = owned[i].Get();
    }
    if (FAILED(frame->GetColorContexts(count, contexts.data(), &count))) return {};
    for (UINT i = 0; i < count; ++i) {
        WICColorContextType type{};
        UINT size = 0;
        if (FAILED(contexts[i]->GetType(&type)) || type != WICColorContextProfile) continue;
        if (FAILED(contexts[i]->GetProfileBytes(0, nullptr, &size)) || !size) continue;
        std::vector<uint8_t> icc(size);
        if (SUCCEEDED(contexts[i]->GetProfileBytes(size, icc.data(), &size))) return icc;
    }
    return {};
}

int ReadTiffOrientation(IWICBitmapFrameDecode* frame) {
    ComPtr<IWICMetadataQueryReader> reader;
    if (FAILED(frame->GetMetadataQueryReader(&reader))) return 1;
    PROPVARIANT value;
    PropVariantInit(&value);
    int orientation = 1;
    if (SUCCEEDED(reader->GetMetadataByName(L"/ifd/{ushort=274}", &value)) && value.vt == VT_UI2 &&
        value.uiVal >= 1 && value.uiVal <= 8)
        orientation = value.uiVal;
    PropVariantClear(&value);
    return orientation;
}

bool CopyRgba(IWICImagingFactory* factory, IWICBitmapSource* source, uint32_t width, uint32_t height,
              std::vector<uint8_t>& out) {
    const UINT stride = width * 4;
    const UINT size = stride * height;
    // RGBA directly where WIC can; BGRA (every source converts to it) otherwise.
    for (const bool bgra : {false, true}) {
        ComPtr<IWICFormatConverter> converter;
        if (FAILED(factory->CreateFormatConverter(&converter))) return false;
        const WICPixelFormatGUID target = bgra ? GUID_WICPixelFormat32bppBGRA : GUID_WICPixelFormat32bppRGBA;
        if (FAILED(converter->Initialize(source, target, WICBitmapDitherTypeNone, nullptr, 0.0, WICBitmapPaletteTypeCustom)))
            continue;
        if (FAILED(converter->CopyPixels(nullptr, stride, size, out.data()))) continue;
        if (bgra)
            for (size_t i = 0; i < out.size(); i += 4) std::swap(out[i], out[i + 2]);
        return true;
    }
    return false;
}

}  // namespace imgconv::wic
