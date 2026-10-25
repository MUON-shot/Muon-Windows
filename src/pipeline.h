#pragma once
#include "gpu.h"
#include "params.h"
#include <map>
#include <string>
#include <vector>

struct LoadedImage;

struct Geo {
    int canvasW = 0, canvasH = 0;              // oriented source size (px)
    float cx0 = 0, cy0 = 0, cw = 1, ch = 1;    // crop rect, canvas-normalized
    int fullW = 0, fullH = 0;                  // crop size at full resolution
};

struct RenderOpts {
    bool before = false;        // show unedited image (geometry kept)
    bool cropOverride = false;  // ignore crop (crop tool shows whole canvas)
    int overlayMask = -1;       // draw this mask's weight as a red overlay
    bool clipWarn = false;
    int altView = 0;            // 1 highlights clipping view, 2 shadows clipping view
};

class Pipeline {
public:
    bool init();
    void shutdown();
    void setSource(const LoadedImage& img);
    bool hasSource() const { return src != nullptr; }
    bool isRaw() const { return srcIsRaw; }

    Geo geometry(const Params& p, bool cropOverride) const;

    // Render for the screen. Result lands in `display` (RGBA8) and the histogram is refreshed.
    void renderDisplay(const Params& p, int workLong, const RenderOpts& o);
    // Render for export at an exact size (0 = full resolution); returns 16-bit RGBA rows.
    bool renderExport(const Params& p, int outW, int outH, float outSharpen, std::vector<uint16_t>& rgba, int& w, int& h);

    // Average of the linear source (after geometry, before edits) around output uv.
    bool sampleSource(float u, float v, float rgb[3]);
    // Developed (display) color around output uv.
    bool sampleDeveloped(float u, float v, float rgb[3]);
    // Nearness (0..1) of the aligned depth map around output uv; false when no depth was used in the last render.
    bool sampleDepth(float u, float v, float& n);

    // Depth map in source orientation (nearness 0..1). Without it a flat stand-in is used.
    void setDepth(const std::vector<float>& d, int w, int h);
    void clearDepth();
    bool hasDepth() const { return depthTex != nullptr; }
    bool needsDepth(const Params& p) const;

    Tex* display = nullptr;
    int dispW = 0, dispH = 0;
    float hist[4][256] = {};
    double lastRenderMs = 0;
    int srcW = 0, srcH = 0;

private:
    Tex* renderCore(const Params& p, const Geo& g, int W, int H, const RenderOpts& o, std::vector<Tex*>& keep);
    Tex* blur(Tex* in, float sx, float sy, DXGI_FORMAT fmt);
    Tex* applyEffect(const Effect& e, Tex* in, int W, int H, const std::vector<Tex*>& maskW);
    Tex* brushTexture(const Mask& m, const Geo& g);
    Tex* glyphAtlas(const std::string& chars, int& count);
    void updateLut(const Params& p, bool& identity);
    void computeHistogram(Tex* t);
    void releaseKeep();
    void drawDabs(Tex* target, const std::vector<Dab>& dabs, size_t from, float aspect, ID3D11PixelShader* ps, ID3D11BlendState* blend);
    bool initOptics();
    void shutdownOptics();
    bool opticsActive(const Params& p) const;
    Tex* depthEditTexture(const Params& p, const Geo& g);
    Tex* refineDepth(const Params& p, const Geo& g, Tex* aligned, Tex* guide, int W, int H);
    Tex* applyOptics(const Params& p, const Geo& g, Tex* cur, Tex* depth, int W, int H);

    Tex* src = nullptr;
    bool srcIsRaw = false;
    Tex* lut = nullptr;
    Tex* histSmall = nullptr;

    ID3D11PixelShader *psGeo = nullptr, *psLumPrep = nullptr, *psDown = nullptr, *psBlur = nullptr, *psDenoise = nullptr, *psNrPrep = nullptr,
                      *psDevelop = nullptr, *psMaskW = nullptr, *psLocal = nullptr, *psSharpen = nullptr, *psFinish = nullptr,
                      *psOutput = nullptr, *psDab = nullptr, *psCombine = nullptr, *psConvert = nullptr, *psDispLum = nullptr;
    ID3D11PixelShader* psFx[FX_COUNT] = {};
    ID3D11PixelShader *psDepthRefine = nullptr, *psDabAdd = nullptr, *psDepthView = nullptr, *psFogDof = nullptr,
                      *psDepthMax = nullptr, *psLightsLow = nullptr, *psLightsFull = nullptr, *psGlass = nullptr;

    struct BrushCache { Tex* tex = nullptr; uint32_t rev = 0; size_t count = 0; int w = 0, h = 0; };
    std::map<uint32_t, BrushCache> brushes;
    std::map<std::string, std::pair<Tex*, int>> glyphs;
    BrushCache depthEdit;
    Tex* depthTex = nullptr;
    Tex* flatDepth = nullptr;
    int depthW_ = 0, depthH_ = 0;
    Tex* lastDepth = nullptr;

    std::vector<Tex*> keep;   // textures alive until the next render (geo/dev/masks for picking & overlays)
    Tex* lastGeo = nullptr;
    Tex* lastDev = nullptr;
    std::vector<Tex*> lastMaskW;
    std::vector<Tex*> retired;   // old display textures, freed one render later
};

extern Pipeline g_pipe;
