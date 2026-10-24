#include "effects.h"
#include "i18n.h"
#include <random>
#include <cstring>
#include <algorithm>

const char* BlendNames() {
    return T("Обычный\0Экран\0Умножение\0Перекрытие\0Мягкий свет\0Сложение\0Разница\0Цветность\0Яркость\0Только светлее\0Только темнее\0",
             "Normal\0Screen\0Multiply\0Overlay\0Soft light\0Add\0Difference\0Color\0Luminosity\0Lighten\0Darken\0");
}

const char* kAsciiSets[] = {
    " .:-=+*#%@",
    " .'`^\",:;Il!i><~+_-?][}{1)(|/tfjrxnuvczXYUJCLQ0OZmwqpdbkhao*#MW&8%B$@",
    " ░▒▓█",
    " 01",
    " ·•●",
};

static std::vector<FxDesc> BuildTable() {
    std::vector<FxDesc> t(FX_COUNT);
    auto S = [](const char* l, float mn, float mx, float d) { FxParam p; p.label = l; p.mn = mn; p.mx = mx; p.def = d; return p; };
    auto B = [](const char* l, bool d) { FxParam p; p.label = l; p.mn = 0; p.mx = 1; p.def = d ? 1.f : 0.f; p.kind = PK_TOGGLE; return p; };
    auto C = [](const char* l, const char* opts, int d) { FxParam p; p.label = l; p.mn = 0; p.mx = 16; p.def = (float)d; p.kind = PK_COMBO; p.options = opts; return p; };

    t[FX_ASCII] = { "ASCII", T("Картинка складывается из текстовых символов. Хорошо смотрится, если ограничить эффект тенями или одним цветом.",
                              "Builds the image out of text characters. Looks best limited to shadows or a single colour."),
        { S(T("Размер символа", "Character size"), 3, 60, 12),
          C(T("Символы", "Characters"), T("Классика\0Подробные\0Блоки\0Двоичный код\0Точки\0Свои\0", "Classic\0Detailed\0Blocks\0Binary\0Dots\0Custom\0"), 0),
          C(T("Цвет", "Colour"), T("Как на фото\0Один цвет\0Матрица\0Два цвета\0", "From photo\0Single colour\0Matrix\0Two colours\0"), 0),
          S(T("Фото на фоне", "Photo behind"), 0, 100, 0), S(T("Гамма", "Gamma"), 0.3f, 3, 1), B(T("Инвертировать яркость", "Invert brightness"), false),
          S(T("Ширина символа", "Character width"), 0.4f, 1.2f, 0.6f) },
        { {T("Символы", "Characters"), 1, 1, 1}, {T("Фон", "Background"), 0, 0, 0} }, 0 };
    t[FX_HALFTONE] = { T("Полутоновый растр", "Halftone"), T("Типографский растр из точек разного размера: чёрно-белый или CMYK.", "Print-style dot screen: black and white or CMYK."),
        { S(T("Шаг растра", "Dot spacing"), 3, 50, 8), S(T("Угол", "Angle"), 0, 90, 45),
          C(T("Режим", "Mode"), T("Чёрно-белый\0CMYK\0Цветные точки\0", "Black & white\0CMYK\0Colour dots\0"), 0),
          S(T("Мягкость краёв", "Edge softness"), 0, 100, 15), S(T("Размер точки", "Dot size"), 50, 150, 100) },
        { {T("Краска", "Ink"), 0.08f, 0.08f, 0.1f}, {T("Бумага", "Paper"), 0.96f, 0.94f, 0.9f} }, 0 };
    t[FX_DITHER] = { T("Дизеринг", "Dither"), T("Ретро-растрирование ограниченной палитрой: Bayer, шум, Game Boy, CGA.", "Retro dithering to a limited palette: Bayer, noise, Game Boy, CGA."),
        { S(T("Размер пикселя", "Pixel size"), 1, 16, 2), S(T("Число уровней", "Levels"), 2, 8, 2),
          C(T("Узор", "Pattern"), T("Bayer 8×8\0Bayer 4×4\0Шум\0Линии\0", "Bayer 8×8\0Bayer 4×4\0Noise\0Lines\0"), 0),
          C(T("Палитра", "Palette"), T("Чёрно-белая\0Два цвета\0Game Boy\0CGA\0Цвета фото\0Сепия\0", "Black & white\0Two colours\0Game Boy\0CGA\0Photo colours\0Sepia\0"), 0),
          S(T("Гамма", "Gamma"), 0.3f, 3, 1), S(T("Яркость", "Brightness"), -50, 50, 0) },
        { {T("Тёмный", "Dark"), 0.1f, 0.05f, 0.2f}, {T("Светлый", "Light"), 1, 0.9f, 0.7f} }, 0 };
    t[FX_GRAINPRO] = { T("Плёночное зерно", "Film grain"), T("Серебряное и цветное зерно, цифровой шум, пыль и царапины.", "Silver and colour grain, digital noise, dust and scratches."),
        { S(T("Сила", "Amount"), 0, 100, 40), S(T("Размер", "Size"), 0.5f, 8, 1.5f),
          C(T("Тип", "Type"), T("Серебряное\0Цветное\0Цифровой шум\0Пыль и царапины\0", "Silver\0Colour\0Digital noise\0Dust & scratches\0"), 0),
          S(T("Неровность", "Roughness"), 0, 100, 50), S(T("В тенях", "In shadows"), 0, 100, 100), S(T("В полутонах", "In midtones"), 0, 100, 100), S(T("В светах", "In highlights"), 0, 100, 40) },
        {}, 0 };
    t[FX_HALATION] = { T("Свечение и ореол", "Glow & halation"), T("Красноватый плёночный ореол вокруг ярких участков, мягкое свечение или анаморфный блик.", "Film halation around bright areas, soft glow or an anamorphic streak."),
        { S(T("Порог яркости", "Threshold"), 0, 100, 70), S(T("Радиус", "Radius"), 1, 200, 30), S(T("Сила", "Amount"), 0, 300, 100),
          C(T("Режим", "Mode"), T("Плёночный ореол\0Свечение\0Анаморфный блик\0", "Film halation\0Glow\0Anamorphic streak\0"), 0) },
        { {T("Цвет", "Colour"), 1.0f, 0.25f, 0.08f} }, 0 };
    t[FX_RGBSPLIT] = { T("Сдвиг каналов", "RGB split"), T("Красный и синий каналы расходятся: по направлению, от центра или по строкам.", "Red and blue channels drift apart: linear, radial or per line."),
        { S(T("Смещение", "Offset"), 0, 60, 6), S(T("Угол", "Angle"), 0, 360, 0),
          C(T("Режим", "Mode"), T("По направлению\0От центра\0Случайно по строкам\0", "Directional\0Radial\0Random lines\0"), 0) },
        {}, 0 };
    t[FX_GLITCH] = { T("Глитч", "Glitch"), T("Цифровые сбои: сдвиги блоков, перепутанные каналы, помехи.", "Digital corruption: shifted blocks, swapped channels, interference."),
        { S(T("Интенсивность", "Intensity"), 0, 100, 50), S(T("Размер блоков", "Block size"), 5, 200, 40), S(T("Сдвиг каналов", "Channel shift"), 0, 100, 30),
          S(T("Строки развёртки", "Scanlines"), 0, 100, 20), S(T("Шум", "Noise"), 0, 100, 15) },
        {}, 0 };
    t[FX_PIXELSORT] = { T("Сортировка пикселей", "Pixel sort"), T("Пиксели в выбранном диапазоне яркости вытягиваются в отсортированные полосы.", "Pixels within a brightness range are stretched into sorted streaks."),
        { S(T("Нижний порог", "Lower threshold"), 0, 100, 25), S(T("Верхний порог", "Upper threshold"), 0, 100, 90), S(T("Длина полос", "Streak length"), 10, 800, 250),
          C(T("Направление", "Direction"), T("Вправо\0Вниз\0Влево\0Вверх\0", "Right\0Down\0Left\0Up\0"), 1),
          C(T("Сортировать по", "Sort by"), T("Яркости\0Оттенку\0Насыщенности\0", "Brightness\0Hue\0Saturation\0"), 0) },
        {}, 0 };
    t[FX_OIL] = { T("Живопись", "Painting"), T("Мазки масляной краски или акварель (фильтр Кувахары).", "Oil brush strokes or watercolour (Kuwahara filter)."),
        { S(T("Размер мазка", "Stroke size"), 1, 14, 4), C(T("Техника", "Style"), T("Масло\0Акварель\0", "Oil\0Watercolour\0"), 0), S(T("Фактура холста", "Canvas texture"), 0, 100, 25) },
        {}, 0 };
    t[FX_NEON] = { T("Неоновые контуры", "Neon edges"), T("Светящиеся контуры объектов на тёмном фоне.", "Glowing outlines on a dark background."),
        { S(T("Чувствительность", "Threshold"), 0, 100, 15), S(T("Толщина", "Thickness"), 0.5f, 6, 1.2f), S(T("Свечение", "Glow"), 0, 100, 60), S(T("Фото на фоне", "Photo behind"), 0, 100, 10),
          C(T("Цвет", "Colour"), T("Как на фото\0Свой цвет\0Радуга\0", "From photo\0Custom\0Rainbow\0"), 0) },
        { {T("Цвет", "Colour"), 0.1f, 1.0f, 0.9f} }, 0 };
    t[FX_GRADMAP] = { T("Градиентная карта", "Gradient map"), T("Яркость переводится в градиент из двух или трёх цветов (дуотон, тритон).", "Brightness is mapped onto a two or three colour gradient (duotone, tritone)."),
        { C(T("Цветов", "Colours"), T("Два\0Три\0", "Two\0Three\0"), 1), S(T("Положение середины", "Midpoint"), 10, 90, 50), S(T("Контраст", "Contrast"), -100, 100, 0) },
        { {T("Тени", "Shadows"), 0.08f, 0.05f, 0.25f}, {T("Света", "Highlights"), 1.0f, 0.88f, 0.6f}, {T("Середина", "Midtones"), 0.85f, 0.3f, 0.4f} }, 0 };
    t[FX_INFRARED] = { T("Инфракрасная плёнка", "Infrared film"), T("Имитация ИК-плёнки: листва становится красной, белой или золотой.", "Infrared film look: foliage turns red, white or gold."),
        { C(T("Плёнка", "Film"), T("Цветная (Aerochrome)\0Чёрно-белая\0Золотая\0", "Colour (Aerochrome)\0Black & white\0Golden\0"), 0),
          S(T("Окрашивание зелени", "Foliage effect"), 0, 100, 60), S(T("Свечение", "Glow"), 0, 100, 30) },
        {}, 0 };
    t[FX_THERMAL] = { T("Тепловизор", "Thermal"), T("Яркость показана условными цветами, как на тепловизоре.", "Brightness shown in false colour, like a thermal camera."),
        { C(T("Палитра", "Palette"), T("Раскалённая\0Радуга\0Ночное видение\0Лёд\0", "Iron\0Rainbow\0Night vision\0Ice\0"), 0), S(T("Контраст", "Contrast"), -100, 100, 0) },
        {}, 0 };
    t[FX_LEAK] = { T("Засветка", "Light leak"), T("Тёплые плёночные засветки по краям кадра.", "Warm film light leaks along the frame."),
        { S(T("Интенсивность", "Intensity"), 0, 100, 60), S(T("Размер", "Size"), 10, 100, 50), S(T("Расположение", "Placement"), 0, 100, 0),
          C(T("Форма", "Shape"), T("С края\0Из угла\0Полосы\0Боке\0", "Edge\0Corner\0Stripes\0Bokeh\0"), 0) },
        { {T("Цвет 1", "Colour 1"), 1.0f, 0.3f, 0.05f}, {T("Цвет 2", "Colour 2"), 1.0f, 0.8f, 0.3f} }, 1 };
    t[FX_POSTER] = { T("Постеризация", "Posterize"), T("Тона сводятся к нескольким ступеням, можно добавить контуры как в комиксе.", "Tones collapse into a few steps, with optional comic-style outlines."),
        { S(T("Число ступеней", "Steps"), 2, 12, 5), S(T("Контуры", "Outlines"), 0, 100, 0), S(T("Сохранение цвета", "Keep colour"), 0, 100, 100) },
        {}, 0 };
    t[FX_PIXELATE] = { T("Мозаика", "Mosaic"), T("Крупные пиксели: квадраты, соты, круги или кирпичики.", "Large pixels: squares, hexagons, circles or bricks."),
        { S(T("Размер", "Size"), 2, 120, 16), C(T("Форма", "Shape"), T("Квадраты\0Соты\0Круги\0Кирпичики\0", "Squares\0Hexagons\0Circles\0Bricks\0"), 0), S(T("Зазор", "Gap"), 0, 50, 10) },
        { {T("Цвет зазора", "Gap colour"), 0.05f, 0.05f, 0.05f} }, 0 };
    t[FX_VHS] = { T("VHS и кинескоп", "VHS & CRT"), T("Видеокассета или старый телевизор: строки, растекание цвета, помехи.", "Video tape or old TV: scanlines, colour bleed, noise."),
        { S(T("Строки развёртки", "Scanlines"), 0, 100, 40), S(T("Растекание цвета", "Colour bleed"), 0, 100, 40), S(T("Помехи", "Noise"), 0, 100, 25),
          S(T("Дрожание", "Jitter"), 0, 100, 20), S(T("Сетка кинескопа", "Phosphor mask"), 0, 100, 0), S(T("Выпуклость экрана", "Screen curve"), 0, 100, 0) },
        {}, 0 };
    t[FX_KALEIDO] = { T("Калейдоскоп", "Kaleidoscope"), T("Зеркальные отражения и калейдоскоп.", "Mirror reflections and kaleidoscope."),
        { C(T("Режим", "Mode"), T("Зеркало по горизонтали\0Зеркало по вертикали\0Четыре зеркала\0Калейдоскоп\0", "Mirror horizontally\0Mirror vertically\0Four mirrors\0Kaleidoscope\0"), 3),
          S(T("Число сегментов", "Segments"), 2, 16, 6), S(T("Поворот", "Rotation"), 0, 360, 0), S(T("Масштаб", "Zoom"), 30, 300, 100) },
        {}, 0 };
    t[FX_TILTSHIFT] = { T("Тилт-шифт", "Tilt-shift"), T("Эффект миниатюры: резкая полоса, вокруг размытие.", "Miniature look: a sharp band with blur around it."),
        { S(T("Положение полосы", "Band position"), 0, 100, 55), S(T("Ширина полосы", "Band width"), 0, 100, 12), S(T("Плавность перехода", "Transition"), 1, 100, 25),
          S(T("Размытие", "Blur"), 1, 100, 40), S(T("Наклон", "Tilt"), -90, 90, 0), S(T("Насыщенность", "Saturation"), 0, 100, 25) },
        {}, 0 };
    t[FX_WAVE] = { T("Искажения", "Distortion"), T("Волны, рябь, водоворот или дрожание.", "Waves, ripples, swirl or wobble."),
        { C(T("Тип", "Type"), T("Волна\0Рябь\0Водоворот\0Дрожание\0", "Wave\0Ripple\0Swirl\0Wobble\0"), 0), S(T("Амплитуда", "Amplitude"), 0, 100, 20),
          S(T("Частота", "Frequency"), 1, 50, 8), S(T("Угол", "Angle"), 0, 360, 0), S(T("Фаза", "Phase"), 0, 360, 0) },
        {}, 0 };
    t[FX_HUESHIFT] = { T("Замена цвета", "Colour shift"), T("Сдвигает оттенок и насыщенность. С областью «Цвет» меняет один цвет, а с инверсией и насыщенностью -100 оставляет цветным только его.",
                                                     "Shifts hue and saturation. With the Colour area it replaces one colour; inverted with saturation -100 it keeps only that colour."),
        { S(T("Оттенок", "Hue"), -180, 180, 0), S(T("Насыщенность", "Saturation"), -100, 100, 0), S(T("Яркость", "Lightness"), -100, 100, 0), B(T("Окрасить в цвет", "Colourise"), false) },
        { {T("Цвет", "Colour"), 0.9f, 0.2f, 0.3f} }, 0 };
    t[FX_XEROX] = { T("Ксерокопия", "Photocopy"), T("Резкая двухтоновая копия с шумом тонера и полосами.", "Harsh two-tone copy with toner noise and streaks."),
        { S(T("Порог", "Threshold"), 0, 100, 50), S(T("Шум", "Noise"), 0, 100, 30), S(T("Жёсткость", "Hardness"), 0, 100, 70), S(T("Полосы", "Streaks"), 0, 100, 20) },
        { {T("Тонер", "Toner"), 0.05f, 0.05f, 0.06f}, {T("Бумага", "Paper"), 0.95f, 0.94f, 0.9f} }, 0 };
    return t;
}

const FxDesc& GetFxDesc(int type) {
    static std::vector<FxDesc> tables[2];
    int l = g_lang == 1 ? 1 : 0;
    if (tables[l].empty()) tables[l] = BuildTable();
    if (type < 0 || type >= FX_COUNT) type = 0;
    return tables[l][type];
}

Effect MakeEffect(int type) {
    Effect e;
    e.id = NewId();
    e.type = type;
    const FxDesc& d = GetFxDesc(type);
    for (size_t i = 0; i < d.params.size() && i < 12; i++) e.p[i] = d.params[i].def;
    for (size_t i = 0; i < d.colors.size() && i < 3; i++) { e.col[i][0] = d.colors[i].r; e.col[i][1] = d.colors[i].g; e.col[i][2] = d.colors[i].b; }
    e.blend = d.defBlend;
    e.seed = (float)(NewId() % 1000);
    if (type == FX_ASCII) e.text = " .oO@";
    return e;
}

void RandomizeEffect(Effect& e) {
    static std::mt19937 rng{ std::random_device{}() };
    const FxDesc& d = GetFxDesc(e.type);
    std::uniform_real_distribution<float> u(0, 1);
    for (size_t i = 0; i < d.params.size(); i++) {
        const FxParam& p = d.params[i];
        if (p.kind == PK_COMBO) {
            int n = 0; for (const char* s = p.options; *s; s += strlen(s) + 1) n++;
            e.p[i] = (float)(int)(u(rng) * n);
        } else if (p.kind == PK_TOGGLE) e.p[i] = u(rng) > 0.5f ? 1.f : 0.f;
        else {
            // stay near the default so the result is still usable
            float span = (p.mx - p.mn) * 0.35f;
            e.p[i] = std::min(p.mx, std::max(p.mn, p.def + (u(rng) * 2 - 1) * span));
        }
    }
    for (size_t i = 0; i < d.colors.size(); i++)
        for (int k = 0; k < 3; k++) e.col[i][k] = u(rng);
    e.seed = u(rng) * 1000;
}

// ------------------------------------------------------------------ HLSL

static const char* kFxHeader = R"HLSL(
cbuffer FX : register(b1) { float4 P[3]; float4 C[3]; float4 X; };
float prm(int i) { return P[i >> 2][i & 3]; }
float3 col(int i) { return C[i].rgb; }
float UU() { return max(uFull.x, uFull.y) / 1500.0; }
float2 toUnit(float2 uv) { return uv * uFull / UU(); }
float2 fromUnit(float2 q) { return q * UU() / uFull; }
float pxUnit() { return 1.0 / (UU() * uScale); }    // one working pixel in units
float3 img(float2 uv) { return t0.SampleLevel(sLin, uv, 0).rgb; }
float3 imgM(float2 uv) { return t0.SampleLevel(sMirror, uv, 0).rgb; }
float aspect() { return uFull.x / uFull.y; }
)HLSL";

static const char* kFx[FX_COUNT] = {
// ---------------------------------------------------------------- ASCII
R"HLSL(
float4 main(VSO i) : SV_Target {
    float cs = prm(0), asp = prm(6);
    float2 cell = float2(cs * asp, cs);
    float2 q = toUnit(i.uv);
    float2 ci = floor(q / cell);
    float2 cuv = fromUnit((ci + 0.5) * cell);
    float2 o = fromUnit(cell * 0.25);
    float3 c = (img(cuv) * 2 + img(cuv + o) + img(cuv - o) + img(cuv + float2(o.x, -o.y)) + img(cuv + float2(-o.x, o.y))) / 6;
    float b = pow(saturate(luma(c)), prm(4));
    if (prm(5) > 0.5) b = 1 - b;
    float n = X.y;
    float gi = min(floor(b * n), n - 1);
    float2 f = frac(q / cell);
    float g = t1.SampleLevel(sLin, float2((gi + clamp(f.x, 0.02, 0.98)) / n, clamp(f.y, 0.02, 0.98)), 0).r;
    int cm = (int)prm(2);
    float3 fg;
    if (cm == 0) fg = saturate(c / max(max(c.r, max(c.g, c.b)), 0.04) * (0.55 + 0.45 * b));
    else if (cm == 1 || cm == 3) fg = col(0);
    else fg = float3(0.15, 1.0, 0.35) * (0.35 + 0.9 * b);
    float3 bg = lerp(col(1), img(i.uv), prm(3) / 100);
    return float4(lerp(bg, fg, g), 1);
}
)HLSL",
// ---------------------------------------------------------------- HALFTONE
R"HLSL(
float halfDot(float2 q, float ang, float size, int ch, int mode, float scl, float soft, out float3 cellCol) {
    float2 r = rot2(q, ang);
    float2 cc = (floor(r / size) + 0.5) * size;
    float2 cuv = fromUnit(rot2(cc, -ang));
    float3 c = img(cuv);
    cellCol = c;
    float v;
    if (mode == 1) {
        float k = 1 - max(c.r, max(c.g, c.b));
        float3 cmy = (1 - c - k) / max(1 - k, 1e-3);
        v = ch == 3 ? k : (ch == 0 ? cmy.x : (ch == 1 ? cmy.y : cmy.z));
    } else v = 1 - luma(c);
    float rad = sqrt(saturate(v)) * size * 0.7071 * scl;
    float d = length(r - cc);
    float aa = max(size * soft * 0.5, pxUnit() * 0.8);
    return 1 - smoothstep(rad - aa, rad + aa, d);
}
float4 main(VSO i) : SV_Target {
    float2 q = toUnit(i.uv);
    float size = prm(0), ang = radians(prm(1)), soft = prm(3) / 100, scl = prm(4) / 100;
    int mode = (int)prm(2);
    float3 paper = col(1), ink = col(0), cc;
    if (mode == 1) {
        float3 rgb = paper;
        float dc = halfDot(q, ang + radians(15), size, 0, 1, scl, soft, cc);
        float dm = halfDot(q, ang + radians(75), size, 1, 1, scl, soft, cc);
        float dy = halfDot(q, ang, size, 2, 1, scl, soft, cc);
        float dk = halfDot(q, ang + radians(45), size, 3, 1, scl, soft, cc);
        rgb *= lerp(1, float3(0.0, 0.68, 0.94), dc);
        rgb *= lerp(1, float3(0.93, 0.0, 0.55), dm);
        rgb *= lerp(1, float3(1.0, 0.95, 0.0), dy);
        rgb *= lerp(1, ink, dk);
        return float4(rgb, 1);
    }
    float d = halfDot(q, ang, size, 0, 0, scl, soft, cc);
    float3 dotc = mode == 2 ? saturate(cc * 1.2) : ink;
    return float4(lerp(paper, dotc, d), 1);
}
)HLSL",
// ---------------------------------------------------------------- DITHER
R"HLSL(
float bayer(int2 p, int k) {
    int v = 0;
    for (int j = 0; j < k; j++) { int bx = (p.x >> j) & 1, by = (p.y >> j) & 1; int b = ((bx ^ by) << 1) | by; v += b << (2 * (k - 1 - j)); }
    return (v + 0.5) / (float)(1 << (2 * k));
}
float3 pal4(int idx, int pal) {
    if (pal == 2) { float3 g[4] = { float3(0.06,0.22,0.06), float3(0.19,0.38,0.19), float3(0.55,0.67,0.06), float3(0.61,0.74,0.06) }; return g[idx]; }
    if (pal == 3) { float3 g[4] = { float3(0,0,0), float3(1,0.33,1), float3(0.33,1,1), float3(1,1,1) }; return g[idx]; }
    float3 s[4] = { float3(0.1,0.06,0.03), float3(0.42,0.3,0.2), float3(0.75,0.62,0.45), float3(0.98,0.93,0.82) }; return s[idx];
}
float4 main(VSO i) : SV_Target {
    float ps = prm(0);
    float2 q = toUnit(i.uv);
    float2 cell = floor(q / ps);
    float3 c = img(fromUnit((cell + 0.5) * ps));
    c = pow(saturate(c), prm(4)) + prm(5) / 100;
    int alg = (int)prm(2), pal = (int)prm(3);
    float L = (float)((int)prm(1));
    float th;
    int2 ip = (int2)cell;
    if (alg == 0) th = bayer(ip & 7, 3);
    else if (alg == 1) th = bayer(ip & 3, 2);
    else if (alg == 2) th = hash12(cell + X.x);
    else th = frac((cell.y + cell.x * 0.5) / 4.0) ;
    if (pal == 2 || pal == 3 || pal == 5) {
        float v = saturate(luma(c));
        int idx = (int)clamp(floor(v * 3 + th), 0, 3);
        return float4(pal4(idx, pal), 1);
    }
    if (pal == 4) {
        float3 qv = floor(saturate(c) * (L - 1) + th) / (L - 1);
        return float4(saturate(qv), 1);
    }
    float v = floor(saturate(luma(c)) * (L - 1) + th) / (L - 1);
    float3 a = pal == 0 ? float3(0,0,0) : col(0);
    float3 b = pal == 0 ? float3(1,1,1) : col(1);
    return float4(lerp(a, b, saturate(v)), 1);
}
)HLSL",
// ---------------------------------------------------------------- GRAIN PRO
R"HLSL(
float4 main(VSO i) : SV_Target {
    float3 c = img(i.uv);
    float amt = prm(0) / 100, size = prm(1), rough = prm(3) / 100;
    int type = (int)prm(2);
    float L = luma(c);
    float w = prm(4) / 100 * pow(1 - L, 2) + prm(5) / 100 * 4 * L * (1 - L) + prm(6) / 100 * L * L;
    w = saturate(w * 1.2);
    float2 q = toUnit(i.uv) / size + X.x * 7.13;
    if (type == 0 || type == 1) {
        float3 n;
        float2 o2 = float2(31.7, 11.3);
        n.r = (vnoise(q) - 0.5) + (vnoise(q * 2.13 + 5.1) - 0.5) * rough + (hash12(floor(q * 2.9)) - 0.5) * 0.35 * rough;
        if (type == 1) {
            n.g = (vnoise(q + o2) - 0.5) + (vnoise(q * 2.13 + o2 + 5.1) - 0.5) * rough;
            n.b = (vnoise(q - o2) - 0.5) + (vnoise(q * 2.13 - o2 + 5.1) - 0.5) * rough;
            n = lerp(n.rrr, n, 0.6);
        } else n = n.rrr;
        n *= 1.6;
        float3 r = c + n * amt * 0.45 * w;
        return float4(saturate(r), 1);
    }
    if (type == 2) {
        float2 fp = floor(i.uv * uFull) + X.x;
        float ln = gnoise(fp) * 0.6;
        float3 cn = float3(gnoise(fp + 3.1), gnoise(fp + 9.7), gnoise(fp + 21.3)) * 0.5;
        float3 blotch = (float3(vnoise(q * 0.25), vnoise(q * 0.25 + 7), vnoise(q * 0.25 + 13)) - 0.5) * rough;
        float3 r = c + (ln + cn + blotch) * amt * 0.3 * (0.4 + w);
        return float4(saturate(r), 1);
    }
    // dust & scratches
    float2 u = toUnit(i.uv);
    float3 r = c;
    float2 cellSz = 60 * size;
    float2 cid = floor(u / cellSz);
    for (int dy = -1; dy <= 1; dy++) for (int dx = -1; dx <= 1; dx++) {
        float2 id = cid + float2(dx, dy);
        float h = hash12(id + X.x);
        if (h < amt * 0.5) {
            float2 pos = (id + hash22(id + X.x + 3)) * cellSz;
            float rad = (0.6 + hash12(id + 7.7) * 2.5) * size;
            float2 dd = u - pos; dd.x *= 1 + hash12(id + 1.3) * 2;
            float m = 1 - smoothstep(rad * 0.6, rad, length(dd));
            float tone = hash12(id + 5.5) > 0.5 ? 1.0 : 0.0;
            r = lerp(r, tone.xxx, m * 0.85);
        }
    }
    float lines = floor(rough * 6 + 1);
    for (int k = 0; k < 6; k++) {
        if (k >= lines) break;
        float x0 = hash12(float2(k, X.x)) * uFull.x / UU();
        float wob = sin(u.y * 0.01 + k) * 4;
        float d = abs(u.x - x0 - wob);
        float vis = step(0.5, vnoise(float2(u.y * 0.004, k * 3.1)));
        r = lerp(r, float3(0.9, 0.9, 0.88), (1 - smoothstep(0.3, 1.0, d / size)) * vis * amt * 0.7);
    }
    return float4(saturate(r), 1);
}
)HLSL",
// ---------------------------------------------------------------- HALATION  (t1 = blurred bright pass, X.w = 0 -> bright pass)
R"HLSL(
float4 main(VSO i) : SV_Target {
    float3 c = img(i.uv);
    if (X.w < 0.5) {
        float th = prm(0) / 100;
        float L = luma(c);
        float k = smoothstep(th, min(th + 0.15, 1.0), L);
        return float4(c * k, 1);
    }
    float3 g = t1.SampleLevel(sLin, i.uv, 0).rgb;
    float s = prm(2) / 100;
    int mode = (int)prm(3);
    float3 tint = col(0);
    float3 add;
    if (mode == 0) add = luma(g) * tint * 1.5;           // film halation: tinted glow
    else if (mode == 1) add = lerp(g, luma(g).xxx, 0.3);
    else add = luma(g) * lerp(tint, float3(0.4, 0.6, 1.0), 0.6) * 2.0;
    float3 r = 1 - (1 - c) * (1 - saturate(add * s));
    return float4(r, 1);
}
)HLSL",
// ---------------------------------------------------------------- RGB SPLIT
R"HLSL(
float4 main(VSO i) : SV_Target {
    float amt = prm(0);
    int mode = (int)prm(2);
    float2 d;
    if (mode == 0) { float a = radians(prm(1)); d = float2(cos(a), sin(a)) * amt; }
    else if (mode == 1) { float2 v = (i.uv - 0.5) * float2(aspect(), 1); d = v * amt * 2; }
    else { float2 q = toUnit(i.uv); float row = floor(q.y / 6); d = float2((hash12(float2(row, X.x)) - 0.5) * 2 * amt * step(0.6, hash12(float2(row * 1.7, X.x + 3))), 0); }
    float2 du = fromUnit(d);
    float r = imgM(i.uv + du).r, g = img(i.uv).g, b = imgM(i.uv - du).b;
    return float4(r, g, b, 1);
}
)HLSL",
// ---------------------------------------------------------------- GLITCH
R"HLSL(
float4 main(VSO i) : SV_Target {
    float inten = prm(0) / 100, bs = prm(1), cs = prm(2) / 100, scan = prm(3) / 100, nz = prm(4) / 100;
    float2 q = toUnit(i.uv);
    float2 bc = floor(q / float2(bs * 4, bs));
    float r = hash12(bc + X.x);
    float2 off = 0;
    if (r < inten * 0.35) off.x = (hash12(bc + 7.1 + X.x) - 0.5) * bs * 6;
    float row = floor(q.y / max(bs * 0.2, 1));
    if (hash12(float2(row, X.x + 1)) < inten * 0.12) off.x += (hash12(float2(row, X.x + 2)) - 0.5) * bs * 3;
    float2 uv = i.uv + fromUnit(off);
    float2 sh = fromUnit(float2(cs * bs * 0.5 * (hash12(bc + 3.3) - 0.2), 0));
    float3 c = float3(imgM(uv + sh).r, imgM(uv).g, imgM(uv - sh).b);
    float sw = hash12(bc * 1.3 + X.x + 9);
    if (sw > 1 - inten * 0.12) c = c.gbr;
    else if (sw > 1 - inten * 0.2) c = 1 - c;
    c *= 1 - scan * 0.5 * (0.5 + 0.5 * sin(q.y * PI / 1.5));
    c += (hash12(q * 1.31 + X.x) - 0.5) * nz * 0.5;
    return float4(saturate(c), 1);
}
)HLSL",
// ---------------------------------------------------------------- PIXEL SORT
R"HLSL(
float sortKey(float3 c, int k) {
    if (k == 1) return rgb2hsv(c).x;
    if (k == 2) return rgb2hsv(c).y;
    return luma(c);
}
bool inRange(float3 c, float lo, float hi) { float L = luma(c); return L >= lo && L <= hi; }
float4 main(VSO i) : SV_Target {
    float lo = prm(0) / 100, hi = prm(1) / 100;
    int dir = (int)prm(3), key = (int)prm(4);
    float3 c0 = img(i.uv);
    if (!inRange(c0, lo, hi)) return float4(c0, 1);
    float2 d = (dir == 0 || dir == 2) ? float2(uInv.x, 0) : float2(0, uInv.y);
    float maxLen = clamp(prm(2) * UU() * uScale, 4, 2000);
    float stride = max(1.0, maxLen / 64.0);
    float back = 0, fwd = 0;
    [loop] for (int k = 1; k <= 64; k++) {
        float2 uv = i.uv - d * k * stride;
        if (uv.x < 0 || uv.y < 0 || !inRange(img(uv), lo, hi)) break;
        back = k * stride;
    }
    [loop] for (int k2 = 1; k2 <= 64; k2++) {
        float2 uv = i.uv + d * k2 * stride;
        if (uv.x > 1 || uv.y > 1 || !inRange(img(uv), lo, hi)) break;
        fwd = k2 * stride;
    }
    float len = back + fwd;
    if (len < 2) return float4(c0, 1);
    float cnt[16]; float3 sum[16];
    [unroll] for (int b = 0; b < 16; b++) { cnt[b] = 0; sum[b] = 0; }
    const int N = 40;
    [loop] for (int s = 0; s < N; s++) {
        float t = (s + 0.5) / N;
        float3 c = img(i.uv + d * (-back + t * len));
        int bi = (int)clamp(sortKey(c, key) * 16, 0, 15);
        cnt[bi] += 1; sum[bi] += c;
    }
    float f = back / len;
    if (dir >= 2) f = 1 - f;
    float target = f * N, acc = 0;
    float3 outc = c0;
    [unroll] for (int b2 = 0; b2 < 16; b2++) {
        if (cnt[b2] > 0) {
            acc += cnt[b2];
            if (acc >= target) { outc = sum[b2] / cnt[b2]; break; }
            outc = sum[b2] / cnt[b2];
        }
    }
    return float4(outc, 1);
}
)HLSL",
// ---------------------------------------------------------------- OIL / WATERCOLOR (Kuwahara)
R"HLSL(
float4 main(VSO i) : SV_Target {
    float rpx = max(1.0, prm(0) * UU() * uScale);
    float step = max(1.0, rpx / 5.0);
    int n = (int)min(ceil(rpx / step), 6);
    float3 m[4]; float3 s2[4];
    [unroll] for (int k = 0; k < 4; k++) { m[k] = 0; s2[k] = 0; }
    float cnt = (n + 1) * (n + 1);
    [loop] for (int y = 0; y <= n; y++) {
        [loop] for (int x = 0; x <= n; x++) {
            float2 o = float2(x, y) * step * uInv;
            float3 a = img(i.uv + float2(-o.x, -o.y)), b = img(i.uv + float2(o.x, -o.y));
            float3 c = img(i.uv + float2(-o.x, o.y)), d = img(i.uv + float2(o.x, o.y));
            m[0] += a; s2[0] += a * a; m[1] += b; s2[1] += b * b; m[2] += c; s2[2] += c * c; m[3] += d; s2[3] += d * d;
        }
    }
    float best = 1e9; float3 res = 0;
    [unroll] for (int k2 = 0; k2 < 4; k2++) {
        float3 mu = m[k2] / cnt; float3 v = abs(s2[k2] / cnt - mu * mu);
        float vs = v.r + v.g + v.b;
        if (vs < best) { best = vs; res = mu; }
    }
    float2 q = toUnit(i.uv);
    float tex = prm(2) / 100;
    if ((int)prm(1) == 1) {
        float3 c0 = img(i.uv);
        float e = length(float2(luma(img(i.uv + float2(uInv.x * 2, 0))) - luma(img(i.uv - float2(uInv.x * 2, 0))),
                               luma(img(i.uv + float2(0, uInv.y * 2))) - luma(img(i.uv - float2(0, uInv.y * 2)))));
        res = floor(res * 8 + 0.5) / 8;
        res = lerp(res, 1, 0.12);
        res *= 1 - saturate(e * 3) * 0.35;
        float paper = fbm(q * 0.15, 4);
        res *= 1 - (paper - 0.5) * tex * 0.5;
        return float4(saturate(res), 1);
    }
    float canvas = (vnoise(q * float2(1.2, 0.3)) + vnoise(q * float2(0.3, 1.2))) * 0.5 - 0.5;
    res *= 1 + canvas * tex * 0.25;
    return float4(saturate(res), 1);
}
)HLSL",
// ---------------------------------------------------------------- NEON (X.w = 0 edges pass, 1 composite with t1 edges, t2 blurred edges)
R"HLSL(
float4 main(VSO i) : SV_Target {
    if (X.w < 0.5) {
        float2 o = uInv * max(1.0, prm(1) * UU() * uScale);
        float tl = luma(img(i.uv + o * float2(-1, -1))), t = luma(img(i.uv + o * float2(0, -1))), tr = luma(img(i.uv + o * float2(1, -1)));
        float l = luma(img(i.uv + o * float2(-1, 0))), r = luma(img(i.uv + o * float2(1, 0)));
        float bl = luma(img(i.uv + o * float2(-1, 1))), b = luma(img(i.uv + o * float2(0, 1))), br = luma(img(i.uv + o * float2(1, 1)));
        float gx = tr + 2 * r + br - tl - 2 * l - bl, gy = bl + 2 * b + br - tl - 2 * t - tr;
        float e = length(float2(gx, gy));
        float th = prm(0) / 100;
        float m = smoothstep(th, th + 0.15, e);
        int cm = (int)prm(4);
        float3 c = img(i.uv);
        float3 ec = cm == 0 ? saturate(c / max(max(c.r, max(c.g, c.b)), 0.05)) : (cm == 1 ? col(0) : hsv2rgb(float3(frac(atan2(gy, gx) / (2 * PI) + i.uv.x * 0.3), 0.9, 1)));
        return float4(ec * m, 1);
    }
    float3 e = t1.SampleLevel(sLin, i.uv, 0).rgb, g = t2.SampleLevel(sLin, i.uv, 0).rgb;
    float3 bg = img(i.uv) * prm(3) / 100;
    float3 r = 1 - (1 - bg) * (1 - e) * (1 - saturate(g * prm(2) / 100 * 3));
    return float4(r, 1);
}
)HLSL",
// ---------------------------------------------------------------- GRADIENT MAP
R"HLSL(
float4 main(VSO i) : SV_Target {
    float3 c = img(i.uv);
    float L = saturate(luma(c));
    float a = exp(prm(2) / 100 * 1.2);
    float m = 0.5;
    L = L < m ? m * pow(L / m, a) : 1 - (1 - m) * pow((1 - L) / (1 - m), a);
    float3 c0 = srgbDec(col(0)), c1 = srgbDec(col(1)), c2 = srgbDec(col(2));
    float3 r;
    if ((int)prm(0) == 0) r = lerp(c0, c1, L);
    else { float mp = prm(1) / 100; r = L < mp ? lerp(c0, c2, L / mp) : lerp(c2, c1, (L - mp) / (1 - mp)); }
    return float4(srgbEnc(r), 1);
}
)HLSL",
// ---------------------------------------------------------------- INFRARED
R"HLSL(
float4 main(VSO i) : SV_Target {
    float3 c = srgbDec(img(i.uv));
    int mode = (int)prm(0);
    float fol = prm(1) / 100;
    float3 hsv = rgb2hsv(srgbEnc(c));
    float green = smoothstep(0.12, 0.2, hsv.x) * (1 - smoothstep(0.42, 0.5, hsv.x)) * saturate(hsv.y * 2);
    float3 r;
    if (mode == 0) {
        r = float3(c.g * 1.1 + c.r * 0.1, c.r * 0.85, c.b * 0.9 + c.r * 0.1);
        r = lerp(r, float3(0.95, 0.15, 0.35) * (0.4 + luma(c) * 2), green * fol);
    } else if (mode == 1) {
        float ir = saturate(0.3 * c.r + 1.4 * c.g - 0.7 * c.b);
        ir = lerp(ir, 1, green * fol * 0.8);
        r = ir.xxx;
    } else {
        r = float3(c.r * 0.9 + c.g * 0.4, c.r * 0.6 + c.g * 0.5, c.b * 0.4);
        r = lerp(r, float3(1.0, 0.75, 0.25) * (0.5 + luma(c) * 1.5), green * fol);
    }
    float glow = prm(2) / 100;
    float3 blurish = (srgbDec(img(i.uv + uInv * 3)) + srgbDec(img(i.uv - uInv * 3)) + srgbDec(img(i.uv + float2(uInv.x, -uInv.y) * 3)) + srgbDec(img(i.uv + float2(-uInv.x, uInv.y) * 3))) * 0.25;
    r = 1 - (1 - r) * (1 - luma(blurish) * glow * 0.5);
    return float4(srgbEnc(saturate(r)), 1);
}
)HLSL",
// ---------------------------------------------------------------- THERMAL
R"HLSL(
float3 ramp(float t, int p) {
    t = saturate(t);
    if (p == 0) { // iron
        float3 a = float3(0, 0, 0), b = float3(0.3, 0, 0.55), c = float3(0.85, 0.1, 0.35), d = float3(1, 0.55, 0), e = float3(1, 1, 0.75);
        if (t < 0.25) return lerp(a, b, t / 0.25); if (t < 0.5) return lerp(b, c, (t - 0.25) / 0.25);
        if (t < 0.75) return lerp(c, d, (t - 0.5) / 0.25); return lerp(d, e, (t - 0.75) / 0.25);
    }
    if (p == 1) return hsv2rgb(float3((1 - t) * 0.7, 1, 0.3 + t * 0.7));
    if (p == 2) return float3(0.1, 1, 0.2) * (0.05 + t * 1.1);
    return lerp(float3(0, 0.05, 0.2), float3(0.85, 1, 1), t) * float3(0.8 + t * 0.2, 1, 1);
}
float4 main(VSO i) : SV_Target {
    float3 c = img(i.uv);
    float L = saturate(luma(c));
    float a = exp(prm(1) / 100 * 1.5);
    L = L < 0.5 ? 0.5 * pow(L * 2, a) : 1 - 0.5 * pow((1 - L) * 2, a);
    int p = (int)prm(0);
    float3 r = ramp(L, p);
    if (p == 2) {
        r += (hash12(i.uv * uSize + X.x) - 0.5) * 0.15;
        float2 v = i.uv - 0.5; r *= 1 - dot(v, v) * 1.8;
        r *= 0.9 + 0.1 * sin(i.uv.y * uSize.y * 1.5);
    }
    return float4(saturate(r), 1);
}
)HLSL",
// ---------------------------------------------------------------- LIGHT LEAK (outputs light, blended with Screen)
R"HLSL(
float4 main(VSO i) : SV_Target {
    float inten = prm(0) / 100, size = prm(1) / 100, var = prm(2) + X.x;
    int shape = (int)prm(3);
    float2 p = i.uv * float2(aspect(), 1);
    float3 acc = 0;
    if (shape == 2) {
        float x = i.uv.x + sin(i.uv.y * 3 + var) * 0.05;
        for (int k = 0; k < 4; k++) {
            float cx = hash12(float2(k, var)) ;
            float w = (0.02 + hash12(float2(k, var + 3)) * 0.12) * size * 2;
            float m = exp(-pow((x - cx) / w, 2));
            acc += lerp(col(0), col(1), hash12(float2(k, var + 9))) * m;
        }
    } else if (shape == 3) {
        for (int k = 0; k < 24; k++) {
            float2 c = hash22(float2(k, var)) * float2(aspect(), 1);
            float r = (0.02 + hash12(float2(k, var + 1)) * 0.06) * size * 2;
            float d = length(p - c);
            float m = (1 - smoothstep(r * 0.85, r, d)) * (0.5 + 0.5 * hash12(float2(k, var + 2)));
            acc += lerp(col(0), col(1), hash12(float2(k, var + 5))) * m * 0.6;
        }
    } else {
        for (int k = 0; k < 3; k++) {
            float2 h = hash22(float2(k, var));
            float2 c;
            if (shape == 1) c = float2(h.x < 0.5 ? 0 : aspect(), h.y < 0.5 ? 0 : 1) + (hash22(float2(k + 7, var)) - 0.5) * 0.3;
            else { float side = floor(h.x * 4); float t = h.y; c = side < 1 ? float2(0, t) : side < 2 ? float2(aspect(), t) : side < 3 ? float2(t * aspect(), 0) : float2(t * aspect(), 1); }
            float r = (0.25 + hash12(float2(k, var + 4)) * 0.5) * size * 1.6;
            float d = length(p - c) / r;
            float m = exp(-d * d * 2.5);
            float3 cc = lerp(col(1), col(0), saturate(d));
            acc += cc * m;
        }
    }
    float n = 0.85 + 0.3 * vnoise(i.uv * 6 + var);
    return float4(saturate(acc * inten * n), 1);
}
)HLSL",
// ---------------------------------------------------------------- POSTERIZE
R"HLSL(
float4 main(VSO i) : SV_Target {
    float3 c = img(i.uv);
    float lv = floor(prm(0));
    float L = luma(c);
    float qL = (floor(L * lv) + 0.5) / lv;
    float3 colored = c * (qL / max(L, 1e-3));
    float3 flat = floor(c * lv + 0.5) / lv;
    float3 r = lerp(flat, saturate(colored), prm(2) / 100);
    float lines = prm(1) / 100;
    if (lines > 0) {
        float2 o = uInv * max(1.0, 1.2 * UU() * uScale);
        float gx = luma(img(i.uv + float2(o.x, 0))) - luma(img(i.uv - float2(o.x, 0)));
        float gy = luma(img(i.uv + float2(0, o.y))) - luma(img(i.uv - float2(0, o.y)));
        float e = smoothstep(0.08, 0.25, length(float2(gx, gy)));
        r *= 1 - e * lines;
    }
    return float4(saturate(r), 1);
}
)HLSL",
// ---------------------------------------------------------------- PIXELATE
R"HLSL(
float4 main(VSO i) : SV_Target {
    float s = prm(0);
    int shape = (int)prm(1);
    float gap = prm(2) / 100;
    float2 q = toUnit(i.uv);
    float aa = pxUnit();
    if (shape == 1) {
        float2 r = float2(1, 1.7320508) * s;
        float2 h = r * 0.5;
        float2 a = fmod(q, r) - h, b = fmod(q - h, r) - h;
        if (a.x < -h.x) a += r; if (b.x < -h.x) b += r;
        float2 gv = dot(a, a) < dot(b, b) ? a : b;
        float2 center = q - gv;
        float3 c = img(fromUnit(center));
        float d = max(abs(gv.x) * 0.866 + abs(gv.y) * 0.5, abs(gv.y)) / (s * 0.5);
        float m = 1 - smoothstep(1 - gap - aa / s, 1 - gap + aa / s, d);
        return float4(lerp(col(0), c, m), 1);
    }
    float2 cell = floor(q / s);
    if (shape == 3) { float2 bq = q; bq.x += (fmod(floor(q.y / s), 2)) * s * 0.5; cell = floor(bq / s); q = bq; }
    float2 centerU = (cell + 0.5) * s;
    float2 uvc = fromUnit(shape == 3 ? centerU - float2(fmod(floor(q.y / s), 2) * s * 0.5, 0) : centerU);
    float3 c = img(uvc);
    float2 f = frac(q / s) - 0.5;
    float m;
    if (shape == 2) m = 1 - smoothstep(0.5 - gap * 0.5 - aa / s, 0.5 - gap * 0.5 + aa / s, length(f));
    else if (shape == 3) {
        float box = max(abs(f.x), abs(f.y));
        m = 1 - smoothstep(0.5 - gap * 0.3 - aa / s, 0.5 - gap * 0.3 + aa / s, box);
        float stud = length(f);
        float ring = smoothstep(0.22, 0.25, stud) * (1 - smoothstep(0.25, 0.29, stud));
        c *= 1 - ring * 0.35 * step(f.y, 0.0) + ring * 0.25 * step(0.0, f.y);
    }
    else { float box = max(abs(f.x), abs(f.y)); m = 1 - smoothstep(0.5 - gap * 0.5 - aa / s, 0.5 - gap * 0.5 + aa / s, box); }
    return float4(lerp(col(0), c, m), 1);
}
)HLSL",
// ---------------------------------------------------------------- VHS / CRT
R"HLSL(
float4 main(VSO i) : SV_Target {
    float2 uv = i.uv;
    float curv = prm(5) / 100;
    if (curv > 0) { float2 cc = uv - 0.5; float r2 = dot(cc, cc); uv = 0.5 + cc * (1 + r2 * curv * 0.6); if (any(uv < 0) || any(uv > 1)) return float4(0, 0, 0, 1); }
    float2 q = toUnit(uv);
    float jit = prm(3) / 100;
    float row = floor(q.y / 3);
    uv.x += (vnoise(float2(row * 0.05, X.x)) - 0.5) * jit * 0.01 + step(0.985, hash12(float2(floor(q.y / 40), X.x))) * jit * 0.02;
    float bleed = prm(1) / 100;
    float3 c = img(uv);
    float3 yiq = float3(dot(c, float3(0.299, 0.587, 0.114)), dot(c, float3(0.596, -0.274, -0.322)), dot(c, float3(0.211, -0.523, 0.312)));
    float2 ch = 0;
    for (int k = 1; k <= 6; k++) {
        float3 s = img(uv - float2(uInv.x * k * bleed * 4 * UU() * uScale, 0));
        ch += float2(dot(s, float3(0.596, -0.274, -0.322)), dot(s, float3(0.211, -0.523, 0.312)));
    }
    yiq.yz = lerp(yiq.yz, ch / 6, bleed);
    c = float3(yiq.x + 0.956 * yiq.y + 0.621 * yiq.z, yiq.x - 0.272 * yiq.y - 0.647 * yiq.z, yiq.x - 1.106 * yiq.y + 1.703 * yiq.z);
    float sc = prm(0) / 100;
    c *= 1 - sc * 0.45 * (0.5 + 0.5 * cos(q.y * PI * 2 / 3));
    float nz = prm(2) / 100;
    c += (hash12(floor(q * float2(0.5, 1)) + X.x) - 0.5) * nz * 0.25;
    c += step(0.995 - nz * 0.01, hash12(float2(floor(q.y), X.x + 5))) * nz * 0.5;
    float mask = prm(4) / 100;
    if (mask > 0) {
        int m = (int)fmod(floor(q.x / 1.5), 3);
        float3 ph = m == 0 ? float3(1, 0.3, 0.3) : (m == 1 ? float3(0.3, 1, 0.3) : float3(0.3, 0.3, 1));
        c *= lerp(1, ph * 1.6, mask);
    }
    float2 v = uv - 0.5; c *= 1 - dot(v, v) * curv * 1.5;
    return float4(saturate(c), 1);
}
)HLSL",
// ---------------------------------------------------------------- KALEIDO
R"HLSL(
float4 main(VSO i) : SV_Target {
    int mode = (int)prm(0);
    float2 uv = i.uv;
    float zoom = prm(3) / 100;
    if (mode == 0) uv.x = uv.x > 0.5 ? 1 - uv.x : uv.x;
    else if (mode == 1) uv.y = uv.y > 0.5 ? 1 - uv.y : uv.y;
    else if (mode == 2) { uv.x = uv.x > 0.5 ? 1 - uv.x : uv.x; uv.y = uv.y > 0.5 ? 1 - uv.y : uv.y; }
    else {
        float2 p = (uv - 0.5) * float2(aspect(), 1) / zoom;
        float seg = 2 * PI / floor(prm(1));
        float a = atan2(p.y, p.x) + radians(prm(2));
        float r = length(p);
        a = fmod(abs(a), seg); if (a > seg * 0.5) a = seg - a;
        p = float2(cos(a), sin(a)) * r;
        uv = p / float2(aspect(), 1) + 0.5;
        return float4(imgM(uv), 1);
    }
    uv = (uv - 0.5) / zoom + 0.5;
    return float4(imgM(uv), 1);
}
)HLSL",
// ---------------------------------------------------------------- TILT SHIFT (t1 small blur, t2 big blur)
R"HLSL(
float4 main(VSO i) : SV_Target {
    float2 p = (i.uv - 0.5) * float2(aspect(), 1);
    float a = radians(prm(4));
    float d = abs(rot2(p, -a).y - (prm(0) / 100 - 0.5));
    float w = prm(1) / 200, tr = prm(2) / 100 * 0.5 + 0.01;
    float f = saturate((d - w) / tr);
    float3 c = img(i.uv), b1 = t1.SampleLevel(sLin, i.uv, 0).rgb, b2 = t2.SampleLevel(sLin, i.uv, 0).rgb;
    float3 r = f < 0.5 ? lerp(c, b1, f * 2) : lerp(b1, b2, f * 2 - 1);
    float s = prm(5) / 100;
    r = lerp(luma(r).xxx, r, 1 + s * 0.8);
    return float4(saturate(r), 1);
}
)HLSL",
// ---------------------------------------------------------------- WAVE
R"HLSL(
float4 main(VSO i) : SV_Target {
    int mode = (int)prm(0);
    float amp = prm(1) / 100, freq = prm(2), ang = radians(prm(3)), ph = radians(prm(4));
    float2 uv = i.uv;
    float2 p = (uv - 0.5) * float2(aspect(), 1);
    if (mode == 0) { float2 dir = float2(cos(ang), sin(ang)); float t = dot(p, dir); float2 perp = float2(-dir.y, dir.x); p += perp * sin(t * freq * 2 * PI + ph) * amp * 0.05; }
    else if (mode == 1) { float r = length(p); p += normalize(p + 1e-5) * sin(r * freq * 4 * PI - ph) * amp * 0.02; }
    else if (mode == 2) { float r = length(p); float s = amp * 6 * saturate(1 - r / 0.6); s *= s; p = rot2(p, s + ph * 0.1); }
    else { float2 q = toUnit(uv); p.x += (vnoise(float2(q.y * 0.02 * freq, X.x + ph)) - 0.5) * amp * 0.08; }
    uv = p / float2(aspect(), 1) + 0.5;
    return float4(imgM(uv), 1);
}
)HLSL",
// ---------------------------------------------------------------- HUE SHIFT
R"HLSL(
float4 main(VSO i) : SV_Target {
    float3 c = img(i.uv);
    float3 h = rgb2hsv(c);
    if (prm(3) > 0.5) { float3 t = rgb2hsv(col(0)); h.x = t.x; h.y = max(h.y, t.y * 0.8); }
    h.x = frac(h.x + prm(0) / 360);
    h.y = saturate(h.y * (1 + prm(1) / 100));
    float3 r = hsv2rgb(h);
    float l = prm(2) / 100;
    r = l > 0 ? lerp(r, 1, l * 0.8) : r * (1 + l * 0.8);
    return float4(saturate(r), 1);
}
)HLSL",
// ---------------------------------------------------------------- XEROX
R"HLSL(
float4 main(VSO i) : SV_Target {
    float2 q = toUnit(i.uv);
    float L = luma(img(i.uv));
    float Lb = luma(img(i.uv + uInv * 4)) * 0.25 + luma(img(i.uv - uInv * 4)) * 0.25 + luma(img(i.uv + float2(uInv.x, -uInv.y) * 4)) * 0.25 + luma(img(i.uv + float2(-uInv.x, uInv.y) * 4)) * 0.25;
    L = L + (L - Lb) * 1.5;
    float th = prm(0) / 100, nz = prm(1) / 100, hard = prm(2) / 100;
    L += (fbm(q * 0.8 + X.x, 3) - 0.5) * nz * 0.5 + (hash12(q + X.x) - 0.5) * nz * 0.3;
    float bw = 0.5 * (1 - hard) + 0.005;
    float ink = 1 - smoothstep(th - bw, th + bw, L);
    float streak = prm(3) / 100 * (vnoise(float2(q.x * 0.05, X.x)) > 0.75 ? 0.4 : 0) * vnoise(float2(q.x * 0.5, q.y * 0.002));
    ink = saturate(ink + streak);
    return float4(lerp(col(1), col(0), ink), 1);
}
)HLSL",
};

std::string FxShaderSource(int type) {
    return std::string(kFxHeader) + kFx[type];
}

std::string FxCombineSource() {
    return R"HLSL(
cbuffer CB : register(b1) {
    float opacity; float blend; float target; float invert;
    float lo; float hi; float feather; float hueC;
    float hueW; float hueF; float satMin; float _p;
};
float4 main(VSO i) : SV_Target {
    float3 a = t0.SampleLevel(sPt, i.uv, 0).rgb;
    float3 b = t1.SampleLevel(sPt, i.uv, 0).rgb;
    float w = 1;
    if (target > 0.5 && target < 1.5) {
        float L = luma(a); float f = max(feather, 1e-3);
        w = smoothstep(lo - f, lo, L) * (1 - smoothstep(hi, hi + f, L));
    } else if (target > 1.5 && target < 2.5) {
        float3 h = rgb2hsv(a);
        float d = abs(frac(h.x - hueC / 360 + 0.5) - 0.5) * 360;
        w = 1 - smoothstep(hueW * 0.5, hueW * 0.5 + hueF + 1e-3, d);
        w *= smoothstep(satMin * 0.5, satMin + 1e-3, h.y);
    } else if (target > 2.5) {
        w = t2.SampleLevel(sLin, i.uv, 0).r;
    }
    if (invert > 0.5) w = 1 - w;
    float3 r = saturate(blendMode(a, b, (int)blend));
    return float4(lerp(a, r, saturate(w * opacity)), 1);
}
)HLSL";
}
