#include "ui/ui.h"
#include "platform/windows/process.h"
#include "imgui.h"
#include "imgui_impl_dx11.h"
#include "imgui_impl_win32.h"
#include <windowsx.h>
#include <algorithm>
#include <cstring>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <unordered_set>
#include <nlohmann/json.hpp>

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);
static std::filesystem::path ConfigDirectory() {
    return std::filesystem::path("C:/LUSTED/configs");
}

static bool IsValidConfigName(const std::string& name) {
    return !name.empty() && name.size() <= 48 &&
        std::all_of(name.begin(), name.end(), [](unsigned char c) {
            return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                   (c >= '0' && c <= '9') || c == '_' || c == '-';
        });
}

static std::string ConfigControlIdentity(int kind, const std::string& tab,
    const std::string& section, const std::string& label, int occurrence) {
    return std::to_string(kind) + "\x1f" + tab + "\x1f" + section + "\x1f" + label +
        "\x1f" + std::to_string(occurrence);
}

void UiManager::SetTab(const std::string& name) {
    std::lock_guard<std::mutex> lk(mtx_);
    currentTab_ = name.empty() ? "Main" : name;
    currentSection_ = "General";
}
void UiManager::SetSection(const std::string& name) {
    std::lock_guard<std::mutex> lk(mtx_);
    currentSection_ = name.empty() ? "General" : name;
}
int UiManager::AddLabel(const std::string& t) {
    int id;
    {
        std::lock_guard<std::mutex> lk(mtx_);
        id = next_++;
        Ctl c; c.kind = 0; c.tab = currentTab_; c.section = currentSection_; c.text = t;
        ctls_[id] = c;
        controlOrder_.push_back(id);
    }
    if (hwnd_) SendMessageA(hwnd_, WM_APP + 1, id, 0);
    return id;
}
void UiManager::SetLabel(int id, const std::string& text) {
    std::lock_guard<std::mutex> lk(mtx_);
    auto it = ctls_.find(id);
    if (it != ctls_.end() && it->second.kind == 0) it->second.text = text;
}
void UiManager::SetStatus(const std::string& text) {
    if (statusCtlId_) SetLabel(statusCtlId_, text);
}
int UiManager::AddButton(const std::string& t) {
    int id;
    {
        std::lock_guard<std::mutex> lk(mtx_);
        id = next_++;
        Ctl c; c.kind = 1; c.tab = currentTab_; c.section = currentSection_; c.text = t;
        ctls_[id] = c;
        controlOrder_.push_back(id);
    }
    if (hwnd_) SendMessageA(hwnd_, WM_APP + 1, id, 0);
    return id;
}
bool UiManager::IsButton(int id) const {
    std::lock_guard<std::mutex> lk(mtx_);
    auto it = ctls_.find(id);
    return it != ctls_.end() && it->second.kind == 1;
}
bool UiManager::IsPressable(int id) const {
    std::lock_guard<std::mutex> lk(mtx_);
    auto it = ctls_.find(id);
    return it != ctls_.end() && (it->second.kind == 1 || it->second.kind == 6);
}
bool UiManager::IsControl(int id) const {
    std::lock_guard<std::mutex> lk(mtx_);
    return ctls_.find(id) != ctls_.end();
}
bool UiManager::ButtonPressed(int id) {
    std::lock_guard<std::mutex> lk(mtx_);
    auto it = ctls_.find(id);
    if (it == ctls_.end()) return false;
    bool p = it->second.pressed;
    it->second.pressed = false;
    return p;
}
bool UiManager::TakeEvent(int id) {
    std::lock_guard<std::mutex> lk(mtx_);
    auto it = ctls_.find(id);
    if (it == ctls_.end()) return false;
    bool isPressControl = it->second.kind == 1 || it->second.kind == 6;
    bool occurred = isPressControl ? it->second.pressed : it->second.changed;
    it->second.pressed = false;
    it->second.changed = false;
    return occurred;
}
int UiManager::AddKeybind(const std::string& label, int defaultKey) {
    std::lock_guard<std::mutex> lk(mtx_);
    int id = next_++;
    Ctl c; c.kind = 6; c.tab = currentTab_; c.section = currentSection_; c.text = label;
    c.key = (std::clamp)(defaultKey, 0, 255);
    const int key = c.key;
    ctls_[id] = std::move(c);
    controlOrder_.push_back(id);
    // These two keybinds have native actions (see Poll) so they work even when
    // the worker thread is busy; remapping them here retargets the native key.
    if (label == "Toggle UI") { toggleCtlId_ = id; toggleKey_.store(key); }
    else if (label == "Unload") { unloadCtlId_ = id; unloadKey_.store(key); }
    if (hwnd_) PostMessageA(hwnd_, WM_APP + 1, id, 0);
    return id;
}
int UiManager::KeybindValue(int id) {
    std::lock_guard<std::mutex> lk(mtx_);
    auto it = ctls_.find(id);
    return it == ctls_.end() || it->second.kind != 6 ? 0 : it->second.key;
}
void UiManager::SetKeybind(int id, int key) {
    std::lock_guard<std::mutex> lk(mtx_);
    auto it = ctls_.find(id);
    if (it == ctls_.end() || it->second.kind != 6) return;
    it->second.key = (std::clamp)(key, 0, 255);
    if (id == toggleCtlId_) toggleKey_.store(it->second.key);
    else if (id == unloadCtlId_) unloadKey_.store(it->second.key);
}
void UiManager::SetDependency(int id, int sourceId, bool expected) {
    std::lock_guard<std::mutex> lk(mtx_);
    auto it = ctls_.find(id);
    auto source = ctls_.find(sourceId);
    if (it == ctls_.end() || source == ctls_.end() || source->second.kind != 2) return;
    it->second.dependencyId = sourceId;
    it->second.dependencyValue = expected;
}
int UiManager::AddToggle(const std::string& t, bool d) {
    int id;
    {
        std::lock_guard<std::mutex> lk(mtx_);
        id = next_++;
        Ctl c; c.kind = 2; c.tab = currentTab_; c.section = currentSection_; c.text = t; c.toggle = d;
        ctls_[id] = c;
        controlOrder_.push_back(id);
    }
    if (hwnd_) SendMessageA(hwnd_, WM_APP + 1, id, 0);
    return id;
}
bool UiManager::ToggleState(int id) {
    std::lock_guard<std::mutex> lk(mtx_);
    auto it = ctls_.find(id);
    return it == ctls_.end() ? false : it->second.toggle;
}
void UiManager::SetToggle(int id, bool v) {
    {
        std::lock_guard<std::mutex> lk(mtx_);
        auto it = ctls_.find(id);
        if (it == ctls_.end() || it->second.kind != 2) return;
        it->second.toggle = v;
    }
    if (hwnd_) SendMessageA(hwnd_, WM_APP + 2, id, v ? 1 : 0);
}
int UiManager::AddSlider(const std::string& t, int mn, int mx, int d) {
    int id;
    {
        std::lock_guard<std::mutex> lk(mtx_);
        id = next_++;
        Ctl c; c.kind = 3; c.tab = currentTab_; c.section = currentSection_; c.text = t;
        c.mn = (std::min)(mn, mx); c.mx = (std::max)(mn, mx);
        c.val = (std::clamp)(d, c.mn, c.mx);
        ctls_[id] = c;
        controlOrder_.push_back(id);
    }
    if (hwnd_) SendMessageA(hwnd_, WM_APP + 1, id, 0);
    return id;
}
int UiManager::SliderValue(int id) {
    std::lock_guard<std::mutex> lk(mtx_);
    auto it = ctls_.find(id);
    return it == ctls_.end() ? 0 : it->second.val;
}
void UiManager::SetSlider(int id, int v) {
    {
        std::lock_guard<std::mutex> lk(mtx_);
        auto it = ctls_.find(id);
        if (it == ctls_.end() || it->second.kind != 3) return;
        it->second.val = (std::clamp)(v, it->second.mn, it->second.mx);
        v = it->second.val;
    }
    if (hwnd_) SendMessageA(hwnd_, WM_APP + 3, id, v);
}

int UiManager::AddDropdown(const std::string& label, const std::vector<std::string>& options, int selected) {
    std::lock_guard<std::mutex> lk(mtx_);
    int id = next_++;
    Ctl c; c.kind = 4; c.tab = currentTab_; c.section = currentSection_; c.text = label;
    c.options = options;
    c.selected = c.options.empty() ? 0 : (std::clamp)(selected, 0, (int)c.options.size() - 1);
    ctls_[id] = std::move(c);
    controlOrder_.push_back(id);
    if (hwnd_) PostMessageA(hwnd_, WM_APP + 1, id, 0);
    return id;
}
int UiManager::DropdownValue(int id) {
    std::lock_guard<std::mutex> lk(mtx_);
    auto it = ctls_.find(id);
    return it == ctls_.end() || it->second.kind != 4 ? -1 : it->second.selected;
}
void UiManager::SetDropdown(int id, int selected) {
    std::lock_guard<std::mutex> lk(mtx_);
    auto it = ctls_.find(id);
    if (it == ctls_.end() || it->second.kind != 4 || it->second.options.empty()) return;
    it->second.selected = (std::clamp)(selected, 0, (int)it->second.options.size() - 1);
}
int UiManager::AddInput(const std::string& label, const std::string& placeholder) {
    std::lock_guard<std::mutex> lk(mtx_);
    int id = next_++;
    Ctl c; c.kind = 5; c.tab = currentTab_; c.section = currentSection_;
    c.text = label; c.placeholder = placeholder;
    ctls_[id] = std::move(c);
    controlOrder_.push_back(id);
    if (hwnd_) PostMessageA(hwnd_, WM_APP + 1, id, 0);
    return id;
}
std::string UiManager::InputValue(int id) {
    std::lock_guard<std::mutex> lk(mtx_);
    auto it = ctls_.find(id);
    return it == ctls_.end() || it->second.kind != 5 ? std::string() : it->second.buffer.data();
}
void UiManager::SetInput(int id, const std::string& value) {
    std::lock_guard<std::mutex> lk(mtx_);
    auto it = ctls_.find(id);
    if (it == ctls_.end() || it->second.kind != 5) return;
    strncpy_s(it->second.buffer.data(), it->second.buffer.size(), value.c_str(), _TRUNCATE);
}

bool UiManager::SaveConfig(const std::string& name) const {
    if (!IsValidConfigName(name)) return false;
    nlohmann::json config = nlohmann::json::object();
    std::unordered_map<std::string, int> occurrences;
    {
        std::lock_guard<std::mutex> lk(mtx_);
        for (int id : controlOrder_) {
            auto it = ctls_.find(id);
            if (it == ctls_.end()) continue;
            const Ctl& c = it->second;
            if (c.kind < 2 || c.kind > 6) continue;
            std::string identity = std::to_string(c.kind) + "\x1f" + c.tab + "\x1f" + c.section + "\x1f" + c.text;
            std::string key = ConfigControlIdentity(c.kind, c.tab, c.section, c.text, occurrences[identity]++);
            if (c.kind == 2) config[key] = c.toggle;
            else if (c.kind == 3) config[key] = c.val;
            else if (c.kind == 4) config[key] = c.selected;
            else if (c.kind == 5) config[key] = c.buffer.data();
            else if (c.kind == 6) config[key] = c.key;
        }
    }

    std::error_code ec;
    auto directory = ConfigDirectory();
    std::filesystem::create_directories(directory, ec);
    if (ec) return false;
    auto destination = directory / (name + ".json");
    auto temporary = directory / (name + ".tmp");
    {
        std::ofstream out(temporary, std::ios::binary | std::ios::trunc);
        if (!out) return false;
        out << config.dump(2);
        if (!out) { out.close(); std::filesystem::remove(temporary, ec); return false; }
    }
    if (!MoveFileExW(temporary.c_str(), destination.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        std::filesystem::remove(temporary, ec);
        return false;
    }
    return true;
}

bool UiManager::LoadConfig(const std::string& name) {
    if (!IsValidConfigName(name)) return false;
    std::ifstream in(ConfigDirectory() / (name + ".json"), std::ios::binary);
    if (!in) return false;
    nlohmann::json config;
    try { in >> config; } catch (...) { return false; }
    if (!config.is_object()) return false;

    std::lock_guard<std::mutex> lk(mtx_);
    std::unordered_map<std::string, int> occurrences;
    for (int id : controlOrder_) {
        auto it = ctls_.find(id);
        if (it == ctls_.end()) continue;
        Ctl& c = it->second;
        if (c.kind < 2 || c.kind > 6) continue;
        std::string identity = std::to_string(c.kind) + "\x1f" + c.tab + "\x1f" + c.section + "\x1f" + c.text;
        std::string key = ConfigControlIdentity(c.kind, c.tab, c.section, c.text, occurrences[identity]++);
        auto value = config.find(key);
        if (value == config.end()) continue;
        try {
            if (c.kind == 2 && value->is_boolean()) c.toggle = value->get<bool>();
            else if (c.kind == 3 && value->is_number_integer()) c.val = (std::clamp)(value->get<int>(), c.mn, c.mx);
            else if (c.kind == 4 && value->is_number_integer() && !c.options.empty())
                c.selected = (std::clamp)(value->get<int>(), 0, (int)c.options.size() - 1);
            else if (c.kind == 5 && value->is_string())
                strncpy_s(c.buffer.data(), c.buffer.size(), value->get<std::string>().c_str(), _TRUNCATE);
            else if (c.kind == 6 && value->is_number_integer()) c.key = (std::clamp)(value->get<int>(), 0, 255);
        } catch (...) { return false; }
    }
    return true;
}

bool UiManager::DeleteConfig(const std::string& name) const {
    if (!IsValidConfigName(name)) return false;
    std::error_code ec;
    bool removed = std::filesystem::remove(ConfigDirectory() / (name + ".json"), ec);
    return removed && !ec;
}

std::vector<std::string> UiManager::ConfigNames() const {
    std::vector<std::string> names;
    std::error_code ec;
    const auto directory = ConfigDirectory();
    if (!std::filesystem::exists(directory, ec)) return names;
    for (const auto& entry : std::filesystem::directory_iterator(directory, ec)) {
        if (ec) break;
        if (!entry.is_regular_file(ec) || entry.path().extension() != ".json") continue;
        const std::string name = entry.path().stem().string();
        if (IsValidConfigName(name)) names.push_back(name);
    }
    std::sort(names.begin(), names.end());
    return names;
}
