#include "ui_shared.h"
#include "preset.h"
#include "pipeline.h"
#include "effects.h"
#include "widgets.h"
#include "i18n.h"
#include "version.h"
#include "imgui.h"
#include "imgui_internal.h"
#include <shlwapi.h>
#include <shellapi.h>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <ctime>

App g_app;
int g_lang = 0;

static double Now() { return ImGui::GetTime(); }

void SetStatus(const std::string& s) { g_app.status = s; g_app.statusTime = Now(); }

// =====================================================================  history / sidecar
void Changed() { g_app.rev++; }

static void Commit() {
    App& a = g_app;
    if (a.histPos + 1 < (int)a.history.size()) a.history.resize(a.histPos + 1);
    a.history.push_back(a.params);
    if (a.history.size() > 200) a.history.erase(a.history.begin());
    a.histPos = (int)a.history.size() - 1;
    a.committedRev = a.rev;
}

static void RestoreHistory(int pos) {
    App& a = g_app;
    if (pos < 0 || pos >= (int)a.history.size()) return;
    a.histPos = pos;
    a.params = a.history[pos];
    for (auto& m : a.params.masks) m.brushRev = NewRev();
    if (a.selMask >= (int)a.params.masks.size()) a.selMask = (int)a.params.masks.size() - 1;
    a.rev++;
    a.committedRev = a.rev;
}
static void Undo() { if (g_app.histPos > 0) { RestoreHistory(g_app.histPos - 1); SetStatus(T("Отменено", "Undone")); } }
static void Redo() { if (g_app.histPos + 1 < (int)g_app.history.size()) { RestoreHistory(g_app.histPos + 1); SetStatus(T("Возвращено", "Redone")); } }

static std::wstring LocalDir(const wchar_t* sub) {
    wchar_t buf[MAX_PATH];
    if (!GetEnvironmentVariableW(L"LOCALAPPDATA", buf, MAX_PATH)) return L"";
    std::wstring d = std::wstring(buf) + L"\\Muon";
    CreateDirectoryW(d.c_str(), nullptr);
    if (sub) { d += L"\\"; d += sub; CreateDirectoryW(d.c_str(), nullptr); }
    return d;
}

static std::wstring SidecarPath(const std::wstring& img) { return img + L".muon"; }

// Used when the photo's folder is read-only: %LOCALAPPDATA%\Muon\sidecars\<hash>.muon
static std::wstring FallbackSidecar(const std::wstring& img) {
    std::wstring d = LocalDir(L"sidecars");
    if (d.empty()) return L"";
    uint64_t h = 1469598103934665603ull;
    for (wchar_t c : img) { h ^= (uint64_t)towlower(c); h *= 1099511628211ull; }
    wchar_t name[32]; swprintf(name, 32, L"\\%016llx.muon", (unsigned long long)h);
    return d + name;
}

static void SaveSidecar(bool force = false) {
    App& a = g_app;
    if (a.curPath.empty() || a.batchActive || a.noPersist) return;
    if (!force && a.savedRev == a.committedRev) return;
    std::string data = SerializeParams(a.params, true, true);
    if (!WriteTextFile(SidecarPath(a.curPath), data)) WriteTextFile(FallbackSidecar(a.curPath), data);
    a.savedRev = a.committedRev;
    a.lastSave = Now();
}

static std::wstring PresetDir() {
    static std::wstring dir;
    if (!dir.empty()) return dir;
    std::wstring d = ExeDir() + L"\\presets";
    CreateDirectoryW(d.c_str(), nullptr);
    std::wstring test = d + L"\\.write_test";
    if (WriteTextFile(test, "x")) { DeleteFileW(test.c_str()); dir = d; return dir; }
    dir = LocalDir(L"presets");
    return dir;
}

static std::wstring SafeFileName(const std::string& name) {
    std::wstring n = Utf8ToWide(name);
    for (auto& c : n) if (wcschr(L"\\/:*?\"<>|", c)) c = L'_';
    if (n.empty()) n = L"preset";
    return n;
}

static void ScanPresets() {
    auto& v = g_app.userPresets;
    v.clear();
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW((PresetDir() + L"\\*.muonpreset").c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return;
    do {
        std::wstring f = PresetDir() + L"\\" + fd.cFileName;
        std::string s;
        Preset p;
        if (ReadTextFile(f, s) && PresetFromText(s, p)) {
            if (p.name.empty()) { std::wstring n = fd.cFileName; p.name = WideToUtf8(n.substr(0, n.size() - 11)); }
            p.file = f;
            v.push_back(p);
        }
    } while (FindNextFileW(h, &fd));
    FindClose(h);
    std::sort(v.begin(), v.end(), [](const Preset& a, const Preset& b) { return a.name < b.name; });
}

// Stores a preset in the presets folder (never overwrites another preset).
static bool StorePreset(const Preset& p) {
    std::wstring base = PresetDir() + L"\\" + SafeFileName(p.name);
    std::wstring f = base + L".muonpreset";
    Preset named = p;
    for (int i = 2; GetFileAttributesW(f.c_str()) != INVALID_FILE_ATTRIBUTES && i < 1000; i++) {
        f = base + L" (" + std::to_wstring(i) + L").muonpreset";
        named.name = p.name + " (" + std::to_string(i) + ")";
    }
    bool ok = WriteTextFile(f, PresetToText(named));
    ScanPresets();
    return ok;
}

static std::wstring s_lastPath;
static int s_forceAlt = 0;   // --uitest only

// =====================================================================  app settings (language, export, view)
static std::wstring SettingsPath() { std::wstring d = LocalDir(nullptr); return d.empty() ? L"" : d + L"\\settings.txt"; }

static void SaveSettings() {
    App& a = g_app;
    if (a.noPersist) return;
    ExportSettings& e = a.exp;
    char buf[2048];
    snprintf(buf, sizeof(buf),
             "lang %d\nfilmstrip %d\nexp.fmt %d\nexp.bits16 %d\nexp.quality %d\nexp.chroma444 %d\nexp.tiffComp %d\nexp.limitSize %d\nexp.maxKB %d\n"
             "exp.sizeMode %d\nexp.sizeValue %d\nexp.megapixels %g\nexp.percent %d\nexp.noUpscale %d\nexp.sharpen %d\nexp.sharpenAmt %d\n"
             "exp.dest %d\nexp.overwrite %d\nexp.reveal %d\nexp.watermark %d\nexp.wmPos %d\nexp.wmSize %g\nexp.wmOpacity %g\nexp.wmDark %d\n",
             g_lang, (int)a.showFilmstrip, e.fmt, (int)e.bits16, e.quality, (int)e.chroma444, e.tiffComp, (int)e.limitSize, e.maxKB,
             e.sizeMode, e.sizeValue, e.megapixels, e.percent, (int)e.noUpscale, e.sharpen, e.sharpenAmt,
             e.dest, (int)e.overwrite, (int)e.reveal, (int)e.watermark, e.wmPos, e.wmSize, e.wmOpacity, (int)e.wmDark);
    std::string s = buf;
    char lb[128];
    snprintf(lb, sizeof(lb), "lib.thumb %g\nlib.open %d\n", a.libThumb, (int)a.libOpenAfterImport);
    s += lb;
    s += "lib.folder " + WideToUtf8(a.libFolder) + "\n";
    s += "last " + WideToUtf8(a.curPath.empty() ? s_lastPath : a.curPath) + "\n";
    s += "exp.folder " + WideToUtf8(e.folder) + "\n";
    s += std::string("exp.nameTpl ") + e.nameTpl + "\n";
    s += std::string("exp.wmText ") + e.wmText + "\n";
    std::wstring p = SettingsPath();
    if (!p.empty()) WriteTextFile(p, s);
}

static void LoadSettings() {
    App& a = g_app;
    ExportSettings& e = a.exp;
    LANGID ui = GetUserDefaultUILanguage();
    g_lang = PRIMARYLANGID(ui) == LANG_RUSSIAN ? 0 : 1;
    std::string s;
    std::wstring p = SettingsPath();
    if (p.empty() || !ReadTextFile(p, s)) return;
    size_t pos = 0;
    while (pos < s.size()) {
        size_t nl = s.find('\n', pos);
        std::string line = s.substr(pos, nl == std::string::npos ? std::string::npos : nl - pos);
        pos = nl == std::string::npos ? s.size() : nl + 1;
        if (!line.empty() && line.back() == '\r') line.pop_back();
        size_t sp = line.find(' ');
        if (sp == std::string::npos) continue;
        std::string k = line.substr(0, sp), v = line.substr(sp + 1);
        int iv = atoi(v.c_str());
        float fv = (float)atof(v.c_str());
        if (k == "lang") g_lang = iv == 1 ? 1 : 0;
        else if (k == "filmstrip") a.showFilmstrip = iv != 0;
        else if (k == "exp.fmt") e.fmt = std::clamp(iv, 0, 2);
        else if (k == "exp.bits16") e.bits16 = iv != 0;
        else if (k == "exp.quality") e.quality = std::clamp(iv, 1, 100);
        else if (k == "exp.chroma444") e.chroma444 = iv != 0;
        else if (k == "exp.tiffComp") e.tiffComp = std::clamp(iv, 0, 2);
        else if (k == "exp.limitSize") e.limitSize = iv != 0;
        else if (k == "exp.maxKB") e.maxKB = std::max(50, iv);
        else if (k == "exp.sizeMode") e.sizeMode = std::clamp(iv, 0, 6);
        else if (k == "exp.sizeValue") e.sizeValue = std::max(16, iv);
        else if (k == "exp.megapixels") e.megapixels = std::max(0.1f, fv);
        else if (k == "exp.percent") e.percent = std::clamp(iv, 1, 400);
        else if (k == "exp.noUpscale") e.noUpscale = iv != 0;
        else if (k == "exp.sharpen") e.sharpen = std::clamp(iv, 0, 3);
        else if (k == "exp.sharpenAmt") e.sharpenAmt = std::clamp(iv, 0, 2);
        else if (k == "exp.dest") e.dest = std::clamp(iv, 0, 2);
        else if (k == "exp.overwrite") e.overwrite = iv != 0;
        else if (k == "exp.reveal") e.reveal = iv != 0;
        else if (k == "exp.watermark") e.watermark = iv != 0;
        else if (k == "exp.wmPos") e.wmPos = std::clamp(iv, 0, 8);
        else if (k == "exp.wmSize") e.wmSize = std::clamp(fv, 0.5f, 20.0f);
        else if (k == "exp.wmOpacity") e.wmOpacity = std::clamp(fv, 0.0f, 100.0f);
        else if (k == "exp.wmDark") e.wmDark = iv != 0;
        else if (k == "exp.folder") e.folder = Utf8ToWide(v);
        else if (k == "lib.folder") a.libFolder = Utf8ToWide(v);
        else if (k == "lib.thumb") a.libThumb = std::clamp(fv, 110.0f, 320.0f);
        else if (k == "lib.open") a.libOpenAfterImport = iv != 0;
        else if (k == "last") s_lastPath = Utf8ToWide(v);
        else if (k == "exp.nameTpl") { strncpy(e.nameTpl, v.c_str(), sizeof(e.nameTpl) - 1); }
        else if (k == "exp.wmText") { strncpy(e.wmText, v.c_str(), sizeof(e.wmText) - 1); }
    }
}

std::wstring ReadGpuPreference() {
    std::wstring d = LocalDir(nullptr);
    std::string s;
    if (d.empty() || !ReadTextFile(d + L"\\gpu.txt", s)) return L"";
    while (!s.empty() && (s.back() == '\n' || s.back() == '\r')) s.pop_back();
    return Utf8ToWide(s);
}

void WriteGpuPreference(const std::wstring& name) {
    std::wstring d = LocalDir(nullptr);
    if (!d.empty()) WriteTextFile(d + L"\\gpu.txt", WideToUtf8(name));
}

std::wstring AppCurrentPath() { return g_app.curPath; }

void ApplyLanguage() {
    if (g_app.hwnd) SetWindowTextW(g_app.hwnd, TW(L"Muon. Фоторедактор", L"Muon. Photo editor"));
}

static void SetLanguage(int l) {
    if (g_lang == l) return;
    g_lang = l;
    ApplyLanguage();
    SaveSettings();
}

// =====================================================================  loading
void AppOpenFile(const std::wstring& path) {
    App& a = g_app;
    if (a.loading) return;
    SaveSidecar();
    a.loading = true;
    a.loadingPath = path;
    a.loader = std::async(std::launch::async, [path]() {
        auto img = std::make_shared<LoadedImage>();
        LoadImageFileSafe(path, *img);
        return img;
    });
}

void ApplyDefaultsForNew(Params& p, bool raw) {
    if (raw) { p.sharpAmount = 30; p.nrColor = 20; }
}

static bool ExportCurrent(int batchIndex, const std::wstring& forcedPath = L"");

static void OnLoaded(std::shared_ptr<LoadedImage> img) {
    App& a = g_app;
    a.loading = false;
    if (!img->error.empty() || (img->rgba16.empty() && img->rgba8.empty())) {
        SetStatus(img->error.empty() ? T("Не удалось открыть файл", "Could not open the file") : img->error);
        if (a.batchActive) a.batchIndex++;
        return;
    }
    g_pipe.setSource(*img);
    delete a.current;
    a.current = new LoadedImage();
    a.current->preview = img->preview; a.current->sw = img->sw; a.current->sh = img->sh; a.current->isRaw = img->isRaw;
    a.curPath = img->path;
    a.info = img->info;
    a.isRaw = img->isRaw;
    Params p;
    std::string side;
    bool hadSidecar = (ReadTextFile(SidecarPath(a.curPath), side) || ReadTextFile(FallbackSidecar(a.curPath), side)) && DeserializeParams(side, p, true, true);
    if (!hadSidecar) ApplyDefaultsForNew(p, a.isRaw);
    a.params = p;
    a.history.clear(); a.histPos = -1;
    a.rev++;
    Commit();
    a.savedRev = a.committedRev;
    a.selMask = a.params.masks.empty() ? -1 : 0;
    a.selEffect = -1;
    a.fit = true; a.panX = a.panY = 0;
    a.pick = PICK_NONE;
    if (a.tool == TOOL_CROP) a.tool = TOOL_EDIT;

    if (!a.batchActive) {
        CollectionAdd({ a.curPath });
        if (a.bottomTab == TAB_LIBRARY) a.bottomTab = TAB_CLASSIC;
    }
    a.curIndex = CollectionIndex(a.curPath);
    if (!a.batchActive && a.curIndex >= 0 && (a.curIndex >= (int)a.collSel.size() || !a.collSel[a.curIndex])) CollectionSelectOnly(a.curIndex);
    // photos that already carry edits get a fresh thumbnail right away
    a.thumbRev = hadSidecar ? 0 : a.committedRev;
    a.lastCommitTime = 0;
    if (!a.batchActive) {
        SetStatus(WideToUtf8(PathFindFileNameW(a.curPath.c_str())));
        if (!img->notice.empty()) { SetStatus(img->notice); a.statusTime = Now() + 6; }
        s_lastPath = a.curPath;
        SaveSettings();
    }
    if (a.batchActive) {
        if (ExportCurrent((int)a.batchIndex + 1)) a.batchDone++;
        a.batchIndex++;
    }
}

static void PollLoader() {
    App& a = g_app;
    if (a.loading && a.loader.valid() && a.loader.wait_for(std::chrono::seconds(0)) == std::future_status::ready) OnLoaded(a.loader.get());
    if (a.batchActive && !a.loading) {
        if (a.batchIndex < a.batchFiles.size()) AppOpenFile(a.batchFiles[a.batchIndex]);
        else {
            a.batchActive = false;
            char b[160];
            snprintf(b, sizeof(b), T("Пакетный экспорт завершён: %d из %d", "Batch export finished: %d of %d"), a.batchDone, (int)a.batchFiles.size());
            SetStatus(b);
            if (a.exp.reveal && !a.batchOut.empty()) ShellExecuteW(nullptr, L"open", a.batchOut.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
            if (!a.batchReturn.empty()) AppOpenFile(a.batchReturn);
        }
    }
}

// =====================================================================  auto tools
static void SolveWB(float r, float g, float b, Params& p) {
    r = std::max(r, 1e-5f); g = std::max(g, 1e-5f); b = std::max(b, 1e-5f);
    float t = (std::log(b) - std::log(r)) / 1.1f;
    float ti = (std::log(g) - std::log(r) - 0.55f * t) / 0.35f;
    p.temp = std::clamp(t * 100, -100.0f, 100.0f);
    p.tint = std::clamp(ti * 100, -100.0f, 100.0f);
}

static void AutoWB() {
    App& a = g_app;
    if (!a.current) return;
    double s[3] = {}; int n = 0;
    auto& sm = a.current->preview;
    for (size_t i = 0; i + 2 < sm.size(); i += 3) {
        float mx = std::max(sm[i], std::max(sm[i + 1], sm[i + 2]));
        if (mx > 0.95f || mx < 0.01f) continue;
        for (int c = 0; c < 3; c++) s[c] += sm[i + c];
        n++;
    }
    if (!n) return;
    SolveWB((float)s[0], (float)s[1], (float)s[2], a.params);
    Changed();
    SetStatus(T("Баланс белого подобран автоматически", "Auto white balance applied"));
}

static void AutoTone() {
    App& a = g_app;
    if (!a.current) return;
    auto& sm = a.current->preview;
    std::vector<float> L;
    double logSum = 0;
    for (size_t i = 0; i + 2 < sm.size(); i += 3) {
        float l = 0.2126f * sm[i] + 0.7152f * sm[i + 1] + 0.0722f * sm[i + 2];
        L.push_back(l);
        logSum += std::log(std::max(l, 1e-4f));
    }
    if (L.empty()) return;
    std::sort(L.begin(), L.end());
    float geo = (float)std::exp(logSum / L.size());
    float baseline = a.isRaw ? 0.5f : 0.0f;
    float ev = std::clamp(std::log2(0.19f / std::max(geo, 1e-4f)) - baseline, -3.0f, 3.0f);
    float gain = std::pow(2.0f, ev + baseline);
    float p99 = L[(size_t)(L.size() * 0.99)] * gain, p03 = L[(size_t)(L.size() * 0.03)] * gain;
    Params& p = a.params;
    p.exposure = std::round(ev * 20) / 20;
    p.highlights = std::clamp(-(p99 - 0.8f) * 70, -70.0f, 0.0f);
    p.shadows = std::clamp((0.04f - p03) * 600, 0.0f, 50.0f);
    p.whites = std::clamp((0.9f - p99) * 40, -20.0f, 25.0f);
    p.blacks = -5;
    p.vibrance = std::max(p.vibrance, 10.0f);
    Changed();
    SetStatus(T("Тон подобран автоматически", "Auto tone applied"));
}

void ResetAll() {
    App& a = g_app;
    Params n;
    ApplyDefaultsForNew(n, a.isRaw);
    a.params = n;
    a.selMask = -1;
    a.selEffect = -1;
    a.pick = PICK_NONE;
    Changed();
    SetStatus(T("Все правки сброшены. Ctrl+Z вернёт их", "All edits reset. Ctrl+Z brings them back"));
}

// =====================================================================  export
static void ExportTargetSize(const ExportSettings& e, int& W, int& H) {
    Geo g = g_pipe.geometry(g_app.params, false);
    float fw = (float)g.fullW, fh = (float)g.fullH, s = 1;
    switch (e.sizeMode) {
    case 1: s = e.sizeValue / std::max(fw, fh); break;
    case 2: s = e.sizeValue / std::min(fw, fh); break;
    case 3: s = e.sizeValue / fw; break;
    case 4: s = e.sizeValue / fh; break;
    case 5: s = std::sqrt(e.megapixels * 1e6f / (fw * fh)); break;
    case 6: s = e.percent / 100.0f; break;
    }
    if (e.noUpscale) s = std::min(s, 1.0f);
    W = std::max(1, (int)std::lround(fw * s));
    H = std::max(1, (int)std::lround(fh * s));
}

static float OutputSharpen(const ExportSettings& e) {
    if (e.sharpen == 0) return 0;
    static const float amt[3] = { 0.25f, 0.45f, 0.75f };
    static const float kind[4] = { 0, 1.0f, 1.45f, 1.15f };
    return amt[e.sharpenAmt] * kind[e.sharpen];
}

static const wchar_t* ExtFor(int fmt) { return fmt == EXP_JPEG ? L".jpg" : (fmt == EXP_TIFF ? L".tif" : L".png"); }

// Expand {name} {n} {date} {w} {h} in the file name template.
static std::wstring ExpandName(const std::wstring& src, int index, int w, int h) {
    std::wstring stem = PathFindFileNameW(src.c_str());
    stem = stem.substr(0, stem.find_last_of(L'.'));
    std::wstring tpl = Utf8ToWide(g_app.exp.nameTpl);
    if (tpl.empty()) tpl = L"{name}";
    time_t t = time(nullptr); tm lt; localtime_s(&lt, &t);
    wchar_t date[32]; wcsftime(date, 32, L"%Y-%m-%d", &lt);
    wchar_t num[16]; swprintf(num, 16, L"%03d", index);
    auto rep = [&](const std::wstring& key, const std::wstring& val) {
        size_t p;
        while ((p = tpl.find(key)) != std::wstring::npos) tpl.replace(p, key.size(), val);
    };
    rep(L"{name}", stem); rep(L"{n}", num); rep(L"{date}", date);
    rep(L"{w}", std::to_wstring(w)); rep(L"{h}", std::to_wstring(h));
    for (auto& c : tpl) if (wcschr(L"\\/:*?\"<>|", c)) c = L'_';
    return tpl;
}

static std::wstring UniquePath(const std::wstring& path) {
    if (g_app.exp.overwrite || GetFileAttributesW(path.c_str()) == INVALID_FILE_ATTRIBUTES) return path;
    size_t dot = path.find_last_of(L'.');
    std::wstring base = path.substr(0, dot), ext = path.substr(dot);
    for (int i = 2; i < 10000; i++) {
        std::wstring p = base + L"_" + std::to_wstring(i) + ext;
        if (GetFileAttributesW(p.c_str()) == INVALID_FILE_ATTRIBUTES) return p;
    }
    return path;
}

// Renders and writes the current photo. batchIndex > 0 for batch runs (then the folder is a.batchOut).
static bool ExportCurrent(int batchIndex, const std::wstring& forcedPath) {
    App& a = g_app;
    ExportSettings& e = a.exp;
    int W, H;
    ExportTargetSize(e, W, H);
    std::wstring out = forcedPath;
    if (out.empty()) {
        std::wstring name = ExpandName(a.curPath, std::max(1, batchIndex), W, H) + ExtFor(e.fmt);
        std::wstring dir;
        if (batchIndex > 0) dir = a.batchOut;
        else if (e.dest == 1) { wchar_t f[MAX_PATH * 2]; wcsncpy(f, a.curPath.c_str(), MAX_PATH * 2 - 1); f[MAX_PATH * 2 - 1] = 0; PathRemoveFileSpecW(f); dir = f; }
        else if (e.dest == 2 && !e.folder.empty()) dir = e.folder;
        if (dir.empty()) {
            out = SaveFileDialog(a.hwnd, name.substr(0, name.size() - 4), e.fmt);
            if (out.empty()) return false;
        } else out = UniquePath(dir + L"\\" + name);
    }
    std::vector<uint16_t> rgba; int w, h;
    if (!g_pipe.renderExport(a.params, W, H, OutputSharpen(e), rgba, w, h)) { SetStatus(T("Ошибка при обработке для экспорта", "Rendering for export failed")); return false; }
    if (e.watermark && e.wmText[0]) ApplyWatermark(rgba, w, h, Utf8ToWide(e.wmText), e.wmSize, e.wmOpacity / 100.0f, e.wmPos, e.wmDark);
    EncodeOptions eo;
    eo.fmt = e.fmt; eo.bits16 = e.bits16; eo.quality = e.quality; eo.chroma444 = e.chroma444; eo.tiffComp = e.tiffComp;
    eo.maxKB = (e.fmt == EXP_JPEG && e.limitSize) ? e.maxKB : 0;
    int q = 0;
    a.needRender = true;
    if (!SaveImage(out, eo, rgba, w, h, &q)) { SetStatus(T("Не удалось сохранить файл", "Could not save the file")); return false; }
    char b[300];
    std::string fn = WideToUtf8(PathFindFileNameW(out.c_str()));
    if (eo.maxKB > 0) snprintf(b, sizeof(b), T("Сохранено: %s  ·  %d × %d  ·  качество %d", "Saved: %s  ·  %d × %d  ·  quality %d"), fn.c_str(), w, h, q);
    else snprintf(b, sizeof(b), T("Сохранено: %s  ·  %d × %d", "Saved: %s  ·  %d × %d"), fn.c_str(), w, h);
    SetStatus(b);
    if (batchIndex == 0 && e.reveal) RevealInExplorer(out);
    return true;
}

static void StartBatch(const std::vector<std::wstring>& files) {
    App& a = g_app;
    if (files.empty()) return;
    std::wstring out;
    if (a.exp.dest == 2 && !a.exp.folder.empty()) out = a.exp.folder;
    else if (a.exp.dest == 1) {
        wchar_t f[MAX_PATH * 2]; wcsncpy(f, files[0].c_str(), MAX_PATH * 2 - 1); f[MAX_PATH * 2 - 1] = 0; PathRemoveFileSpecW(f); out = f;
    } else out = PickFolderDialog(a.hwnd);
    if (out.empty()) return;
    SaveSidecar(true);
    a.batchFiles = files;
    a.batchIndex = 0;
    a.batchDone = 0;
    a.batchOut = out;
    a.batchReturn = a.curPath;
    a.batchActive = true;
}

void OpenExport(const std::vector<std::wstring>& paths) { g_app.exportTargets = paths; g_app.exportOpen = true; }

// =====================================================================  small helpers
static const char* HueName(int i) {
    static const char* ru[8] = { "Красный", "Оранжевый", "Жёлтый", "Зелёный", "Голубой", "Синий", "Фиолетовый", "Пурпурный" };
    static const char* en[8] = { "Red", "Orange", "Yellow", "Green", "Aqua", "Blue", "Purple", "Magenta" };
    return g_lang == 1 ? en[i] : ru[i];
}
static const float kHueCenters[8] = { 0, 30, 60, 120, 180, 225, 270, 315 };
static const ImU32 kGradTemp[2] = { IM_COL32(60, 120, 230, 255), IM_COL32(235, 200, 60, 255) };
static const ImU32 kGradTint[2] = { IM_COL32(60, 200, 80, 255), IM_COL32(220, 70, 210, 255) };
static const ImU32 kGradSat[2] = { IM_COL32(128, 128, 128, 255), IM_COL32(230, 60, 160, 255) };
static const ImU32 kGradLum[2] = { IM_COL32(0, 0, 0, 255), IM_COL32(255, 255, 255, 255) };
static void HueGrad(ImU32 g[7]) { for (int i = 0; i < 7; i++) g[i] = W::HueColor(i * 60.0f); }

static bool Sl(const char* label, float* v, float mn, float mx, float def, const char* fmt = nullptr, const ImU32* g = nullptr, int n = 0) {
    bool c = W::Slider(label, v, mn, mx, def, fmt, g, n);
    if (c) Changed();
    return c;
}
// Alt + drag shows clipping like Lightroom: highlights view or shadows view.
static bool SlClip(const char* label, float* v, float mn, float mx, float def, int mode, const char* fmt = nullptr) {
    bool c = Sl(label, v, mn, mx, def, fmt);
    if (ImGui::IsItemActive() && ImGui::GetIO().KeyAlt) g_app.altView = mode;
    W::Hint(T("Зажмите Alt во время перетаскивания, чтобы увидеть, где теряются детали", "Hold Alt while dragging to see where detail is clipped"));
    return c;
}
static bool Tg(const char* label, bool* v) { bool c = W::Toggle(label, v); if (c) Changed(); return c; }

static float HalfWidth() { return (ImGui::GetContentRegionAvail().x - ImGui::GetStyle().ItemSpacing.x) * 0.5f; }
static float ThirdWidth() { return (ImGui::GetContentRegionAvail().x - ImGui::GetStyle().ItemSpacing.x * 2) / 3; }

static std::vector<Preset>& Builtins() {
    static std::vector<Preset> v[2];
    int l = g_lang == 1 ? 1 : 0;
    if (v[l].empty()) v[l] = BuiltinPresetList();
    return v[l];
}

static void ApplyPreset(const Preset& pr) {
    CopyGroups(g_app.params, pr.params, pr.groups);
    Changed();
    SetStatus(std::string(T("Пресет: ", "Preset: ")) + pr.name);
}

static std::string PresetTooltip(const Preset& p) {
    std::string s = p.name;
    if (!p.author.empty()) s += std::string("\n") + T("Автор: ", "Author: ") + p.author;
    if (!p.description.empty()) s += "\n" + p.description;
    s += std::string("\n\n") + T("Меняет: ", "Changes: ");
    bool first = true;
    for (int b = 0; b < SG_COUNT; b++) if (p.groups & (1u << b)) { if (!first) s += ", "; s += GroupName(b); first = false; }
    return s;
}

static void ImportPresetFiles(const std::vector<std::wstring>& files) {
    int ok = 0;
    for (auto& f : files) {
        std::string s, err;
        Preset p;
        if (ReadTextFile(f, s) && PresetFromText(s, p, &err)) {
            if (p.name.empty()) { std::wstring n = PathFindFileNameW(f.c_str()); p.name = WideToUtf8(n.substr(0, n.find_last_of(L'.'))); }
            if (StorePreset(p)) ok++;
        } else SetStatus(err.empty() ? T("Не удалось прочитать пресет", "Could not read the preset") : err);
    }
    if (ok) { char b[96]; snprintf(b, sizeof(b), T("Импортировано пресетов: %d", "Presets imported: %d"), ok); SetStatus(b); }
}

static void PastePresetCode() {
    std::string clip = ClipboardGetText(), err;
    Preset p;
    if (!PresetFromCode(clip, p, &err)) { SetStatus(err); return; }
    if (StorePreset(p)) SetStatus(std::string(T("Пресет добавлен: ", "Preset added: ")) + p.name);
}

static void GroupToggles(uint32_t& g) {
    for (int b = 0; b < SG_COUNT; b++) {
        bool on = (g & (1u << b)) != 0;
        ImGui::PushID(b);
        if (W::Toggle(GroupName(b), &on)) g = on ? (g | (1u << b)) : (g & ~(1u << b));
        ImGui::PopID();
    }
    float hw = HalfWidth();
    if (W::Button(T("Отметить всё", "Select all"), ImVec2(hw, 0))) g = SG_ALL;
    ImGui::SameLine();
    if (W::Button(T("Снять всё", "Clear all"), ImVec2(-1, 0))) g = 0;
}

static char s_pName[96], s_pAuthor[64], s_pDesc[256];
static uint32_t s_pGroups = SG_LOOK | SG_EFFECTS;

static void NewPresetModal() {
    App& a = g_app;
    const char* title = T("Новый пресет###newpreset", "New preset###newpreset");
    if (a.newPresetOpen) { ImGui::OpenPopup(title); a.newPresetOpen = false; }
    ImGui::SetNextWindowSize(ImVec2(470 * a.dpi, 0));
    ImGui::SetNextWindowPos(ImGui::GetMainViewport()->GetCenter(), ImGuiCond_Always, ImVec2(0.5f, 0.5f));
    if (!ImGui::BeginPopupModal(title, nullptr, ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove)) return;
    ImGui::SetNextItemWidth(-1);
    if (ImGui::IsWindowAppearing()) ImGui::SetKeyboardFocusHere();
    ImGui::InputTextWithHint("##n", T("Название", "Name"), s_pName, sizeof(s_pName));
    ImGui::SetNextItemWidth(-1);
    ImGui::InputTextWithHint("##a", T("Автор (необязательно)", "Author (optional)"), s_pAuthor, sizeof(s_pAuthor));
    ImGui::InputTextMultiline("##d", s_pDesc, sizeof(s_pDesc), ImVec2(-1, ImGui::GetFrameHeight() * 2.2f));
    W::Caption(T("Что войдёт в пресет:", "What the preset includes:"));
    ImGui::BeginChild("##g", ImVec2(0, ImGui::GetFrameHeightWithSpacing() * 8.0f), 0);
    GroupToggles(s_pGroups);
    ImGui::EndChild();
    ImGui::Spacing();
    float bw = ThirdWidth();
    if (W::Button(T("Отмена", "Cancel"), ImVec2(bw, 0))) ImGui::CloseCurrentPopup();
    ImGui::SameLine();
    bool can = s_pName[0] != 0 && s_pGroups != 0;
    ImGui::BeginDisabled(!can);
    bool save = W::Button(T("Сохранить", "Save"), ImVec2(bw, 0), W::BTN_PRIMARY);
    ImGui::SameLine();
    bool saveCode = W::Button(T("Сохранить + код", "Save + code"), ImVec2(-1, 0));
    W::Hint(T("Сохранить и сразу скопировать код, которым можно поделиться в сообщении", "Save and copy a code you can share in a message"));
    ImGui::EndDisabled();
    if (save || saveCode) {
        Preset p;
        p.name = s_pName; p.author = s_pAuthor; p.description = s_pDesc; p.groups = s_pGroups; p.params = a.params;
        if (StorePreset(p)) {
            if (saveCode && ClipboardSetText(PresetToCode(p))) SetStatus(T("Пресет сохранён, код скопирован в буфер обмена", "Preset saved, share code copied"));
            else SetStatus(T("Пресет сохранён", "Preset saved"));
        } else SetStatus(T("Не удалось сохранить пресет", "Could not save the preset"));
        s_pName[0] = s_pDesc[0] = 0;
        ImGui::CloseCurrentPopup();
    }
    ImGui::EndPopup();
}

static bool PresetButton(const Preset& p, int key, ImVec2 size) {
    App& a = g_app;
    bool rescanned = false;
    ImGui::PushID(key);
    if (W::Button(p.name.c_str(), size)) ApplyPreset(p);
    if (ImGui::IsItemHovered()) {
        a.hoverPreset = key;
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal) && ImGui::BeginTooltip()) {
            ImGui::PushTextWrapPos(320 * a.dpi);
            ImGui::TextUnformatted(PresetTooltip(p).c_str());
            ImGui::PopTextWrapPos();
            ImGui::EndTooltip();
        }
    }
    if (ImGui::BeginPopupContextItem("pctx")) {
        if (ImGui::MenuItem(T("Применить", "Apply"))) ApplyPreset(p);
        if (ImGui::MenuItem(T("Копировать код для обмена", "Copy share code")))
            if (ClipboardSetText(PresetToCode(p))) SetStatus(T("Код пресета скопирован, его можно отправить кому угодно", "Preset code copied, send it to anyone"));
        if (ImGui::MenuItem(T("Сохранить в файл…", "Save to file…"))) {
            std::wstring f = SavePresetDialog(a.hwnd, SafeFileName(p.name));
            if (!f.empty()) SetStatus(WriteTextFile(f, PresetToText(p)) ? T("Пресет сохранён в файл", "Preset saved to file") : T("Не удалось сохранить файл", "Could not save the file"));
        }
        if (!p.builtin) {
            if (ImGui::MenuItem(T("Показать в проводнике", "Show in Explorer"))) RevealInExplorer(p.file);
            ImGui::Separator();
            if (ImGui::MenuItem(T("Удалить", "Delete"))) { DeleteFileW(p.file.c_str()); rescanned = true; }
        }
        ImGui::EndPopup();
    }
    ImGui::PopID();
    if (rescanned) ScanPresets();
    return rescanned;
}

// ---------------------------------------------------------------- settings of other photos, clipboard
bool ReadSidecarFor(const std::wstring& path, Params& out) {
    std::string s;
    if (!(ReadTextFile(SidecarPath(path), s) || ReadTextFile(FallbackSidecar(path), s))) return false;
    Params p;
    if (!DeserializeParams(s, p, true, true)) return false;
    out = p;
    return true;
}

void WriteSidecarFor(const std::wstring& path, const Params& p) {
    if (g_app.noPersist) return;
    std::string data = SerializeParams(p, true, true);
    if (!WriteTextFile(SidecarPath(path), data)) WriteTextFile(FallbackSidecar(path), data);
}

void DeleteSidecarFor(const std::wstring& path) {
    if (g_app.noPersist) return;
    DeleteFileW(SidecarPath(path).c_str());
    std::wstring f = FallbackSidecar(path);
    if (!f.empty()) DeleteFileW(f.c_str());
}

static bool IsCurrent(const std::wstring& path) { return !g_app.curPath.empty() && _wcsicmp(path.c_str(), g_app.curPath.c_str()) == 0; }

static Params ParamsOf(const std::wstring& path) {
    if (IsCurrent(path)) return g_app.params;
    Params p;
    if (!ReadSidecarFor(path, p)) ApplyDefaultsForNew(p, IsRawExt(path));
    return p;
}

void CopySettingsFrom(const std::wstring& path, uint32_t groups) {
    if (path.empty()) return;
    g_app.clip.p = ParamsOf(path);
    g_app.clip.groups = groups;
    g_app.clip.valid = true;
    SetStatus(T("Настройки скопированы", "Settings copied"));
}

void OpenCopyDialog(const std::wstring& path) {
    if (path.empty()) return;
    g_app.copySource = path;
    if (!g_app.copyGroups) g_app.copyGroups = SG_DEVELOP;
    g_app.copyDialogOpen = true;
}

void PasteSettingsTo(const std::vector<std::wstring>& paths) {
    App& a = g_app;
    if (!a.clip.valid) { SetStatus(T("Сначала скопируйте настройки", "Copy settings first")); return; }
    int n = 0;
    for (auto& path : paths) {
        if (IsCurrent(path)) { CopyGroups(a.params, a.clip.p, a.clip.groups); Changed(); }
        else { Params p = ParamsOf(path); CopyGroups(p, a.clip.p, a.clip.groups); WriteSidecarFor(path, p); }
        n++;
    }
    char b[128]; snprintf(b, sizeof(b), T("Настройки вставлены в фото: %d", "Settings pasted into %d photo(s)"), n);
    SetStatus(b);
}

void ResetSettingsOf(const std::vector<std::wstring>& paths) {
    int n = 0;
    for (auto& path : paths) {
        if (IsCurrent(path)) ResetAll();
        else DeleteSidecarFor(path);
        n++;
    }
    char b[128]; snprintf(b, sizeof(b), T("Правки сброшены у фото: %d", "Edits reset on %d photo(s)"), n);
    SetStatus(b);
}

static void CopyDialogModal() {
    App& a = g_app;
    const char* title = T("Копировать настройки###copyset", "Copy settings###copyset");
    if (a.copyDialogOpen) { ImGui::OpenPopup(title); a.copyDialogOpen = false; }
    ImGui::SetNextWindowSize(ImVec2(440 * a.dpi, 0));
    ImGui::SetNextWindowPos(ImGui::GetMainViewport()->GetCenter(), ImGuiCond_Always, ImVec2(0.5f, 0.5f));
    if (!ImGui::BeginPopupModal(title, nullptr, ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove)) return;
    W::Caption((std::string(T("Из фото: ", "From: ")) + WideToUtf8(PathFindFileNameW(a.copySource.c_str()))).c_str());
    ImGui::BeginChild("##g", ImVec2(0, ImGui::GetFrameHeightWithSpacing() * 9.0f), 0);
    GroupToggles(a.copyGroups);
    ImGui::EndChild();
    ImGui::Spacing();
    float hw = HalfWidth();
    if (W::Button(T("Отмена", "Cancel"), ImVec2(hw, 0))) ImGui::CloseCurrentPopup();
    ImGui::SameLine();
    ImGui::BeginDisabled(a.copyGroups == 0);
    if (W::Button(T("Копировать", "Copy"), ImVec2(-1, 0), W::BTN_PRIMARY)) { CopySettingsFrom(a.copySource, a.copyGroups); ImGui::CloseCurrentPopup(); }
    ImGui::EndDisabled();
    ImGui::EndPopup();
}

void AppDropFiles(const std::vector<std::wstring>& paths) {
    App& a = g_app;
    std::vector<std::wstring> imgs, presets;
    for (auto& p : paths) {
        if (IsDirectory(p)) { LibraryOpenFolder(p); a.bottomTab = TAB_LIBRARY; continue; }
        std::wstring ext = PathFindExtensionW(p.c_str());
        for (auto& c : ext) c = (wchar_t)towlower(c);
        if (ext == L".muonpreset") presets.push_back(p);
        else if (IsSupportedExt(p)) imgs.push_back(p);
    }
    if (!presets.empty()) ImportPresetFiles(presets);
    if (!imgs.empty()) {
        CollectionAdd(imgs);
        if (a.bottomTab == TAB_LIBRARY) a.bottomTab = TAB_CLASSIC;
        CollectionSelectOnly(CollectionIndex(imgs[0]));
        AppOpenFile(imgs[0]);
    }
}

// =====================================================================  classic panels
static void PanelPresets() {
    App& a = g_app;
    if (!W::Section(T("Пресеты", "Presets"), false)) return;
    W::Caption(T("Наведите курсор на пресет, чтобы сразу увидеть его на фото. Правым кликом можно поделиться.",
                 "Hover a preset to preview it on the photo. Right-click to share."));
    auto& bi = Builtins();
    float w = HalfWidth();
    for (size_t i = 0; i < bi.size(); i++) {
        if (i % 2) ImGui::SameLine();
        PresetButton(bi[i], 1000 + (int)i, ImVec2(i % 2 ? -1 : w, 0));
    }
    ImGui::Spacing();
    W::Caption(T("Мои пресеты", "My presets"));
    if (a.userPresets.empty()) W::Caption(T("Пока пусто: создайте свой пресет или импортируйте чужой.", "Empty: create a preset or import someone else's."));
    for (size_t i = 0; i < a.userPresets.size(); i++) {
        if (i % 2) ImGui::SameLine();
        if (PresetButton(a.userPresets[i], 2000 + (int)i, ImVec2(i % 2 ? -1 : w, 0))) break;
    }
    ImGui::Spacing();
    if (W::Button(T("+  Создать пресет из текущих правок", "+  New preset from current edits"), ImVec2(-1, 0), W::BTN_PRIMARY)) a.newPresetOpen = true;
    float hw = HalfWidth();
    if (W::Button(T("Импорт файлов…", "Import files…"), ImVec2(hw, 0))) ImportPresetFiles(OpenPresetFilesDialog(a.hwnd));
    W::Hint(T("Добавить пресеты из файлов .muonpreset (можно и перетащить их в окно)", "Add presets from .muonpreset files (or drop them onto the window)"));
    ImGui::SameLine();
    if (W::Button(T("Вставить код", "Paste code"), ImVec2(-1, 0))) PastePresetCode();
    W::Hint(T("Добавить пресет из кода MUON1-…, скопированного из сообщения", "Add a preset from a MUON1-… code copied from a message"));
}

static void PanelBasic() {
    Params& p = g_app.params;
    bool reset = false;
    bool open = W::Section(T("Основные", "Basic"), true, &reset);
    if (reset) {
        Params d;
#define R(n, l, mi, ma, df) p.n = d.n;
        BASIC_PARAMS(R)
#undef R
        p.profile = 0;
        Changed();
    }
    if (!open) return;
    ImGui::PushID("basic");
    const char* profiles[] = { T("Стандартный", "Standard"), T("Нейтральный", "Neutral"), T("Яркий", "Vivid"), T("Плоский", "Flat") };
    ImGui::AlignTextToFramePadding();
    ImGui::TextDisabled("%s", T("Профиль", "Profile"));
    ImGui::SameLine(ImGui::GetContentRegionAvail().x * 0.3f);
    ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x - 76 * g_app.dpi);
    if (ImGui::Combo("##profile", &p.profile, profiles, 4)) Changed();
    ImGui::SameLine();
    if (W::Button(T("Авто", "Auto"), ImVec2(-1, 0), W::BTN_PRIMARY)) AutoTone();
    W::Hint(T("Автоматически подобрать экспозицию и тон", "Automatically set exposure and tone"));

    ImGui::Spacing();
    ImGui::AlignTextToFramePadding();
    ImGui::TextDisabled("%s", T("Баланс белого", "White balance"));
    float bw = 76 * g_app.dpi;
    ImGui::SameLine(ImGui::GetContentRegionAvail().x - bw * 2 - ImGui::GetStyle().ItemSpacing.x);
    if (W::Button(T("Пипетка", "Picker"), ImVec2(bw, 0), W::BTN_DEFAULT, g_app.pick == PICK_WB)) g_app.pick = g_app.pick == PICK_WB ? PICK_NONE : PICK_WB;
    W::Hint(T("Кликните по нейтрально-серому участку фото", "Click a neutral grey area of the photo"));
    ImGui::SameLine();
    if (W::Button(T("Авто##wb", "Auto##wb"), ImVec2(bw, 0))) AutoWB();
    Sl(T("Температура", "Temperature"), &p.temp, -100, 100, 0, nullptr, kGradTemp, 2);
    Sl(T("Оттенок", "Tint"), &p.tint, -100, 100, 0, nullptr, kGradTint, 2);
    ImGui::Separator();
    SlClip(T("Экспозиция", "Exposure"), &p.exposure, -5, 5, 0, ALT_HIGHLIGHTS, "%+.2f");
    Sl(T("Контраст", "Contrast"), &p.contrast, -100, 100, 0);
    ImGui::Separator();
    SlClip(T("Света", "Highlights"), &p.highlights, -100, 100, 0, ALT_HIGHLIGHTS);
    SlClip(T("Тени", "Shadows"), &p.shadows, -100, 100, 0, ALT_SHADOWS);
    SlClip(T("Белые", "Whites"), &p.whites, -100, 100, 0, ALT_HIGHLIGHTS);
    SlClip(T("Чёрные", "Blacks"), &p.blacks, -100, 100, 0, ALT_SHADOWS);
    ImGui::Separator();
    Sl(T("Текстура", "Texture"), &p.texture, -100, 100, 0);
    Sl(T("Чёткость", "Clarity"), &p.clarity, -100, 100, 0);
    Sl(T("Устранение дымки", "Dehaze"), &p.dehaze, -100, 100, 0);
    ImGui::Separator();
    Sl(T("Красочность", "Vibrance"), &p.vibrance, -100, 100, 0, nullptr, kGradSat, 2);
    Sl(T("Насыщенность", "Saturation"), &p.saturation, -100, 100, 0, nullptr, kGradSat, 2);
    ImGui::PopID();
}

static void PanelCurve() {
    Params& p = g_app.params;
    bool reset = false;
    bool open = W::Section(T("Тоновая кривая", "Tone curve"), false, &reset);
    if (reset) { for (auto& c : p.curve) c = Curve(); Changed(); }
    if (!open) return;
    static const char* ch[] = { "RGB", "R", "G", "B" };
    W::Segmented("curvech", &g_app.curveCh, ch, 4);
    ImGui::Spacing();
    static const ImU32 cols[] = { IM_COL32(235, 235, 235, 255), IM_COL32(240, 80, 80, 255), IM_COL32(80, 220, 100, 255), IM_COL32(90, 140, 255, 255) };
    float sz = ImGui::GetContentRegionAvail().x;
    int c = g_app.curveCh;
    const float* hist = g_pipe.hist[c == 0 ? 3 : c - 1];
    if (W::CurveEditor("curve", p.curve[c], cols[c], hist, sz)) Changed();
    W::Caption(T("Клик добавляет точку, правый клик удаляет её.", "Click to add a point, right-click to remove it."));
    if (W::Button(T("Сбросить этот канал", "Reset this channel"), ImVec2(-1, 0))) { p.curve[c] = Curve(); Changed(); }
}

static void PanelHSL() {
    Params& p = g_app.params;
    bool reset = false;
    bool open = W::Section(T("Цвет (HSL)", "Colour (HSL)"), false, &reset);
    if (reset) { for (int i = 0; i < 8; i++) p.hslH[i] = p.hslS[i] = p.hslL[i] = 0; Changed(); }
    if (!open) return;
    const char* tabs[] = { T("Оттенок", "Hue"), T("Насыщенность", "Saturation"), T("Яркость", "Luminance") };
    W::Segmented("hsltab", &g_app.hslTab, tabs, 3);
    ImGui::Spacing();
    for (int i = 0; i < 8; i++) {
        ImGui::PushID(i);
        float h = kHueCenters[i];
        if (g_app.hslTab == 0) {
            ImU32 g[3] = { W::HueColor(h - 35), W::HueColor(h), W::HueColor(h + 35) };
            Sl(HueName(i), &p.hslH[i], -100, 100, 0, nullptr, g, 3);
        } else if (g_app.hslTab == 1) {
            ImU32 g[2] = { W::HueColor(h, 0.05f, 0.6f), W::HueColor(h, 0.95f, 0.9f) };
            Sl(HueName(i), &p.hslS[i], -100, 100, 0, nullptr, g, 2);
        } else {
            ImU32 g[2] = { W::HueColor(h, 0.8f, 0.25f), W::HueColor(h, 0.4f, 1.0f) };
            Sl(HueName(i), &p.hslL[i], -100, 100, 0, nullptr, g, 2);
        }
        ImGui::PopID();
    }
}

static void PanelGrading() {
    Params& p = g_app.params;
    bool reset = false;
    bool open = W::Section(T("Цветокоррекция", "Colour grading"), false, &reset);
    if (reset) { for (auto& w : p.grade) w = Wheel(); p.gradBlend = 50; p.gradBalance = 0; Changed(); }
    if (!open) return;
    const char* tabs[] = { T("Тени", "Shadows"), T("Средние", "Midtones"), T("Света", "Highlights"), T("Общий", "Global") };
    W::Segmented("gradetab", &g_app.gradeTab, tabs, 4);
    Wheel& w = p.grade[g_app.gradeTab];
    float sz = std::min(ImGui::GetContentRegionAvail().x, 190 * g_app.dpi);
    ImGui::Spacing();
    ImGui::SetCursorPosX(ImGui::GetCursorPosX() + (ImGui::GetContentRegionAvail().x - sz) * 0.5f);
    if (W::ColorWheel("wheel", w, sz)) Changed();
    ImGui::PushID(g_app.gradeTab);
    ImU32 hg[7]; HueGrad(hg);
    Sl(T("Оттенок", "Hue"), &w.hue, 0, 360, 0, "%.0f°", hg, 7);
    Sl(T("Насыщенность", "Saturation"), &w.sat, 0, 100, 0);
    Sl(T("Яркость", "Luminance"), &w.lum, -100, 100, 0);
    ImGui::PopID();
    ImGui::Separator();
    Sl(T("Смешивание", "Blending"), &p.gradBlend, 0, 100, 50);
    Sl(T("Баланс", "Balance"), &p.gradBalance, -100, 100, 0);
}

static void PanelBW() {
    Params& p = g_app.params;
    bool reset = false;
    bool open = W::Section(T("Чёрно-белое", "Black & white"), false, &reset);
    if (reset) { p.bw = false; for (float& v : p.bwMix) v = 0; Changed(); }
    if (!open) return;
    Tg(T("Сделать чёрно-белым", "Convert to black & white"), &p.bw);
    if (!p.bw) return;
    W::Caption(T("Насколько светлым станет каждый цвет:", "How light each colour becomes:"));
    for (int i = 0; i < 8; i++) {
        ImGui::PushID(100 + i);
        ImU32 g[2] = { W::HueColor(kHueCenters[i], 0.6f, 0.2f), W::HueColor(kHueCenters[i], 0.5f, 1.0f) };
        Sl(HueName(i), &p.bwMix[i], -100, 100, 0, nullptr, g, 2);
        ImGui::PopID();
    }
}

static void PanelDetail() {
    Params& p = g_app.params;
    bool reset = false;
    bool open = W::Section(T("Детализация", "Detail"), false, &reset);
    if (reset) {
        Params d;
#define R(n, l, mi, ma, df) p.n = d.n;
        DETAIL_PARAMS(R)
#undef R
        Changed();
    }
    if (!open) return;
    W::Caption(T("Резкость", "Sharpening"));
    Sl(T("Величина", "Amount"), &p.sharpAmount, 0, 150, 0);
    Sl(T("Радиус", "Radius"), &p.sharpRadius, 0.5f, 3, 1, "%.1f");
    Sl(T("Детали##sh", "Detail##sh"), &p.sharpDetail, 0, 100, 25);
    Sl(T("Маскирование", "Masking"), &p.sharpMask, 0, 100, 0);
    ImGui::Separator();
    W::Caption(T("Шумоподавление", "Noise reduction"));
    Sl(T("Яркостный шум", "Luminance"), &p.nrLuma, 0, 100, 0);
    Sl(T("Детали##nr", "Detail##nr"), &p.nrDetail, 0, 100, 50);
    Sl(T("Цветовой шум", "Colour"), &p.nrColor, 0, 100, 0);
    Sl(T("Гладкость цвета", "Colour smoothness"), &p.nrSmooth, 0, 100, 50);
    W::Caption(T("Резкость и шум лучше оценивать в масштабе 100% (клавиша Z).", "Judge sharpening and noise at 100% zoom (key Z)."));
}

static void PanelOptics() {
    Params& p = g_app.params;
    bool reset = false;
    bool open = W::Section(T("Оптика", "Lens corrections"), false, &reset);
    if (reset) { p.lensDist = p.lensCA = p.lensVig = 0; Changed(); }
    if (!open) return;
    Sl(T("Дисторсия", "Distortion"), &p.lensDist, -100, 100, 0);
    Sl(T("Хроматические аберрации", "Chromatic aberration"), &p.lensCA, -100, 100, 0);
    Sl(T("Виньетирование объектива", "Lens vignetting"), &p.lensVig, -100, 100, 0);
}

static void PanelEffects() {
    Params& p = g_app.params;
    bool reset = false;
    bool open = W::Section(T("Виньетка и зерно", "Vignette & grain"), false, &reset);
    if (reset) {
        Params d;
        p.vigAmount = d.vigAmount; p.vigMid = d.vigMid; p.vigRound = d.vigRound; p.vigFeather = d.vigFeather; p.vigHigh = d.vigHigh;
        p.grainAmount = d.grainAmount; p.grainSize = d.grainSize; p.grainRough = d.grainRough;
        Changed();
    }
    if (!open) return;
    W::Caption(T("Виньетка", "Vignette"));
    Sl(T("Сила##v", "Amount##v"), &p.vigAmount, -100, 100, 0);
    Sl(T("Размер", "Midpoint"), &p.vigMid, 0, 100, 50);
    Sl(T("Округлость", "Roundness"), &p.vigRound, -100, 100, 0);
    Sl(T("Растушёвка", "Feather"), &p.vigFeather, 0, 100, 50);
    Sl(T("Защита светов", "Highlights"), &p.vigHigh, 0, 100, 0);
    ImGui::Separator();
    W::Caption(T("Зерно", "Grain"));
    Sl(T("Сила##g", "Amount##g"), &p.grainAmount, 0, 100, 0);
    Sl(T("Размер##g", "Size##g"), &p.grainSize, 0, 100, 25);
    Sl(T("Неровность", "Roughness"), &p.grainRough, 0, 100, 50);
}

// ---------------------------------------------------------------- crop panel
static const float kAspectVals[] = { 0, 0, 1, 0.8f, 1.25f, 1.5f, 0.6667f, 1.7778f, 0.5625f, 1.3333f, 0.75f, 2.39f };

static float AspectTarget(const Params& p, const Geo& g) {
    if (p.cropAspect == 1) return 0;
    if (p.cropAspect == 0) return (float)g.canvasW / g.canvasH;
    return kAspectVals[p.cropAspect];
}

static void FitCropToAspect(Params& p) {
    Geo g = g_pipe.geometry(p, true);
    float ar = AspectTarget(p, g);
    if (ar <= 0) return;
    float cx = (p.cropX0 + p.cropX1) * 0.5f, cy = (p.cropY0 + p.cropY1) * 0.5f;
    float wpx = std::fabs(p.cropX1 - p.cropX0) * g.canvasW, hpx = std::fabs(p.cropY1 - p.cropY0) * g.canvasH;
    float area = wpx * hpx;
    float nw = std::sqrt(area * ar), nh = nw / ar;
    float s = std::min(std::min(1.0f, g.canvasW / nw), std::min(1.0f, g.canvasH / nh));
    nw *= s; nh *= s;
    float hw = nw / g.canvasW * 0.5f, hh = nh / g.canvasH * 0.5f;
    cx = std::clamp(cx, hw, 1 - hw); cy = std::clamp(cy, hh, 1 - hh);
    p.cropX0 = cx - hw; p.cropX1 = cx + hw; p.cropY0 = cy - hh; p.cropY1 = cy + hh;
}

static void PanelCrop() {
    Params& p = g_app.params;
    ImGui::Spacing();
    const char* aspects[] = { T("Как у снимка", "As shot"), T("Свободно", "Free"), "1 : 1", "4 : 5", "5 : 4", "3 : 2", "2 : 3", "16 : 9", "9 : 16", "4 : 3", "3 : 4", "2.39 : 1" };
    if (W::Combo(T("Пропорции", "Aspect"), &p.cropAspect, aspects, IM_ARRAYSIZE(aspects))) { FitCropToAspect(p); Changed(); }
    if (W::Button(T("Сменить ориентацию рамки", "Swap frame orientation"), ImVec2(-1, 0))) {
        if (p.cropAspect >= 2) { static const int swapIdx[] = { 0, 1, 2, 4, 3, 6, 5, 8, 7, 10, 9, 11 }; p.cropAspect = swapIdx[p.cropAspect]; }
        float cx = (p.cropX0 + p.cropX1) * 0.5f, cy = (p.cropY0 + p.cropY1) * 0.5f;
        Geo g = g_pipe.geometry(p, true);
        float wpx = (p.cropX1 - p.cropX0) * g.canvasW, hpx = (p.cropY1 - p.cropY0) * g.canvasH;
        float hw = hpx / g.canvasW * 0.5f, hh = wpx / g.canvasH * 0.5f;
        p.cropX0 = cx - hw; p.cropX1 = cx + hw; p.cropY0 = cy - hh; p.cropY1 = cy + hh;
        if (p.cropX0 < 0 || p.cropY0 < 0 || p.cropX1 > 1 || p.cropY1 > 1) {
            p.cropX0 = std::max(0.f, p.cropX0); p.cropY0 = std::max(0.f, p.cropY0); p.cropX1 = std::min(1.f, p.cropX1); p.cropY1 = std::min(1.f, p.cropY1);
            FitCropToAspect(p);
        }
        Changed();
    }
    ImGui::Spacing();
    Sl(T("Выравнивание горизонта", "Straighten"), &p.straighten, -45, 45, 0, "%+.1f°");
    float bw = (ImGui::GetContentRegionAvail().x - ImGui::GetStyle().ItemSpacing.x * 3) / 4;
    if (W::Button("⟲ 90°", ImVec2(bw, 0))) { p.rot90 = (p.rot90 + 3) & 3; p.cropX0 = p.cropY0 = 0; p.cropX1 = p.cropY1 = 1; Changed(); }
    W::Hint(T("Повернуть против часовой стрелки", "Rotate counter-clockwise"));
    ImGui::SameLine();
    if (W::Button("⟳ 90°", ImVec2(bw, 0))) { p.rot90 = (p.rot90 + 1) & 3; p.cropX0 = p.cropY0 = 0; p.cropX1 = p.cropY1 = 1; Changed(); }
    W::Hint(T("Повернуть по часовой стрелке", "Rotate clockwise"));
    ImGui::SameLine();
    if (W::Button("⇆", ImVec2(bw, 0), W::BTN_DEFAULT, p.flipH)) { p.flipH = !p.flipH; Changed(); }
    W::Hint(T("Отразить по горизонтали", "Flip horizontally"));
    ImGui::SameLine();
    if (W::Button("⇅", ImVec2(bw, 0), W::BTN_DEFAULT, p.flipV)) { p.flipV = !p.flipV; Changed(); }
    W::Hint(T("Отразить по вертикали", "Flip vertically"));
    ImGui::Spacing();
    W::Caption(T("Перспектива", "Perspective"));
    Sl(T("По вертикали", "Vertical"), &p.perspV, -100, 100, 0);
    Sl(T("По горизонтали", "Horizontal"), &p.perspH, -100, 100, 0);
    Sl(T("Масштаб", "Scale"), &p.geoScale, 50, 150, 100);
    ImGui::Spacing();
    if (W::Button(T("Сбросить кадрирование", "Reset crop"), ImVec2(HalfWidth(), 0))) {
        p.cropX0 = p.cropY0 = 0; p.cropX1 = p.cropY1 = 1; p.straighten = 0; p.perspV = p.perspH = 0; p.geoScale = 100; p.cropAspect = 0;
        Changed();
    }
    ImGui::SameLine();
    if (W::Button(T("Готово", "Done"), ImVec2(-1, 0), W::BTN_PRIMARY)) g_app.tool = TOOL_EDIT;
    W::Hint("Enter");
    ImGui::Spacing();
    W::Caption(T("Тяните углы и стороны рамки. Чтобы передвинуть рамку, тяните внутри неё.", "Drag the corners and edges of the frame, drag inside to move it."));
}

// ---------------------------------------------------------------- mask panel
static void AddMask(int shape, int range) {
    Mask m = MakeMask(shape, range);
    if (range == MR_LUMA) { m.lumLo = 0.6f; m.lumHi = 1.0f; }
    if (shape == MS_LINEAR) { m.lx0 = 0.5f; m.ly0 = g_app.params.cropY0 + 0.05f; m.lx1 = 0.5f; m.ly1 = (g_app.params.cropY0 + g_app.params.cropY1) * 0.5f; }
    if (shape == MS_RADIAL) { m.cx = (g_app.params.cropX0 + g_app.params.cropX1) * 0.5f; m.cy = (g_app.params.cropY0 + g_app.params.cropY1) * 0.5f; }
    g_app.params.masks.push_back(m);
    g_app.selMask = (int)g_app.params.masks.size() - 1;
    if (range == MR_COLOR) g_app.pick = PICK_MASK_COLOR;
    Changed();
}

static void PanelMasks() {
    Params& p = g_app.params;
    ImGui::Spacing();
    W::Caption(T("Новая маска", "New mask"));
    float bw = ThirdWidth();
    ImVec2 tile(bw, ImGui::GetFrameHeight() * 1.25f);
    if (W::Button(T("◧  Градиент", "◧  Linear"), tile)) AddMask(MS_LINEAR, MR_NONE);
    ImGui::SameLine();
    if (W::Button(T("◎  Радиальная", "◎  Radial"), tile)) AddMask(MS_RADIAL, MR_NONE);
    ImGui::SameLine();
    if (W::Button(T("✎  Кисть", "✎  Brush"), tile)) AddMask(MS_BRUSH, MR_NONE);
    if (W::Button(T("◐  Яркость", "◐  Luminance"), tile)) AddMask(MS_FULL, MR_LUMA);
    ImGui::SameLine();
    if (W::Button(T("◕  Цвет", "◕  Colour"), tile)) AddMask(MS_FULL, MR_COLOR);
    ImGui::SameLine();
    if (W::Button(T("▣  Всё фото", "▣  Whole"), tile)) AddMask(MS_FULL, MR_NONE);

    if (p.masks.empty()) {
        ImGui::Spacing();
        W::Caption(T("Маска позволяет править только часть кадра: небо градиентом, лицо радиальной маской, мелкие детали кистью, а светлые участки или нужный цвет диапазоном.",
                     "A mask limits edits to part of the frame: the sky with a gradient, a face with a radial mask, details with the brush, bright areas or a colour with a range."));
        return;
    }
    ImGui::Spacing();
    Tg(T("Показывать маску красным (O)", "Show mask overlay (O)"), &g_app.showOverlay);
    ImGui::Spacing();
    int del = -1, dup = -1;
    ImDrawList* dl = ImGui::GetWindowDrawList();
    for (int i = 0; i < (int)p.masks.size(); i++) {
        Mask& m = p.masks[i];
        ImGui::PushID((int)m.id);
        bool sel = g_app.selMask == i;
        ImVec2 r0 = ImGui::GetCursorScreenPos();
        float rh = ImGui::GetFrameHeight() + 4;
        float w = ImGui::GetContentRegionAvail().x;
        if (sel) {
            dl->AddRectFilled(r0, ImVec2(r0.x + w, r0.y + rh), IM_COL32(255, 255, 255, 18), 6);
            dl->AddRectFilled(r0, ImVec2(r0.x + 3, r0.y + rh), W::kAccent, 2);
        }
        ImGui::SetCursorScreenPos(ImVec2(r0.x + 6, r0.y + 2));
        if (W::IconButton(m.enabled ? "◉" : "○", T("Включить или выключить маску", "Enable or disable the mask"), W::BTN_GHOST)) { m.enabled = !m.enabled; Changed(); }
        ImGui::SameLine();
        float nameW = w - ImGui::GetFrameHeight() * 3 - 30;
        ImGui::PushStyleColor(ImGuiCol_Text, m.enabled ? ImVec4(0.9f, 0.91f, 0.93f, 1) : ImVec4(0.5f, 0.52f, 0.56f, 1));
        ImGui::PushStyleVar(ImGuiStyleVar_SelectableTextAlign, ImVec2(0, 0.5f));
        if (ImGui::Selectable(m.name.c_str(), false, ImGuiSelectableFlags_AllowOverlap, ImVec2(nameW, ImGui::GetFrameHeight()))) g_app.selMask = i;
        ImGui::PopStyleVar();
        ImGui::PopStyleColor();
        ImGui::SameLine();
        if (W::IconButton("⧉", T("Дублировать", "Duplicate"))) dup = i;
        ImGui::SameLine();
        if (W::IconButton("✕", T("Удалить", "Delete"), W::BTN_DANGER)) del = i;
        ImGui::SetCursorScreenPos(ImVec2(r0.x, r0.y + rh + 2));
        ImGui::Dummy(ImVec2(0, 0));
        ImGui::PopID();
    }
    if (dup >= 0) { Mask c = p.masks[dup]; c.id = NewId(); c.brushRev = NewRev(); c.name += T(" (копия)", " (copy)"); p.masks.insert(p.masks.begin() + dup + 1, c); g_app.selMask = dup + 1; Changed(); }
    if (del >= 0) { p.masks.erase(p.masks.begin() + del); g_app.selMask = std::min(g_app.selMask, (int)p.masks.size() - 1); Changed(); }
    if (g_app.selMask < 0 || g_app.selMask >= (int)p.masks.size()) return;

    Mask& m = p.masks[g_app.selMask];
    ImGui::PushID((int)m.id + 100000);
    bool reset = false;
    if (W::Section(T("Настройки маски", "Mask settings"), true)) {
        static char nameBuf[128];
        strncpy(nameBuf, m.name.c_str(), sizeof(nameBuf) - 1); nameBuf[sizeof(nameBuf) - 1] = 0;
        ImGui::SetNextItemWidth(-1);
        if (ImGui::InputText("##name", nameBuf, sizeof(nameBuf))) { m.name = nameBuf; Changed(); }
        Sl(T("Сила воздействия", "Amount"), &m.amount, 0, 100, 100);
        Tg(T("Инвертировать", "Invert"), &m.invert);
        if (m.shape == MS_RADIAL) {
            Sl(T("Растушёвка", "Feather"), &m.feather, 0, 100, 50);
            Sl(T("Поворот", "Rotation"), &m.angle, -180, 180, 0, "%.0f°");
        }
        if (m.shape == MS_BRUSH) {
            W::Caption(T("Размер кисти меняется клавишами [ и ], с Alt кисть стирает", "Brush: [ ] size, Alt erases"));
            float sz = g_app.brushSize * 100;
            if (W::Slider(T("Размер кисти", "Brush size"), &sz, 0.5f, 40, 6, "%.1f")) g_app.brushSize = sz / 100;
            float f = g_app.brushFeather * 100;
            if (W::Slider(T("Мягкость", "Feather"), &f, 0, 100, 60)) g_app.brushFeather = f / 100;
            float fl = g_app.brushFlow * 100;
            if (W::Slider(T("Нажим", "Flow"), &fl, 5, 100, 60)) g_app.brushFlow = fl / 100;
            float tw = ThirdWidth();
            if (W::Button(T("Рисовать", "Paint"), ImVec2(tw, 0), W::BTN_DEFAULT, !g_app.brushErase)) g_app.brushErase = false;
            ImGui::SameLine();
            if (W::Button(T("Стирать", "Erase"), ImVec2(tw, 0), W::BTN_DEFAULT, g_app.brushErase)) g_app.brushErase = true;
            ImGui::SameLine();
            if (W::Button(T("Очистить", "Clear"), ImVec2(-1, 0), W::BTN_DANGER)) { m.dabs.clear(); m.brushRev = NewRev(); Changed(); }
        }
        ImGui::Spacing();
        W::Caption(T("Уточнить по диапазону", "Refine by range"));
        const char* rn[] = { T("Нет", "None"), T("Яркость", "Luminance"), T("Цвет", "Colour") };
        if (W::Segmented("range", &m.range, rn, 3)) Changed();
        if (m.range == MR_LUMA) {
            float lo = m.lumLo * 100, hi = m.lumHi * 100, fe = m.lumFeather * 100;
            if (Sl(T("От", "From"), &lo, 0, 100, 50, nullptr, kGradLum, 2)) m.lumLo = lo / 100;
            if (Sl(T("До", "To"), &hi, 0, 100, 100, nullptr, kGradLum, 2)) m.lumHi = hi / 100;
            if (Sl(T("Плавность", "Smoothness"), &fe, 0, 50, 15)) m.lumFeather = fe / 100;
            if (W::Button(T("Взять яркость с фото", "Pick from photo"), ImVec2(-1, 0), W::BTN_DEFAULT, g_app.pick == PICK_MASK_LUMA))
                g_app.pick = g_app.pick == PICK_MASK_LUMA ? PICK_NONE : PICK_MASK_LUMA;
        } else if (m.range == MR_COLOR) {
            ImU32 hg[7]; HueGrad(hg);
            Sl(T("Оттенок", "Hue"), &m.hueC, 0, 360, 30, "%.0f°", hg, 7);
            Sl(T("Ширина", "Width"), &m.hueWidth, 2, 180, 40, "%.0f°");
            Sl(T("Плавность##h", "Smoothness##h"), &m.hueFeather, 0, 90, 20, "%.0f°");
            float sm = m.satMin * 100;
            if (Sl(T("Мин. насыщенность", "Min. saturation"), &sm, 0, 100, 10)) m.satMin = sm / 100;
            if (W::Button(T("Взять цвет с фото", "Pick from photo"), ImVec2(-1, 0), W::BTN_DEFAULT, g_app.pick == PICK_MASK_COLOR))
                g_app.pick = g_app.pick == PICK_MASK_COLOR ? PICK_NONE : PICK_MASK_COLOR;
        }
    }
    bool open = W::Section(T("Коррекция внутри маски", "Adjustments in mask"), true, &reset);
    LocalAdj& a = m.adj;
    if (reset) { a = LocalAdj(); Changed(); }
    if (open) {
        Sl(T("Температура##l", "Temperature##l"), &a.temp, -100, 100, 0, nullptr, kGradTemp, 2);
        Sl(T("Оттенок##l", "Tint##l"), &a.tint, -100, 100, 0, nullptr, kGradTint, 2);
        Sl(T("Экспозиция##l", "Exposure##l"), &a.exposure, -4, 4, 0, "%+.2f");
        Sl(T("Контраст##l", "Contrast##l"), &a.contrast, -100, 100, 0);
        Sl(T("Света##l", "Highlights##l"), &a.highlights, -100, 100, 0);
        Sl(T("Тени##l", "Shadows##l"), &a.shadows, -100, 100, 0);
        Sl(T("Белые##l", "Whites##l"), &a.whites, -100, 100, 0);
        Sl(T("Чёрные##l", "Blacks##l"), &a.blacks, -100, 100, 0);
        Sl(T("Чёткость##l", "Clarity##l"), &a.clarity, -100, 100, 0);
        Sl(T("Дымка##l", "Dehaze##l"), &a.dehaze, -100, 100, 0);
        Sl(T("Резкость##l", "Sharpness##l"), &a.sharpness, -100, 100, 0);
        Sl(T("Сдвиг оттенка##l", "Hue shift##l"), &a.hue, -180, 180, 0, "%+.0f°");
        Sl(T("Насыщенность##l", "Saturation##l"), &a.saturation, -100, 100, 0, nullptr, kGradSat, 2);
        ImGui::AlignTextToFramePadding();
        ImGui::TextDisabled("%s", T("Тонирование", "Colour tint"));
        ImGui::SameLine(ImGui::GetContentRegionAvail().x * 0.42f);
        if (ImGui::ColorEdit3("##lc", a.color, ImGuiColorEditFlags_NoInputs | ImGuiColorEditFlags_NoLabel)) Changed();
        Sl(T("Сила тонирования", "Tint amount"), &a.colorAmt, 0, 100, 0);
    }
    ImGui::PopID();
}

// =====================================================================  experimental panel
static bool MiniSwitch(const char* id, bool* v) {
    float fs = ImGui::GetFontSize();
    ImVec2 sz(fs * 1.9f, fs * 1.05f);
    ImVec2 p = ImGui::GetCursorScreenPos();
    float off = (ImGui::GetFrameHeight() - sz.y) * 0.5f;
    ImGui::SetCursorScreenPos(ImVec2(p.x, p.y + off));
    bool r = ImGui::InvisibleButton(id, sz);
    if (r) *v = !*v;
    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImVec2 a = ImGui::GetItemRectMin(), b = ImGui::GetItemRectMax();
    dl->AddRectFilled(a, b, *v ? W::kAccent : IM_COL32(255, 255, 255, ImGui::IsItemHovered() ? 45 : 30), sz.y * 0.5f);
    float kr = sz.y * 0.5f - 2.5f;
    dl->AddCircleFilled(ImVec2(*v ? b.x - sz.y * 0.5f : a.x + sz.y * 0.5f, (a.y + b.y) * 0.5f), kr, *v ? IM_COL32(16, 20, 30, 255) : IM_COL32(215, 217, 222, 255));
    return r;
}

static void EffectCard(Effect& e, int idx, int count, int& moveUp, int& moveDown, int& del, int& dup) {
    const FxDesc& d = GetFxDesc(e.type);
    ImGui::PushID((int)e.id);
    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImVec2 p0 = ImGui::GetCursorScreenPos();
    float w = ImGui::GetContentRegionAvail().x;
    float pad = 10 * g_app.dpi;
    dl->ChannelsSplit(2);
    dl->ChannelsSetCurrent(1);
    ImGui::SetCursorScreenPos(ImVec2(p0.x + pad, p0.y + pad * 0.6f));
    ImGui::BeginGroup();
    ImGui::PushItemWidth(w - pad * 2);
    float innerW = w - pad * 2;
    // header: switch, name, actions
    if (MiniSwitch("##en", &e.enabled)) Changed();
    ImGui::SameLine();
    float fh = ImGui::GetFrameHeight();
    float actionsW = fh * 4 + 6;
    ImGui::SetCursorScreenPos(ImVec2(ImGui::GetCursorScreenPos().x, p0.y + pad * 0.6f));
    ImGui::PushFont(g_app.fontBold);
    ImGui::PushStyleColor(ImGuiCol_Text, e.enabled ? ImVec4(0.93f, 0.94f, 0.96f, 1) : ImVec4(0.55f, 0.57f, 0.61f, 1));
    float nameW = innerW - (ImGui::GetCursorScreenPos().x - (p0.x + pad)) - actionsW;
    ImGui::PushStyleVar(ImGuiStyleVar_SelectableTextAlign, ImVec2(0, 0.5f));
    if (ImGui::Selectable(d.name, false, ImGuiSelectableFlags_AllowOverlap, ImVec2(nameW, fh))) e.expanded = !e.expanded;
    ImGui::PopStyleVar();
    ImGui::PopStyleColor();
    ImGui::PopFont();
    W::Hint(d.desc);
    ImGui::SameLine(0, 2);
    ImGui::BeginDisabled(idx == 0);
    if (W::IconButton("↑", T("Выше", "Move up"))) moveUp = idx;
    ImGui::EndDisabled();
    ImGui::SameLine(0, 2);
    ImGui::BeginDisabled(idx + 1 >= count);
    if (W::IconButton("↓", T("Ниже", "Move down"))) moveDown = idx;
    ImGui::EndDisabled();
    ImGui::SameLine(0, 2);
    if (W::IconButton("⧉", T("Дублировать", "Duplicate"))) dup = idx;
    ImGui::SameLine(0, 2);
    if (W::IconButton("✕", T("Удалить", "Delete"), W::BTN_DANGER)) del = idx;

    if (e.expanded) {
        ImGui::Dummy(ImVec2(0, 2));
        ImGui::BeginChild("##body", ImVec2(innerW, 0), ImGuiChildFlags_AutoResizeY, ImGuiWindowFlags_NoBackground | ImGuiWindowFlags_NoScrollbar);
        for (size_t i = 0; i < d.params.size(); i++) {
            const FxParam& fp = d.params[i];
            ImGui::PushID((int)i);
            if (fp.kind == PK_COMBO) {
                int v = (int)e.p[i];
                if (W::Combo(fp.label, &v, fp.options)) { e.p[i] = (float)v; Changed(); }
            } else if (fp.kind == PK_TOGGLE) {
                bool b = e.p[i] > 0.5f;
                if (W::Toggle(fp.label, &b)) { e.p[i] = b ? 1.f : 0.f; Changed(); }
            } else {
                const char* fmt = (fp.mx - fp.mn) <= 10 ? "%.2f" : "%.0f";
                Sl(fp.label, &e.p[i], fp.mn, fp.mx, fp.def, fmt);
            }
            ImGui::PopID();
        }
        if (e.type == FX_ASCII && (int)e.p[1] == 5) {
            char buf[256];
            strncpy(buf, e.text.c_str(), sizeof(buf) - 1); buf[sizeof(buf) - 1] = 0;
            ImGui::SetNextItemWidth(-1);
            if (ImGui::InputTextWithHint("##txt", T("Символы от тёмного к светлому", "Characters from dark to light"), buf, sizeof(buf))) { e.text = buf; Changed(); }
        }
        if (!d.colors.empty()) {
            for (size_t i = 0; i < d.colors.size(); i++) {
                ImGui::PushID(50 + (int)i);
                if (i) ImGui::SameLine(0, 14 * g_app.dpi);
                if (ImGui::ColorEdit3("##col", e.col[i], ImGuiColorEditFlags_NoInputs | ImGuiColorEditFlags_NoLabel)) Changed();
                ImGui::SameLine(0, 5);
                ImGui::AlignTextToFramePadding();
                ImGui::TextDisabled("%s", d.colors[i].label);
                ImGui::PopID();
            }
        }
        ImGui::Spacing();
        ImGui::Separator();
        Sl(T("Непрозрачность", "Opacity"), &e.opacity, 0, 100, 100);
        if (W::Combo(T("Наложение", "Blend mode"), &e.blend, BlendNames())) Changed();
        ImGui::Spacing();
        W::Caption(T("Где применять", "Apply to"));
        const char* tn[] = { T("Везде", "Everywhere"), T("Яркость", "Luminance"), T("Цвет", "Colour"), T("Маска", "Mask") };
        if (W::Segmented("tgt", &e.target, tn, 4)) Changed();
        if (e.target == TG_LUMA) {
            float lo = e.tLo * 100, hi = e.tHi * 100, fe = e.tFeather * 100;
            if (Sl(T("От", "From"), &lo, 0, 100, 0, nullptr, kGradLum, 2)) e.tLo = lo / 100;
            if (Sl(T("До", "To"), &hi, 0, 100, 35, nullptr, kGradLum, 2)) e.tHi = hi / 100;
            if (Sl(T("Плавность", "Smoothness"), &fe, 0, 50, 10)) e.tFeather = fe / 100;
            if (W::Button(T("Взять яркость с фото", "Pick from photo"), ImVec2(-1, 0), W::BTN_DEFAULT, g_app.pick == PICK_FX_LUMA && g_app.selEffect == idx)) { g_app.pick = PICK_FX_LUMA; g_app.selEffect = idx; }
        } else if (e.target == TG_COLOR) {
            ImU32 hg[7]; HueGrad(hg);
            Sl(T("Оттенок", "Hue"), &e.tHue, 0, 360, 200, "%.0f°", hg, 7);
            Sl(T("Ширина", "Width"), &e.tHueWidth, 2, 180, 60, "%.0f°");
            Sl(T("Плавность", "Smoothness"), &e.tHueFeather, 0, 90, 30, "%.0f°");
            float sm = e.tSatMin * 100;
            if (Sl(T("Мин. насыщенность", "Min. saturation"), &sm, 0, 100, 10)) e.tSatMin = sm / 100;
            if (W::Button(T("Взять цвет с фото", "Pick from photo"), ImVec2(-1, 0), W::BTN_DEFAULT, g_app.pick == PICK_FX_COLOR && g_app.selEffect == idx)) { g_app.pick = PICK_FX_COLOR; g_app.selEffect = idx; }
        } else if (e.target == TG_MASK) {
            auto& ms = g_app.params.masks;
            if (ms.empty()) W::Caption(T("Масок пока нет. Создайте их во вкладке «Классика» → «Маски».", "No masks yet. Create them in Classic → Masks."));
            else {
                std::string cur = e.tMask < (int)ms.size() ? ms[e.tMask].name : "-";
                ImGui::SetNextItemWidth(-1);
                if (ImGui::BeginCombo("##mask", cur.c_str())) {
                    for (int k = 0; k < (int)ms.size(); k++) {
                        ImGui::PushID(k);
                        if (ImGui::Selectable(ms[k].name.c_str(), e.tMask == k)) { e.tMask = k; Changed(); }
                        ImGui::PopID();
                    }
                    ImGui::EndCombo();
                }
            }
        }
        if (e.target != TG_ALL) Tg(T("Инвертировать область", "Invert area"), &e.targetInvert);
        ImGui::Spacing();
        float bw = ThirdWidth();
        if (W::Button(T("Случайно", "Randomise"), ImVec2(bw, 0))) { RandomizeEffect(e); Changed(); }
        W::Hint(T("Случайные настройки и цвета", "Random settings and colours"));
        ImGui::SameLine();
        if (W::Button(T("Перемешать", "Reseed"), ImVec2(bw, 0))) { e.seed = (float)(rand() % 1000); Changed(); }
        W::Hint(T("Другой вариант случайного узора с теми же настройками", "Another random pattern with the same settings"));
        ImGui::SameLine();
        if (W::Button(T("Сбросить", "Reset"), ImVec2(-1, 0))) { Effect n = MakeEffect(e.type); n.id = e.id; e = n; Changed(); }
        ImGui::EndChild();
    }
    ImGui::PopItemWidth();
    ImGui::EndGroup();
    ImVec2 p1 = ImVec2(p0.x + w, ImGui::GetItemRectMax().y + pad * 0.6f);
    dl->ChannelsSetCurrent(0);
    dl->AddRectFilled(p0, p1, e.enabled ? IM_COL32(255, 255, 255, 12) : IM_COL32(255, 255, 255, 5), 10);
    dl->AddRect(p0, p1, IM_COL32(255, 255, 255, 14), 10);
    dl->ChannelsMerge();
    ImGui::SetCursorScreenPos(ImVec2(p0.x, p1.y + 8 * g_app.dpi));
    ImGui::Dummy(ImVec2(0, 0));
    ImGui::PopID();
}

static void AddEffectPopup() {
    Params& p = g_app.params;
    ImGui::SetNextWindowSize(ImVec2(560 * g_app.dpi, 0));
    if (!ImGui::BeginPopup("addfx")) return;
    static char filter[64] = "";
    if (ImGui::IsWindowAppearing()) { filter[0] = 0; ImGui::SetKeyboardFocusHere(); }
    ImGui::SetNextItemWidth(-1);
    ImGui::InputTextWithHint("##flt", T("Поиск эффекта", "Search effects"), filter, sizeof(filter));
    ImGui::Spacing();
    float cw = HalfWidth();
    int shown = 0;
    std::wstring f = Utf8ToWide(filter);
    CharLowerBuffW(&f[0], (DWORD)f.size());
    for (int t = 0; t < FX_COUNT; t++) {
        const FxDesc& d = GetFxDesc(t);
        if (!f.empty()) {
            std::wstring n = Utf8ToWide(d.name) + L" " + Utf8ToWide(d.desc);
            CharLowerBuffW(&n[0], (DWORD)n.size());
            if (n.find(f) == std::wstring::npos) continue;
        }
        if (shown % 2) ImGui::SameLine();
        ImGui::PushID(t);
        if (W::Button(d.name, ImVec2(cw, ImGui::GetFrameHeight() * 1.35f))) {
            for (auto& e : p.effects) e.expanded = false;
            p.effects.push_back(MakeEffect(t));
            Changed();
            ImGui::CloseCurrentPopup();
        }
        W::Hint(d.desc);
        ImGui::PopID();
        shown++;
    }
    if (!shown) W::Caption(T("Ничего не найдено", "Nothing found"));
    ImGui::EndPopup();
}

// Header of the Experimental tab: stays in place while the effect list scrolls.
static void ExperimentalHeader() {
    Params& p = g_app.params;
    ImGui::PushFont(g_app.fontBold);
    ImGui::TextUnformatted(T("Эксперименты", "Experiments"));
    ImGui::PopFont();
    W::Caption(T("Эффекты накладываются по порядку, сверху вниз, поверх основной обработки.", "Effects stack from top to bottom on top of the classic edit."));
    ImGui::Spacing();
    if (W::Button(T("+  Добавить эффект", "+  Add effect"), ImVec2(-1, ImGui::GetFrameHeight() * 1.3f), W::BTN_PRIMARY)) ImGui::OpenPopup("addfx");
    AddEffectPopup();
    float bw = ThirdWidth();
    if (W::Button(T("Случайный микс", "Random mix"), ImVec2(bw, 0))) {
        int n = 2 + rand() % 2;
        for (auto& e : p.effects) e.expanded = false;
        for (int k = 0; k < n; k++) {
            Effect e = MakeEffect(rand() % FX_COUNT);
            RandomizeEffect(e);
            e.opacity = 60.0f + rand() % 41;
            e.expanded = false;
            p.effects.push_back(e);
        }
        Changed();
    }
    W::Hint(T("Добавить 2-3 случайных эффекта", "Add 2-3 random effects"));
    ImGui::SameLine();
    ImGui::BeginDisabled(p.effects.empty());
    bool anyOpen = false;
    for (auto& e : p.effects) anyOpen |= e.expanded;
    if (W::Button(anyOpen ? T("Свернуть", "Collapse") : T("Развернуть", "Expand"), ImVec2(bw, 0))) for (auto& e : p.effects) e.expanded = !anyOpen;
    ImGui::SameLine();
    if (W::Button(T("Очистить", "Clear all"), ImVec2(-1, 0), W::BTN_DANGER)) { p.effects.clear(); Changed(); }
    ImGui::EndDisabled();
    ImGui::Spacing();
    ImGui::Separator();
}

static void ExperimentalList() {
    Params& p = g_app.params;
    if (p.effects.empty()) {
        ImGui::Spacing();
        W::Caption(T("Пока пусто. Идеи для начала:\n• ASCII только в тенях: «Где применять» → «Яркость», от 0 до 35\n• неоновые контуры только по синему цвету\n• свечение и ореол + плёночное зерно\n• глитч по маске неба",
                     "Nothing here yet. Ideas to start with:\n• ASCII only in the shadows: Apply to → Luminance, 0 to 35\n• neon edges on blue tones only\n• glow & halation + film grain\n• glitch through a sky mask"));
        return;
    }
    ImGui::Spacing();
    int up = -1, down = -1, del = -1, dup = -1;
    int n = (int)p.effects.size();
    for (int i = 0; i < n; i++) EffectCard(p.effects[i], i, n, up, down, del, dup);
    if (up > 0) { std::swap(p.effects[up], p.effects[up - 1]); Changed(); }
    if (down >= 0 && down + 1 < (int)p.effects.size()) { std::swap(p.effects[down], p.effects[down + 1]); Changed(); }
    if (dup >= 0) { Effect c = p.effects[dup]; c.id = NewId(); p.effects.insert(p.effects.begin() + dup + 1, c); Changed(); }
    if (del >= 0) { p.effects.erase(p.effects.begin() + del); Changed(); }
}

// =====================================================================  canvas
struct CanvasXf {
    ImVec2 imgMin, imgSize;
    Geo geo;
    bool cropMode;
    ImVec2 uvToScr(float u, float v) const { return ImVec2(imgMin.x + u * imgSize.x, imgMin.y + v * imgSize.y); }
    ImVec2 scrToUv(ImVec2 s) const { return ImVec2((s.x - imgMin.x) / imgSize.x, (s.y - imgMin.y) / imgSize.y); }
    ImVec2 cnvToScr(float x, float y) const { return uvToScr((x - geo.cx0) / geo.cw, (y - geo.cy0) / geo.ch); }
    ImVec2 scrToCnv(ImVec2 s) const { ImVec2 u = scrToUv(s); return ImVec2(geo.cx0 + u.x * geo.cw, geo.cy0 + u.y * geo.ch); }
};

static float Dist2(ImVec2 a, ImVec2 b) { return (a.x - b.x) * (a.x - b.x) + (a.y - b.y) * (a.y - b.y); }

static void HandleDot(ImDrawList* dl, ImVec2 p, bool hot, float r = 6) {
    r *= g_app.dpi;
    dl->AddCircleFilled(p, r + 2, IM_COL32(0, 0, 0, 140));
    dl->AddCircleFilled(p, r, hot ? W::kAccent : IM_COL32(245, 245, 245, 255));
}

static int s_drag = -1;
static ImVec2 s_dragStart;
static Mask s_maskStart;
static float s_cropStart[4];

static void CropInteract(const CanvasXf& X, ImDrawList* dl, bool hovered) {
    Params& p = g_app.params;
    ImGuiIO& io = ImGui::GetIO();
    ImVec2 a = X.cnvToScr(p.cropX0, p.cropY0), b = X.cnvToScr(p.cropX1, p.cropY1);
    ImVec2 iMin = X.imgMin, iMax(X.imgMin.x + X.imgSize.x, X.imgMin.y + X.imgSize.y);
    ImU32 shade = IM_COL32(0, 0, 0, 150);
    dl->AddRectFilled(iMin, ImVec2(iMax.x, a.y), shade);
    dl->AddRectFilled(ImVec2(iMin.x, b.y), iMax, shade);
    dl->AddRectFilled(ImVec2(iMin.x, a.y), ImVec2(a.x, b.y), shade);
    dl->AddRectFilled(ImVec2(b.x, a.y), ImVec2(iMax.x, b.y), shade);
    dl->AddRect(a, b, IM_COL32(255, 255, 255, 220), 0, 0, 1.5f);
    for (int i = 1; i < 3; i++) {
        float t = i / 3.0f;
        dl->AddLine(ImVec2(a.x + (b.x - a.x) * t, a.y), ImVec2(a.x + (b.x - a.x) * t, b.y), IM_COL32(255, 255, 255, 70));
        dl->AddLine(ImVec2(a.x, a.y + (b.y - a.y) * t), ImVec2(b.x, a.y + (b.y - a.y) * t), IM_COL32(255, 255, 255, 70));
    }
    ImVec2 hs[8] = { a, ImVec2((a.x + b.x) / 2, a.y), ImVec2(b.x, a.y), ImVec2(b.x, (a.y + b.y) / 2), b, ImVec2((a.x + b.x) / 2, b.y), ImVec2(a.x, b.y), ImVec2(a.x, (a.y + b.y) / 2) };
    ImVec2 m = io.MousePos;
    float hr = 12 * g_app.dpi;
    int hot = -1;
    for (int i = 0; i < 8; i++) if (Dist2(m, hs[i]) < hr * hr) hot = i;
    if (hot < 0 && m.x > a.x && m.x < b.x && m.y > a.y && m.y < b.y) hot = 8;
    if (s_drag >= 0) hot = s_drag;
    for (int i = 0; i < 8; i++) {
        ImVec2 c = hs[i];
        float L = 14 * g_app.dpi;
        ImU32 col = hot == i ? W::kAccent : IM_COL32(255, 255, 255, 255);
        if (i % 2 == 0) {
            float sx = (i == 0 || i == 6) ? 1.f : -1.f, sy = (i == 0 || i == 2) ? 1.f : -1.f;
            dl->AddLine(c, ImVec2(c.x + sx * L, c.y), col, 3.5f);
            dl->AddLine(c, ImVec2(c.x, c.y + sy * L), col, 3.5f);
        } else dl->AddCircleFilled(c, 4 * g_app.dpi, col);
    }
    if (hovered && ImGui::IsMouseClicked(0) && hot >= 0) {
        s_drag = hot; s_dragStart = m;
        s_cropStart[0] = p.cropX0; s_cropStart[1] = p.cropY0; s_cropStart[2] = p.cropX1; s_cropStart[3] = p.cropY1;
    }
    if (s_drag >= 0 && io.MouseDown[0]) {
        ImVec2 c0 = X.scrToCnv(s_dragStart), c1 = X.scrToCnv(m);
        float dx = c1.x - c0.x, dy = c1.y - c0.y;
        float x0 = s_cropStart[0], y0 = s_cropStart[1], x1 = s_cropStart[2], y1 = s_cropStart[3];
        if (s_drag == 8) {
            float w = x1 - x0, h = y1 - y0;
            x0 = std::clamp(x0 + dx, 0.0f, 1 - w); y0 = std::clamp(y0 + dy, 0.0f, 1 - h);
            x1 = x0 + w; y1 = y0 + h;
        } else {
            int hd = s_drag;
            bool L = hd == 0 || hd == 6 || hd == 7, R = hd == 2 || hd == 3 || hd == 4, Tp = hd == 0 || hd == 1 || hd == 2, B = hd == 4 || hd == 5 || hd == 6;
            const float minS = 0.03f;
            if (L) x0 = std::clamp(x0 + dx, 0.0f, x1 - minS);
            if (R) x1 = std::clamp(x1 + dx, x0 + minS, 1.0f);
            if (Tp) y0 = std::clamp(y0 + dy, 0.0f, y1 - minS);
            if (B) y1 = std::clamp(y1 + dy, y0 + minS, 1.0f);
            float ar = AspectTarget(p, X.geo);
            if (ar > 0) {
                float cw = (float)X.geo.canvasW, ch = (float)X.geo.canvasH;
                float wpx = (x1 - x0) * cw, hpx = (y1 - y0) * ch;
                bool horizEdge = hd == 1 || hd == 5, vertEdge = hd == 3 || hd == 7;
                if (horizEdge) wpx = hpx * ar;
                else if (vertEdge) hpx = wpx / ar;
                else { if (wpx / hpx > ar) wpx = hpx * ar; else hpx = wpx / ar; }
                float nw = wpx / cw, nh = hpx / ch;
                if (horizEdge) { float cx = (x0 + x1) / 2; x0 = cx - nw / 2; x1 = cx + nw / 2; }
                else if (L) x0 = x1 - nw; else x1 = x0 + nw;
                if (vertEdge) { float cy = (y0 + y1) / 2; y0 = cy - nh / 2; y1 = cy + nh / 2; }
                else if (Tp) y0 = y1 - nh; else y1 = y0 + nh;
                if (x0 < 0 || x1 > 1 || y0 < 0 || y1 > 1) { x0 = p.cropX0; y0 = p.cropY0; x1 = p.cropX1; y1 = p.cropY1; }
            }
        }
        if (x0 != p.cropX0 || y0 != p.cropY0 || x1 != p.cropX1 || y1 != p.cropY1) { p.cropX0 = x0; p.cropY0 = y0; p.cropX1 = x1; p.cropY1 = y1; Changed(); }
    }
    if (!io.MouseDown[0]) s_drag = -1;
    if (hot >= 0 && hot < 8) ImGui::SetMouseCursor((hot % 4 == 1) ? ImGuiMouseCursor_ResizeNS : (hot % 4 == 3) ? ImGuiMouseCursor_ResizeEW : (hot == 0 || hot == 4) ? ImGuiMouseCursor_ResizeNWSE : ImGuiMouseCursor_ResizeNESW);
    else if (hot == 8) ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeAll);
}

static bool MaskInteract(const CanvasXf& X, ImDrawList* dl, bool hovered) {
    App& a = g_app;
    Params& p = a.params;
    if (a.selMask < 0 || a.selMask >= (int)p.masks.size()) return false;
    Mask& m = p.masks[a.selMask];
    ImGuiIO& io = ImGui::GetIO();
    ImVec2 mp = io.MousePos;
    float hr = 11 * a.dpi;
    bool used = false;
    float asp = (float)X.geo.canvasW / X.geo.canvasH;
    if (m.shape == MS_LINEAR) {
        ImVec2 p0 = X.cnvToScr(m.lx0, m.ly0), p1 = X.cnvToScr(m.lx1, m.ly1);
        ImVec2 mid((p0.x + p1.x) / 2, (p0.y + p1.y) / 2);
        ImVec2 d(p1.x - p0.x, p1.y - p0.y);
        float len = std::sqrt(d.x * d.x + d.y * d.y) + 1e-3f;
        ImVec2 n(-d.y / len * 2000, d.x / len * 2000);
        dl->AddLine(ImVec2(p0.x - n.x, p0.y - n.y), ImVec2(p0.x + n.x, p0.y + n.y), IM_COL32(255, 255, 255, 170), 1.5f);
        dl->AddLine(ImVec2(p1.x - n.x, p1.y - n.y), ImVec2(p1.x + n.x, p1.y + n.y), IM_COL32(255, 255, 255, 90), 1.0f);
        dl->AddLine(ImVec2(mid.x - n.x, mid.y - n.y), ImVec2(mid.x + n.x, mid.y + n.y), IM_COL32(255, 255, 255, 50), 1.0f);
        dl->AddLine(p0, p1, IM_COL32(255, 255, 255, 120), 1.0f);
        int hot = s_drag >= 0 ? s_drag : (Dist2(mp, p0) < hr * hr ? 0 : Dist2(mp, p1) < hr * hr ? 1 : Dist2(mp, mid) < hr * hr ? 2 : -1);
        HandleDot(dl, p0, hot == 0); HandleDot(dl, p1, hot == 1, 5); HandleDot(dl, mid, hot == 2, 4);
        if (hovered && ImGui::IsMouseClicked(0) && hot >= 0) { s_drag = hot; s_dragStart = mp; s_maskStart = m; used = true; }
        if (s_drag >= 0 && io.MouseDown[0]) {
            ImVec2 c0 = X.scrToCnv(s_dragStart), c1 = X.scrToCnv(mp);
            float dx = c1.x - c0.x, dy = c1.y - c0.y;
            if (s_drag == 0 || s_drag == 2) { m.lx0 = s_maskStart.lx0 + dx; m.ly0 = s_maskStart.ly0 + dy; }
            if (s_drag == 1 || s_drag == 2) { m.lx1 = s_maskStart.lx1 + dx; m.ly1 = s_maskStart.ly1 + dy; }
            Changed(); used = true;
        }
    } else if (m.shape == MS_RADIAL) {
        ImVec2 c = X.cnvToScr(m.cx, m.cy);
        float ang = m.angle * 3.14159265f / 180;
        float ca = std::cos(ang), sa = std::sin(ang);
        auto pt = [&](float ex, float ey) {
            float qx = ex * m.rx * asp, qy = ey * m.ry;
            float rx = ca * qx - sa * qy, ry = sa * qx + ca * qy;
            return X.cnvToScr(m.cx + rx / asp, m.cy + ry);
        };
        const int N = 64;
        ImVec2 ring[N], inner[N];
        float f = 1 - m.feather / 100;
        for (int i = 0; i < N; i++) {
            float t = i * 6.2831853f / N;
            ring[i] = pt(std::cos(t), std::sin(t));
            inner[i] = pt(std::cos(t) * f, std::sin(t) * f);
        }
        dl->AddPolyline(ring, N, IM_COL32(255, 255, 255, 200), ImDrawFlags_Closed, 1.5f);
        dl->AddPolyline(inner, N, IM_COL32(255, 255, 255, 80), ImDrawFlags_Closed, 1.0f);
        ImVec2 hx = pt(1, 0), hy = pt(0, -1), hx2 = pt(-1, 0), hy2 = pt(0, 1);
        int hot = -1;
        if (s_drag >= 0) hot = s_drag;
        else if (Dist2(mp, hx) < hr * hr || Dist2(mp, hx2) < hr * hr) hot = 1;
        else if (Dist2(mp, hy) < hr * hr || Dist2(mp, hy2) < hr * hr) hot = 2;
        else if (Dist2(mp, c) < hr * hr * 2) hot = 0;
        HandleDot(dl, c, hot == 0);
        HandleDot(dl, hx, hot == 1, 5); HandleDot(dl, hx2, hot == 1, 5);
        HandleDot(dl, hy, hot == 2, 5); HandleDot(dl, hy2, hot == 2, 5);
        if (hovered && ImGui::IsMouseClicked(0) && hot >= 0) { s_drag = hot; s_dragStart = mp; s_maskStart = m; used = true; }
        if (s_drag >= 0 && io.MouseDown[0]) {
            ImVec2 c0 = X.scrToCnv(s_dragStart), c1 = X.scrToCnv(mp);
            if (s_drag == 0) { m.cx = s_maskStart.cx + c1.x - c0.x; m.cy = s_maskStart.cy + c1.y - c0.y; }
            else {
                float qx = (c1.x - m.cx) * asp, qy = c1.y - m.cy;
                float lx = ca * qx + sa * qy, ly = -sa * qx + ca * qy;
                if (s_drag == 1) m.rx = std::max(0.005f, std::fabs(lx) / asp);
                else m.ry = std::max(0.005f, std::fabs(ly));
                if (io.KeyShift) { if (s_drag == 1) m.ry = m.rx * asp; else m.rx = m.ry / asp; }
            }
            Changed(); used = true;
        }
    } else if (m.shape == MS_BRUSH) {
        float rScr = a.brushSize * X.imgSize.y / X.geo.ch;
        bool erase = a.brushErase || io.KeyAlt;
        dl->AddCircle(mp, rScr, erase ? IM_COL32(255, 120, 120, 220) : IM_COL32(255, 255, 255, 220), 48, 1.5f);
        dl->AddCircle(mp, rScr * (1 - a.brushFeather), IM_COL32(255, 255, 255, 90), 48, 1.0f);
        if (hovered && ImGui::IsMouseClicked(0) && !io.KeyCtrl) { a.painting = true; a.lastDabX = -1; }
        if (a.painting && io.MouseDown[0]) {
            ImVec2 c = X.scrToCnv(mp);
            float spacing = a.brushSize * 0.2f;
            auto addDab = [&](float x, float y) { m.dabs.push_back(Dab{ x, y, a.brushSize, a.brushFeather, a.brushFlow * 0.5f, erase ? 1.f : 0.f }); };
            if (a.lastDabX < 0) { addDab(c.x, c.y); a.lastDabX = c.x; a.lastDabY = c.y; Changed(); }
            else {
                float dx = (c.x - a.lastDabX) * asp, dy = c.y - a.lastDabY;
                float d = std::sqrt(dx * dx + dy * dy);
                if (d >= spacing) {
                    int n = (int)(d / spacing);
                    for (int k = 1; k <= n; k++) {
                        float t = k * spacing / d;
                        addDab(a.lastDabX + (c.x - a.lastDabX) * t, a.lastDabY + (c.y - a.lastDabY) * t);
                    }
                    float t = n * spacing / d;
                    a.lastDabX += (c.x - a.lastDabX) * t; a.lastDabY += (c.y - a.lastDabY) * t;
                    Changed();
                }
            }
            used = true;
        }
        if (!io.MouseDown[0]) a.painting = false;
        if (hovered) ImGui::SetMouseCursor(ImGuiMouseCursor_None);
        used = used || hovered;
    }
    if (!io.MouseDown[0]) s_drag = -1;
    return used || s_drag >= 0;
}

static void DoPick(const CanvasXf& X) {
    App& a = g_app;
    ImVec2 uv = X.scrToUv(ImGui::GetIO().MousePos);
    if (uv.x < 0 || uv.y < 0 || uv.x > 1 || uv.y > 1) return;
    float rgb[3];
    if (a.pick == PICK_WB) {
        if (g_pipe.sampleSource(uv.x, uv.y, rgb)) { SolveWB(rgb[0], rgb[1], rgb[2], a.params); Changed(); SetStatus(T("Баланс белого выставлен по точке", "White balance set from the picked point")); }
        a.pick = PICK_NONE;
        return;
    }
    if (!g_pipe.sampleDeveloped(uv.x, uv.y, rgb)) return;
    float h, s, v;
    ImGui::ColorConvertRGBtoHSV(rgb[0], rgb[1], rgb[2], h, s, v);
    float L = 0.2126f * rgb[0] + 0.7152f * rgb[1] + 0.0722f * rgb[2];
    if ((a.pick == PICK_MASK_COLOR || a.pick == PICK_MASK_LUMA) && a.selMask >= 0 && a.selMask < (int)a.params.masks.size()) {
        Mask& m = a.params.masks[a.selMask];
        if (a.pick == PICK_MASK_COLOR) { m.range = MR_COLOR; m.hueC = h * 360; m.satMin = std::min(0.1f, s * 0.5f); }
        else { m.range = MR_LUMA; m.lumLo = std::max(0.0f, L - 0.15f); m.lumHi = std::min(1.0f, L + 0.15f); }
        Changed();
    }
    if ((a.pick == PICK_FX_COLOR || a.pick == PICK_FX_LUMA) && a.selEffect >= 0 && a.selEffect < (int)a.params.effects.size()) {
        Effect& e = a.params.effects[a.selEffect];
        if (a.pick == PICK_FX_COLOR) { e.target = TG_COLOR; e.tHue = h * 360; e.tSatMin = std::min(0.1f, s * 0.5f); }
        else { e.target = TG_LUMA; e.tLo = std::max(0.0f, L - 0.15f); e.tHi = std::min(1.0f, L + 0.15f); }
        Changed();
    }
    a.pick = PICK_NONE;
}

static int WorkTier(float needed, int fullLong) {
    static const int tiers[] = { 1600, 2400, 3600, 5400, 8192 };
    int t = 8192;
    for (int x : tiers) if (x >= needed) { t = x; break; }
    return std::min(t, std::max(1, fullLong));
}

static void Badge(ImDrawList* dl, ImVec2 p, const char* text, ImU32 bg, ImU32 fg) {
    ImVec2 s = ImGui::CalcTextSize(text);
    dl->AddRectFilled(ImVec2(p.x - 9, p.y - 5), ImVec2(p.x + s.x + 9, p.y + s.y + 5), bg, 7);
    dl->AddText(p, fg, text);
}

static void Canvas(ImVec2 pos, ImVec2 size) {
    App& a = g_app;
    ImGuiIO& io = ImGui::GetIO();
    ImGui::SetCursorScreenPos(pos);
    ImGui::BeginChild("##canvas", size, 0, ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse | ImGuiWindowFlags_NoBackground);
    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->AddRectFilled(pos, ImVec2(pos.x + size.x, pos.y + size.y), IM_COL32(14, 15, 17, 255));

    if (!g_pipe.hasSource()) {
        ImGui::PushFont(a.fontBig);
        const char* t1 = "Muon";
        ImVec2 ts = ImGui::CalcTextSize(t1);
        dl->AddText(ImVec2(pos.x + (size.x - ts.x) / 2, pos.y + size.y * 0.36f), IM_COL32(230, 232, 236, 255), t1);
        ImGui::PopFont();
        const char* t2 = a.loading ? T("Загрузка…", "Loading…") : T("Перетащите сюда RAW или JPEG либо нажмите Ctrl+O", "Drop a RAW or JPEG here, or press Ctrl+O");
        ts = ImGui::CalcTextSize(t2);
        dl->AddText(ImVec2(pos.x + (size.x - ts.x) / 2, pos.y + size.y * 0.36f + 56 * a.dpi), IM_COL32(140, 144, 152, 255), t2);
        if (!a.loading) {
            ImGui::SetCursorScreenPos(ImVec2(pos.x + size.x / 2 - 100 * a.dpi, pos.y + size.y * 0.36f + 94 * a.dpi));
            if (W::Button(T("Открыть фото", "Open photo"), ImVec2(200 * a.dpi, 40 * a.dpi), W::BTN_PRIMARY)) { auto f = OpenFileDialog(a.hwnd); if (!f.empty()) AppOpenFile(f); }
        }
        ImGui::EndChild();
        return;
    }

    bool cropMode = a.tool == TOOL_CROP && a.bottomTab == TAB_CLASSIC;
    CanvasXf X;
    X.cropMode = cropMode;
    X.geo = g_pipe.geometry(a.params, cropMode);
    float fw = (float)X.geo.fullW, fh = (float)X.geo.fullH;
    float fitZ = std::min(size.x / fw, size.y / fh) * (cropMode ? 0.86f : 0.95f);
    bool useFit = a.fit || cropMode;
    float z = useFit ? fitZ : a.zoom;
    X.imgSize = ImVec2(fw * z, fh * z);
    X.imgMin = ImVec2(pos.x + (size.x - X.imgSize.x) / 2 + (useFit ? 0 : a.panX), pos.y + (size.y - X.imgSize.y) / 2 + (useFit ? 0 : a.panY));
    a.workLong = WorkTier(std::max(X.imgSize.x, X.imgSize.y), std::max(X.geo.fullW, X.geo.fullH));

    ImGui::SetCursorScreenPos(pos);
    ImGui::InvisibleButton("##cv", size, ImGuiButtonFlags_MouseButtonLeft | ImGuiButtonFlags_MouseButtonRight | ImGuiButtonFlags_MouseButtonMiddle);
    bool hovered = ImGui::IsItemHovered();

    if (g_pipe.display) dl->AddImage(TexId(g_pipe.display), X.imgMin, ImVec2(X.imgMin.x + X.imgSize.x, X.imgMin.y + X.imgSize.y));

    bool consumed = false;
    bool spaceHeld = ImGui::IsKeyDown(ImGuiKey_Space) && !io.WantTextInput;
    if (a.pick != PICK_NONE && !spaceHeld) {
        if (hovered) ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
        if (hovered && ImGui::IsMouseClicked(0)) DoPick(X);
        consumed = hovered;
    } else if (cropMode && !spaceHeld) {
        CropInteract(X, dl, hovered);
        consumed = true;
    } else if (a.tool == TOOL_MASK && a.bottomTab == TAB_CLASSIC && !spaceHeld) {
        consumed = MaskInteract(X, dl, hovered);
    }

    if (hovered && io.MouseWheel != 0 && !cropMode) {
        if (io.KeyCtrl && a.tool == TOOL_MASK) {
            a.brushSize = std::clamp(a.brushSize * (io.MouseWheel > 0 ? 1.12f : 0.89f), 0.003f, 0.4f);
        } else {
            float nz = std::clamp(z * std::pow(1.18f, io.MouseWheel), fitZ * 0.25f, 16.0f);
            ImVec2 m = io.MousePos;
            ImVec2 rel((m.x - X.imgMin.x) / z, (m.y - X.imgMin.y) / z);
            ImVec2 newMin(m.x - rel.x * nz, m.y - rel.y * nz);
            a.panX = newMin.x - (pos.x + (size.x - fw * nz) / 2);
            a.panY = newMin.y - (pos.y + (size.y - fh * nz) / 2);
            a.zoom = nz; a.fit = false;
        }
    }
    bool panDrag = ImGui::IsItemActive() && (ImGui::IsMouseDragging(2, 0) || (spaceHeld && ImGui::IsMouseDragging(0, 0)) || (!consumed && ImGui::IsMouseDragging(0, 2)));
    if (panDrag && !cropMode) {
        if (a.fit) { a.fit = false; a.zoom = z; a.panX = a.panY = 0; }
        a.panX += io.MouseDelta.x; a.panY += io.MouseDelta.y;
    }
    if (hovered && ImGui::IsMouseDoubleClicked(0) && !consumed && !cropMode) {
        if (a.fit) {
            ImVec2 m = io.MousePos;
            ImVec2 rel((m.x - X.imgMin.x) / z, (m.y - X.imgMin.y) / z);
            float nz = 1.0f;
            ImVec2 newMin(m.x - rel.x * nz, m.y - rel.y * nz);
            a.panX = newMin.x - (pos.x + (size.x - fw * nz) / 2);
            a.panY = newMin.y - (pos.y + (size.y - fh * nz) / 2);
            a.zoom = nz; a.fit = false;
        } else a.fit = true;
    }

    // overlays
    float m12 = 14 * a.dpi;
    char buf[128];
    snprintf(buf, sizeof(buf), "%s%.0f%%", useFit ? T("Вписано · ", "Fit · ") : "", z * 100);
    ImVec2 ts = ImGui::CalcTextSize(buf);
    Badge(dl, ImVec2(pos.x + size.x - ts.x - m12 - 4, pos.y + m12), buf, IM_COL32(0, 0, 0, 140), IM_COL32(205, 207, 212, 255));
    if (a.before) Badge(dl, ImVec2(pos.x + m12 + 4, pos.y + m12), T("ДО", "BEFORE"), IM_COL32(222, 128, 46, 230), IM_COL32(255, 255, 255, 255));
    if (a.pick != PICK_NONE) {
        const char* t = a.pick == PICK_WB ? T("Кликните по нейтрально-серому участку  ·  Esc отменяет", "Click a neutral grey area  ·  Esc to cancel")
                                          : T("Кликните по фото, чтобы выбрать  ·  Esc отменяет", "Click the photo to pick  ·  Esc to cancel");
        ImVec2 s2 = ImGui::CalcTextSize(t);
        Badge(dl, ImVec2(pos.x + (size.x - s2.x) / 2, pos.y + m12), t, IM_COL32(0, 0, 0, 180), IM_COL32(255, 255, 255, 255));
    }
    // file info, bottom right
    bool statusOn = !a.status.empty() && Now() - a.statusTime < 3.5;
    if (!a.curPath.empty() && !statusOn) {
        ImGui::PushFont(a.fontSmall);
        std::string info = WideToUtf8(PathFindFileNameW(a.curPath.c_str())) + "  ·  " + a.info;
        ImVec2 s2 = ImGui::CalcTextSize(info.c_str());
        if (s2.x < size.x * 0.6f) Badge(dl, ImVec2(pos.x + size.x - s2.x - m12 - 4, pos.y + size.y - s2.y - m12), info.c_str(), IM_COL32(0, 0, 0, 120), IM_COL32(170, 173, 180, 255));
        ImGui::PopFont();
    }
    if (a.loading || a.batchActive) {
        float t = (float)Now();
        ImVec2 c(pos.x + size.x / 2, pos.y + size.y / 2);
        dl->AddCircleFilled(c, 46 * a.dpi, IM_COL32(0, 0, 0, 150));
        dl->PathArcTo(c, 22 * a.dpi, t * 6, t * 6 + 4.2f, 32);
        dl->PathStroke(W::kAccent, 0, 3.5f * a.dpi);
        char lt[96];
        if (a.batchActive) snprintf(lt, sizeof(lt), T("Экспорт %d из %d…", "Exporting %d of %d…"), (int)a.batchIndex + 1, (int)a.batchFiles.size());
        else snprintf(lt, sizeof(lt), "%s", T("Загрузка…", "Loading…"));
        ImVec2 s2 = ImGui::CalcTextSize(lt);
        Badge(dl, ImVec2(c.x - s2.x / 2, c.y + 56 * a.dpi), lt, IM_COL32(0, 0, 0, 170), IM_COL32(230, 230, 230, 255));
    }
    if (!a.status.empty() && Now() - a.statusTime < 3.5) {
        float alpha = (float)std::clamp((3.5 - (Now() - a.statusTime)) * 2.0, 0.0, 1.0);
        ImVec2 s2 = ImGui::CalcTextSize(a.status.c_str());
        Badge(dl, ImVec2(pos.x + m12 + 4, pos.y + size.y - s2.y - m12), a.status.c_str(), IM_COL32(0, 0, 0, (int)(180 * alpha)), IM_COL32(235, 235, 235, (int)(255 * alpha)));
    }
    ImGui::EndChild();
}

// =====================================================================  bars
static void TopBar(ImVec2 pos, ImVec2 size) {
    App& a = g_app;
    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->AddRectFilled(pos, ImVec2(pos.x + size.x, pos.y + size.y), IM_COL32(19, 20, 22, 255));
    dl->AddLine(ImVec2(pos.x, pos.y + size.y - 1), ImVec2(pos.x + size.x, pos.y + size.y - 1), IM_COL32(255, 255, 255, 12));
    float bh = size.y - 14 * a.dpi;
    float y0 = pos.y + 7 * a.dpi;
    float nx = pos.x + 16 * a.dpi;
    auto place = [&](float gap) { ImGui::SetCursorScreenPos(ImVec2(nx + gap, y0)); };
    auto advance = [&]() { nx = ImGui::GetItemRectMax().x; };
    auto sep = [&]() { nx += 10 * a.dpi; dl->AddLine(ImVec2(nx, y0 + 6), ImVec2(nx, y0 + bh - 6), IM_COL32(255, 255, 255, 22)); nx += 4 * a.dpi; };

    ImGui::PushFont(a.fontBold);
    ImVec2 ls = ImGui::CalcTextSize("MUON");
    dl->AddText(ImVec2(nx, pos.y + (size.y - ls.y) / 2), W::kAccent, "MUON");
    ImGui::PopFont();
    nx += ls.x + 12 * a.dpi;
    bool has = g_pipe.hasSource();

    place(4);
    if (W::Button(T("Открыть", "Open"), ImVec2(0, bh), W::BTN_GHOST)) { auto f = OpenFileDialog(a.hwnd); if (!f.empty()) AppOpenFile(f); }
    W::Hint("Ctrl+O");
    advance();
    ImGui::BeginDisabled(!has);
    place(6);
    if (W::Button(T("Экспорт", "Export"), ImVec2(90 * a.dpi, bh), W::BTN_PRIMARY)) OpenExport(CollectionSelection());
    W::Hint("Ctrl+E");
    advance();
    sep();
    place(4);
    ImGui::BeginDisabled(a.histPos <= 0);
    ImGui::SetWindowFontScale(1.45f);
    if (W::IconButton("↶", T("Отменить (Ctrl+Z)", "Undo (Ctrl+Z)"), W::BTN_GHOST, false, bh)) Undo();
    ImGui::SetWindowFontScale(1.0f);
    ImGui::EndDisabled();
    advance();
    place(2);
    ImGui::BeginDisabled(a.histPos + 1 >= (int)a.history.size());
    ImGui::SetWindowFontScale(1.45f);
    if (W::IconButton("↷", T("Повторить (Ctrl+Y)", "Redo (Ctrl+Y)"), W::BTN_GHOST, false, bh)) Redo();
    ImGui::SetWindowFontScale(1.0f);
    ImGui::EndDisabled();
    advance();
    sep();
    place(4);
    if (W::Button(T("До / после", "Before / after"), ImVec2(0, bh), W::BTN_GHOST, a.before)) a.before = !a.before;
    W::Hint(T("Клавиша \\", "Key \\"));
    advance();
    place(4);
    if (W::Button(a.fit ? "100%" : T("Вписать", "Fit"), ImVec2(0, bh), W::BTN_GHOST)) { if (a.fit) { a.fit = false; a.zoom = 1; a.panX = a.panY = 0; } else a.fit = true; }
    W::Hint(T("Клавиша Z или двойной клик по фото", "Key Z or double-click the photo"));
    advance();
    ImGui::EndDisabled();

    // right side
    float rx = pos.x + size.x - 16 * a.dpi;
    float menuW = bh * 1.2f;
    float langW = 84 * a.dpi;
    const char* resetLabel = T("Сбросить всё", "Reset all");
    float resetW = ImGui::CalcTextSize(resetLabel).x + 28 * a.dpi;
    rx -= menuW;
    ImGui::SetCursorScreenPos(ImVec2(rx, y0));
    if (W::IconButton("⋯", T("Ещё", "More"), W::BTN_GHOST, false, bh)) ImGui::OpenPopup("more");
    rx -= langW + 8 * a.dpi;
    ImGui::SetCursorScreenPos(ImVec2(rx, y0 + (bh - ImGui::GetFrameHeight()) * 0.5f));
    ImGui::BeginGroup();
    ImGui::PushItemWidth(langW);
    {
        int l = g_lang;
        const char* langs[] = { "RU", "EN" };
        ImGui::BeginChild("##lang", ImVec2(langW, ImGui::GetFrameHeight()), 0, ImGuiWindowFlags_NoBackground | ImGuiWindowFlags_NoScrollbar);
        if (W::Segmented("lang", &l, langs, 2)) SetLanguage(l);
        ImGui::EndChild();
    }
    ImGui::PopItemWidth();
    ImGui::EndGroup();
    W::Hint(T("Язык интерфейса", "Interface language"));
    rx -= resetW + 8 * a.dpi;
    ImGui::SetCursorScreenPos(ImVec2(rx, y0));
    ImGui::BeginDisabled(!has);
    if (W::Button(resetLabel, ImVec2(resetW, bh), W::BTN_DANGER)) ResetAll();
    W::Hint(T("Сбросить все правки этого фото, включая кадр, маски и эффекты. Отменяется через Ctrl+Z.",
              "Reset every edit of this photo, including crop, masks and effects. Undo with Ctrl+Z."));
    ImGui::EndDisabled();

    if (ImGui::BeginPopup("more")) {
        if (ImGui::MenuItem(T("Копировать настройки", "Copy settings"), "Ctrl+Shift+C", false, has)) CopySettingsFrom(a.curPath, SG_DEVELOP);
        if (ImGui::MenuItem(T("Копировать выборочно…", "Copy selected settings…"), nullptr, false, has)) OpenCopyDialog(a.curPath);
        if (ImGui::MenuItem(T("Вставить настройки", "Paste settings"), "Ctrl+Shift+V", false, a.clip.valid)) PasteSettingsTo(CollectionSelection());
        ImGui::Separator();
        if (ImGui::MenuItem(T("Создать пресет…", "New preset…"), nullptr, false, has)) a.newPresetOpen = true;
        if (ImGui::MenuItem(T("Вставить пресет из кода", "Add preset from code"))) PastePresetCode();
        ImGui::Separator();
        if (ImGui::MenuItem(T("Лента снимков", "Filmstrip"), "Tab", a.showFilmstrip)) { a.showFilmstrip = !a.showFilmstrip; SaveSettings(); }
        if (ImGui::MenuItem(T("Показывать пересветы и провалы", "Show clipping"), "J", a.clipHi)) { a.clipHi = !a.clipHi; a.clipLo = a.clipHi; }
        ImGui::Separator();
        if (ImGui::BeginMenu(T("Видеокарта", "Graphics card"))) {
            std::wstring pref = ReadGpuPreference();
            if (ImGui::MenuItem(T("Самая мощная (автоматически)", "Most powerful (automatic)"), nullptr, pref.empty())) {
                WriteGpuPreference(L"");
                SetStatus(T("Видеокарта сменится после перезапуска Muon", "The graphics card changes after restarting Muon"));
            }
            ImGui::Separator();
            for (int i = 0; i < (int)g_gpuNames.size(); i++) {
                std::string n = WideToUtf8(g_gpuNames[i]);
                if (i == g_gpuUsed) n += T("  (сейчас)", "  (in use)");
                ImGui::PushID(i);
                if (ImGui::MenuItem(n.c_str(), nullptr, !pref.empty() && pref == g_gpuNames[i])) {
                    WriteGpuPreference(g_gpuNames[i]);
                    SetStatus(T("Видеокарта сменится после перезапуска Muon", "The graphics card changes after restarting Muon"));
                }
                ImGui::PopID();
            }
            ImGui::EndMenu();
        }
        if (ImGui::MenuItem(T("Горячие клавиши", "Keyboard shortcuts"), "F1")) a.showHelp = true;
        ImGui::EndPopup();
    }
}

static void BottomTabs(ImVec2 pos, ImVec2 size) {
    App& a = g_app;
    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->AddRectFilled(pos, ImVec2(pos.x + size.x, pos.y + size.y), IM_COL32(19, 20, 22, 255));
    dl->AddLine(pos, ImVec2(pos.x + size.x, pos.y), IM_COL32(255, 255, 255, 12));
    const char* names[3] = { T("БИБЛИОТЕКА", "LIBRARY"), T("КЛАССИКА", "CLASSIC"), T("ЭКСПЕРИМЕНТЫ", "EXPERIMENTS") };
    const char* keys[3] = { "G", "D", "" };
    float tw = 180 * a.dpi, th = size.y - 12 * a.dpi, gap = 6;
    float x0 = pos.x + (size.x - tw * 3 - gap * 2) / 2;
    ImGui::PushFont(a.fontBold);
    for (int i = 0; i < 3; i++) {
        ImVec2 p(x0 + i * (tw + gap), pos.y + 6 * a.dpi);
        ImGui::SetCursorScreenPos(p);
        ImGui::PushID(i);
        if (ImGui::InvisibleButton("##tab", ImVec2(tw, th))) {
            a.bottomTab = i;
            if (i != TAB_CLASSIC && a.tool == TOOL_CROP) a.tool = TOOL_EDIT;
            a.pick = PICK_NONE;
        }
        if (keys[i][0]) W::Hint(keys[i]);
        bool hov = ImGui::IsItemHovered();
        bool sel = a.bottomTab == i;
        if (sel) dl->AddRectFilled(p, ImVec2(p.x + tw, p.y + th), IM_COL32(255, 255, 255, 20), 8);
        else if (hov) dl->AddRectFilled(p, ImVec2(p.x + tw, p.y + th), IM_COL32(255, 255, 255, 9), 8);
        if (sel) dl->AddRectFilled(ImVec2(p.x + tw * 0.32f, p.y + th - 3), ImVec2(p.x + tw * 0.68f, p.y + th - 1), W::kAccent, 2);
        ImVec2 ts = ImGui::CalcTextSize(names[i]);
        dl->AddText(ImVec2(p.x + (tw - ts.x) / 2, p.y + (th - ts.y) / 2 - 1), sel ? IM_COL32(240, 242, 246, 255) : IM_COL32(130, 134, 142, 255), names[i]);
        ImGui::PopID();
    }
    ImGui::PopFont();
    if (!a.collection.empty() && a.bottomTab != TAB_LIBRARY) {
        int nSel = 0;
        for (char c : a.collSel) nSel += c ? 1 : 0;
        char b[128];
        if (nSel > 1) snprintf(b, sizeof(b), T("%d / %d  ·  выбрано %d", "%d / %d  ·  %d selected"), a.curIndex + 1, (int)a.collection.size(), nSel);
        else snprintf(b, sizeof(b), "%d / %d", a.curIndex + 1, (int)a.collection.size());
        ImVec2 ts = ImGui::CalcTextSize(b);
        dl->AddText(ImVec2(pos.x + 16 * a.dpi, pos.y + (size.y - ts.y) / 2), IM_COL32(120, 124, 132, 255), b);
    }
    const char* hint = T("F1: горячие клавиши", "F1: shortcuts");
    ImVec2 ts = ImGui::CalcTextSize(hint);
    dl->AddText(ImVec2(pos.x + size.x - ts.x - 16 * a.dpi, pos.y + (size.y - ts.y) / 2), IM_COL32(90, 94, 102, 255), hint);
}

static void RightPanel(ImVec2 pos, ImVec2 size) {
    App& a = g_app;
    ImGui::SetCursorScreenPos(pos);
    ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0.095f, 0.098f, 0.107f, 1));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(14 * a.dpi, 12 * a.dpi));
    ImGui::BeginChild("##panel", size, 0, ImGuiWindowFlags_AlwaysUseWindowPadding | ImGuiWindowFlags_NoScrollbar);
    float w = ImGui::GetContentRegionAvail().x;
    if (a.bottomTab == TAB_LIBRARY) {
        ImGui::BeginChild("##libp", ImVec2(0, 0), 0, ImGuiWindowFlags_NoBackground);
        LibraryPanel();
        ImGui::EndChild();
        ImGui::EndChild();
        ImGui::PopStyleVar();
        ImGui::PopStyleColor();
        return;
    }
    W::Histogram(g_pipe.hist, ImVec2(w, 92 * a.dpi), &a.clipLo, &a.clipHi);
    W::Hint(T("Треугольники в углах включают подсветку провалов в тенях и пересветов (J)", "Corner triangles toggle shadow and highlight clipping (J)"));
    ImGui::Spacing();
    if (!g_pipe.hasSource()) {
        W::Caption(T("Откройте фото, чтобы начать.", "Open a photo to get started."));
    } else if (a.bottomTab == TAB_CLASSIC) {
        const char* tools[] = { T("Правка", "Edit"), T("Кадр", "Crop"), T("Маски", "Masks") };
        int t = a.tool;
        if (W::Segmented("tools", &t, tools, 3, ImGui::GetFrameHeight() * 1.15f)) { a.tool = t; a.pick = PICK_NONE; }
        ImGui::Spacing();
        ImGui::BeginChild("##scroll", ImVec2(0, 0), 0, ImGuiWindowFlags_NoBackground);
        if (a.tool == TOOL_EDIT) {
            PanelPresets();
            PanelBasic();
            PanelCurve();
            PanelHSL();
            PanelGrading();
            PanelBW();
            PanelDetail();
            PanelOptics();
            PanelEffects();
            ImGui::Dummy(ImVec2(0, 30 * a.dpi));
        } else if (a.tool == TOOL_CROP) PanelCrop();
        else { PanelMasks(); ImGui::Dummy(ImVec2(0, 30 * a.dpi)); }
        ImGui::EndChild();
    } else {
        ExperimentalHeader();
        ImGui::BeginChild("##scrollx", ImVec2(0, 0), 0, ImGuiWindowFlags_NoBackground);
        ExperimentalList();
        ImGui::Dummy(ImVec2(0, 30 * a.dpi));
        ImGui::EndChild();
    }
    ImGui::EndChild();
    ImGui::PopStyleVar();
    ImGui::PopStyleColor();
}

// ---------------------------------------------------------------- export dialog
static void ExportModal() {
    App& a = g_app;
    ExportSettings& e = a.exp;
    const char* title = T("Экспорт###export", "Export###export");
    if (a.exportOpen) { ImGui::OpenPopup(title); a.exportOpen = false; }
    ImVec2 vs = ImGui::GetMainViewport()->Size;
    ImGui::SetNextWindowSize(ImVec2(std::min(600 * a.dpi, vs.x * 0.9f), std::min(720 * a.dpi, vs.y * 0.9f)));
    ImGui::SetNextWindowPos(ImGui::GetMainViewport()->GetCenter(), ImGuiCond_Always, ImVec2(0.5f, 0.5f));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(20 * a.dpi, 16 * a.dpi));
    if (!ImGui::BeginPopupModal(title, nullptr, ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove)) { ImGui::PopStyleVar(); return; }
    ImGui::PopStyleVar();
    if (a.exportTargets.size() > 1) {
        char hb[96]; snprintf(hb, sizeof(hb), T("Выбрано фото: %d", "Photos selected: %d"), (int)a.exportTargets.size());
        W::Caption(hb);
    }
    bool changed = false;
    float footer = ImGui::GetFrameHeight() * 1.4f + ImGui::GetStyle().ItemSpacing.y * 3 + 4;
    ImGui::BeginChild("##expbody", ImVec2(0, -footer), 0, ImGuiWindowFlags_NoBackground);

    // format
    if (W::Section(T("Формат файла", "File format"), true)) {
        const char* fm[] = { "JPEG", "PNG", "TIFF" };
        changed |= W::Segmented("fmt", &e.fmt, fm, 3);
        ImGui::Spacing();
        if (e.fmt == EXP_JPEG) {
            float q = (float)e.quality;
            if (W::Slider(T("Качество", "Quality"), &q, 10, 100, 92)) { e.quality = (int)q; changed = true; }
            changed |= W::Toggle(T("Полное цветовое разрешение (4:4:4)", "Full colour resolution (4:4:4)"), &e.chroma444);
            changed |= W::Toggle(T("Ограничить размер файла", "Limit file size"), &e.limitSize);
            if (e.limitSize) {
                float kb = (float)e.maxKB;
                if (W::Slider(T("Не больше, КБ", "Max size, KB"), &kb, 100, 20000, 2000)) { e.maxKB = (int)kb; changed = true; }
                W::Caption(T("Качество будет подобрано автоматически, но не выше заданного.", "Quality is lowered automatically, never above the value set."));
            }
        } else {
            const char* bits[] = { T("8 бит", "8-bit"), T("16 бит", "16-bit") };
            int b = e.bits16 ? 1 : 0;
            if (W::Segmented("bits", &b, bits, 2)) { e.bits16 = b == 1; changed = true; }
            if (e.fmt == EXP_TIFF) {
                const char* comp[] = { T("Без сжатия", "None"), "LZW", "ZIP" };
                changed |= W::Combo(T("Сжатие", "Compression"), &e.tiffComp, comp, 3);
            }
            W::Caption(T("16 бит сохраняют больше оттенков для дальнейшей обработки.", "16-bit keeps more tonal steps for further editing."));
        }
    }
    // size
    if (W::Section(T("Размер", "Image size"), true)) {
        const char* modes[] = { T("Исходный", "Original"), T("Длинная сторона", "Long edge"), T("Короткая сторона", "Short edge"), T("Ширина", "Width"),
                                T("Высота", "Height"), T("Мегапиксели", "Megapixels"), T("Проценты", "Percent") };
        changed |= W::Combo(T("Изменить размер", "Resize to"), &e.sizeMode, modes, 7);
        if (e.sizeMode >= 1 && e.sizeMode <= 4) {
            ImGui::AlignTextToFramePadding();
            ImGui::TextDisabled("%s", T("Пикселей", "Pixels"));
            ImGui::SameLine(ImGui::GetContentRegionAvail().x * 0.42f);
            ImGui::SetNextItemWidth(-1);
            if (ImGui::InputInt("##px", &e.sizeValue, 100, 500)) { e.sizeValue = std::clamp(e.sizeValue, 16, 16384); changed = true; }
            static const int presets[] = { 1080, 1600, 2048, 3840, 6000 };
            float pw = (ImGui::GetContentRegionAvail().x - ImGui::GetStyle().ItemSpacing.x * 4) / 5;
            for (int i = 0; i < 5; i++) {
                if (i) ImGui::SameLine();
                char b[16]; snprintf(b, sizeof(b), "%d", presets[i]);
                if (W::Button(b, ImVec2(pw, 0), W::BTN_DEFAULT, e.sizeValue == presets[i])) { e.sizeValue = presets[i]; changed = true; }
            }
        } else if (e.sizeMode == 5) {
            if (W::Slider(T("Мегапиксели", "Megapixels"), &e.megapixels, 0.5f, 60, 12, "%.1f")) changed = true;
        } else if (e.sizeMode == 6) {
            float pc = (float)e.percent;
            if (W::Slider(T("Процент", "Percent"), &pc, 5, 200, 50, "%.0f%%")) { e.percent = (int)pc; changed = true; }
        }
        changed |= W::Toggle(T("Не увеличивать сверх исходного", "Don't enlarge"), &e.noUpscale);
        int W_, H_;
        ExportTargetSize(e, W_, H_);
        char b[128];
        snprintf(b, sizeof(b), T("Итог: %d × %d пикс., %.1f Мп", "Result: %d × %d px, %.1f MP"), W_, H_, W_ * (double)H_ / 1e6);
        W::Caption(b);
    }
    // output sharpening
    if (W::Section(T("Резкость при выводе", "Output sharpening"), false)) {
        const char* sh[] = { T("Не добавлять", "None"), T("Для экрана", "Screen"), T("Для матовой бумаги", "Matte paper"), T("Для глянцевой бумаги", "Glossy paper") };
        changed |= W::Combo(T("Назначение", "Sharpen for"), &e.sharpen, sh, 4);
        if (e.sharpen) {
            const char* am[] = { T("Слабая", "Low"), T("Средняя", "Standard"), T("Сильная", "High") };
            changed |= W::Segmented("sham", &e.sharpenAmt, am, 3);
        }
    }
    // watermark
    if (W::Section(T("Водяной знак", "Watermark"), false)) {
        changed |= W::Toggle(T("Добавить подпись", "Add a text watermark"), &e.watermark);
        if (e.watermark) {
            ImGui::SetNextItemWidth(-1);
            if (ImGui::InputTextWithHint("##wm", T("Текст, например © Иван Петров", "Text, e.g. © Jane Doe"), e.wmText, sizeof(e.wmText))) changed = true;
            ImGui::Spacing();
            ImGui::AlignTextToFramePadding();
            ImGui::TextDisabled("%s", T("Положение", "Position"));
            ImGui::SameLine(ImGui::GetContentRegionAvail().x * 0.42f);
            ImGui::BeginGroup();
            float cs = ImGui::GetFrameHeight() * 0.9f;
            for (int i = 0; i < 9; i++) {
                if (i % 3) ImGui::SameLine(0, 3);
                ImGui::PushID(i);
                if (W::Button(e.wmPos == i ? "●" : "", ImVec2(cs * 1.4f, cs), W::BTN_DEFAULT, e.wmPos == i)) { e.wmPos = i; changed = true; }
                ImGui::PopID();
            }
            ImGui::EndGroup();
            ImGui::Spacing();
            changed |= W::Slider(T("Размер", "Size"), &e.wmSize, 1, 12, 3, "%.1f%%");
            changed |= W::Slider(T("Непрозрачность", "Opacity"), &e.wmOpacity, 5, 100, 70);
            const char* wc[] = { T("Белый", "White"), T("Чёрный", "Black") };
            int c = e.wmDark ? 1 : 0;
            if (W::Segmented("wmc", &c, wc, 2)) { e.wmDark = c == 1; changed = true; }
        }
    }
    // file
    if (W::Section(T("Куда и как сохранить", "Destination & naming"), true)) {
        const char* dm[] = { T("Спрашивать каждый раз", "Ask every time"), T("Рядом с оригиналом", "Next to the original"), T("В выбранную папку", "To a chosen folder") };
        changed |= W::Combo(T("Папка", "Folder"), &e.dest, dm, 3);
        if (e.dest == 2) {
            std::string f = e.folder.empty() ? T("Папка не выбрана", "No folder chosen") : WideToUtf8(e.folder);
            if (W::Button(T("Выбрать папку…", "Choose folder…"), ImVec2(-1, 0))) { auto d = PickFolderDialog(a.hwnd); if (!d.empty()) { e.folder = d; changed = true; } }
            W::Caption(f.c_str());
        }
        ImGui::AlignTextToFramePadding();
        ImGui::TextDisabled("%s", T("Имя файла", "File name"));
        ImGui::SameLine(ImGui::GetContentRegionAvail().x * 0.42f);
        ImGui::SetNextItemWidth(-1);
        if (ImGui::InputText("##tpl", e.nameTpl, sizeof(e.nameTpl))) changed = true;
        W::Caption(T("Можно подставить: {name} имя оригинала, {n} номер, {date} дату, {w} и {h} размеры.",
                     "Tokens: {name} original name, {n} number, {date} date, {w} and {h} size."));
        if (!a.curPath.empty()) {
            int W_, H_; ExportTargetSize(e, W_, H_);
            std::string ex = std::string(T("Пример: ", "Example: ")) + WideToUtf8(ExpandName(a.curPath, 1, W_, H_) + ExtFor(e.fmt));
            W::Caption(ex.c_str());
        }
        changed |= W::Toggle(T("Перезаписывать существующие файлы", "Overwrite existing files"), &e.overwrite);
        changed |= W::Toggle(T("Показать файл после экспорта", "Show the file after export"), &e.reveal);
    }
    ImGui::EndChild();

    // footer
    ImGui::Separator();
    ImGui::Spacing();
    float bw = (ImGui::GetContentRegionAvail().x - ImGui::GetStyle().ItemSpacing.x * 2) / 3;
    float bh = ImGui::GetFrameHeight() * 1.4f;
    int nT = (int)a.exportTargets.size();
    if (W::Button(T("Отмена", "Cancel"), ImVec2(bw, bh))) ImGui::CloseCurrentPopup();
    ImGui::SameLine();
    char lb[96];
    if (nT > 1) {
        if (W::Button(T("Только открытое", "Just the open one"), ImVec2(bw, bh))) { ImGui::CloseCurrentPopup(); SaveSettings(); ExportCurrent(0); }
        ImGui::SameLine();
        snprintf(lb, sizeof(lb), T("Экспортировать %d фото", "Export %d photos"), nT);
        if (W::Button(lb, ImVec2(bw, bh), W::BTN_PRIMARY)) { ImGui::CloseCurrentPopup(); SaveSettings(); StartBatch(a.exportTargets); }
    } else {
        ImGui::BeginDisabled(a.collection.size() < 2);
        snprintf(lb, sizeof(lb), T("Вся лента (%d)", "Whole filmstrip (%d)"), (int)a.collection.size());
        if (W::Button(lb, ImVec2(bw, bh))) { ImGui::CloseCurrentPopup(); SaveSettings(); StartBatch(a.collection); }
        ImGui::EndDisabled();
        W::Hint(T("Экспортировать все фото рабочей ленты с их сохранёнными правками", "Export every photo in the filmstrip with its saved edits"));
        ImGui::SameLine();
        if (W::Button(T("Экспортировать", "Export"), ImVec2(bw, bh), W::BTN_PRIMARY)) { ImGui::CloseCurrentPopup(); SaveSettings(); ExportCurrent(0); }
    }
    if (changed) SaveSettings();
    ImGui::EndPopup();
}

static void HelpWindow() {
    App& a = g_app;
    if (!a.showHelp) return;
    ImGui::SetNextWindowSize(ImVec2(540 * a.dpi, 0), ImGuiCond_Always);
    ImGui::SetNextWindowPos(ImGui::GetMainViewport()->GetCenter(), ImGuiCond_Always, ImVec2(0.5f, 0.5f));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(20 * a.dpi, 16 * a.dpi));
    if (ImGui::Begin(T("Горячие клавиши###help", "Keyboard shortcuts###help"), &a.showHelp, ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoResize)) {
        struct Row { const char* kru; const char* ken; const char* ru; const char* en; };
        static const Row rows[] = {
            { "Ctrl+O", "Ctrl+O", "Открыть фото", "Open a photo" }, { "Ctrl+E", "Ctrl+E", "Экспорт", "Export" },
            { "Ctrl+Z / Ctrl+Y", "Ctrl+Z / Ctrl+Y", "Отменить / повторить", "Undo / redo" },
            { "Ctrl+Shift+C / V", "Ctrl+Shift+C / V", "Копировать / вставить настройки", "Copy / paste settings" },
            { "← →", "← →", "Предыдущее / следующее фото в папке", "Previous / next photo in the folder" },
            { "\\", "\\", "До / после", "Before / after" }, { "Z", "Z", "Вписать / 100%", "Fit / 100%" },
            { "Колесо мыши", "Mouse wheel", "Масштаб под курсором", "Zoom at the cursor" },
            { "Пробел + мышь", "Space + drag", "Перемещение по фото", "Pan the photo" },
            { "R / M / E", "R / M / E", "Кадр / маски / правка", "Crop / masks / edit" },
            { "G / D", "G / D", "Библиотека / классика", "Library / classic" },
            { "Ctrl+A", "Ctrl+A", "Выбрать все фото", "Select all photos" },
            { "Ctrl / Shift + клик", "Ctrl / Shift + click", "Выбрать несколько фото", "Select several photos" },
            { "Правый клик по ленте", "Right-click filmstrip", "Копировать, вставить, экспорт…", "Copy, paste, export…" },
            { "Alt + ползунок", "Alt + slider", "Показать пересветы и провалы", "Show clipped areas" },
            { "Enter / Esc", "Enter / Esc", "Применить кадр / отменить пипетку", "Apply crop / cancel picker" },
            { "O", "O", "Показать маску", "Show mask overlay" }, { "J", "J", "Пересветы и провалы", "Clipping warning" },
            { "[ ]", "[ ]", "Размер кисти", "Brush size" }, { "Alt + кисть", "Alt + brush", "Стирание", "Erase" },
            { "Delete", "Delete", "Удалить выбранную маску", "Delete the selected mask" },
            { "Tab", "Tab", "Показать / скрыть ленту", "Show / hide filmstrip" },
            { "Двойной клик", "Double-click", "Сбросить значение ползунка", "Reset a slider" },
            { "Ctrl + клик", "Ctrl + click", "Ввести число", "Type a value" },
            { "Shift + тянуть", "Shift + drag", "Точная настройка", "Fine adjustment" },
        };
        if (ImGui::BeginTable("keys", 2)) {
            ImGui::TableSetupColumn("k", ImGuiTableColumnFlags_WidthFixed, 170 * a.dpi);
            for (auto& r : rows) {
                ImGui::TableNextRow();
                ImGui::TableNextColumn(); ImGui::TextColored(ImVec4(0.43f, 0.66f, 1.0f, 1), "%s", g_lang == 1 ? r.ken : r.kru);
                ImGui::TableNextColumn(); ImGui::TextUnformatted(g_lang == 1 ? r.en : r.ru);
            }
            ImGui::EndTable();
        }
        ImGui::Spacing();
        W::Caption("Muon " MUON_VERSION_STR);
        W::Caption(T("Правки сохраняются автоматически рядом с фото в файле .muon, исходный файл не меняется.",
                     "Edits are saved automatically next to the photo in a .muon file; the original is never changed."));
    }
    ImGui::End();
    ImGui::PopStyleVar();
}

static void Shortcuts() {
    App& a = g_app;
    ImGuiIO& io = ImGui::GetIO();
    if (io.WantTextInput) return;
    bool ctrl = io.KeyCtrl, shift = io.KeyShift;
    if (ctrl && ImGui::IsKeyPressed(ImGuiKey_O, false)) { auto f = OpenFilesDialog(a.hwnd); if (!f.empty()) AppDropFiles(f); }
    if (ImGui::IsKeyPressed(ImGuiKey_F1, false)) a.showHelp = !a.showHelp;
    if (ctrl && ImGui::IsKeyPressed(ImGuiKey_A, false)) SelectAllInView();
    if (!ctrl && ImGui::IsKeyPressed(ImGuiKey_G, false)) a.bottomTab = TAB_LIBRARY;
    if (!ctrl && ImGui::IsKeyPressed(ImGuiKey_D, false)) { a.bottomTab = TAB_CLASSIC; a.tool = TOOL_EDIT; }
    if (a.bottomTab == TAB_LIBRARY) return;
    if (ctrl && shift && ImGui::IsKeyPressed(ImGuiKey_C, false) && !a.curPath.empty()) CopySettingsFrom(a.curPath, SG_DEVELOP);
    if (ctrl && shift && ImGui::IsKeyPressed(ImGuiKey_V, false)) PasteSettingsTo(CollectionSelection());
    if (!g_pipe.hasSource()) return;
    if (ctrl && ImGui::IsKeyPressed(ImGuiKey_E, false)) OpenExport(CollectionSelection());
    if (ctrl && !shift && ImGui::IsKeyPressed(ImGuiKey_Z)) Undo();
    if (ctrl && (ImGui::IsKeyPressed(ImGuiKey_Y) || (shift && ImGui::IsKeyPressed(ImGuiKey_Z)))) Redo();
    if (ctrl) return;
    if (ImGui::IsKeyPressed(ImGuiKey_Backslash, false)) a.before = !a.before;
    if (ImGui::IsKeyPressed(ImGuiKey_Z, false)) { if (a.fit) { a.fit = false; a.zoom = 1; a.panX = a.panY = 0; } else a.fit = true; }
    if (ImGui::IsKeyPressed(ImGuiKey_J, false)) { a.clipHi = !a.clipHi; a.clipLo = a.clipHi; }
    if (ImGui::IsKeyPressed(ImGuiKey_O, false)) a.showOverlay = !a.showOverlay;
    if (ImGui::IsKeyPressed(ImGuiKey_Tab, false)) a.showFilmstrip = !a.showFilmstrip;
    if (a.bottomTab == TAB_CLASSIC) {
        if (ImGui::IsKeyPressed(ImGuiKey_R, false)) { a.tool = a.tool == TOOL_CROP ? TOOL_EDIT : TOOL_CROP; a.pick = PICK_NONE; }
        if (ImGui::IsKeyPressed(ImGuiKey_M, false)) { a.tool = a.tool == TOOL_MASK ? TOOL_EDIT : TOOL_MASK; a.pick = PICK_NONE; }
        if (ImGui::IsKeyPressed(ImGuiKey_E, false)) a.tool = TOOL_EDIT;
    }
    if (ImGui::IsKeyPressed(ImGuiKey_Escape, false)) { if (a.pick) a.pick = PICK_NONE; else if (a.tool == TOOL_CROP) a.tool = TOOL_EDIT; }
    if ((ImGui::IsKeyPressed(ImGuiKey_Enter, false) || ImGui::IsKeyPressed(ImGuiKey_KeypadEnter, false)) && a.tool == TOOL_CROP) a.tool = TOOL_EDIT;
    if (ImGui::IsKeyPressed(ImGuiKey_LeftBracket)) a.brushSize = std::max(0.003f, a.brushSize * 0.88f);
    if (ImGui::IsKeyPressed(ImGuiKey_RightBracket)) a.brushSize = std::min(0.4f, a.brushSize * 1.12f);
    if (ImGui::IsKeyPressed(ImGuiKey_Delete, false) && a.tool == TOOL_MASK && a.selMask >= 0 && a.selMask < (int)a.params.masks.size()) {
        a.params.masks.erase(a.params.masks.begin() + a.selMask);
        a.selMask = std::min(a.selMask, (int)a.params.masks.size() - 1);
        Changed();
    }
    if (!a.collection.empty() && a.curIndex >= 0 && !a.loading) {
        int next = -1;
        if (ImGui::IsKeyPressed(ImGuiKey_RightArrow) && a.curIndex + 1 < (int)a.collection.size()) next = a.curIndex + 1;
        if (ImGui::IsKeyPressed(ImGuiKey_LeftArrow) && a.curIndex > 0) next = a.curIndex - 1;
        if (next >= 0) { CollectionSelectOnly(next); AppOpenFile(a.collection[next]); }
    }
}

// =====================================================================  frame
void AppInit() {
    App& a = g_app;
    if (!a.noPersist) LoadSettings();
    ApplyLanguage();
    ScanPresets();
    ThumbsStart();
    if (!a.noPersist) CollectionLoad();
    if (!a.libFolder.empty() && IsDirectory(a.libFolder)) LibraryOpenFolder(a.libFolder);
    else a.libFolder.clear();
    a.bottomTab = a.collection.empty() ? TAB_LIBRARY : TAB_CLASSIC;
}

void AppEmergencySave() {
    if (g_app.rev != g_app.committedRev) Commit();
    SaveSidecar(true);
    SaveSettings();
    CollectionSave();
}

void AppShutdown() {
    App& a = g_app;
    SaveSidecar();
    SaveSettings();
    CollectionSave();
    if (a.loader.valid()) a.loader.wait();
    ThumbsStop();
    delete a.current; a.current = nullptr;
}

static const Preset* HoveredPreset() {
    App& a = g_app;
    int k = a.hoverPreset;
    if (k >= 2000 && k - 2000 < (int)a.userPresets.size()) return &a.userPresets[k - 2000];
    if (k >= 1000 && k - 1000 < (int)Builtins().size()) return &Builtins()[k - 1000];
    return nullptr;
}

void AppFrame() {
    App& a = g_app;
    ImGuiIO& io = ImGui::GetIO();
    static bool first = true;
    if (first) {
        first = false;
        // reopen the last photo when nothing was passed on the command line
        if (!a.loading && !g_pipe.hasSource() && !a.noPersist && !s_lastPath.empty() && CollectionIndex(s_lastPath) >= 0 &&
            GetFileAttributesW(s_lastPath.c_str()) != INVALID_FILE_ATTRIBUTES) { AppOpenFile(s_lastPath); a.bottomTab = TAB_CLASSIC; }
        else if (!a.loading && !g_pipe.hasSource() && !a.noPersist && !a.collection.empty()) { AppOpenFile(a.collection[0]); a.bottomTab = TAB_CLASSIC; }
    }
    PollLoader();
    ThumbsPoll();
    Shortcuts();
    if (g_gpu.outOfMemory) {
        g_gpu.outOfMemory = false;
        SetStatus(T("Не хватает видеопамяти: уменьшите масштаб или закройте другие программы", "Out of video memory: zoom out or close other programs"));
        a.statusTime = Now() + 4;
    }
    a.altView = s_forceAlt;
    a.hoverPreset = -1;

    ImGuiViewport* vp = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(vp->Pos);
    ImGui::SetNextWindowSize(vp->Size);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0);
    ImGui::Begin("##main", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoBringToFrontOnFocus |
                                        ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoScrollWithMouse);
    ImGui::PopStyleVar(2);
    float W_ = vp->Size.x, H_ = vp->Size.y;
    float topH = 46 * a.dpi, tabH = 46 * a.dpi;
    bool lib = a.bottomTab == TAB_LIBRARY;
    float filmH = (!lib && a.showFilmstrip && a.collection.size() > 1) ? 96 * a.dpi : 0;
    float panelW = std::min(380 * a.dpi, W_ * 0.45f);
    ImVec2 o = vp->Pos;
    TopBar(o, ImVec2(W_, topH));
    float midH = H_ - topH - tabH - filmH;
    if (lib) LibraryGrid(ImVec2(o.x, o.y + topH), ImVec2(W_ - panelW, midH));
    else Canvas(ImVec2(o.x, o.y + topH), ImVec2(W_ - panelW, midH));
    RightPanel(ImVec2(o.x + W_ - panelW, o.y + topH), ImVec2(panelW, midH));
    if (filmH > 0) Filmstrip(ImVec2(o.x, o.y + topH + midH), ImVec2(W_, filmH));
    BottomTabs(ImVec2(o.x, o.y + H_ - tabH), ImVec2(W_, tabH));
    ExportModal();
    NewPresetModal();
    CopyDialogModal();
    ImGui::End();
    HelpWindow();

    // commit an undo step once the interaction has finished
    if (a.rev != a.committedRev && !ImGui::IsAnyItemActive() && !io.MouseDown[0] && !a.painting) { Commit(); a.lastCommitTime = Now(); }
    if (a.committedRev != a.savedRev && Now() - a.lastSave > 1.5) SaveSidecar();

    // refresh the filmstrip thumbnail of the open photo once edits settle
    if (g_pipe.hasSource() && a.thumbRev != a.committedRev && Now() - a.lastCommitTime > 0.6 && !io.MouseDown[0] && !a.loading && !a.batchActive) {
        a.thumbRev = a.committedRev;
        Geo g = g_pipe.geometry(a.params, false);
        float k = 256.0f / std::max(g.fullW, g.fullH);
        std::vector<uint16_t> px; int w, h;
        if (g_pipe.renderExport(a.params, std::max(1, (int)(g.fullW * k)), std::max(1, (int)(g.fullH * k)), 0, px, w, h)) {
            std::vector<uint8_t> rgba(px.size());
            for (size_t i = 0; i < px.size(); i++) rgba[i] = (uint8_t)(px[i] >> 8);
            ThumbReplace(a.curPath, rgba, w, h);
        }
        a.needRender = true;
    }

    // re-render the preview when anything relevant changed
    if (g_pipe.hasSource() && !lib) {
        static uint64_t lastKey = 0;
        bool cropMode = a.tool == TOOL_CROP && a.bottomTab == TAB_CLASSIC;
        int overlay = -1;
        if (a.tool == TOOL_MASK && a.bottomTab == TAB_CLASSIC && (a.showOverlay || a.painting || s_drag >= 0)) overlay = a.selMask;
        const Preset* hp = HoveredPreset();
        uint64_t key = (uint64_t)a.rev * 1000003ull ^ ((uint64_t)a.workLong << 40) ^ ((uint64_t)cropMode << 1) ^ ((uint64_t)a.before << 2) ^
                       ((uint64_t)(overlay + 1) << 3) ^ ((uint64_t)a.clipHi << 12) ^ ((uint64_t)a.altView << 14) ^
                       ((uint64_t)(a.hoverPreset + 1) << 52) ^ ((uint64_t)(uintptr_t)a.current << 20);
        if (key != lastKey || a.needRender) {
            RenderOpts ro;
            ro.before = a.before; ro.cropOverride = cropMode; ro.overlayMask = overlay; ro.clipWarn = a.clipHi; ro.altView = a.altView;
            if (hp) {
                Params tmp = a.params;
                CopyGroups(tmp, hp->params, hp->groups);
                g_pipe.renderDisplay(tmp, std::max(256, a.workLong), ro);
            } else g_pipe.renderDisplay(a.params, std::max(256, a.workLong), ro);
            lastKey = key;
            a.needRender = false;
        }
    }
}

// Puts the UI into a known state for layout screenshots (--uitest).
void UiTestSetup(int scenario) {
    App& a = g_app;
    if (scenario >= 10) { g_lang = 1; ApplyLanguage(); scenario -= 10; }
    switch (scenario) {
    case 1: a.tool = TOOL_CROP; break;
    case 2: a.tool = TOOL_MASK; AddMask(MS_RADIAL, MR_NONE); a.params.masks.back().adj.exposure = 0.8f; AddMask(MS_BRUSH, MR_NONE); a.selMask = 0; break;
    case 3: {
        a.bottomTab = TAB_EXPERIMENTS;
        Effect glow = MakeEffect(FX_HALATION); glow.p[0] = 55; glow.p[2] = 140; glow.expanded = false; a.params.effects.push_back(glow);
        Effect grain = MakeEffect(FX_GRAINPRO); grain.p[0] = 30; grain.expanded = false; a.params.effects.push_back(grain);
        Effect ascii = MakeEffect(FX_ASCII); ascii.target = TG_LUMA; ascii.tLo = 0; ascii.tHi = 0.32f; ascii.p[0] = 9; a.params.effects.push_back(ascii);
        Changed();
        break;
    }
    case 4: a.exportOpen = true; a.exp.watermark = true; strcpy(a.exp.wmText, "© Muon"); break;
    case 5: a.showHelp = true; break;
    case 8: a.params.exposure = 1.3f; s_forceAlt = ALT_HIGHLIGHTS; Changed(); break;
    case 9: a.params.exposure = -1.5f; s_forceAlt = ALT_SHADOWS; Changed(); break;
    case 6: {
        wchar_t f[MAX_PATH]; wcsncpy(f, a.curPath.c_str(), MAX_PATH - 1); f[MAX_PATH - 1] = 0; PathRemoveFileSpecW(f);
        LibraryOpenFolder(f);
        for (size_t i = 0; i < a.libSel.size() && i < 3; i++) a.libSel[i] = i != 1;
        a.bottomTab = TAB_LIBRARY;
        break;
    }
    case 7: {
        wchar_t f[MAX_PATH]; wcsncpy(f, a.curPath.c_str(), MAX_PATH - 1); f[MAX_PATH - 1] = 0; PathRemoveFileSpecW(f);
        CollectionAdd(ListFolderImages(f));
        a.curIndex = CollectionIndex(a.curPath);
        for (size_t i = 0; i < a.collSel.size(); i++) a.collSel[i] = (i % 2) == 0;
        a.hoverPreset = 1001;
        break;
    }
    }
}
