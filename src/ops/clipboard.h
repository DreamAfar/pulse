// clipboard.h — System clipboard interop with Explorer (CF_HDROP + drop effect).
#pragma once
#include <windows.h>
#include <cstdint>
#include <string>
#include <vector>

namespace pulse::ops {

struct ClipboardData {
    std::vector<std::wstring> paths;
    bool cut = false; // CFSTR_PREFERREDDROPEFFECT == DROPEFFECT_MOVE
    uint32_t sequence = 0;
};

// CFSTR_PREFERREDDROPEFFECT -> "is this a cut?". Only an exact MOVE counts:
// many applications publish COPY|MOVE (3) to mean "either is fine", and a
// bitwise test reads that as a cut, so pasting would move - and delete - the
// user's originals instead of copying them.
bool PreferredEffectIsCut(uint32_t preferred_effect);

// Writes paths as CF_HDROP + CFSTR_PREFERREDDROPEFFECT so Explorer can paste them.
// Must be called on the UI thread (OpenClipboard affinity).
bool WriteClipboard(const std::vector<std::wstring>& paths, bool cut);

// Writes plain text (Ctrl+Shift+C "copy path"). UI thread only.
bool WriteClipboardText(const std::wstring& text);

// What 粘贴为文件 can make from the clipboard. Both stay false while it holds
// files: those paste as themselves.
struct ClipboardFileKinds {
    bool text = false;
    bool image = false;
};
ClipboardFileKinds AvailableClipboardFileKinds();

// UTF-8 without a BOM. False when there is no text, or it is empty. UI thread.
bool ReadClipboardTextUtf8(std::vector<uint8_t>& out);

// The app's own PNG when it published one (browsers do; it keeps transparency),
// otherwise the DIB (screenshots), which still needs EncodeDibAsPng. UI thread.
struct ClipboardImage {
    std::vector<uint8_t> data;
    bool is_dib = false;
};
bool ReadClipboardImage(ClipboardImage& out);

// Reads an Explorer copy/cut (CF_HDROP). Returns false if no files present.
bool ReadClipboard(ClipboardData& out);

// Files with no path to read: copied out of a zip, a Remote Desktop session,
// Outlook... They come as Shell IDList Array / FileGroupDescriptorW (virtual
// files) without CF_HDROP, so ReadClipboard sees nothing (#109). Inline:
// the view model reads it in targets that do not link this file.
inline bool ClipboardHasShellFiles() {
    return IsClipboardFormatAvailable(RegisterClipboardFormatW(L"Shell IDList Array")) ||
           IsClipboardFormatAvailable(RegisterClipboardFormatW(L"FileGroupDescriptorW"));
}

// Ctrl+V has something to paste: paths, or the shell formats above.
inline bool ClipboardHasPasteableFiles() {
    return IsClipboardFormatAvailable(CF_HDROP) || ClipboardHasShellFiles();
}

// Pastes the clipboard into `dir` the way Explorer does: a simulated drop on
// the folder's shell drop target, which writes virtual files out and shows
// its own progress. For what ReadClipboard cannot read. UI thread (OLE).
bool PasteShellClipboard(const std::wstring& dir);

// A phone's items (shell parsing names, "::{...}") on the clipboard the
// way Explorer puts them there: the shell's own data object for the items
// (Shell IDList Array, file contents) with the copy / cut effect. Owned by
// this process, so it stays pasteable while Pulse runs, wherever the paste
// happens (Pulse, Explorer, the desktop). UI thread (OLE).
bool WriteShellItemsClipboard(const std::vector<std::wstring>& shell_ids, bool cut);

// Shell items on the clipboard with no CF_HDROP paths (a phone's, copied in
// Pulse or in Explorer): their parsing names and the cut flag. False when
// there are no such items, or when CF_HDROP paths are there (ReadClipboard).
// Only "::" shell ids are returned: zip contents and other virtual files
// stay with PasteShellClipboard. UI thread (OLE).
bool ReadClipboardShellItems(std::vector<std::wstring>& shell_ids, bool& cut);

// Clears a completed cut only if the clipboard still contains the exact cut
// captured by ReadClipboard. This never clears a newer clipboard value.
bool CompleteCutClipboard(uint32_t sequence, const std::vector<std::wstring>& expected_paths);

} // namespace pulse::ops
