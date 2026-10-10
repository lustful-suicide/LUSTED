#include "explorer.h"
#include "instance.h"
#include "imgui.h"
#include "log.h"
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>

namespace {

constexpr int kMaxDepth = 8;
constexpr size_t kMaxKids = 2000;
constexpr size_t kMaxProps = 400;

std::string AddrStr(uintptr_t a) {
    char b[32];
    snprintf(b, sizeof(b), "0x%llX", (unsigned long long)a);
    return b;
}

bool NameMatch(const std::string& name, const char* filter) {
    if (!filter || !filter[0]) return true;
    std::string n = name, f = filter;
    for (auto& c : n) c = (char)tolower((unsigned char)c);
    for (auto& c : f) c = (char)tolower((unsigned char)c);
    return n.find(f) != std::string::npos;
}

} // namespace

void Explorer::LoadKids(uintptr_t addr) {
    if (!store_ || !addr) return;
    {
        std::lock_guard<std::mutex> lk(mtx_);
        auto it = nodes_.find(addr);
        if (it != nodes_.end() && it->second.kidsLoaded) return;
    }
    // Remote reads run lock-free (the loader thread shares nodes_).
    const std::string name = store_->GetName(addr);
    const std::string cls = store_->GetClass(addr);
    // Snapshot + live vectors only: instant, never a heap sweep.
    std::vector<uintptr_t> kids = store_->GetChildren(addr, false);
    const bool truncated = kids.size() > kMaxKids;
    if (truncated) kids.resize(kMaxKids);
    std::vector<std::string> names(kids.size()), clss(kids.size());
    for (size_t i = 0; i < kids.size(); ++i) {
        names[i] = store_->GetName(kids[i]);
        clss[i] = store_->GetClass(kids[i]);
    }
    bool needDeep = false;
    {
        std::lock_guard<std::mutex> lk(mtx_);
        Node& node = nodes_[addr];
        if (node.kidsLoaded) return; // background loader beat us
        if (node.name.empty()) node.name = name;
        if (node.cls.empty()) node.cls = cls;
        node.kids = std::move(kids);
        node.kidNames = std::move(names);
        node.kidCls = std::move(clss);
        node.truncated = truncated;
        node.kidsLoaded = true;
        // Empty after snapshot: hidden vector (Players/Workspace hold 1.5GB
        // of instances behind concealed vectors). Deep load covers it.
        if (node.kids.empty() && !node.deepDone && !node.deepLoading) {
            node.deepLoading = true;
            needDeep = true;
        }
    }
    if (needDeep) QueueDeepLoad(addr);
}

void Explorer::QueueDeepLoad(uintptr_t addr) {
    if (!store_ || !addr) return;
    {
        std::lock_guard<std::mutex> lk(qmtx_);
        if (!deepQueued_.insert(addr).second) return;
        deepQueue_.push_back(addr);
    }
    std::call_once(loaderOnce_, [&] { loader_ = std::thread(&Explorer::LoaderLoop, this); });
    qcv_.notify_one();
}

void Explorer::LoaderLoop() {
    for (;;) {
        uintptr_t addr = 0;
        bool haveWork = false;
        {
            std::unique_lock<std::mutex> lk(qmtx_);
            // Idle 5s with no deep loads -> run the live refresh pass instead
            // of sleeping: open nodes stay current as instances spawn/despawn.
            qcv_.wait_for(lk, std::chrono::seconds(5),
                          [&] { return stopLoader_.load() || !deepQueue_.empty(); });
            if (stopLoader_.load()) return;
            if (!deepQueue_.empty()) {
                addr = deepQueue_.front();
                deepQueue_.pop_front();
                haveWork = true;
            }
        }
        if (!haveWork) {
            if (open_.load()) RefreshExpanded();
            continue;
        }
        // Full lookup: snapshot + vectors + one throttled sweep shared with
        // every other misser. Runs here, never on the UI thread.
        std::vector<uintptr_t> kids;
        std::vector<std::string> names, clss;
        size_t found = 0;
        if (store_ && addr) {
            kids = store_->GetChildren(addr, true);
            if (kids.size() > kMaxKids) kids.resize(kMaxKids);
            names.resize(kids.size());
            clss.resize(kids.size());
            for (size_t i = 0; i < kids.size(); ++i) {
                names[i] = store_->GetName(kids[i]);
                clss[i] = store_->GetClass(kids[i]);
            }
            found = kids.size();
        }
        {
            std::lock_guard<std::mutex> lk(mtx_);
            Node& node = nodes_[addr];
            node.kids = std::move(kids);
            node.kidNames = std::move(names);
            node.kidCls = std::move(clss);
            node.kidsLoaded = true;
            node.deepLoading = false;
            node.deepDone = true;
        }
        {
            std::lock_guard<std::mutex> lk(qmtx_);
            deepQueued_.erase(addr);
        }
        if (found) {
            char b[96];
            snprintf(b, sizeof(b), "explorer deep %zu for 0x%llX",
                     found, (unsigned long long)addr);
            LustedLog(b);
        }
    }
}

Explorer::~Explorer() {
    stopLoader_.store(true);
    qcv_.notify_all();
    if (loader_.joinable()) loader_.join();
}

void Explorer::RefreshExpanded() {
    if (!store_) return;
    std::vector<uintptr_t> addrs;
    {
        std::lock_guard<std::mutex> lk(mtx_);
        addrs.assign(expanded_.begin(), expanded_.end());
    }
    for (uintptr_t addr : addrs) {
        if (stopLoader_.load()) return;
        // Dead parent: drop the whole subtree from the tree.
        if (!store_->IsAlive(addr)) {
            std::lock_guard<std::mutex> lk(mtx_);
            nodes_.erase(addr);
            expanded_.erase(addr);
            if (selected_ == addr) {
                selected_ = 0;
                selProps_.clear();
                selName_.clear(); selCls_.clear();
            }
            continue;
        }
        std::vector<uintptr_t> fresh = store_->GetChildren(addr, false);
        std::unordered_set<uintptr_t> freshSet(fresh.begin(), fresh.end());
        std::lock_guard<std::mutex> lk(mtx_);
        auto it = nodes_.find(addr);
        if (it == nodes_.end()) continue;
        Node& node = it->second;
        // Drop only proven-dead children (stale snapshot reads must not flap
        // live rows away); re-read "?" names that failed transiently.
        for (size_t i = 0; i < node.kids.size();) {
            uintptr_t c = node.kids[i];
            bool dead = !store_->IsAlive(c) || store_->GetParent(c) != addr;
            if (dead) {
                if (selected_ == c) {
                    selected_ = 0;
                    selProps_.clear();
                    selName_.clear(); selCls_.clear();
                }
                node.kids.erase(node.kids.begin() + (ptrdiff_t)i);
                node.kidNames.erase(node.kidNames.begin() + (ptrdiff_t)i);
                node.kidCls.erase(node.kidCls.begin() + (ptrdiff_t)i);
                continue;
            }
            if (node.kidNames[i].empty()) {
                node.kidNames[i] = store_->GetName(c);
                if (node.kidCls[i].empty()) node.kidCls[i] = store_->GetClass(c);
            }
            ++i;
        }
        // Arrivals.
        std::unordered_set<uintptr_t> have(node.kids.begin(), node.kids.end());
        for (uintptr_t c : fresh) {
            if (stopLoader_.load()) return;
            if (!have.insert(c).second) continue;
            if (!store_->IsAlive(c) || store_->GetParent(c) != addr) continue;
            node.kids.push_back(c);
            node.kidNames.push_back(store_->GetName(c));
            node.kidCls.push_back(store_->GetClass(c));
        }
        if (selected_ == addr) LoadProps(addr);
    }
}

void Explorer::LoadProps(uintptr_t addr) {
    selProps_.clear();
    selName_.clear(); selCls_.clear(); selAddr_.clear();
    selParent_.clear(); selCounts_.clear();
    if (!store_ || !addr) return;
    selCls_ = store_->GetClass(addr);
    selName_ = store_->GetName(addr);
    selAddr_ = AddrStr(addr);
    uintptr_t parent = store_->GetParent(addr);
    if (parent) selParent_ = store_->GetName(parent) + " (" + AddrStr(parent) + ")";
    else selParent_ = "(none)";
    auto members = store_->MembersOfClass(selCls_);
    // child count is cheap-ish (cached vectors); show it in the header.
    size_t nkids = 0;
    {
        auto it = nodes_.find(addr);
        if (it != nodes_.end() && it->second.kidsLoaded) nkids = it->second.kids.size();
        else nkids = store_->GetChildren(addr, false).size();
    }
    char cb[96];
    snprintf(cb, sizeof(cb), "%zu children, %zu members", nkids, members.size());
    selCounts_ = cb;
    const size_t n = std::min(members.size(), kMaxProps);
    selProps_.reserve(n);
    for (size_t i = 0; i < n; ++i)
        selProps_.emplace_back(members[i], store_->ReadPropDisplay(addr, selCls_, members[i]));
    if (members.size() > kMaxProps) selProps_.emplace_back("...", "(truncated)");
}

void Explorer::DrawNode(uintptr_t addr, int depth) {
    if (depth > kMaxDepth || !store_) return;
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
    if (!open_.load() || !store_) return;
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
        root = store_->GetDataModel(pid);
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

