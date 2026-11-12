// convert_job.h - One "转换图片格式" run: converts the files into a private
// host stage per folder, then hands each stage to Pulse to publish (host
// confirmation, conflicts, undo).
#pragma once
#include "converter.h"
#include <pulse/staging.h>
#include <pulse/tasks.h>
#include <atomic>
#include <string>
#include <vector>

class ConvertJob {
public:
    ConvertJob(std::vector<std::wstring> sources, imgconv::Options options, PulseStagingV1 staging);
    PulseTaskState Run(const PulseTaskContext& task);
    void Cancel() { cancel_ = true; }

private:
    struct Group;
    bool Cancelled(const PulseTaskContext& task);
    std::vector<std::wstring> sources_;
    imgconv::Options options_;
    PulseStagingV1 staging_;
    std::atomic<bool> cancel_{false};
};

// Output name for `source` inside a stage: same stem, new extension, and
// " (2)", " (3)", ... when two sources share a stem. `taken` is lower-case.
std::wstring OutputName(const std::wstring& source, imgconv::Format format, std::vector<std::wstring>& taken);
