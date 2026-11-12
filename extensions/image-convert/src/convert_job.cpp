// convert_job.cpp - see convert_job.h.
#include "convert_job.h"
#include <pulse/sdk.hpp>
#include <windows.h>
#include <algorithm>
#include <filesystem>
#include <functional>
#include <mutex>
#include <thread>

using pulse::sdk::View;

namespace {

std::string Utf8(const std::wstring& value) {
    if (value.empty()) return {};
    const int count = WideCharToMultiByte(CP_UTF8, 0, value.data(), static_cast<int>(value.size()), nullptr, 0, nullptr, nullptr);
    std::string text(static_cast<size_t>(std::max(count, 0)), '\0');
    if (count) WideCharToMultiByte(CP_UTF8, 0, value.data(), static_cast<int>(value.size()), text.data(), count, nullptr, nullptr);
    return text;
}

std::wstring Wide(const std::string& value) {
    if (value.empty()) return {};
    const int count = MultiByteToWideChar(CP_UTF8, 0, value.data(), static_cast<int>(value.size()), nullptr, 0);
    std::wstring text(static_cast<size_t>(std::max(count, 0)), L'\0');
    if (count) MultiByteToWideChar(CP_UTF8, 0, value.data(), static_cast<int>(value.size()), text.data(), count);
    return text;
}

std::wstring Lower(std::wstring text) {
    if (!text.empty())
        CharLowerBuffW(text.data(), static_cast<DWORD>(text.size()));
    return text;
}

struct Stage {
    PulseStagingV1 api{};
    std::atomic<bool> created{false};
    PulseStatus status = PULSE_ERROR_STATE;
    PulseStageId id = 0;
    std::string path;
    static void PULSE_CALL Created(void* value, PulseStatus status, PulseStageId stage, PulseStringView path) noexcept {
        auto& self = *static_cast<Stage*>(value);
        self.id = stage;
        self.status = status;
        try { if (path.data && path.length) self.path.assign(path.data, path.length); }
        catch (...) { self.status = PULSE_ERROR_INITIALIZATION; }
        self.created.store(true, std::memory_order_release);
    }
    struct Snapshot {
        PulseStageState state = PULSE_STAGE_PREPARING;
        std::string error;
        static void PULSE_CALL Read(void* value, const PulseStageSnapshot* snapshot) noexcept {
            auto& self = *static_cast<Snapshot*>(value);
            self.state = snapshot->state;
            try { if (snapshot->error.data && snapshot->error.length) self.error.assign(snapshot->error.data, snapshot->error.length); }
            catch (...) { self.error.clear(); }
        }
    };
    bool Get(Snapshot& snapshot) { return api.get(api.context, id, Snapshot::Read, &snapshot) == PULSE_OK; }
    // Waits for a submitted commit to finish; cancels it when asked.
    PulseStageState WaitCommit(const std::function<bool()>& cancelled, std::string& error) {
        bool cancel_sent = false;
        for (;;) {
            if (!cancel_sent && cancelled()) { api.cancel(api.context, id); cancel_sent = true; }
            Snapshot snapshot;
            if (!Get(snapshot)) { error = "无法读取发布状态"; return PULSE_STAGE_FAILED; }
            if (snapshot.state >= PULSE_STAGE_COMPLETED) { error = snapshot.error; return snapshot.state; }
            Sleep(20);
        }
    }
};

}  // namespace

std::wstring OutputName(const std::wstring& source, imgconv::Format format, std::vector<std::wstring>& taken) {
    const std::wstring stem = std::filesystem::path(source).stem().wstring();
    const std::wstring extension = imgconv::Extension(format);
    for (int n = 1;; ++n) {
        std::wstring name = n == 1 ? stem + extension : stem + L" (" + std::to_wstring(n) + L")" + extension;
        const std::wstring key = Lower(name);
        if (std::find(taken.begin(), taken.end(), key) == taken.end()) {
            taken.push_back(key);
            return name;
        }
    }
}

ConvertJob::ConvertJob(std::vector<std::wstring> sources, imgconv::Options options, PulseStagingV1 staging)
    : sources_(std::move(sources)), options_(options), staging_(staging) {}

bool ConvertJob::Cancelled(const PulseTaskContext& task) {
    if (!cancel_ && task.cancelled(task.context)) cancel_ = true;
    return cancel_;
}

PulseTaskState ConvertJob::Run(const PulseTaskContext& task) {
    // Files of one folder share a stage; search results can span folders.
    std::vector<std::pair<std::wstring, std::vector<std::wstring>>> groups;
    for (const auto& source : sources_) {
        const std::wstring parent = std::filesystem::path(source).parent_path().wstring();
        auto it = std::find_if(groups.begin(), groups.end(), [&](const auto& group) { return Lower(group.first) == Lower(parent); });
        if (it == groups.end()) groups.push_back({parent, {source}});
        else it->second.push_back(source);
    }

    const uint64_t total = sources_.size();
    std::atomic<uint64_t> done{0};
    std::mutex mutex;  // failures, current
    std::vector<std::string> failures;
    std::string current;
    size_t converted = 0, published = 0;
    bool any_stage_problem = false;
    std::string stage_error;

    PulseTaskProgress progress{};
    progress.size = sizeof(progress);
    progress.total_items = total;
    uint32_t last_percent = 0;
    const auto report = [&](uint32_t percent, const std::string& item) {
        last_percent = std::max(last_percent, std::min<uint32_t>(percent, 100));
        progress.percent = last_percent;
        progress.completed_items = std::min<uint64_t>(done.load(), total);
        progress.current_item = View(item);
        task.report(task.context, &progress);
        progress.current_item = {};
    };
    const auto cancelled = [&] { return Cancelled(task); };
    // Converting fills 0-90%; the host's publication finishes the rest.
    const auto ConvertPercent = [&] {
        return static_cast<uint32_t>(std::min<uint64_t>(done.load(), total) * 90 / std::max<uint64_t>(total, 1));
    };
    report(0, {});

    for (auto& [folder, files] : groups) {
        if (cancelled()) break;
        Stage stage;
        stage.api = staging_;
        const std::string destination = Utf8(folder);
        if (staging_.create(staging_.context, View(destination), Stage::Created, &stage) != PULSE_OK) {
            std::lock_guard lock(mutex);
            for (const auto& file : files) failures.push_back(Utf8(std::filesystem::path(file).filename().wstring()) + "（无法在此位置发布）");
            done += files.size();
            continue;
        }
        // An accepted request always calls back, even after cancellation.
        while (!stage.created.load(std::memory_order_acquire)) Sleep(10);
        if (stage.status != PULSE_OK || !stage.id || stage.path.empty()) {
            std::lock_guard lock(mutex);
            for (const auto& file : files) failures.push_back(Utf8(std::filesystem::path(file).filename().wstring()) + "（无法创建暂存目录，可能是网络位置）");
            done += files.size();
            if (stage.id) staging_.release(staging_.context, stage.id);
            continue;
        }
        const std::filesystem::path stage_dir = Wide(stage.path);

        std::vector<std::wstring> taken;
        std::vector<std::wstring> outputs;
        for (const auto& file : files) outputs.push_back(OutputName(file, options_.format, taken));

        // A few files at a time; each decoder may use more threads itself.
        std::atomic<size_t> next{0};
        std::atomic<size_t> group_converted{0};
        const size_t workers = std::min<size_t>({files.size(), 4, std::max(1u, std::thread::hardware_concurrency() / 2)});
        std::atomic<size_t> active{workers};
        std::vector<std::thread> threads;
        for (size_t w = 0; w < workers; ++w) {
            threads.emplace_back([&] {
                for (;;) {
                    const size_t i = next++;
                    if (i >= files.size() || cancel_) break;
                    const std::wstring name = std::filesystem::path(files[i]).filename().wstring();
                    const auto result = imgconv::Convert(files[i], (stage_dir / outputs[i]).wstring(), options_,
                                                         [this] { return cancel_.load(); });
                    std::lock_guard lock(mutex);
                    if (result.ok) ++group_converted;
                    else if (!result.cancelled) failures.push_back(Utf8(name) + "（" + result.error + "）");
                    current = Utf8(name);
                    ++done;
                }
                --active;
            });
        }
        while (active.load() > 0) {
            cancelled();
            std::string item;
            { std::lock_guard lock(mutex); item = current; }
            report(ConvertPercent(), item);
            Sleep(50);
        }
        for (auto& thread : threads) thread.join();
        converted += group_converted;

        if (cancel_ || group_converted == 0) {
            staging_.release(staging_.context, stage.id);
            continue;
        }
        if (staging_.commit(staging_.context, stage.id) != PULSE_OK) {
            any_stage_problem = true;
            stage_error = "无法提交转换结果";
            staging_.release(staging_.context, stage.id);
            continue;
        }
        report(ConvertPercent(), "等待确认并保存到原文件夹");
        std::string error;
        const PulseStageState state = stage.WaitCommit(cancelled, error);
        if (state == PULSE_STAGE_COMPLETED) published += group_converted;
        else if (state == PULSE_STAGE_CANCELLED) cancel_ = true;
        else { any_stage_problem = true; stage_error = error.empty() ? "保存到原文件夹失败" : error; }
        staging_.release(staging_.context, stage.id);
    }

    std::string message;
    if (!failures.empty()) {
        message = std::to_string(failures.size()) + " 个文件未转换：";
        for (size_t i = 0; i < failures.size(); ++i) {
            const std::string part = (i ? "；" : "") + failures[i];
            if (message.size() + part.size() > 3800) { message += "……"; break; }
            message += part;
        }
    }
    if (any_stage_problem) message += (message.empty() ? "" : "\n") + stage_error;
    if (!message.empty()) {
        progress.error = View(message);
        task.report(task.context, &progress);
        progress.error = {};
    }
    if (cancel_) return PULSE_TASK_CANCELLED;
    if (published == total) {
        report(100, {});
        return PULSE_TASK_COMPLETED;
    }
    return published > 0 || converted > 0 ? PULSE_TASK_PARTIAL : PULSE_TASK_FAILED;
}
