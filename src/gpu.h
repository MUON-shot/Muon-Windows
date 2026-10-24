#pragma once
#include <d3d11.h>
#include <dxgi.h>
#include <vector>
#include <string>
#include <initializer_list>
#include <cstdint>

// A 2D GPU texture that can be sampled and rendered into.
struct Tex {
    ID3D11Texture2D* tex = nullptr;
    ID3D11ShaderResourceView* srv = nullptr;
    ID3D11RenderTargetView* rtv = nullptr;
    int w = 0, h = 0;
    DXGI_FORMAT fmt = DXGI_FORMAT_UNKNOWN;
    bool pooled = false;
    bool inUse = false;
    bool dummy = false;      // 1x1 stand-in handed out when video memory runs out
    int lastUse = 0;
    void release();
};

// Constant block every pass receives at register b0.
struct CommonCB {
    float size[2];     // output size in pixels
    float inv[2];      // 1/size
    float full[2];     // full-resolution (export) size of the current frame, for resolution independent effects
    float scale;       // working px / full px
    float seed;
    float time;
    float pad[3];
};

struct Gpu {
    ID3D11Device* dev = nullptr;
    ID3D11DeviceContext* ctx = nullptr;

    ID3D11VertexShader* vsFull = nullptr;
    ID3D11VertexShader* vsDab = nullptr;
    ID3D11SamplerState* samp[5] = {};   // s0 linear clamp, s1 point clamp, s2 linear wrap, s3 aniso border, s4 linear mirror
    ID3D11Buffer* cbCommon = nullptr;
    ID3D11Buffer* cbPass = nullptr;     // 16 KB dynamic
    ID3D11BlendState* blendAlpha = nullptr;
    ID3D11RasterizerState* raster = nullptr;

    CommonCB common{};
    std::vector<Tex*> pool;
    std::vector<Tex*> dummies;
    int frame = 0;               // advanced once per render; used to age the pool
    bool outOfMemory = false;    // set when an allocation failed even after freeing the pool

    bool init(ID3D11Device* d, ID3D11DeviceContext* c);
    void shutdown();

    // Shader compilation (HLSL source is prefixed with the common header).
    ID3D11PixelShader* compilePS(const char* name, const std::string& body);
    ID3D11VertexShader* compileVS(const char* name, const std::string& src);

    Tex* create(int w, int h, DXGI_FORMAT fmt, bool rt = true, const void* data = nullptr, int pitch = 0, bool mips = false);
    Tex* acquire(int w, int h, DXGI_FORMAT fmt = DXGI_FORMAT_R16G16B16A16_FLOAT);
    void release(Tex*& t);
    void trimPool(int maxAge = 0);   // frees idle pooled textures not used in the last maxAge renders
    size_t poolBytes(bool idleOnly) const;

    // Run a fullscreen pixel shader into `out`.
    void pass(ID3D11PixelShader* ps, Tex* out, std::initializer_list<ID3D11ShaderResourceView*> srvs,
              const void* cb = nullptr, size_t cbSize = 0, ID3D11BlendState* blend = nullptr);
    void clear(Tex* t, float r, float g, float b, float a);

    // Read back a box of pixels as float4 (converts from the texture format).
    bool readPixels(Tex* t, int x, int y, int w, int h, std::vector<float>& out);
    // Read the whole texture into memory (raw bytes, row pitch = w * bpp).
    bool readTexture(Tex* t, std::vector<uint8_t>& out, int& bpp);

    void unbindAll();
};

extern Gpu g_gpu;
extern bool g_quietErrors;      // log shader errors instead of showing a message box
extern std::string g_logFile;
void LogLine(const std::string& s);
std::string HlslCommon();
