#include "preset.h"
#include "i18n.h"
#include <windows.h>
#include <sstream>
#include <cstring>
#include <ctime>

const char* GroupName(int bit) {
    switch (1u << bit) {
    case SG_WB: return T("Баланс белого", "White balance");
    case SG_EXPOSURE: return T("Экспозиция", "Exposure");
    case SG_TONE: return T("Тон (контраст, света, тени…)", "Tone (contrast, highlights…)");
    case SG_PRESENCE: return T("Чёткость, дымка, насыщенность", "Presence (clarity, saturation…)");
    case SG_CURVE: return T("Тоновая кривая", "Tone curve");
    case SG_HSL: return T("Цвет (HSL)", "Colour (HSL)");
    case SG_GRADE: return T("Цветокоррекция", "Colour grading");
    case SG_BW: return T("Чёрно-белое", "Black & white");
    case SG_DETAIL: return T("Резкость и шумоподавление", "Sharpening & noise");
    case SG_LENS: return T("Оптика", "Lens corrections");
    case SG_VIGGRAIN: return T("Виньетка и зерно", "Vignette & grain");
    case SG_EFFECTS: return T("Эксперименты", "Experiments");
    case SG_CROP: return T("Кадрирование и поворот", "Crop & rotation");
    case SG_MASKS: return T("Маски", "Masks");
    }
    return "";
}

void CopyGroups(Params& d, const Params& s, uint32_t g) {
    if (g & SG_WB) { d.temp = s.temp; d.tint = s.tint; }
    if (g & SG_EXPOSURE) d.exposure = s.exposure;
    if (g & SG_TONE) { d.contrast = s.contrast; d.highlights = s.highlights; d.shadows = s.shadows; d.whites = s.whites; d.blacks = s.blacks; }
    if (g & SG_PRESENCE) { d.texture = s.texture; d.clarity = s.clarity; d.dehaze = s.dehaze; d.vibrance = s.vibrance; d.saturation = s.saturation; d.profile = s.profile; }
    if (g & SG_CURVE) for (int i = 0; i < 4; i++) d.curve[i] = s.curve[i];
    if (g & SG_HSL) for (int i = 0; i < 8; i++) { d.hslH[i] = s.hslH[i]; d.hslS[i] = s.hslS[i]; d.hslL[i] = s.hslL[i]; }
    if (g & SG_GRADE) { for (int i = 0; i < 4; i++) d.grade[i] = s.grade[i]; d.gradBlend = s.gradBlend; d.gradBalance = s.gradBalance; }
    if (g & SG_BW) { d.bw = s.bw; for (int i = 0; i < 8; i++) d.bwMix[i] = s.bwMix[i]; }
    if (g & SG_DETAIL) {
#define C(n, l, mi, ma, df) d.n = s.n;
        DETAIL_PARAMS(C)
#undef C
    }
    if (g & SG_LENS) { d.lensDist = s.lensDist; d.lensCA = s.lensCA; d.lensVig = s.lensVig; }
    if (g & SG_VIGGRAIN) {
        d.vigAmount = s.vigAmount; d.vigMid = s.vigMid; d.vigRound = s.vigRound; d.vigFeather = s.vigFeather; d.vigHigh = s.vigHigh;
        d.grainAmount = s.grainAmount; d.grainSize = s.grainSize; d.grainRough = s.grainRough;
    }
    if (g & SG_EFFECTS) { d.effects = s.effects; for (auto& e : d.effects) e.id = NewId(); }
    if (g & SG_CROP) {
        d.rot90 = s.rot90; d.flipH = s.flipH; d.flipV = s.flipV;
        d.cropX0 = s.cropX0; d.cropY0 = s.cropY0; d.cropX1 = s.cropX1; d.cropY1 = s.cropY1; d.cropAspect = s.cropAspect;
        d.straighten = s.straighten; d.perspV = s.perspV; d.perspH = s.perspH; d.geoScale = s.geoScale;
    }
    if (g & SG_MASKS) { d.masks = s.masks; for (auto& m : d.masks) { m.id = NewId(); m.brushRev = NewRev(); } }
}

// ------------------------------------------------------------------ text format
static std::string Esc(const std::string& s) {
    std::string r; for (char c : s) { if (c == '\n') r += "\\n"; else if (c == '\r') {} else if (c == '\\') r += "\\\\"; else r += c; } return r;
}
static std::string Unesc(const std::string& s) {
    std::string r;
    for (size_t i = 0; i < s.size(); i++) {
        if (s[i] == '\\' && i + 1 < s.size()) { r += s[i + 1] == 'n' ? '\n' : s[i + 1]; i++; }
        else r += s[i];
    }
    return r;
}

static uint32_t Crc32(const std::string& s) {
    uint32_t c = 0xFFFFFFFFu;
    for (unsigned char ch : s) {
        c ^= ch;
        for (int k = 0; k < 8; k++) c = (c >> 1) ^ (0xEDB88320u & (0u - (c & 1)));
    }
    return ~c;
}

std::string PresetToText(const Preset& p) {
    bool geo = (p.groups & SG_CROP) != 0, masks = (p.groups & SG_MASKS) != 0;
    Params tmp;                                      // keep only the groups the preset carries
    CopyGroups(tmp, p.params, p.groups);
    std::string body = SerializeParams(tmp, geo, masks, true);
    char crc[16]; snprintf(crc, sizeof(crc), "%08x", Crc32(body));
    char grp[16]; snprintf(grp, sizeof(grp), "%x", p.groups);
    time_t t = time(nullptr); tm lt; localtime_s(&lt, &t);
    char date[32]; strftime(date, sizeof(date), "%Y-%m-%d", &lt);
    std::string s = "MUONPRESET 2\n";
    s += "name: " + Esc(p.name) + "\n";
    if (!p.author.empty()) s += "author: " + Esc(p.author) + "\n";
    if (!p.description.empty()) s += "desc: " + Esc(p.description) + "\n";
    s += std::string("groups: ") + grp + "\n";
    s += std::string("created: ") + date + "\n";
    s += std::string("crc: ") + crc + "\n";
    s += "---\n";
    s += body;
    return s;
}

bool PresetFromText(const std::string& textIn, Preset& out, std::string* err) {
    std::string text = textIn;
    if (text.size() >= 3 && (unsigned char)text[0] == 0xEF) text = text.substr(3);   // UTF-8 BOM
    std::string norm;
    for (char c : text) if (c != '\r') norm += c;
    Preset p;
    if (norm.rfind("muon 1", 0) == 0) {
        // first-generation preset: plain parameter block
        p.groups = SG_DEVELOP;
        if (!DeserializeParams(norm, p.params, false, false)) { if (err) *err = T("Файл повреждён", "The file is damaged"); return false; }
        out = p;
        return true;
    }
    if (norm.rfind("MUONPRESET", 0) != 0) { if (err) *err = T("Это не пресет Muon", "This is not a Muon preset"); return false; }
    size_t sep = norm.find("\n---\n");
    if (sep == std::string::npos) { if (err) *err = T("Пресет повреждён", "The preset is damaged"); return false; }
    std::string head = norm.substr(0, sep), body = norm.substr(sep + 5);
    std::istringstream hs(head);
    std::string line, crc;
    std::getline(hs, line);
    while (std::getline(hs, line)) {
        size_t c = line.find(": ");
        if (c == std::string::npos) continue;
        std::string k = line.substr(0, c), v = line.substr(c + 2);
        if (k == "name") p.name = Unesc(v);
        else if (k == "author") p.author = Unesc(v);
        else if (k == "desc") p.description = Unesc(v);
        else if (k == "groups") p.groups = (uint32_t)strtoul(v.c_str(), nullptr, 16) & SG_ALL;
        else if (k == "crc") crc = v;
    }
    if (!crc.empty()) {
        char mine[16]; snprintf(mine, sizeof(mine), "%08x", Crc32(body));
        if (crc != mine) { if (err) *err = T("Пресет повреждён или обрезан", "The preset is damaged or truncated"); return false; }
    }
    if (!DeserializeParams(body, p.params, true, true)) { if (err) *err = T("Пресет повреждён", "The preset is damaged"); return false; }
    if (p.name.empty()) p.name = T("Без названия", "Untitled");
    out = p;
    return true;
}

// ------------------------------------------------------------------ share codes (base64url)
static const char* kB64 = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";

static std::string B64Encode(const std::string& in) {
    std::string out;
    unsigned val = 0; int bits = -6;
    for (unsigned char c : in) {
        val = (val << 8) + c; bits += 8;
        while (bits >= 0) { out += kB64[(val >> bits) & 63]; bits -= 6; }
    }
    if (bits > -6) out += kB64[((val << 8) >> (bits + 8)) & 63];
    return out;
}

static std::string B64Decode(const std::string& in) {
    int T_[256]; for (int i = 0; i < 256; i++) T_[i] = -1;
    for (int i = 0; i < 64; i++) T_[(unsigned char)kB64[i]] = i;
    T_['+'] = 62; T_['/'] = 63;
    std::string out;
    unsigned val = 0; int bits = -8;
    for (unsigned char c : in) {
        if (T_[c] < 0) continue;
        val = (val << 6) + T_[c]; bits += 6;
        if (bits >= 0) { out += char((val >> bits) & 0xFF); bits -= 8; }
    }
    return out;
}

std::string PresetToCode(const Preset& p) { return "MUON1-" + B64Encode(PresetToText(p)); }

bool LooksLikePresetCode(const std::string& s) {
    size_t i = s.find_first_not_of(" \t\r\n");
    return i != std::string::npos && s.compare(i, 6, "MUON1-") == 0;
}

bool PresetFromCode(const std::string& code, Preset& out, std::string* err) {
    size_t i = code.find("MUON1-");
    if (i == std::string::npos) { if (err) *err = T("В буфере обмена нет кода пресета", "No preset code in the clipboard"); return false; }
    // Messengers wrap long codes and people add text after them, so collect the
    // whitespace-separated pieces and take the longest prefix whose checksum matches.
    std::vector<std::string> parts(1);
    for (size_t k = i + 6; k < code.size(); k++) {
        char c = code[k];
        if (isalnum((unsigned char)c) || c == '-' || c == '_' || c == '+' || c == '/') parts.back() += c;
        else if (c == ' ' || c == '\r' || c == '\n' || c == '\t') { if (!parts.back().empty()) parts.emplace_back(); }
        else break;
    }
    if (parts.back().empty()) parts.pop_back();
    std::string lastErr;
    for (size_t n = parts.size(); n >= 1; n--) {
        std::string b;
        for (size_t k = 0; k < n; k++) b += parts[k];
        std::string e;
        if (PresetFromText(B64Decode(b), out, &e)) return true;
        lastErr = e;
    }
    if (err) *err = lastErr.empty() ? T("Код пресета повреждён", "The preset code is damaged") : lastErr;
    return false;
}

// ------------------------------------------------------------------ clipboard
bool ClipboardSetText(const std::string& utf8) {
    int n = MultiByteToWideChar(CP_UTF8, 0, utf8.c_str(), -1, nullptr, 0);
    if (n <= 0 || !OpenClipboard(nullptr)) return false;
    EmptyClipboard();
    HGLOBAL h = GlobalAlloc(GMEM_MOVEABLE, n * sizeof(wchar_t));
    if (h) {
        wchar_t* w = (wchar_t*)GlobalLock(h);
        MultiByteToWideChar(CP_UTF8, 0, utf8.c_str(), -1, w, n);
        GlobalUnlock(h);
        SetClipboardData(CF_UNICODETEXT, h);
    }
    CloseClipboard();
    return h != nullptr;
}

std::string ClipboardGetText() {
    std::string r;
    if (!OpenClipboard(nullptr)) return r;
    HANDLE h = GetClipboardData(CF_UNICODETEXT);
    if (h) {
        const wchar_t* w = (const wchar_t*)GlobalLock(h);
        if (w) {
            int n = WideCharToMultiByte(CP_UTF8, 0, w, -1, nullptr, 0, nullptr, nullptr);
            if (n > 1) { r.resize(n - 1); WideCharToMultiByte(CP_UTF8, 0, w, -1, &r[0], n, nullptr, nullptr); }
            GlobalUnlock(h);
        }
    }
    CloseClipboard();
    return r;
}

// ------------------------------------------------------------------ built-ins
std::vector<Preset> BuiltinPresetList() {
    std::vector<Preset> v;
    for (auto& b : BuiltinPresets()) {
        Preset p;
        p.name = g_lang == 1 ? b.nameEn : b.name;
        p.author = "Muon";
        p.builtin = true;
        b.apply(p.params);
        p.groups = SG_LOOK;
        if (p.params.temp != 0 || p.params.tint != 0) p.groups |= SG_WB;
        v.push_back(p);
    }
    return v;
}
