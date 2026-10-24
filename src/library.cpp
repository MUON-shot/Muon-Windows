// Library tab, filmstrip (the working collection) and the thumbnail cache.
#include "ui_shared.h"
#include "pipeline.h"
#include "widgets.h"
#include "i18n.h"
#include "imgui_internal.h"
#include <shlwapi.h>
#include <shellapi.h>
#include <unordered_map>
#include <algorithm>
#include <cstdio>

// =====================================================================  thumbnail cache
static std::unordered_map<std::wstring, ThumbEntry> s_thumbs;
static std::vector<Tex*> s_retired;      // replaced thumbnails, freed one frame later
static int s_frame = 0;

static void ThumbWorker() {
    App& a = g_app;
    while (!a.quit) {
        std::wstring path;
        {
            std::unique_lock<std::mutex> lk(a.thumbMx);
            a.thumbCv.wait(lk, [&] { return a.quit || !a.thumbQueue.empty(); });
            if (a.quit) return;
            // newest request first: what is on screen right now
            path = a.thumbQueue.back();
            a.thumbQueue.pop_back();
        }
        ThumbResult r;
        r.path = path;
        r.ok = LoadThumbnailSafe(path, 256, r.rgba, r.w, r.h);
        std::lock_guard<std::mutex> lk(g_app.thumbMx);
        g_app.thumbDone.push_back(std::move(r));
    }
}

void ThumbsStart() {
    // two workers: RAW previews are cheap, large JPEGs are not
    g_app.thumbThread = std::thread([] {
        std::thread second(ThumbWorker);
        ThumbWorker();
        second.join();
    });
}

void ThumbsStop() {
    App& a = g_app;
    a.quit = true;
    a.thumbCv.notify_all();
    if (a.thumbThread.joinable()) a.thumbThread.join();
    for (auto& kv : s_thumbs) if (kv.second.tex) { kv.second.tex->release(); delete kv.second.tex; }
    s_thumbs.clear();
    for (Tex* t : s_retired) { t->release(); delete t; }
    s_retired.clear();
}

void ThumbsPoll() {
    App& a = g_app;
    s_frame++;
    for (Tex* t : s_retired) { t->release(); delete t; }
    s_retired.clear();
    std::vector<ThumbResult> done;
    { std::lock_guard<std::mutex> lk(a.thumbMx); done.swap(a.thumbDone); }
    for (auto& r : done) {
        auto it = s_thumbs.find(r.path);
        if (it == s_thumbs.end()) continue;
        ThumbEntry& t = it->second;
        if (!r.ok) { t.failed = true; continue; }
        if (t.tex) continue;   // an edited thumbnail arrived first
        t.tex = g_gpu.create(r.w, r.h, DXGI_FORMAT_R8G8B8A8_UNORM, false, r.rgba.data(), r.w * 4);
        t.w = r.w; t.h = r.h;
    }
    // keep GPU memory bounded in huge folders
    if (s_thumbs.size() > 900) {
        for (auto it = s_thumbs.begin(); it != s_thumbs.end();) {
            if (s_frame - it->second.lastUse > 300 && CollectionIndex(it->first) < 0) {
                if (it->second.tex) s_retired.push_back(it->second.tex);
                it = s_thumbs.erase(it);
            } else ++it;
        }
    }
}

ThumbEntry* ThumbFor(const std::wstring& path, bool request) {
    ThumbEntry& t = s_thumbs[path];
    t.lastUse = s_frame;
    if (request && !t.requested) {
        t.requested = true;
        { std::lock_guard<std::mutex> lk(g_app.thumbMx); g_app.thumbQueue.push_back(path); }
        g_app.thumbCv.notify_one();
    }
    return &t;
}

void ThumbReplace(const std::wstring& path, const std::vector<uint8_t>& rgba, int w, int h) {
    ThumbEntry& t = s_thumbs[path];
    if (t.tex) s_retired.push_back(t.tex);
    t.tex = g_gpu.create(w, h, DXGI_FORMAT_R8G8B8A8_UNORM, false, rgba.data(), w * 4);
    t.w = w; t.h = h; t.requested = true; t.failed = false;
}

// =====================================================================  collection
static std::wstring CollectionFile() {
    wchar_t buf[MAX_PATH];
    if (!GetEnvironmentVariableW(L"LOCALAPPDATA", buf, MAX_PATH)) return L"";
    std::wstring d = std::wstring(buf) + L"\\Muon";
    CreateDirectoryW(d.c_str(), nullptr);
    return d + L"\\collection.txt";
}

int CollectionIndex(const std::wstring& path) {
    auto& c = g_app.collection;
    for (int i = 0; i < (int)c.size(); i++) if (_wcsicmp(c[i].c_str(), path.c_str()) == 0) return i;
    return -1;
}

void CollectionSave() {
    if (g_app.noPersist) return;
    std::string s;
    for (auto& p : g_app.collection) s += WideToUtf8(p) + "\n";
    std::wstring f = CollectionFile();
    if (!f.empty()) WriteTextFile(f, s);
}

void CollectionLoad() {
    std::string s;
    std::wstring f = CollectionFile();
    if (f.empty() || !ReadTextFile(f, s)) return;
    size_t pos = 0;
    while (pos < s.size()) {
        size_t nl = s.find('\n', pos);
        std::string line = s.substr(pos, nl == std::string::npos ? std::string::npos : nl - pos);
        pos = nl == std::string::npos ? s.size() : nl + 1;
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.empty()) continue;
        std::wstring p = Utf8ToWide(line);
        if (GetFileAttributesW(p.c_str()) != INVALID_FILE_ATTRIBUTES && CollectionIndex(p) < 0) g_app.collection.push_back(p);
    }
    g_app.collSel.assign(g_app.collection.size(), 0);
}

void CollectionAdd(const std::vector<std::wstring>& paths) {
    App& a = g_app;
    bool added = false;
    for (auto& p : paths) if (CollectionIndex(p) < 0) { a.collection.push_back(p); added = true; }
    a.collSel.resize(a.collection.size(), 0);
    if (added) CollectionSave();
}

void CollectionRemove(const std::vector<std::wstring>& paths) {
    App& a = g_app;
    for (auto& p : paths) {
        int i = CollectionIndex(p);
        if (i < 0) continue;
        a.collection.erase(a.collection.begin() + i);
        a.collSel.erase(a.collSel.begin() + i);
    }
    a.curIndex = a.curPath.empty() ? -1 : CollectionIndex(a.curPath);
    a.collAnchor = -1;
    CollectionSave();
}

std::vector<std::wstring> CollectionSelection() {
    App& a = g_app;
    std::vector<std::wstring> r;
    for (size_t i = 0; i < a.collection.size(); i++) if (i < a.collSel.size() && a.collSel[i]) r.push_back(a.collection[i]);
    if (r.empty() && !a.curPath.empty()) r.push_back(a.curPath);
    return r;
}

void CollectionSelectOnly(int index) {
    App& a = g_app;
    a.collSel.assign(a.collection.size(), 0);
    if (index >= 0 && index < (int)a.collSel.size()) a.collSel[index] = 1;
    a.collAnchor = index;
}

// Lightroom-style selection: click, Ctrl+click toggles, Shift+click selects a range.
static void ClickSelect(std::vector<char>& sel, int& anchor, int i, bool ctrl, bool shift) {
    if (shift && anchor >= 0) {
        if (!ctrl) std::fill(sel.begin(), sel.end(), 0);
        int lo = std::min(anchor, i), hi = std::max(anchor, i);
        for (int k = lo; k <= hi; k++) sel[k] = 1;
        return;
    }
    if (ctrl) { sel[i] = !sel[i]; anchor = i; return; }
    std::fill(sel.begin(), sel.end(), 0);
    sel[i] = 1;
    anchor = i;
}

static int CountSel(const std::vector<char>& s) { int n = 0; for (char c : s) n += c ? 1 : 0; return n; }

void SelectAllInView() {
    App& a = g_app;
    if (a.bottomTab == TAB_LIBRARY) std::fill(a.libSel.begin(), a.libSel.end(), 1);
    else std::fill(a.collSel.begin(), a.collSel.end(), 1);
}

// Draws a thumbnail fitted into a box.
static void DrawThumb(ImDrawList* dl, const std::wstring& path, ImVec2 p, ImVec2 box, bool bright, float rounding) {
    ThumbEntry* t = ThumbFor(path, true);
    if (t->tex) {
        float s = std::min(box.x / t->w, box.y / t->h);
        float w = t->w * s, h = t->h * s;
        ImVec2 q(p.x + (box.x - w) / 2, p.y + (box.y - h) / 2);
        dl->AddImageRounded(TexId(t->tex), q, ImVec2(q.x + w, q.y + h), ImVec2(0, 0), ImVec2(1, 1), bright ? IM_COL32_WHITE : IM_COL32(185, 185, 185, 255), rounding);
    } else {
        std::string n = WideToUtf8(PathFindExtensionW(path.c_str()));
        if (t->failed) n = "?";
        ImVec2 ts = ImGui::CalcTextSize(n.c_str());
        dl->AddText(ImVec2(p.x + (box.x - ts.x) / 2, p.y + (box.y - ts.y) / 2), IM_COL32(110, 110, 110, 255), n.c_str());
    }
}

// =====================================================================  filmstrip
static int s_ctxIndex = -1;

static void FilmstripMenu() {
    App& a = g_app;
    if (!ImGui::BeginPopup("filmctx")) return;
    if (s_ctxIndex < 0 || s_ctxIndex >= (int)a.collection.size()) { ImGui::EndPopup(); return; }
    std::wstring path = a.collection[s_ctxIndex];
    std::vector<std::wstring> sel = CollectionSelection();
    int n = (int)sel.size();
    char b[160];
    ImGui::TextDisabled("%s", WideToUtf8(PathFindFileNameW(path.c_str())).c_str());
    ImGui::Separator();
    if (ImGui::MenuItem(T("Открыть", "Open"), nullptr, false, s_ctxIndex != a.curIndex)) AppOpenFile(path);
    if (ImGui::MenuItem(T("Копировать настройки", "Copy settings"), "Ctrl+Shift+C")) CopySettingsFrom(path, SG_DEVELOP);
    if (ImGui::MenuItem(T("Копировать выборочно…", "Copy selected settings…"))) OpenCopyDialog(path);
    snprintf(b, sizeof(b), T("Вставить настройки (%d)", "Paste settings (%d)"), n);
    if (ImGui::MenuItem(b, "Ctrl+Shift+V", false, a.clip.valid)) PasteSettingsTo(sel);
    snprintf(b, sizeof(b), T("Сбросить правки (%d)", "Reset edits (%d)"), n);
    if (ImGui::MenuItem(b)) ResetSettingsOf(sel);
    ImGui::Separator();
    snprintf(b, sizeof(b), T("Экспортировать (%d)…", "Export (%d)…"), n);
    if (ImGui::MenuItem(b)) OpenExport(sel);
    if (ImGui::MenuItem(T("Показать в проводнике", "Show in Explorer"))) RevealInExplorer(path);
    ImGui::Separator();
    snprintf(b, sizeof(b), T("Убрать из ленты (%d)", "Remove from filmstrip (%d)"), n);
    if (ImGui::MenuItem(b)) CollectionRemove(sel);
    ImGui::EndPopup();
}

void Filmstrip(ImVec2 pos, ImVec2 size) {
    App& a = g_app;
    ImGuiIO& io = ImGui::GetIO();
    a.collSel.resize(a.collection.size(), 0);
    ImGui::SetCursorScreenPos(pos);
    ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0.06f, 0.063f, 0.07f, 1));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(10, 8));
    ImGui::BeginChild("##film", size, 0, ImGuiWindowFlags_HorizontalScrollbar | ImGuiWindowFlags_AlwaysUseWindowPadding);
    float th = size.y - 26 * a.dpi, tw = th * 1.45f;
    ImDrawList* dl = ImGui::GetWindowDrawList();
    if (ImGui::IsWindowHovered() && io.MouseWheel != 0) ImGui::SetScrollX(ImGui::GetScrollX() - io.MouseWheel * tw);
    static int lastIndex = -2;
    int openIdx = -1;
    bool openCtx = false;
    for (int i = 0; i < (int)a.collection.size(); i++) {
        if (i) ImGui::SameLine(0, 6);
        ImGui::PushID(i);
        ImVec2 p = ImGui::GetCursorScreenPos();
        ImGui::InvisibleButton("##t", ImVec2(tw, th), ImGuiButtonFlags_MouseButtonLeft | ImGuiButtonFlags_MouseButtonRight);
        bool hov = ImGui::IsItemHovered();
        if (ImGui::IsItemClicked(0)) {
            if (io.KeyCtrl || io.KeyShift) ClickSelect(a.collSel, a.collAnchor, i, io.KeyCtrl, io.KeyShift);
            else { CollectionSelectOnly(i); if (i != a.curIndex) openIdx = i; }
        }
        if (ImGui::IsItemClicked(1)) {
            if (!a.collSel[i]) CollectionSelectOnly(i);
            s_ctxIndex = i;
            openCtx = true;
        }
        if (i == a.curIndex && lastIndex != a.curIndex) ImGui::SetScrollHereX(0.5f);
        bool sel = a.collSel[i] != 0;
        dl->AddRectFilled(p, ImVec2(p.x + tw, p.y + th), sel ? IM_COL32(60, 74, 100, 255) : IM_COL32(28, 29, 32, 255), 6);
        if (ImGui::IsItemVisible()) DrawThumb(dl, a.collection[i], ImVec2(p.x + 3, p.y + 3), ImVec2(tw - 6, th - 6), hov || sel || i == a.curIndex, 4);
        if (i == a.curIndex) dl->AddRect(ImVec2(p.x - 2, p.y - 2), ImVec2(p.x + tw + 2, p.y + th + 2), W::kAccent, 7, 0, 2.0f);
        else if (sel) dl->AddRect(ImVec2(p.x - 1, p.y - 1), ImVec2(p.x + tw + 1, p.y + th + 1), IM_COL32(140, 170, 230, 160), 7, 0, 1.0f);
        if (hov && !ImGui::IsPopupOpen("filmctx")) ImGui::SetTooltip("%s", WideToUtf8(PathFindFileNameW(a.collection[i].c_str())).c_str());
        ImGui::PopID();
    }
    lastIndex = a.curIndex;
    if (openCtx) ImGui::OpenPopup("filmctx");   // outside the per-thumbnail ID scope
    FilmstripMenu();
    ImGui::EndChild();
    ImGui::PopStyleVar();
    ImGui::PopStyleColor();
    if (openIdx >= 0) AppOpenFile(a.collection[openIdx]);
}

// =====================================================================  library
void LibraryOpenFolder(const std::wstring& folder) {
    App& a = g_app;
    a.libFolder = folder;
    a.libFiles = ListFolderImages(folder);
    a.libSel.assign(a.libFiles.size(), 0);
    a.libAnchor = -1;
}

static void ImportPaths(const std::vector<std::wstring>& paths) {
    App& a = g_app;
    if (paths.empty()) return;
    CollectionAdd(paths);
    char b[128];
    snprintf(b, sizeof(b), T("В ленту добавлено фото: %d", "Added to the filmstrip: %d"), (int)paths.size());
    SetStatus(b);
    a.bottomTab = TAB_CLASSIC;
    int first = CollectionIndex(paths[0]);
    if (a.libOpenAfterImport || a.curPath.empty()) { CollectionSelectOnly(first); AppOpenFile(paths[0]); }
}

static std::vector<std::wstring> LibSelected() {
    App& a = g_app;
    std::vector<std::wstring> r;
    for (size_t i = 0; i < a.libFiles.size(); i++) if (a.libSel[i]) r.push_back(a.libFiles[i]);
    return r;
}

static void ChooseLibraryFolder() {
    auto d = PickFolderDialog(g_app.hwnd);
    if (!d.empty()) LibraryOpenFolder(d);
}

void LibraryGrid(ImVec2 pos, ImVec2 size) {
    App& a = g_app;
    ImGuiIO& io = ImGui::GetIO();
    ImGui::SetCursorScreenPos(pos);
    ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0.055f, 0.059f, 0.066f, 1));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(18 * a.dpi, 14 * a.dpi));
    ImGui::BeginChild("##lib", size, 0, ImGuiWindowFlags_AlwaysUseWindowPadding | ImGuiWindowFlags_NoScrollbar);
    // header
    ImGui::PushFont(a.fontBold);
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted(T("Библиотека", "Library"));
    ImGui::PopFont();
    ImGui::SameLine(0, 16 * a.dpi);
    if (W::Button(T("Выбрать папку…", "Choose folder…"), ImVec2(0, 0), W::BTN_PRIMARY)) ChooseLibraryFolder();
    if (!a.libFolder.empty()) {
        ImGui::SameLine();
        if (W::Button(T("Обновить", "Refresh"))) LibraryOpenFolder(a.libFolder);
        ImGui::SameLine();
        ImGui::AlignTextToFramePadding();
        float sw = 150 * a.dpi;
        // long paths are shortened from the left so they never run under the size slider
        float room = ImGui::GetWindowContentRegionMax().x - sw - ImGui::GetCursorPosX() - 16 * a.dpi;
        std::wstring shown = a.libFolder;
        while (shown.size() > 4 && ImGui::CalcTextSize(WideToUtf8(L"…" + shown).c_str()).x > room) shown.erase(0, 1);
        if (shown != a.libFolder) shown = L"…" + shown;
        ImGui::TextDisabled("%s", WideToUtf8(shown).c_str());
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", WideToUtf8(a.libFolder).c_str());
        ImGui::SameLine(ImGui::GetWindowContentRegionMax().x - sw);
        ImGui::SetNextItemWidth(sw);
        ImGui::SliderFloat("##tsize", &a.libThumb, 110, 320, T("Размер %.0f", "Size %.0f"));
    }
    ImGui::Spacing();
    if (a.libFolder.empty()) {
        ImVec2 c = ImGui::GetContentRegionAvail();
        ImGui::Dummy(ImVec2(0, c.y * 0.3f));
        ImGui::PushFont(a.fontBig);
        const char* t = T("Импорт фото", "Import photos");
        ImGui::SetCursorPosX((ImGui::GetWindowWidth() - ImGui::CalcTextSize(t).x) / 2);
        ImGui::TextUnformatted(t);
        ImGui::PopFont();
        const char* t2 = T("Выберите папку, отметьте снимки и добавьте их в рабочую ленту.", "Pick a folder, select photos and add them to the working filmstrip.");
        ImGui::SetCursorPosX((ImGui::GetWindowWidth() - ImGui::CalcTextSize(t2).x) / 2);
        ImGui::TextDisabled("%s", t2);
        ImGui::Spacing();
        ImGui::SetCursorPosX((ImGui::GetWindowWidth() - 220 * a.dpi) / 2);
        if (W::Button(T("Выбрать папку…", "Choose folder…"), ImVec2(220 * a.dpi, 40 * a.dpi), W::BTN_PRIMARY)) ChooseLibraryFolder();
        ImGui::EndChild();
        ImGui::PopStyleVar();
        ImGui::PopStyleColor();
        return;
    }
    if (a.libFiles.empty()) {
        W::Caption(T("В этой папке нет поддерживаемых фото.", "This folder has no supported photos."));
        ImGui::EndChild();
        ImGui::PopStyleVar();
        ImGui::PopStyleColor();
        return;
    }
    // grid
    ImGui::BeginChild("##grid", ImVec2(0, 0), 0, ImGuiWindowFlags_NoBackground);
    ImDrawList* dl = ImGui::GetWindowDrawList();
    float cell = a.libThumb * a.dpi, gap = 10 * a.dpi;
    float capH = ImGui::GetFontSize() * 1.4f;
    float avail = ImGui::GetContentRegionAvail().x;
    int cols = std::max(1, (int)((avail + gap) / (cell + gap)));
    float cw = (avail - gap * (cols - 1)) / cols;
    int n = (int)a.libFiles.size();
    int rows = (n + cols - 1) / cols;
    int dbl = -1;
    bool openCtx = false;
    static int ctxIdx = -1;
    ImGuiListClipper clip;
    clip.Begin(rows, cw * 0.75f + capH + gap);
    while (clip.Step()) {
        for (int r = clip.DisplayStart; r < clip.DisplayEnd; r++) {
            for (int c = 0; c < cols; c++) {
                int i = r * cols + c;
                if (i >= n) break;
                if (c) ImGui::SameLine(0, gap);
                ImGui::PushID(i);
                ImVec2 p = ImGui::GetCursorScreenPos();
                ImVec2 box(cw, cw * 0.75f + capH);
                ImGui::InvisibleButton("##c", box, ImGuiButtonFlags_MouseButtonLeft | ImGuiButtonFlags_MouseButtonRight);
                bool hov = ImGui::IsItemHovered();
                if (ImGui::IsItemClicked(0)) ClickSelect(a.libSel, a.libAnchor, i, io.KeyCtrl, io.KeyShift);
                if (hov && ImGui::IsMouseDoubleClicked(0)) dbl = i;
                if (ImGui::IsItemClicked(1)) {
                    if (!a.libSel[i]) { std::fill(a.libSel.begin(), a.libSel.end(), 0); a.libSel[i] = 1; a.libAnchor = i; }
                    ctxIdx = i;
                    openCtx = true;
                }
                bool sel = a.libSel[i] != 0;
                bool inColl = CollectionIndex(a.libFiles[i]) >= 0;
                dl->AddRectFilled(p, ImVec2(p.x + box.x, p.y + box.y), sel ? IM_COL32(52, 66, 92, 255) : hov ? IM_COL32(34, 36, 40, 255) : IM_COL32(24, 25, 28, 255), 8);
                DrawThumb(dl, a.libFiles[i], ImVec2(p.x + 6, p.y + 6), ImVec2(box.x - 12, cw * 0.75f - 8), true, 4);
                std::string name = WideToUtf8(PathFindFileNameW(a.libFiles[i].c_str()));
                ImGui::PushFont(a.fontSmall);
                ImVec2 ts = ImGui::CalcTextSize(name.c_str());
                ImVec2 tp(p.x + 8, p.y + cw * 0.75f + (capH - ts.y) * 0.5f);
                dl->PushClipRect(ImVec2(p.x + 4, p.y), ImVec2(p.x + box.x - 4, p.y + box.y), true);
                dl->AddText(tp, sel ? IM_COL32(235, 238, 245, 255) : IM_COL32(160, 163, 170, 255), name.c_str());
                dl->PopClipRect();
                ImGui::PopFont();
                if (sel) {
                    dl->AddRect(p, ImVec2(p.x + box.x, p.y + box.y), W::kAccent, 8, 0, 2.0f);
                    ImVec2 cc(p.x + box.x - 16 * a.dpi, p.y + 16 * a.dpi);
                    dl->AddCircleFilled(cc, 9 * a.dpi, W::kAccent);
                    float s = 4 * a.dpi;
                    dl->AddLine(ImVec2(cc.x - s, cc.y), ImVec2(cc.x - s * 0.3f, cc.y + s * 0.7f), IM_COL32(16, 20, 30, 255), 2.0f);
                    dl->AddLine(ImVec2(cc.x - s * 0.3f, cc.y + s * 0.7f), ImVec2(cc.x + s, cc.y - s * 0.7f), IM_COL32(16, 20, 30, 255), 2.0f);
                }
                if (inColl) {
                    const char* tag = T("в ленте", "in filmstrip");
                    ImGui::PushFont(a.fontSmall);
                    ImVec2 s2 = ImGui::CalcTextSize(tag);
                    ImVec2 bp(p.x + 10, p.y + 10);
                    dl->AddRectFilled(ImVec2(bp.x - 5, bp.y - 2), ImVec2(bp.x + s2.x + 5, bp.y + s2.y + 2), IM_COL32(0, 0, 0, 170), 5);
                    dl->AddText(bp, IM_COL32(170, 210, 255, 255), tag);
                    ImGui::PopFont();
                }
                ImGui::PopID();
            }
        }
    }
    clip.End();
    if (openCtx) ImGui::OpenPopup("libctx");
    if (ImGui::BeginPopup("libctx")) {
        auto sel = LibSelected();
        char b[128];
        snprintf(b, sizeof(b), T("Импортировать (%d)", "Import (%d)"), (int)sel.size());
        if (ImGui::MenuItem(b)) ImportPaths(sel);
        if (ctxIdx >= 0 && ctxIdx < n && ImGui::MenuItem(T("Показать в проводнике", "Show in Explorer"))) RevealInExplorer(a.libFiles[ctxIdx]);
        ImGui::EndPopup();
    }
    // click on empty space clears the selection
    if (ImGui::IsWindowHovered() && ImGui::IsMouseClicked(0) && !ImGui::IsAnyItemHovered() && !io.KeyCtrl && !io.KeyShift)
        std::fill(a.libSel.begin(), a.libSel.end(), 0);
    ImGui::EndChild();
    ImGui::EndChild();
    ImGui::PopStyleVar();
    ImGui::PopStyleColor();
    if (dbl >= 0) {
        auto sel = LibSelected();
        if (std::find(sel.begin(), sel.end(), a.libFiles[dbl]) == sel.end()) sel = { a.libFiles[dbl] };
        else { sel.erase(std::find(sel.begin(), sel.end(), a.libFiles[dbl])); sel.insert(sel.begin(), a.libFiles[dbl]); }
        bool keep = a.libOpenAfterImport;
        a.libOpenAfterImport = true;
        ImportPaths(sel);
        a.libOpenAfterImport = keep;
    }
}

void LibraryPanel() {
    App& a = g_app;
    int nSel = CountSel(a.libSel);
    ImGui::PushFont(a.fontBold);
    ImGui::TextUnformatted(T("Импорт", "Import"));
    ImGui::PopFont();
    W::Caption(T("Отметьте фото в сетке и добавьте их в рабочую ленту, они появятся во вкладках «Классика» и «Эксперименты».",
                 "Select photos in the grid and add them to the working filmstrip; they show up in the Classic and Experiments tabs."));
    ImGui::Spacing();
    char b[160];
    if (!a.libFolder.empty()) {
        snprintf(b, sizeof(b), T("В папке: %d фото  ·  выбрано: %d", "In folder: %d photos  ·  selected: %d"), (int)a.libFiles.size(), nSel);
        ImGui::TextUnformatted(b);
        ImGui::Spacing();
        float hw = (ImGui::GetContentRegionAvail().x - ImGui::GetStyle().ItemSpacing.x) * 0.5f;
        if (W::Button(T("Выбрать все", "Select all"), ImVec2(hw, 0))) std::fill(a.libSel.begin(), a.libSel.end(), 1);
        ImGui::SameLine();
        if (W::Button(T("Снять выбор", "Select none"), ImVec2(-1, 0))) std::fill(a.libSel.begin(), a.libSel.end(), 0);
        ImGui::Spacing();
        snprintf(b, sizeof(b), T("Импортировать выбранные (%d)", "Import selected (%d)"), nSel);
        ImGui::BeginDisabled(nSel == 0);
        if (W::Button(b, ImVec2(-1, ImGui::GetFrameHeight() * 1.35f), W::BTN_PRIMARY)) ImportPaths(LibSelected());
        ImGui::EndDisabled();
        snprintf(b, sizeof(b), T("Импортировать всю папку (%d)", "Import whole folder (%d)"), (int)a.libFiles.size());
        ImGui::BeginDisabled(a.libFiles.empty());
        if (W::Button(b, ImVec2(-1, 0))) ImportPaths(a.libFiles);
        ImGui::EndDisabled();
        W::Toggle(T("Сразу открыть первое фото", "Open the first photo"), &a.libOpenAfterImport);
    } else {
        if (W::Button(T("Выбрать папку…", "Choose folder…"), ImVec2(-1, ImGui::GetFrameHeight() * 1.35f), W::BTN_PRIMARY)) ChooseLibraryFolder();
    }
    if (W::Button(T("Добавить отдельные файлы…", "Add individual files…"), ImVec2(-1, 0))) {
        auto files = OpenFilesDialog(a.hwnd);
        if (!files.empty()) ImportPaths(files);
    }
    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();
    snprintf(b, sizeof(b), T("В рабочей ленте: %d фото", "In the filmstrip: %d photos"), (int)a.collection.size());
    ImGui::TextUnformatted(b);
    ImGui::BeginDisabled(a.collection.empty());
    if (W::Button(T("Очистить ленту", "Clear filmstrip"), ImVec2(-1, 0), W::BTN_DANGER)) {
        auto all = a.collection;
        CollectionRemove(all);
        SetStatus(T("Лента очищена. Сами файлы и их правки не тронуты.", "Filmstrip cleared. Files and their edits are untouched."));
    }
    ImGui::EndDisabled();
    ImGui::Spacing();
    W::Caption(T("Клик выбирает фото, Ctrl+клик добавляет к выбору, Shift+клик выбирает диапазон, Ctrl+A выбирает все, двойной клик сразу открывает.",
                 "Click selects, Ctrl+click adds, Shift+click selects a range, Ctrl+A selects all, double-click opens right away."));
}
