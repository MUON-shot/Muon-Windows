#pragma once
#include <vector>
#include <string>
#include <cstdint>

// X(name, label, min, max, default)
#define BASIC_PARAMS(X) \
    X(temp,        "Температура",   -100, 100, 0) \
    X(tint,        "Оттенок",       -100, 100, 0) \
    X(exposure,    "Экспозиция",    -5,   5,   0) \
    X(contrast,    "Контраст",      -100, 100, 0) \
    X(highlights,  "Света",         -100, 100, 0) \
    X(shadows,     "Тени",          -100, 100, 0) \
    X(whites,      "Белые",         -100, 100, 0) \
    X(blacks,      "Чёрные",        -100, 100, 0) \
    X(texture,     "Текстура",      -100, 100, 0) \
    X(clarity,     "Чёткость",      -100, 100, 0) \
    X(dehaze,      "Удаление дымки",-100, 100, 0) \
    X(vibrance,    "Сочность",      -100, 100, 0) \
    X(saturation,  "Насыщенность",  -100, 100, 0)

#define DETAIL_PARAMS(X) \
    X(sharpAmount, "Величина",      0,    150, 0) \
    X(sharpRadius, "Радиус",        0.5f, 3,   1) \
    X(sharpDetail, "Детализация",   0,    100, 25) \
    X(sharpMask,   "Маскирование",  0,    100, 0) \
    X(nrLuma,      "Шум: яркость",  0,    100, 0) \
    X(nrDetail,    "Шум: детали",   0,    100, 50) \
    X(nrColor,     "Шум: цвет",     0,    100, 0) \
    X(nrSmooth,    "Шум: гладкость",0,    100, 50)

#define OPTICS_PARAMS(X) \
    X(lensDist,    "Дисторсия",          -100, 100, 0) \
    X(lensCA,      "Хром. аберрации",    -100, 100, 0) \
    X(lensVig,     "Виньетирование",     -100, 100, 0) \
    X(perspV,      "Перспектива: верт.", -100, 100, 0) \
    X(perspH,      "Перспектива: гориз.",-100, 100, 0) \
    X(geoScale,    "Масштаб",            50,   150, 100) \
    X(straighten,  "Угол",               -45,  45,  0)

#define EFFECT_PARAMS(X) \
    X(vigAmount,   "Виньетка",       -100, 100, 0) \
    X(vigMid,      "Средняя точка",  0,    100, 50) \
    X(vigRound,    "Округлость",     -100, 100, 0) \
    X(vigFeather,  "Растушёвка",     0,    100, 50) \
    X(vigHigh,     "Сохр. светов",   0,    100, 0) \
    X(grainAmount, "Зерно",          0,    100, 0) \
    X(grainSize,   "Размер зерна",   0,    100, 25) \
    X(grainRough,  "Шероховатость",  0,    100, 50) \
    X(gradBlend,   "Смешивание",     0,    100, 50) \
    X(gradBalance, "Баланс",         -100, 100, 0)

#define ALL_SCALAR_PARAMS(X) BASIC_PARAMS(X) DETAIL_PARAMS(X) OPTICS_PARAMS(X) EFFECT_PARAMS(X)

#define LOCAL_PARAMS(X) \
    X(exposure,   "Экспозиция",    -4,   4,   0) \
    X(contrast,   "Контраст",      -100, 100, 0) \
    X(highlights, "Света",         -100, 100, 0) \
    X(shadows,    "Тени",          -100, 100, 0) \
    X(whites,     "Белые",         -100, 100, 0) \
    X(blacks,     "Чёрные",        -100, 100, 0) \
    X(temp,       "Температура",   -100, 100, 0) \
    X(tint,       "Оттенок",       -100, 100, 0) \
    X(hue,        "Сдвиг тона",    -180, 180, 0) \
    X(saturation, "Насыщенность",  -100, 100, 0) \
    X(clarity,    "Чёткость",      -100, 100, 0) \
    X(dehaze,     "Дымка",         -100, 100, 0) \
    X(sharpness,  "Резкость",      -100, 100, 0) \
    X(colorAmt,   "Цвет: сила",    0,    100, 0)

struct CurvePt { float x, y; };
struct Curve {
    std::vector<CurvePt> pts{ {0, 0}, {1, 1} };
    bool isIdentity() const;
};

struct Wheel { float hue = 0, sat = 0, lum = 0; };

enum MaskShape { MS_FULL = 0, MS_LINEAR, MS_RADIAL, MS_BRUSH };
enum MaskRange { MR_NONE = 0, MR_LUMA, MR_COLOR, MR_DEPTH };   // MR_DEPTH reuses lumLo/lumHi/lumFeather as nearness

struct Dab { float x, y, r, feather, flow; float erase; };

struct LocalAdj {
#define DECL(n, l, mi, ma, d) float n = d;
    LOCAL_PARAMS(DECL)
#undef DECL
    float color[3] = { 1.0f, 0.6f, 0.3f };
};

struct Mask {
    uint32_t id = 0;
    std::string name;
    bool enabled = true;
    bool invert = false;
    int shape = MS_RADIAL;
    float amount = 100;
    // linear gradient: full effect at p0, none at p1 (canvas-normalized coords)
    float lx0 = 0.5f, ly0 = 0.25f, lx1 = 0.5f, ly1 = 0.6f;
    // radial
    float cx = 0.5f, cy = 0.5f, rx = 0.25f, ry = 0.25f, angle = 0, feather = 50;
    // brush
    std::vector<Dab> dabs;
    uint32_t brushRev = 0;
    // range refinement
    int range = MR_NONE;
    float lumLo = 0.5f, lumHi = 1.0f, lumFeather = 0.15f;
    float hueC = 30, hueWidth = 40, hueFeather = 20, satMin = 0.1f;
    LocalAdj adj;
};

enum EffectType {
    FX_ASCII = 0, FX_HALFTONE, FX_DITHER, FX_GRAINPRO, FX_HALATION, FX_RGBSPLIT, FX_GLITCH, FX_PIXELSORT,
    FX_OIL, FX_NEON, FX_GRADMAP, FX_INFRARED, FX_THERMAL, FX_LEAK, FX_POSTER, FX_PIXELATE, FX_VHS,
    FX_KALEIDO, FX_TILTSHIFT, FX_WAVE, FX_HUESHIFT, FX_XEROX, FX_COUNT
};

enum TargetKind { TG_ALL = 0, TG_LUMA, TG_COLOR, TG_MASK, TG_DEPTH };   // TG_DEPTH reuses tLo/tHi/tFeather as nearness

struct Effect {
    uint32_t id = 0;
    int type = FX_ASCII;
    bool enabled = true;
    bool expanded = true;
    float opacity = 100;
    int blend = 0;
    float p[12] = {};
    float col[3][3] = { {1, 1, 1}, {0, 0, 0}, {1, 0.5f, 0.2f} };
    std::string text;      // custom characters for ASCII
    float seed = 0;
    // target / range
    int target = TG_ALL;
    bool targetInvert = false;
    float tLo = 0.0f, tHi = 0.35f, tFeather = 0.1f;
    float tHue = 200, tHueWidth = 60, tHueFeather = 30, tSatMin = 0.1f;
    int tMask = 0;
};

// Glass objects and light sources placed on the photo ("Optics & light").
enum OpticKind { OP_PRISM = 0, OP_LENS, OP_SPHERE, OP_GLASS, OP_DROPS, OP_LIGHT, OP_SPOT, OP_BEAM, OP_COUNT };
inline bool OpticIsLight(int k) { return k >= OP_LIGHT; }

struct OpticObj {
    uint32_t id = 0;
    int kind = OP_PRISM;
    bool enabled = true;
    std::string name;
    float x = 0.5f, y = 0.5f;      // centre, canvas-normalized
    float size = 0.16f;            // radius / half width in canvas heights
    float stretch = 2.5f;          // length to width (prism, glass plate, cylindrical lens)
    float angle = 0;               // degrees
    float height = 45;             // glass: height above the far plane; light: height of the source (0..100)
    // glass
    int variant = 0;               // lens: convex, concave, cylinder; glass: clear, ribbed, hammered, frosted
    float ior = 1.52f, dispersion = 45, curvature = 55, roughness = 0, reflection = 40, caustics = 60, bevel = 35;
    float tint[3] = { 0.86f, 0.95f, 1.0f };
    float tintAmt = 0;
    // light
    float color[3] = { 1.0f, 0.86f, 0.68f };
    float intensity = 120, range = 45, softness = 30, ambient = 15, glow = 40, rays = 0;
    float dir = 0, cone = 35, width = 5;   // spot / beam direction (degrees), spot cone, beam width
    float gloss = 30;                      // shininess of the surfaces this light reveals
};

struct Params {
#define DECL(n, l, mi, ma, d) float n = d;
    ALL_SCALAR_PARAMS(DECL)
#undef DECL
    int profile = 0;            // 0 standard, 1 neutral, 2 vivid, 3 flat
    bool bw = false;
    float hslH[8] = {}, hslS[8] = {}, hslL[8] = {};
    float bwMix[8] = {};
    Wheel grade[4];             // shadows, mids, highlights, global
    Curve curve[4];             // master, R, G, B
    // geometry
    int rot90 = 0;              // clockwise quarter turns
    bool flipH = false, flipV = false;
    float cropX0 = 0, cropY0 = 0, cropX1 = 1, cropY1 = 1;
    int cropAspect = 0;
    std::vector<Mask> masks;
    std::vector<Effect> effects;
    // depth map, optics and light
    std::vector<OpticObj> optics;
    std::vector<Dab> depthDabs;    // hand corrections of the depth map (erase = push further away)
    uint32_t depthRev = 0;
    float relief = 50;             // how strongly the depth map shapes light and refraction
    float microRelief = 35;        // fine surface relief taken from the picture for lighting
    float fogAmount = 0, fogStart = 40;
    float fogColor[3] = { 0.78f, 0.82f, 0.9f };
    float dofAmount = 0, dofFocus = 70, dofRange = 15;
    bool showDepth = false;
};

OpticObj MakeOptic(int kind);

uint32_t NewId();
uint32_t NewRev();

std::string SerializeParams(const Params& p, bool includeGeometry, bool includeMasks, bool diffOnly = false);
bool DeserializeParams(const std::string& s, Params& p, bool applyGeometry, bool applyMasks);
Mask MakeMask(int shape, int range);

// Built-in presets
struct BuiltinPreset { const char* name; const char* nameEn; void (*apply)(Params&); };
const std::vector<BuiltinPreset>& BuiltinPresets();

void BuildCurveLut(const Curve& c, float* lut256);
