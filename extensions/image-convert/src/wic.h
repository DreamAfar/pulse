// wic.h - Small helpers around the Windows Imaging Component.
#pragma once
#include <windows.h>
#include <wincodec.h>
#include <wrl/client.h>
#include <cstdint>
#include <vector>

using Microsoft::WRL::ComPtr;

namespace imgconv::wic {

// COM for the calling thread; tolerates a thread that already chose another
// apartment (the factory works in either).
struct Com {
    HRESULT hr;
    Com() : hr(CoInitializeEx(nullptr, COINIT_MULTITHREADED)) {}
    ~Com() { if (SUCCEEDED(hr)) CoUninitialize(); }
    Com(const Com&) = delete;
    Com& operator=(const Com&) = delete;
};

ComPtr<IWICImagingFactory> Factory();
std::vector<uint8_t> ReadIcc(IWICImagingFactory* factory, IWICBitmapFrameDecode* frame);
int ReadTiffOrientation(IWICBitmapFrameDecode* frame);
// Straight RGBA, top row first; `out` is already width * height * 4 bytes.
bool CopyRgba(IWICImagingFactory* factory, IWICBitmapSource* source, uint32_t width, uint32_t height,
              std::vector<uint8_t>& out);

}  // namespace imgconv::wic
