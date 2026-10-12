#include "ui/explorer.h"
#include "roblox/instance.h"
#include "imgui.h"
#include "support/log.h"
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>

namespace {

constexpr int kMaxDepth = 8;
// Shared with explorer_load.cpp (same cap; guarded so src/ui/explorer.cpp
// can include both TUs without a redefinition).
#ifndef LUSTED_EXPLORER_KMAXKIDS_DEFINED
#define LUSTED_EXPLORER_KMAXKIDS_DEFINED
constexpr size_t kMaxKids = 2000;
#endif

bool NameMatch(const std::string& name, const char* filter) {
    if (!filter || !filter[0]) return true;
    std::string n = name, f = filter;
    for (auto& c : n) c = (char)tolower((unsigned char)c);
    for (auto& c : f) c = (char)tolower((unsigned char)c);
    return n.find(f) != std::string::npos;
}

} // namespace

void Explorer::DrawNode(uintptr_t addr, int depth) {
    InstanceStore* st = store_.load(std::memory_order_acquire);
    if (depth > kMaxDepth || !st) return;
    LoadKids(addr);
    Node snapshot;
    {
        std::lock_guard<std::mutex> lk(mtx_);
        auto it = nodes_.find(addr);
        if (it == nodes_.end()) return;
        snapshot = it->second; // copy names/kids; remote reads stay outside the lock
    }
    const std::string nm = snapshot.name.empty() ? "?" : snapshot.name;
    const std::string cn = snapshot.cls.empty() ? "?" : snapshot.cls;
    // Services read "X [X]"; collapse the redundant half for narrow columns.
    const std::string label = (nm == cn) ? nm : (nm + " [" + cn + "]");
    // Single click both selects and expands (no arrow-hunting).
    ImGuiTreeNodeFlags flags = ImGuiTreeNodeFlags_OpenOnDoubleClick;
    if (snapshot.kids.empty()) flags |= ImGuiTreeNodeFlags_Leaf | ImGuiTreeNodeFlags_NoTreePushOnOpen;
    bool isSel = false;
    {
        std::lock_guard<std::mutex> lk(mtx_);
        isSel = (selected_ == addr);
    }
    if (isSel) flags |= ImGuiTreeNodeFlags_Selected;
    ImGui::PushID((int)(addr & 0x7FFFFFFF));
    bool open = ImGui::TreeNodeEx(label.c_str(), flags);
    {
        std::lock_guard<std::mutex> lk(mtx_);
        if (open) expanded_.insert(addr);
        else expanded_.erase(addr);
    }
    if (ImGui::IsItemClicked()) {
        std::lock_guard<std::mutex> lk(mtx_);
        selected_ = addr;
        LoadProps(addr);
    }
    if (open && !(flags & ImGuiTreeNodeFlags_NoTreePushOnOpen)) {
        bool anyShown = false;
        for (size_t i = 0; i < snapshot.kids.size(); ++i) {
            if (!NameMatch(snapshot.kidNames[i], filter_)) continue;
            anyShown = true;
            DrawNode(snapshot.kids[i], depth + 1);
        }
        if (snapshot.deepLoading && snapshot.kids.empty() && !anyShown)
            ImGui::TextDisabled("scanning hidden children…");
        if (snapshot.truncated) ImGui::TextDisabled("(first %zu shown)", kMaxKids);
        ImGui::TreePop();
    }
    ImGui::PopID();
}

void Explorer::Render() {
    std::shared_lock storeLock(storeAccessMutex_);
    InstanceStore* st = store_.load(std::memory_order_acquire);
    if (!open_.load() || !st) return;
    DWORD pid = pid_ ? *pid_ : 0;
    if (!pid) return;
    // Resolve root once per pid; Refresh button clears it below.
    {
        std::lock_guard<std::mutex> lk(mtx_);
        if (!root_ || rootPid_ != pid) {
            root_ = 0;
            rootPid_ = pid;
        }
    }
    uintptr_t root = 0;
    {
        std::lock_guard<std::mutex> lk(mtx_);
        root = root_;
    }
    if (!root) {
        root = st->GetDataModel(pid);
        if (!root) return; // no live DataModel yet; try again next frame
        std::lock_guard<std::mutex> lk(mtx_);
        root_ = root;
        rootPid_ = pid;
    }
    // Live refresh runs on the loader thread even if no deep load ever
    // queued (it starts here, once, instead of on first empty node).
    std::call_once(loaderOnce_, [&] { loader_ = std::thread(&Explorer::LoaderLoop, this); });

    bool keepOpen = true;
    // Wide but short floating panel: fits service rows without eating the game.
    {
        const ImVec2 vp = ImGui::GetIO().DisplaySize;
        const float x0 = vp.x > 1100.0f ? 500.0f : 8.0f;
        const float w = vp.x > 1100.0f
            ? (std::min)(vp.x - x0 - 16.0f, 880.0f)
            : (vp.x - 16.0f);
        const float h = (std::max)(360.0f, (std::min)(vp.y * 0.55f, 560.0f));
        ImGui::SetNextWindowPos(ImVec2(x0, 20), ImGuiCond_FirstUseEver);
        ImGui::SetNextWindowSize(ImVec2((std::max)(w, 360.0f), h),
                                 ImGuiCond_FirstUseEver);
    }
    if (!ImGui::Begin("LUSTED Explorer", &keepOpen)) { ImGui::End(); open_.store(keepOpen); return; }
    open_.store(keepOpen);

    if (ImGui::Button("Refresh")) {
        std::lock_guard<std::mutex> lk(mtx_);
        nodes_.clear();
        expanded_.clear();
        root_ = 0;
        selected_ = 0;
        firstShown_ = false;
        selProps_.clear();
        {
            std::lock_guard<std::mutex> qlk(qmtx_);
            deepQueue_.clear();
            deepQueued_.clear();
        }
    }
    ImGui::SameLine();
    ImGui::InputTextWithHint("##expfilter", "filter...", filter_, sizeof(filter_));
    ImGui::SameLine();
    {
        std::lock_guard<std::mutex> lk(mtx_);
        ImGui::TextDisabled("%zu cached", nodes_.size());
    }
    ImGui::Separator();
    if (ImGui::BeginTable("##exptable", 2, ImGuiTableFlags_Resizable)) {
        ImGui::TableSetupColumn("Tree", ImGuiTableColumnFlags_WidthStretch, 1.5f);
        ImGui::TableSetupColumn("Properties", ImGuiTableColumnFlags_WidthStretch, 1.0f);
        ImGui::TableNextRow();
        ImGui::TableSetColumnIndex(0);
        ImGui::BeginChild("##exptree", ImVec2(0, 0), ImGuiChildFlags_Border,
            ImGuiWindowFlags_AlwaysVerticalScrollbar | ImGuiWindowFlags_HorizontalScrollbar);
        // First show: open the root and select it so the tree reads populated
        // and the props panel demonstrates itself immediately.
        {
            std::lock_guard<std::mutex> lk(mtx_);
            if (!firstShown_) {
                firstShown_ = true;
                selected_ = root;
                LoadProps(root);
                ImGui::SetNextItemOpen(true, ImGuiCond_Once);
            }
        }
        DrawNode(root, 0);
        ImGui::EndChild();
        ImGui::TableSetColumnIndex(1);
        ImGui::BeginChild("##expprops", ImVec2(0, 0), ImGuiChildFlags_Border,
            ImGuiWindowFlags_AlwaysVerticalScrollbar);
        std::string hName, hCls, hAddr, hParent, hCounts;
        std::vector<std::pair<std::string, std::string>> props;
        bool hasSel = false;
        {
            std::lock_guard<std::mutex> lk(mtx_);
            hasSel = selected_ != 0;
            hName = selName_; hCls = selCls_; hAddr = selAddr_;
            hParent = selParent_; hCounts = selCounts_;
            props = selProps_;
        }
        if (!hasSel) {
            ImGui::TextDisabled("Click an instance.");
        } else {
            ImGui::TextWrapped("%s", hName.c_str());
            ImGui::TextDisabled("%s  %s", hCls.c_str(), hAddr.c_str());
            ImGui::TextDisabled("parent: %s", hParent.c_str());
            ImGui::TextDisabled("%s", hCounts.c_str());
            ImGui::Separator();
            if (ImGui::BeginTable("##exppropsrows", 2,
                    ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingStretchProp |
                    ImGuiTableFlags_Resizable)) {
                ImGui::TableSetupColumn("Member", ImGuiTableColumnFlags_WidthFixed, 150.0f);
                ImGui::TableSetupColumn("Value", ImGuiTableColumnFlags_WidthStretch);
                for (auto& [m, v] : props) {
                    ImGui::TableNextRow();
                    ImGui::TableSetColumnIndex(0);
                    ImGui::TextUnformatted(m.c_str());
                    ImGui::TableSetColumnIndex(1);
                    ImGui::TextWrapped("%s", v.c_str());
                }
                ImGui::EndTable();
            }
        }
        ImGui::EndChild();
        ImGui::EndTable();
    }
    ImGui::End();
}
