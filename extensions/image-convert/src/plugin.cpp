// plugin.cpp - "转换图片格式…": one context-menu command for selected
// pictures, a Pulse dialog for the options, then a background task.
#include <windows.h>
#include <pulse/client.hpp>
#include <pulse/commands.h>
#include <pulse/dialogs.h>
#include <pulse/resources.h>
#include <pulse/staging.h>
#include <pulse/tasks.h>
#include "convert_job.h"
#include <algorithm>
#include <array>
#include <cstdlib>
#include <memory>
#include <new>
#include <string>
#include <vector>

namespace {
using pulse::sdk::View;
using N = PulseUiModelNode;

constexpr const char* kSettingsKey = "convert.options";
constexpr int kQualityStops = 11;  // 50, 55, ... 100

std::string Copy(PulseStringView value) { return value.data && value.length ? std::string(value.data, value.length) : std::string{}; }

std::wstring Wide(PulseStringView value) {
    const int count = MultiByteToWideChar(CP_UTF8, 0, value.data, static_cast<int>(value.length), nullptr, 0);
    std::wstring text(static_cast<size_t>(count > 0 ? count : 0), L'\0');
    if (count > 0) MultiByteToWideChar(CP_UTF8, 0, value.data, static_cast<int>(value.length), text.data(), count);
    return text;
}

int QualityIndex(int quality) { return std::clamp((quality - 50 + 2) / 5, 0, kQualityStops - 1); }

struct Plugin {
    pulse::sdk::Host host;
    PulseCommandsV1 commands{};
    PulseDialogsV1 dialogs{};
    PulseTasksV1 tasks_api{};
    PulseStagingV1 staging{};
    PulseResourcesV1 resources{};
    pulse::sdk::CommandRegistration command;
    std::unique_ptr<pulse::sdk::TaskGroup> tasks;
    imgconv::Options options;          // last used, loaded from settings
    uint64_t settings_version = 0;
    PulseResourcesRequestId request = 0;
    uint64_t dialog = 0;
    std::vector<std::wstring> pending;  // the selection the dialog is for
    std::string format = "0", jxl_mode = "0";
    explicit Plugin(const PulseHostV1& value) : host(value) {}

    // ---- settings: "1;format;quality;lossless;background;exif" ----------
    std::string Encode() const {
        return "1;" + std::to_string(static_cast<int>(options.format)) + ";" + std::to_string(options.quality) + ";" +
               (options.jxl_lossless ? "1" : "0") + ";" + std::to_string(static_cast<int>(options.background)) + ";" +
               (options.keep_exif ? "1" : "0");
    }
    void Decode(const std::string& text) {
        std::vector<int> v;
        size_t at = 0;
        while (at <= text.size()) {
            const size_t end = text.find(';', at);
            v.push_back(std::atoi(text.substr(at, end == std::string::npos ? std::string::npos : end - at).c_str()));
            if (end == std::string::npos) break;
            at = end + 1;
        }
        if (v.size() != 6 || v[0] != 1) return;
        options.format = static_cast<imgconv::Format>(std::clamp(v[1], 0, 4));
        options.quality = std::clamp(v[2], 50, 100);
        options.jxl_lossless = v[3] != 0;
        options.background = v[4] ? imgconv::Background::Black : imgconv::Background::White;
        options.keep_exif = v[5] != 0;
    }
    static void PULSE_CALL SettingsDone(void* user, const PulseResourcesReply* reply) noexcept {
        auto& self = *static_cast<Plugin*>(user);
        self.request = 0;
        if (!reply) return;
        if (reply->status != PULSE_OK) {
            // Saved from another window meanwhile: take its version for the next save.
            if (reply->operation == PULSE_RESOURCES_SETTINGS_SET &&
                self.resources.settings_get(self.resources.context, SettingsDone, &self, &self.request) != PULSE_OK)
                self.request = 0;
            return;  // keep defaults / last choice
        }
        try {
            self.settings_version = reply->settings.version;
            for (uint32_t i = 0; i < reply->settings.count; ++i)
                if (Copy(reply->settings.values[i].key) == kSettingsKey && reply->operation == PULSE_RESOURCES_SETTINGS_GET)
                    self.Decode(Copy(reply->settings.values[i].value));
        } catch (...) {}
    }
    void Save() {
        if (request || !resources.settings_set) return;  // a load or save is still running
        const std::string value = Encode();
        if (resources.settings_set(resources.context, View(kSettingsKey), View(value), 0, settings_version,
                                   SettingsDone, this, &request) != PULSE_OK)
            request = 0;
    }

    // ---- dialog -------------------------------------------------------
    std::vector<std::pair<std::string, std::string>> Visibility() const {
        const bool jpeg = format == "1", bmp = format == "2", gif = format == "3", jxl = format == "4";
        const auto flag = [](bool value) { return std::string(value ? "true" : "false"); };
        return {{"show_jxl", flag(jxl)},
                {"show_quality", flag(jpeg || (jxl && jxl_mode == "1"))},
                {"show_background", flag(jpeg || bmp)},
                {"show_exif", flag(!bmp && !gif)},
                {"show_gif", flag(gif)}};
    }
    static void PULSE_CALL Changed(void* user, uint64_t dialog, const PulseUiState* changes, uint32_t count) noexcept {
        auto& self = *static_cast<Plugin*>(user);
        try {
            bool relevant = false;
            for (uint32_t i = 0; i < count; ++i) {
                const auto key = Copy(changes[i].key);
                if (key == "format") { self.format = Copy(changes[i].value); relevant = true; }
                if (key == "jxl_mode") { self.jxl_mode = Copy(changes[i].value); relevant = true; }
            }
            if (!relevant) return;
            const auto values = self.Visibility();
            std::vector<PulseUiState> states;
            for (const auto& [key, value] : values) states.push_back({View(key), View(value)});
            self.dialogs.set_states(self.dialogs.context, dialog, states.data(), static_cast<uint32_t>(states.size()));
        } catch (...) {}
    }
    static void PULSE_CALL Closed(void* user, uint64_t, PulseStatus status, PulseStringView button,
                                  const PulseUiState* states, uint32_t count) noexcept {
        auto& self = *static_cast<Plugin*>(user);
        self.dialog = 0;
        try {
            auto files = std::move(self.pending);
            self.pending.clear();
            if (status != PULSE_OK || Copy(button) != "convert" || files.empty()) return;
            for (uint32_t i = 0; i < count; ++i) {
                const auto key = Copy(states[i].key);
                const int value = std::atoi(Copy(states[i].value).c_str());
                if (key == "format") self.options.format = static_cast<imgconv::Format>(std::clamp(value, 0, 4));
                else if (key == "jxl_mode") self.options.jxl_lossless = value == 0;
                else if (key == "quality") self.options.quality = 50 + 5 * std::clamp(value, 0, kQualityStops - 1);
                else if (key == "background") self.options.background = value ? imgconv::Background::Black : imgconv::Background::White;
                else if (key == "exif") self.options.keep_exif = Copy(states[i].value) == "true";
            }
            self.Save();
            self.Start(std::move(files));
        } catch (...) {}
    }
    void Start(std::vector<std::wstring> files) {
        tasks->Collect();
        auto job = std::make_shared<ConvertJob>(std::move(files), options, staging);
        PulseTaskId id = 0;
        tasks->Start("转换图片格式", "Image",
                     [job](const PulseTaskContext& context) { return job->Run(context); },
                     [job](PulseTaskId) { job->Cancel(); }, &id);
    }
    void Show(const PulseContextSelectionSnapshot& selection) {
        if (dialog || !selection.entries || !selection.entry_count) return;
        std::vector<std::wstring> files;
        for (uint32_t i = 0; i < selection.entry_count; ++i)
            if (!(selection.entries[i].flags & PULSE_CONTEXT_ENTRY_DIRECTORY)) files.push_back(Wide(selection.entries[i].path));
        if (files.empty()) return;

        format = std::to_string(static_cast<int>(options.format));
        jxl_mode = options.jxl_lossless ? "0" : "1";
        std::vector<std::string> qualities;
        for (int i = 0; i < kQualityStops; ++i) qualities.push_back(std::to_string(50 + 5 * i));
        using pulse::sdk::ModelNode;
        auto root = ModelNode(PULSE_UI_MODEL_STACK, "root").Int(&N::gap, 10).Children({
            ModelNode(PULSE_UI_MODEL_TEXT, "format-label").String(&N::text, "目标格式"),
            ModelNode(PULSE_UI_MODEL_SELECT, "format-input").String(&N::text, "目标格式").String(&N::state, "format")
                .Options({"PNG", "JPEG", "BMP", "GIF", "JPEG XL"}),
            ModelNode(PULSE_UI_MODEL_TEXT, "jxl-label").String(&N::text, "JPEG XL 压缩方式")
                .String(&N::visible_binding, "state.show_jxl"),
            ModelNode(PULSE_UI_MODEL_SELECT, "jxl-input").String(&N::text, "JPEG XL 压缩方式").String(&N::state, "jxl_mode")
                .Options({"无损（JPEG 原图无损重压缩，可还原）", "有损（按质量压缩）"})
                .String(&N::visible_binding, "state.show_jxl"),
            ModelNode(PULSE_UI_MODEL_TEXT, "quality-label").String(&N::text, "质量（越高画质越好、文件越大）")
                .String(&N::visible_binding, "state.show_quality"),
            ModelNode(PULSE_UI_MODEL_SLIDER, "quality-input").String(&N::text, "质量").String(&N::state, "quality")
                .Int(&N::stops, kQualityStops).Options(qualities).String(&N::visible_binding, "state.show_quality"),
            ModelNode(PULSE_UI_MODEL_TEXT, "background-label").String(&N::text, "透明区域填充为")
                .String(&N::visible_binding, "state.show_background"),
            ModelNode(PULSE_UI_MODEL_SELECT, "background-input").String(&N::text, "透明区域填充为").String(&N::state, "background")
                .Options({"白色", "黑色"}).String(&N::visible_binding, "state.show_background"),
            ModelNode(PULSE_UI_MODEL_CHECKBOX, "exif-input").String(&N::text, "保留 EXIF 信息（拍摄时间、相机型号、位置等）")
                .String(&N::state, "exif").String(&N::visible_binding, "state.show_exif"),
            ModelNode(PULSE_UI_MODEL_TEXT, "gif-note").String(&N::text, "GIF 最多 256 色，照片转成 GIF 会明显失真；动图只保留第一帧。")
                .String(&N::visible_binding, "state.show_gif"),
        });
        auto tree = pulse::sdk::ModelTree(std::move(root))
            .State("format", format).State("jxl_mode", jxl_mode)
            .State("quality", std::to_string(QualityIndex(options.quality)))
            .State("background", options.background == imgconv::Background::Black ? "1" : "0")
            .State("exif", options.keep_exif ? "true" : "false");
        for (const auto& [key, value] : Visibility()) tree.State(key, value);

        const std::string message = "已选择 " + std::to_string(files.size()) +
            " 张图片。转换后的文件保存在原图所在的文件夹，原图保留不变。";
        const std::array<PulseDialogButton, 2> buttons{{
            {sizeof(PulseDialogButton), PULSE_DIALOG_BUTTON_PRIMARY, View("convert"), View("转换")},
            {sizeof(PulseDialogButton), PULSE_DIALOG_BUTTON_STANDARD, View("cancel"), View("取消")},
        }};
        PulseDialogRequest dialog_request{};
        dialog_request.size = sizeof(dialog_request);
        dialog_request.width = 460;
        dialog_request.height = 330;
        dialog_request.default_button = 0;
        dialog_request.cancel_button = 1;
        dialog_request.button_count = static_cast<uint32_t>(buttons.size());
        dialog_request.title = View("转换图片格式");
        dialog_request.message = View(message);
        dialog_request.buttons = buttons.data();
        dialog_request.model = &tree.Build();
        pending = std::move(files);
        if (dialogs.show(dialogs.context, &dialog_request, Changed, Closed, this, &dialog) != PULSE_OK) {
            dialog = 0;
            pending.clear();
        }
    }
};

PulseStatus PULSE_CALL Activate(void*) { return PULSE_OK; }
void PULSE_CALL Deactivate(void*) {}
void PULSE_CALL Destroy(void* value) {
    auto* plugin = static_cast<Plugin*>(value);
    plugin->command.Reset();
    if (plugin->dialog && plugin->dialogs.close) plugin->dialogs.close(plugin->dialogs.context, plugin->dialog);
    if (plugin->request && plugin->resources.cancel_request)
        plugin->resources.cancel_request(plugin->resources.context, plugin->request);
    delete plugin;  // the host has drained task workers and callbacks
}

PulseStatus Register(Plugin& plugin) {
    static const PulseStringView extensions[] = {
        View(".png"), View(".jpg"), View(".jpeg"), View(".jpe"), View(".jfif"), View(".bmp"), View(".dib"),
        View(".gif"), View(".tif"), View(".tiff"), View(".ico"), View(".webp"), View(".heic"), View(".heif"),
        View(".hif"), View(".avif"), View(".jxl"), View(".psd")};
    PulseCommandDescriptor command{};
    command.size = sizeof(command);
    command.id = View("convert");
    command.title = View("转换图片格式…");
    command.icon = PULSE_COMMAND_ICON_DEFAULT;
    command.kinds = PULSE_COMMAND_FILES;
    command.min_selection = 1;
    command.max_selection = 4096;
    command.extensions = extensions;
    command.extension_count = static_cast<uint32_t>(std::size(extensions));
    return plugin.command.Register(plugin.commands, command,
        [&plugin](PulseCommandId, const PulseContextSelectionSnapshot& selection) { plugin.Show(selection); });
}

}  // namespace

PULSE_PLUGIN_SDK_PREVIEW()

extern "C" __declspec(dllexport) PulseStatus PULSE_CALL PulsePluginEntryV1(const PulseHostV1* host, PulsePluginV1* out) {
    if (!host || !out || host->size < sizeof(*host) || out->size < sizeof(*out) ||
        host->version != PULSE_ABI_VERSION_1 || out->version != PULSE_ABI_VERSION_1)
        return PULSE_ERROR_ABI;
    auto* plugin = new (std::nothrow) Plugin(*host);
    if (!plugin) return PULSE_ERROR_INITIALIZATION;
    *out = {sizeof(*out), PULSE_ABI_VERSION_1, plugin, Activate, Deactivate, Destroy};
    try {
        if (plugin->host.Query(PULSE_COMMANDS_INTERFACE_NAME, PULSE_COMMANDS_VERSION_1, plugin->commands) != PULSE_OK ||
            plugin->host.Query(PULSE_DIALOGS_INTERFACE_NAME, PULSE_DIALOGS_VERSION_1, plugin->dialogs) != PULSE_OK ||
            plugin->host.Query(PULSE_TASKS_INTERFACE_NAME, PULSE_TASKS_VERSION_1, plugin->tasks_api) != PULSE_OK ||
            plugin->host.Query(PULSE_STAGING_INTERFACE_NAME, PULSE_STAGING_VERSION_1, plugin->staging) != PULSE_OK)
            return PULSE_ERROR_UNAVAILABLE;
        plugin->tasks = std::make_unique<pulse::sdk::TaskGroup>(plugin->tasks_api);
        // Remembering the last choice is optional.
        if (plugin->host.Query(PULSE_RESOURCES_INTERFACE_NAME, PULSE_RESOURCES_VERSION_1, plugin->resources) == PULSE_OK &&
            plugin->resources.settings_get(plugin->resources.context, Plugin::SettingsDone, plugin, &plugin->request) != PULSE_OK)
            plugin->request = 0;
        return Register(*plugin);
    } catch (...) {
        return PULSE_ERROR_INITIALIZATION;
    }
}
