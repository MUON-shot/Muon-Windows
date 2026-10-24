#include "params.h"
#include "i18n.h"
#include <sstream>
#include <algorithm>
#include <cmath>
#include <atomic>
#include <cstring>

static std::atomic<uint32_t> g_id{ 1 };
static std::atomic<uint32_t> g_rev{ 1 };
uint32_t NewId() { return g_id++; }
uint32_t NewRev() { return g_rev++; }

bool Curve::isIdentity() const {
    for (auto& p : pts) if (std::fabs(p.x - p.y) > 1e-4f) return false;
    return true;
}

// Monotone cubic (Fritsch-Carlson) spline through the curve points.
void BuildCurveLut(const Curve& c, float* lut) {
    std::vector<CurvePt> p = c.pts;
    std::sort(p.begin(), p.end(), [](const CurvePt& a, const CurvePt& b) { return a.x < b.x; });
    int n = (int)p.size();
    if (n < 2) { for (int i = 0; i < 256; i++) lut[i] = i / 255.0f; return; }
    std::vector<float> d(n - 1), m(n);
    for (int i = 0; i < n - 1; i++) {
        float dx = std::max(1e-5f, p[i + 1].x - p[i].x);
        d[i] = (p[i + 1].y - p[i].y) / dx;
    }
    m[0] = d[0]; m[n - 1] = d[n - 2];
    for (int i = 1; i < n - 1; i++) m[i] = (d[i - 1] * d[i] <= 0) ? 0 : (d[i - 1] + d[i]) * 0.5f;
    for (int i = 0; i < n - 1; i++) {
        if (std::fabs(d[i]) < 1e-6f) { m[i] = m[i + 1] = 0; continue; }
        float a = m[i] / d[i], b = m[i + 1] / d[i];
        float s = a * a + b * b;
        if (s > 9) { float t = 3 / std::sqrt(s); m[i] = t * a * d[i]; m[i + 1] = t * b * d[i]; }
    }
    for (int k = 0; k < 256; k++) {
        float x = k / 255.0f, y;
        if (x <= p[0].x) y = p[0].y;
        else if (x >= p[n - 1].x) y = p[n - 1].y;
        else {
            int i = 0;
            while (i < n - 2 && x > p[i + 1].x) i++;
            float h = std::max(1e-5f, p[i + 1].x - p[i].x);
            float t = (x - p[i].x) / h, t2 = t * t, t3 = t2 * t;
            y = (2 * t3 - 3 * t2 + 1) * p[i].y + (t3 - 2 * t2 + t) * h * m[i] + (-2 * t3 + 3 * t2) * p[i + 1].y + (t3 - t2) * h * m[i + 1];
        }
        lut[k] = std::clamp(y, 0.0f, 1.0f);
    }
}

Mask MakeMask(int shape, int range) {
    Mask m;
    m.id = NewId();
    m.shape = shape;
    m.range = range;
    m.brushRev = NewRev();
    const char* names[] = { T("Всё фото", "Whole photo"), T("Линейный градиент", "Linear gradient"),
                            T("Радиальный градиент", "Radial gradient"), T("Кисть", "Brush") };
    m.name = names[shape];
    if (range == MR_LUMA) m.name = T("Диапазон яркости", "Luminance range");
    if (range == MR_COLOR) m.name = T("Цветовой диапазон", "Colour range");
    return m;
}

// ---------------------------------------------------------------- serialization
static void WriteArr(std::ostringstream& o, const char* key, const float* a, int n) {
    o << key; for (int i = 0; i < n; i++) o << ' ' << a[i]; o << '\n';
}
static void ReadArr(std::istringstream& is, float* a, int n) { for (int i = 0; i < n; i++) is >> a[i]; }
static std::string RestOfLine(std::istringstream& is) {
    std::string s; std::getline(is, s);
    if (!s.empty() && s[0] == ' ') s.erase(0, 1);
    return s;
}
static std::string Escape(const std::string& s) {
    std::string r; for (char c : s) { if (c == '\n') r += "\\n"; else if (c == '\\') r += "\\\\"; else r += c; } return r;
}
static std::string Unescape(const std::string& s) {
    std::string r;
    for (size_t i = 0; i < s.size(); i++) {
        if (s[i] == '\\' && i + 1 < s.size()) { r += s[i + 1] == 'n' ? '\n' : s[i + 1]; i++; }
        else r += s[i];
    }
    return r;
}

static bool AnyNonZero(const float* a, int n) { for (int i = 0; i < n; i++) if (a[i] != 0) return true; return false; }

std::string SerializeParams(const Params& p, bool geo, bool masks, bool diffOnly) {
    std::ostringstream o;
    o.precision(6);
    o << "muon 1\n";
    const Params def;
    // diffOnly writes just what differs from the defaults (compact presets and share codes)
#define W(n, l, mi, ma, d) if (!diffOnly || p.n != def.n) o << #n << ' ' << p.n << '\n';
    BASIC_PARAMS(W) DETAIL_PARAMS(W) EFFECT_PARAMS(W)
    if (geo) { OPTICS_PARAMS(W) }
    else {
        if (!diffOnly || p.lensDist != 0) o << "lensDist " << p.lensDist << '\n';
        if (!diffOnly || p.lensCA != 0) o << "lensCA " << p.lensCA << '\n';
        if (!diffOnly || p.lensVig != 0) o << "lensVig " << p.lensVig << '\n';
    }
#undef W
    if (!diffOnly || p.profile != 0) o << "profile " << p.profile << '\n';
    if (!diffOnly || p.bw) o << "bw " << (int)p.bw << '\n';
    if (!diffOnly || AnyNonZero(p.hslH, 8)) WriteArr(o, "hslH", p.hslH, 8);
    if (!diffOnly || AnyNonZero(p.hslS, 8)) WriteArr(o, "hslS", p.hslS, 8);
    if (!diffOnly || AnyNonZero(p.hslL, 8)) WriteArr(o, "hslL", p.hslL, 8);
    if (!diffOnly || AnyNonZero(p.bwMix, 8)) WriteArr(o, "bwMix", p.bwMix, 8);
    for (int i = 0; i < 4; i++)
        if (!diffOnly || p.grade[i].sat != 0 || p.grade[i].lum != 0 || p.grade[i].hue != 0)
            o << "grade" << i << ' ' << p.grade[i].hue << ' ' << p.grade[i].sat << ' ' << p.grade[i].lum << '\n';
    for (int i = 0; i < 4; i++) {
        if (diffOnly && p.curve[i].isIdentity() && p.curve[i].pts.size() == 2) continue;
        o << "curve" << i << ' ' << p.curve[i].pts.size();
        for (auto& pt : p.curve[i].pts) o << ' ' << pt.x << ' ' << pt.y;
        o << '\n';
    }
    if (geo) {
        o << "rot90 " << p.rot90 << "\nflip " << (int)p.flipH << ' ' << (int)p.flipV << '\n';
        o << "crop " << p.cropX0 << ' ' << p.cropY0 << ' ' << p.cropX1 << ' ' << p.cropY1 << ' ' << p.cropAspect << '\n';
    }
    if (masks) {
        for (auto& m : p.masks) {
            o << "mask {\n";
            o << "name " << Escape(m.name) << '\n';
            o << "flags " << (int)m.enabled << ' ' << (int)m.invert << ' ' << m.shape << ' ' << m.range << ' ' << m.amount << '\n';
            o << "linear " << m.lx0 << ' ' << m.ly0 << ' ' << m.lx1 << ' ' << m.ly1 << '\n';
            o << "radial " << m.cx << ' ' << m.cy << ' ' << m.rx << ' ' << m.ry << ' ' << m.angle << ' ' << m.feather << '\n';
            o << "lum " << m.lumLo << ' ' << m.lumHi << ' ' << m.lumFeather << '\n';
            o << "hue " << m.hueC << ' ' << m.hueWidth << ' ' << m.hueFeather << ' ' << m.satMin << '\n';
#define W(n, l, mi, ma, d) o << "adj." #n << ' ' << m.adj.n << '\n';
            LOCAL_PARAMS(W)
#undef W
            WriteArr(o, "adjcolor", m.adj.color, 3);
            o << "dabs " << m.dabs.size();
            for (auto& d : m.dabs) o << ' ' << d.x << ' ' << d.y << ' ' << d.r << ' ' << d.feather << ' ' << d.flow << ' ' << d.erase;
            o << "\n}\n";
        }
    }
    for (auto& e : p.effects) {
        o << "effect {\n";
        o << "type " << e.type << "\nflags " << (int)e.enabled << ' ' << (int)e.expanded << ' ' << e.opacity << ' ' << e.blend << ' ' << e.seed << '\n';
        WriteArr(o, "p", e.p, 12);
        WriteArr(o, "col", &e.col[0][0], 9);
        o << "target " << e.target << ' ' << (int)e.targetInvert << ' ' << e.tLo << ' ' << e.tHi << ' ' << e.tFeather << ' '
          << e.tHue << ' ' << e.tHueWidth << ' ' << e.tHueFeather << ' ' << e.tSatMin << ' ' << e.tMask << '\n';
        o << "text " << Escape(e.text) << '\n';
        o << "}\n";
    }
    return o.str();
}

bool DeserializeParams(const std::string& s, Params& p, bool geo, bool masks) {
    std::istringstream in(s);
    std::string line;
    if (!std::getline(in, line) || line.rfind("muon", 0) != 0) return false;
    Params def;
    // keep the parts we are told not to touch
    std::vector<Mask> keepMasks = p.masks;
    Params old = p;
    p = def;
    if (!geo) {
        p.rot90 = old.rot90; p.flipH = old.flipH; p.flipV = old.flipV;
        p.cropX0 = old.cropX0; p.cropY0 = old.cropY0; p.cropX1 = old.cropX1; p.cropY1 = old.cropY1; p.cropAspect = old.cropAspect;
        p.perspV = old.perspV; p.perspH = old.perspH; p.geoScale = old.geoScale; p.straighten = old.straighten;
    }
    if (!masks) p.masks = keepMasks;
    Mask* cm = nullptr;
    Effect* ce = nullptr;
    while (std::getline(in, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        std::istringstream is(line);
        std::string k; is >> k;
        if (k.empty()) continue;
        if (k == "}") { cm = nullptr; ce = nullptr; continue; }
        if (k == "mask") { if (masks) { p.masks.push_back(Mask()); cm = &p.masks.back(); cm->id = NewId(); cm->brushRev = NewRev(); } else cm = nullptr; ce = nullptr; continue; }
        if (k == "effect") { p.effects.push_back(Effect()); ce = &p.effects.back(); ce->id = NewId(); cm = nullptr; continue; }
        if (cm) {
            Mask& m = *cm;
            if (k == "name") m.name = Unescape(RestOfLine(is));
            else if (k == "flags") { int en, inv; is >> en >> inv >> m.shape >> m.range >> m.amount; m.enabled = en; m.invert = inv; }
            else if (k == "linear") is >> m.lx0 >> m.ly0 >> m.lx1 >> m.ly1;
            else if (k == "radial") is >> m.cx >> m.cy >> m.rx >> m.ry >> m.angle >> m.feather;
            else if (k == "lum") is >> m.lumLo >> m.lumHi >> m.lumFeather;
            else if (k == "hue") is >> m.hueC >> m.hueWidth >> m.hueFeather >> m.satMin;
            else if (k == "adjcolor") ReadArr(is, m.adj.color, 3);
            else if (k == "dabs") {
                size_t n = 0; is >> n; m.dabs.resize(n);
                for (auto& d : m.dabs) is >> d.x >> d.y >> d.r >> d.feather >> d.flow >> d.erase;
            }
#define R(n, l, mi, ma, d) else if (k == "adj." #n) is >> m.adj.n;
            LOCAL_PARAMS(R)
#undef R
            continue;
        }
        if (ce) {
            Effect& e = *ce;
            if (k == "type") is >> e.type;
            else if (k == "flags") { int en, ex; is >> en >> ex >> e.opacity >> e.blend >> e.seed; e.enabled = en; e.expanded = ex; }
            else if (k == "p") ReadArr(is, e.p, 12);
            else if (k == "col") ReadArr(is, &e.col[0][0], 9);
            else if (k == "target") { int inv; is >> e.target >> inv >> e.tLo >> e.tHi >> e.tFeather >> e.tHue >> e.tHueWidth >> e.tHueFeather >> e.tSatMin >> e.tMask; e.targetInvert = inv; }
            else if (k == "text") e.text = Unescape(RestOfLine(is));
            continue;
        }
        if (false) {}
#define R(n, l, mi, ma, d) else if (k == #n) { if (geo || (strcmp(#n, "perspV") && strcmp(#n, "perspH") && strcmp(#n, "geoScale") && strcmp(#n, "straighten"))) is >> p.n; }
        ALL_SCALAR_PARAMS(R)
#undef R
        else if (k == "profile") is >> p.profile;
        else if (k == "bw") { int b; is >> b; p.bw = b; }
        else if (k == "hslH") ReadArr(is, p.hslH, 8);
        else if (k == "hslS") ReadArr(is, p.hslS, 8);
        else if (k == "hslL") ReadArr(is, p.hslL, 8);
        else if (k == "bwMix") ReadArr(is, p.bwMix, 8);
        else if (k.rfind("grade", 0) == 0 && k.size() == 6) { int i = k[5] - '0'; if (i >= 0 && i < 4) is >> p.grade[i].hue >> p.grade[i].sat >> p.grade[i].lum; }
        else if (k.rfind("curve", 0) == 0 && k.size() == 6) {
            int i = k[5] - '0';
            if (i >= 0 && i < 4) {
                size_t n = 0; is >> n; p.curve[i].pts.resize(n);
                for (auto& pt : p.curve[i].pts) is >> pt.x >> pt.y;
                if (p.curve[i].pts.size() < 2) p.curve[i] = Curve();
            }
        }
        else if (k == "rot90" && geo) is >> p.rot90;
        else if (k == "flip" && geo) { int a, b; is >> a >> b; p.flipH = a; p.flipV = b; }
        else if (k == "crop" && geo) is >> p.cropX0 >> p.cropY0 >> p.cropX1 >> p.cropY1 >> p.cropAspect;
    }
    return true;
}

// ---------------------------------------------------------------- presets
static void Curve3(Curve& c, float a, float b, float x, float y) { c.pts = { {0, a}, {x, y}, {1, b} }; }

const std::vector<BuiltinPreset>& BuiltinPresets() {
    static std::vector<BuiltinPreset> v = {
        { "Яркий", "Vivid", [](Params& p) { p.profile = 2; p.vibrance = 30; p.clarity = 15; p.contrast = 15; } },
        { "Мягкая плёнка", "Soft film", [](Params& p) {
            p.contrast = -10; p.highlights = -30; p.shadows = 20; p.vibrance = -10;
            p.curve[0].pts = { {0, 0.08f}, {0.25f, 0.24f}, {0.75f, 0.8f}, {1, 0.94f} };
            p.grade[0] = { 200, 15, 0 }; p.grade[2] = { 40, 15, 0 }; p.grainAmount = 25; p.grainSize = 30;
        } },
        { "Бирюза и оранжевый", "Teal & orange", [](Params& p) {
            p.contrast = 20; p.vibrance = 15;
            p.hslH[1] = -10; p.hslS[1] = 15; p.hslH[3] = 60; p.hslS[3] = -40; p.hslH[4] = -20; p.hslH[5] = -20; p.hslS[5] = 20;
            p.grade[0] = { 195, 30, -5 }; p.grade[2] = { 35, 25, 0 };
        } },
        { "Матовый", "Matte", [](Params& p) {
            Curve3(p.curve[0], 0.12f, 0.92f, 0.5f, 0.5f); p.contrast = 10; p.saturation = -15; p.clarity = 10;
        } },
        { "Контрастное Ч/Б", "Punchy B&W", [](Params& p) {
            p.bw = true; p.contrast = 35; p.clarity = 25; p.blacks = -15; p.whites = 15;
            p.bwMix[0] = 20; p.bwMix[5] = -30; p.grainAmount = 15;
        } },
        { "Мягкое Ч/Б", "Soft B&W", [](Params& p) { p.bw = true; p.contrast = -10; p.highlights = -20; p.shadows = 25; } },
        { "Тёплый закат", "Warm sunset", [](Params& p) { p.temp = 25; p.tint = 8; p.vibrance = 20; p.highlights = -20; p.grade[2] = { 30, 20, 0 }; p.vigAmount = -20; } },
        { "Холодное утро", "Cold morning", [](Params& p) { p.temp = -20; p.saturation = -10; p.dehaze = 10; p.grade[0] = { 220, 20, 0 }; } },
        { "Выцветший", "Faded", [](Params& p) {
            p.curve[0].pts = { {0, 0.15f}, {0.5f, 0.52f}, {1, 0.9f} }; p.saturation = -25; p.grade[1] = { 45, 10, 0 }; p.grainAmount = 20;
        } },
        { "Драма", "Drama", [](Params& p) { p.contrast = 30; p.highlights = -60; p.shadows = 40; p.clarity = 45; p.dehaze = 20; p.vibrance = -10; p.vigAmount = -30; } },
    };
    return v;
}
