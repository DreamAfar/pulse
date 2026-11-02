// paste_as_file.h - 粘贴为文本文件 / 粘贴为图片文件: the clipboard's text or image
// becomes a new file in the folder, from the menu rows or from a plain paste
// when the clipboard holds no files.
#pragma once
#include <cstdint>
#include <string>
#include <vector>

namespace pulse {

struct AppState;

struct PastedImageResult {
    std::wstring dir;
    uint64_t view_generation = 0;
    bool from_dib = false;
    std::vector<uint8_t> data;  // DIB until encoded; PNG once posted back
    bool ok = false;
};

// The caller has checked the folder takes new files.
void PasteClipboardTextAsFile(AppState& s, const std::wstring& dir);
void PasteClipboardImageAsFile(AppState& s, const std::wstring& dir);
// Plain paste: text wins when the clipboard has both, because Office publishes
// a picture of every text copy. Nothing happens when it has neither.
void PasteClipboardContentAsFile(AppState& s, const std::wstring& dir);
// WM_PASTE_IMAGE_DONE: a worker finished encoding.
void ApplyPastedImage(AppState& s, PastedImageResult& result);

} // namespace pulse
