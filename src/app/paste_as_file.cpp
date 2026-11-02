// paste_as_file.cpp - see paste_as_file.h.
#include "paste_as_file.h"
#include "app_internal.h"
#include "app_messages.h"
#include "context_menu.h"
#include "create_rename_intent.h"
#include "../common/localization.h"
#include "../ops/clipboard.h"
#include "../ops/clipboard_image.h"
#include "../ops/ops_manager.h"
#include <cstdio>
#include <memory>
#include <thread>
#include <utility>

namespace pulse {
namespace {

void ReportFailure(AppState& s) {
    s.notification_toast.ShowSnack(s.hwnd, ui::SnackIcon::Suggest,
        l10n::Get(l10n::StringId::PasteAsFileFailed), {}, {}, {}, 3000);
}

std::wstring DefaultStem() {
    SYSTEMTIME now{};
    GetLocalTime(&now);
    wchar_t stamp[32]{};
    swprintf_s(stamp, L" %04u-%02u-%02u %02u%02u%02u", now.wYear, now.wMonth, now.wDay,
               now.wHour, now.wMinute, now.wSecond);
    return l10n::Get(l10n::StringId::PasteFileNamePrefix) + stamp;
}

// As 新建文件: one undoable task; renames in place while the folder is still
// on screen. A slow encode can finish after the user moved on, and the file
// still goes where they pasted it.
void CreatePastedFile(AppState& s, const std::wstring& dir, uint64_t view_generation,
                const std::wstring& ext, std::vector<uint8_t> data) {
    std::wstring path = dir;
    if (!path.ends_with(L"\\")) path += L"\\";
    path += app::UniqueChildName(dir, DefaultStem(), ext);
    ops::OpRequest req;
    req.type = ops::OpType::CreateTextFile;
    req.sources.push_back(std::move(path));
    req.template_data = std::move(data);
    const auto task_id = s.ops.Submit(std::move(req));
    app::Tab* tab = ActiveTab(s);
    if (tab && tab->current_path == dir && tab->view_generation == view_generation)
        app::QueueCreateRenameIntent(tab->create_rename_intents, task_id, dir, view_generation);
}

} // namespace

void PasteClipboardTextAsFile(AppState& s, const std::wstring& dir) {
    const app::Tab* tab = ActiveTab(s);
    std::vector<uint8_t> text;
    if (!tab || !ops::ReadClipboardTextUtf8(text)) {
        ReportFailure(s);
        return;
    }
    CreatePastedFile(s, dir, tab->view_generation, L".txt", std::move(text));
}

void PasteClipboardImageAsFile(AppState& s, const std::wstring& dir) {
    const app::Tab* tab = ActiveTab(s);
    ops::ClipboardImage image;
    if (!tab || !ops::ReadClipboardImage(image)) {
        ReportFailure(s);
        return;
    }
    if (!image.is_dib) {
        CreatePastedFile(s, dir, tab->view_generation, L".png", std::move(image.data));
        return;
    }
    auto job = std::make_unique<PastedImageResult>();
    job->dir = dir;
    job->view_generation = tab->view_generation;
    job->data = std::move(image.data);
    const HWND hwnd = s.hwnd;
    std::thread([hwnd, job = std::move(job)]() mutable {
        std::vector<uint8_t> png;
        job->ok = ops::EncodeDibAsPng(job->data, png);
        job->data = std::move(png);
        if (PostMessageW(hwnd, WM_PASTE_IMAGE_DONE, 0, reinterpret_cast<LPARAM>(job.get())))
            job.release();  // WM_PASTE_IMAGE_DONE owns it now
    }).detach();
}

void ApplyPastedImage(AppState& s, PastedImageResult& result) {
    if (!result.ok) {
        ReportFailure(s);
        return;
    }
    CreatePastedFile(s, result.dir, result.view_generation, L".png", std::move(result.data));
}

} // namespace pulse
