// Headless check: runs every processing stage and effect on one image and writes the results.
#include "app.h"
#include "pipeline.h"
#include "effects.h"
#include "imageio.h"
#include "preset.h"
#include <chrono>
#include <cstdio>
#include <algorithm>

static void Out(const std::wstring& dir, const char* name, const Params& p) {
    auto t0 = std::chrono::high_resolution_clock::now();
    std::vector<uint16_t> rgba; int w = 0, h = 0;
    Geo g = g_pipe.geometry(p, false);
    float k = std::min(1.0f, 1400.0f / std::max(g.fullW, g.fullH));
    bool ok = g_pipe.renderExport(p, (int)(g.fullW * k), (int)(g.fullH * k), 0, rgba, w, h);
    auto t1 = std::chrono::high_resolution_clock::now();
    double ms = std::chrono::duration<double, std::milli>(t1 - t0).count();
    EncodeOptions eo; eo.quality = 88;
    bool saved = ok && SaveImage(dir + L"\\" + Utf8ToWide(name) + L".jpg", eo, rgba, w, h);
    char buf[256];
    snprintf(buf, sizeof(buf), "%-28s %s  %dx%d  %.1f ms", name, saved ? "ok" : "FAIL", w, h, ms);
    LogLine(buf);
}

int RunSelfTest(const std::wstring& in, const std::wstring& dir) {
    LoadedImage img;
    auto t0 = std::chrono::high_resolution_clock::now();
    if (!LoadImageFile(in, img)) { LogLine("load failed: " + img.error); return 2; }
    auto t1 = std::chrono::high_resolution_clock::now();
    LogLine("loaded " + std::to_string(img.w) + "x" + std::to_string(img.h) + (img.isRaw ? " RAW " : " RGB ") + img.info +
            "  " + std::to_string((int)std::chrono::duration<double, std::milli>(t1 - t0).count()) + " ms");
    g_pipe.setSource(img);

    Params base;
    Out(dir, "00_default", base);
    { Params p; p.exposure = 0.5f; p.contrast = 30; p.highlights = -60; p.shadows = 60; p.whites = 10; p.blacks = -10; Out(dir, "01_tone", p); }
    { Params p; p.clarity = 60; p.texture = 50; p.dehaze = 40; Out(dir, "02_clarity_dehaze", p); }
    { Params p; p.temp = 35; p.tint = 15; p.vibrance = 50; p.saturation = 20; Out(dir, "03_wb_color", p); }
    { Params p; p.curve[0].pts = { {0, 0}, {0.25f, 0.18f}, {0.75f, 0.85f}, {1, 1} }; p.curve[3].pts = { {0, 0.1f}, {1, 0.9f} };
      p.hslH[3] = 80; p.hslS[5] = 60; p.hslL[1] = 30; p.grade[0] = { 200, 40, 0 }; p.grade[2] = { 40, 40, 0 }; Out(dir, "04_curve_hsl_grade", p); }
    { Params p; p.bw = true; p.bwMix[5] = -80; p.bwMix[0] = 50; p.contrast = 20; Out(dir, "05_bw", p); }
    { Params p; p.sharpAmount = 120; p.nrLuma = 60; p.nrColor = 60; Out(dir, "06_detail", p); }
    { Params p; p.rot90 = 1; p.straighten = 8; p.perspV = 30; p.cropX0 = 0.1f; p.cropY0 = 0.1f; p.cropX1 = 0.9f; p.cropY1 = 0.8f; p.lensDist = 40; p.lensCA = 50; Out(dir, "07_geometry", p); }
    { Params p; p.vigAmount = -60; p.grainAmount = 60; p.grainSize = 50; Out(dir, "08_vignette_grain", p); }
    { Params p; p.profile = 2; Out(dir, "09_profile_vivid", p); }
    {
        Params p;
        Mask lin = MakeMask(MS_LINEAR, MR_NONE); lin.ly0 = 0; lin.ly1 = 0.5f; lin.adj.exposure = -1.5f; lin.adj.temp = -40; p.masks.push_back(lin);
        Mask rad = MakeMask(MS_RADIAL, MR_NONE); rad.rx = 0.2f; rad.ry = 0.3f; rad.adj.exposure = 1.2f; rad.adj.saturation = 50; p.masks.push_back(rad);
        Mask br = MakeMask(MS_BRUSH, MR_NONE);
        for (int i = 0; i < 200; i++) br.dabs.push_back(Dab{ 0.1f + i * 0.004f, 0.8f, 0.05f, 0.5f, 0.5f, 0 });
        br.adj.colorAmt = 100; br.adj.color[0] = 0; br.adj.color[1] = 1; br.adj.color[2] = 0; p.masks.push_back(br);
        Mask col = MakeMask(MS_FULL, MR_COLOR); col.hueC = 210; col.hueWidth = 60; col.adj.hue = 120; p.masks.push_back(col);
        Mask lum = MakeMask(MS_FULL, MR_LUMA); lum.lumLo = 0; lum.lumHi = 0.3f; lum.adj.exposure = 1; p.masks.push_back(lum);
        Out(dir, "10_masks", p);
    }
    for (int t = 0; t < FX_COUNT; t++) {
        Params p;
        p.effects.push_back(MakeEffect(t));
        char name[64]; snprintf(name, sizeof(name), "fx_%02d", t);
        Out(dir, name, p);
    }
    {
        Params p;
        Effect e = MakeEffect(FX_ASCII); e.target = TG_LUMA; e.tLo = 0; e.tHi = 0.4f; p.effects.push_back(e);
        Effect n = MakeEffect(FX_NEON); n.target = TG_COLOR; n.tHue = 210; p.effects.push_back(n);
        Out(dir, "fx_combo_targets", p);
    }
    // round trip serialization
    {
        Params p; p.exposure = 1.25f; p.masks.push_back(MakeMask(MS_RADIAL, MR_COLOR)); p.effects.push_back(MakeEffect(FX_GLITCH));
        p.curve[1].pts = { {0, 0}, {0.5f, 0.7f}, {1, 1} };
        std::string s = SerializeParams(p, true, true);
        Params q;
        bool ok = DeserializeParams(s, q, true, true) && SerializeParams(q, true, true) == s;
        LogLine(std::string("serialize roundtrip ") + (ok ? "ok" : "FAIL"));
    }
    // interactive-size preview timing
    {
        Params p; p.clarity = 30; p.shadows = 40;
        RenderOpts o;
        auto a = std::chrono::high_resolution_clock::now();
        for (int i = 0; i < 10; i++) g_pipe.renderDisplay(p, 1600, o);
        auto b = std::chrono::high_resolution_clock::now();
        char buf[128]; snprintf(buf, sizeof(buf), "preview 1600px avg %.1f ms", std::chrono::duration<double, std::milli>(b - a).count() / 10);
        LogLine(buf);
        a = std::chrono::high_resolution_clock::now();
        for (int i = 0; i < 5; i++) g_pipe.renderDisplay(p, 100000, o);
        b = std::chrono::high_resolution_clock::now();
        snprintf(buf, sizeof(buf), "preview full-res avg %.1f ms", std::chrono::duration<double, std::milli>(b - a).count() / 5);
        LogLine(buf);
    }
    // export options: size limit, 16-bit TIFF, watermark, output sharpening
    {
        Params p; p.clarity = 20;
        std::vector<uint16_t> rgba; int w = 0, h = 0;
        Geo g = g_pipe.geometry(p, false);
        int H = (int)(1200.0f * g.fullH / g.fullW);
        g_pipe.renderExport(p, 1200, H, 0.45f, rgba, w, h);
        ApplyWatermark(rgba, w, h, L"© Muon test", 4, 0.8f, 8, false);
        EncodeOptions eo; eo.maxKB = 150; eo.quality = 95;
        int q = 0;
        bool ok1 = SaveImage(dir + L"\\exp_limit150.jpg", eo, rgba, w, h, &q);
        EncodeOptions t; t.fmt = EXP_TIFF; t.bits16 = true; t.tiffComp = 2;
        bool ok2 = SaveImage(dir + L"\\exp_16bit.tif", t, rgba, w, h);
        EncodeOptions pn; pn.fmt = EXP_PNG; pn.bits16 = true;
        bool ok3 = SaveImage(dir + L"\\exp_16bit.png", pn, rgba, w, h);
        WIN32_FILE_ATTRIBUTE_DATA fa = {};
        GetFileAttributesExW((dir + L"\\exp_limit150.jpg").c_str(), GetFileExInfoStandard, &fa);
        char buf[200]; snprintf(buf, sizeof(buf), "export options: jpeg<=150KB %s (q=%d, %lu KB), tiff16 %s, png16 %s", ok1 ? "ok" : "FAIL", q, fa.nFileSizeLow / 1024, ok2 ? "ok" : "FAIL", ok3 ? "ok" : "FAIL");
        LogLine(buf);
    }
    // presets: text file and share code round trips, group filtering
    {
        Preset pr;
        pr.name = "Тест «кавычки»\\слэш";
        pr.author = "Muon";
        pr.description = "line1\nline2";
        pr.groups = SG_TONE | SG_HSL | SG_EFFECTS;
        pr.params.contrast = 33; pr.params.exposure = 2; pr.params.hslS[3] = -40;
        pr.params.effects.push_back(MakeEffect(FX_GLITCH));
        std::string txt = PresetToText(pr);
        Preset back;
        bool ok1 = PresetFromText(txt, back) && back.name == pr.name && back.description == pr.description && back.groups == pr.groups &&
                   back.params.contrast == 33 && back.params.exposure == 0 && back.params.hslS[3] == -40 && back.params.effects.size() == 1;
        std::string code = PresetToCode(pr);
        Preset fromCode;
        std::string wrapped = "look at this: " + code.substr(0, 40) + "\n" + code.substr(40) + " ok?";
        bool ok2 = PresetFromCode(wrapped, fromCode) && fromCode.params.contrast == 33 && fromCode.name == pr.name;
        std::string broken = code.substr(0, code.size() - 8);
        Preset bad;
        bool ok3 = !PresetFromCode(broken, bad);
        Params target; target.exposure = 1.5f; target.contrast = -10;
        CopyGroups(target, back.params, back.groups);
        bool ok4 = target.exposure == 1.5f && target.contrast == 33;
        char buf[200];
        snprintf(buf, sizeof(buf), "presets: text %s, code %s (%d chars), damaged code rejected %s, groups %s",
                 ok1 ? "ok" : "FAIL", ok2 ? "ok" : "FAIL", (int)code.size(), ok3 ? "ok" : "FAIL", ok4 ? "ok" : "FAIL");
        LogLine(buf);
    }
    // noise reduction (non-local means)
    {
        Params p; p.nrLuma = 70; p.nrColor = 60;
        Out(dir, "11_noise_reduction", p);
        RenderOpts o;
        auto a = std::chrono::high_resolution_clock::now();
        for (int i = 0; i < 5; i++) g_pipe.renderDisplay(p, 100000, o);
        auto b = std::chrono::high_resolution_clock::now();
        char buf[128]; snprintf(buf, sizeof(buf), "noise reduction full-res avg %.1f ms", std::chrono::duration<double, std::milli>(b - a).count() / 5);
        LogLine(buf);
    }
    // video memory must stay bounded while the crop / zoom changes (this used to grow until a crash)
    {
        Params p; p.clarity = 30; p.texture = 20; p.nrColor = 30;
        RenderOpts o;
        size_t peak = 0;
        for (int i = 0; i < 300; i++) {
            p.cropX0 = 0.05f + (i % 37) * 0.004f;
            p.cropY1 = 0.95f - (i % 23) * 0.006f;
            g_pipe.renderDisplay(p, 1600 + (i % 5) * 400, o);
            peak = std::max(peak, g_gpu.poolBytes(false));
        }
        char buf[160];
        snprintf(buf, sizeof(buf), "pool after 300 crop changes: %zu MB (peak %zu MB) %s", g_gpu.poolBytes(false) >> 20, peak >> 20,
                 peak < ((size_t)1200 << 20) ? "ok" : "FAIL");
        LogLine(buf);
    }
    LogLine("done");
    return 0;
}
