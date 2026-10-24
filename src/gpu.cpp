#include "gpu.h"
#include <d3dcompiler.h>
#include <cstring>
#include <cmath>
#include <algorithm>
#include <cstdio>

Gpu g_gpu;
bool g_quietErrors = false;
std::string g_logFile;

void LogLine(const std::string& s) {
    OutputDebugStringA((s + "\n").c_str());
    if (g_logFile.empty()) return;
    FILE* f = fopen(g_logFile.c_str(), "ab");
    if (f) { fwrite(s.data(), 1, s.size(), f); fwrite("\n", 1, 1, f); fclose(f); }
}

template <class T> static void SafeRelease(T*& p) { if (p) { p->Release(); p = nullptr; } }

void Tex::release() {
    SafeRelease(rtv); SafeRelease(srv); SafeRelease(tex);
}

static const char* kCommonHlsl = R"HLSL(
cbuffer Common : register(b0) {
    float2 uSize; float2 uInv; float2 uFull; float uScale; float uSeed; float uTime; float3 _cpad;
};
Texture2D t0 : register(t0); Texture2D t1 : register(t1); Texture2D t2 : register(t2);
Texture2D t3 : register(t3); Texture2D t4 : register(t4); Texture2D t5 : register(t5);
SamplerState sLin : register(s0); SamplerState sPt : register(s1); SamplerState sWrap : register(s2);
SamplerState sAniso : register(s3); SamplerState sMirror : register(s4);
struct VSO { float4 pos : SV_Position; float2 uv : TEXCOORD0; };
static const float PI = 3.14159265;
float luma(float3 c) { return dot(c, float3(0.2126, 0.7152, 0.0722)); }
float srgbEnc1(float x) { x = max(x, 0); return x <= 0.0031308 ? 12.92 * x : 1.055 * pow(x, 1.0/2.4) - 0.055; }
float srgbDec1(float x) { x = max(x, 0); return x <= 0.04045 ? x / 12.92 : pow((x + 0.055) / 1.055, 2.4); }
float3 srgbEnc(float3 c) { return float3(srgbEnc1(c.r), srgbEnc1(c.g), srgbEnc1(c.b)); }
float3 srgbDec(float3 c) { return float3(srgbDec1(c.r), srgbDec1(c.g), srgbDec1(c.b)); }
float3 rgb2hsv(float3 c) {
    float4 K = float4(0.0, -1.0/3.0, 2.0/3.0, -1.0);
    float4 p = c.g < c.b ? float4(c.bg, K.wz) : float4(c.gb, K.xy);
    float4 q = c.r < p.x ? float4(p.xyw, c.r) : float4(c.r, p.yzx);
    float d = q.x - min(q.w, q.y);
    float e = 1.0e-10;
    return float3(abs(q.z + (q.w - q.y) / (6.0 * d + e)), d / (q.x + e), q.x);
}
float3 hsv2rgb(float3 c) {
    float3 p = abs(frac(c.xxx + float3(1.0, 2.0/3.0, 1.0/3.0)) * 6.0 - 3.0);
    return c.z * lerp(float3(1,1,1), saturate(p - 1.0), c.y);
}
float hash12(float2 p) { float3 p3 = frac(float3(p.xyx) * 0.1031); p3 += dot(p3, p3.yzx + 33.33); return frac((p3.x + p3.y) * p3.z); }
float2 hash22(float2 p) { float3 p3 = frac(float3(p.xyx) * float3(0.1031, 0.1030, 0.0973)); p3 += dot(p3, p3.yzx + 33.33); return frac((p3.xx + p3.yz) * p3.zy); }
float3 hash32(float2 p) { float3 p3 = frac(float3(p.xyx) * float3(0.1031, 0.1030, 0.0973)); p3 += dot(p3, p3.yxz + 33.33); return frac((p3.xxy + p3.yzz) * p3.zyx); }
float vnoise(float2 p) {
    float2 i = floor(p), f = frac(p); float2 u = f * f * (3.0 - 2.0 * f);
    float a = hash12(i), b = hash12(i + float2(1,0)), c = hash12(i + float2(0,1)), d = hash12(i + float2(1,1));
    return lerp(lerp(a, b, u.x), lerp(c, d, u.x), u.y);
}
float gnoise(float2 p) { // approx gaussian-ish noise in [-1,1]
    return (hash12(p) + hash12(p + 17.3) + hash12(p + 41.7) - 1.5) * 1.1547;
}
float fbm(float2 p, int oct) { float s = 0, a = 0.5; for (int i = 0; i < oct; i++) { s += a * vnoise(p); p = p * 2.03 + 11.7; a *= 0.5; } return s; }
float3 blendMode(float3 a, float3 b, int m) {
    if (m == 1) return 1 - (1 - a) * (1 - b);
    if (m == 2) return a * b;
    if (m == 3) return lerp(2 * a * b, 1 - 2 * (1 - a) * (1 - b), step(0.5, a));
    if (m == 4) return lerp(2 * a * b + a * a * (1 - 2 * b), sqrt(max(a, 0)) * (2 * b - 1) + 2 * a * (1 - b), step(0.5, b));
    if (m == 5) return a + b;
    if (m == 6) return abs(a - b);
    if (m == 7) { float3 hb = rgb2hsv(b); float3 ha = rgb2hsv(a); float3 r = hsv2rgb(float3(hb.x, hb.y, ha.z)); return r + (luma(a) - luma(r)); }
    if (m == 8) return a + (luma(b) - luma(a));
    if (m == 9) return max(a, b);
    if (m == 10) return min(a, b);
    return b;
}
float2 rot2(float2 p, float a) { float c = cos(a), s = sin(a); return float2(c * p.x - s * p.y, s * p.x + c * p.y); }
)HLSL";

std::string HlslCommon() { return kCommonHlsl; }

static const char* kVsFull = R"HLSL(
struct VSO { float4 pos : SV_Position; float2 uv : TEXCOORD0; };
VSO main(uint id : SV_VertexID) {
    VSO o; float2 uv = float2((id << 1) & 2, id & 2);
    o.uv = uv; o.pos = float4(uv * float2(2, -2) + float2(-1, 1), 0, 1); return o;
}
)HLSL";

// Brush dabs: one instanced quad per dab, dab data in b1.
static const char* kVsDab = R"HLSL(
cbuffer Dabs : register(b1) { float4 dabA[512]; float4 dabB[512]; float4 info; };
struct DO { float4 pos : SV_Position; float2 q : TEXCOORD0; float4 d : TEXCOORD1; };
DO main(uint vid : SV_VertexID, uint iid : SV_InstanceID) {
    static const float2 corners[6] = { float2(-1,-1), float2(1,-1), float2(-1,1), float2(-1,1), float2(1,-1), float2(1,1) };
    float4 a = dabA[iid]; float4 b = dabB[iid];
    float2 c = corners[vid];
    float2 p = a.xy + c * float2(a.z / info.x, a.z); // info.x = canvas aspect (w/h)
    DO o; o.pos = float4(p.x * 2 - 1, 1 - p.y * 2, 0, 1); o.q = c; o.d = float4(a.w, b.x, b.y, 0); return o;
}
)HLSL";

static ID3DBlob* Compile(const char* name, const std::string& src, const char* target) {
    ID3DBlob* blob = nullptr; ID3DBlob* err = nullptr;
    HRESULT hr = D3DCompile(src.data(), src.size(), name, nullptr, nullptr, "main", target,
                            D3DCOMPILE_OPTIMIZATION_LEVEL2, 0, &blob, &err);
    if (FAILED(hr)) {
        std::string msg = std::string("Shader ") + name + ":\n" + (err ? (const char*)err->GetBufferPointer() : "unknown error");
        LogLine(msg);
        if (!g_quietErrors) MessageBoxA(nullptr, msg.substr(0, 2000).c_str(), "Muon: shader error", MB_OK | MB_ICONERROR);
        if (err) err->Release();
        return nullptr;
    }
    if (err) err->Release();
    return blob;
}

ID3D11PixelShader* Gpu::compilePS(const char* name, const std::string& body) {
    std::string src = std::string(kCommonHlsl) + body;
    ID3DBlob* b = Compile(name, src, "ps_5_0");
    if (!b) return nullptr;
    ID3D11PixelShader* ps = nullptr;
    dev->CreatePixelShader(b->GetBufferPointer(), b->GetBufferSize(), nullptr, &ps);
    b->Release();
    return ps;
}

ID3D11VertexShader* Gpu::compileVS(const char* name, const std::string& src) {
    ID3DBlob* b = Compile(name, src, "vs_5_0");
    if (!b) return nullptr;
    ID3D11VertexShader* vs = nullptr;
    dev->CreateVertexShader(b->GetBufferPointer(), b->GetBufferSize(), nullptr, &vs);
    b->Release();
    return vs;
}

bool Gpu::init(ID3D11Device* d, ID3D11DeviceContext* c) {
    dev = d; ctx = c;
    vsFull = compileVS("vs_full", kVsFull);
    vsDab = compileVS("vs_dab", kVsDab);
    if (!vsFull || !vsDab) return false;

    D3D11_SAMPLER_DESC sd = {};
    sd.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    sd.AddressU = sd.AddressV = sd.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    sd.MaxLOD = D3D11_FLOAT32_MAX;
    dev->CreateSamplerState(&sd, &samp[0]);
    sd.Filter = D3D11_FILTER_MIN_MAG_MIP_POINT;
    dev->CreateSamplerState(&sd, &samp[1]);
    sd.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    sd.AddressU = sd.AddressV = sd.AddressW = D3D11_TEXTURE_ADDRESS_WRAP;
    dev->CreateSamplerState(&sd, &samp[2]);
    sd.Filter = D3D11_FILTER_ANISOTROPIC; sd.MaxAnisotropy = 8;
    sd.AddressU = sd.AddressV = sd.AddressW = D3D11_TEXTURE_ADDRESS_BORDER;
    sd.BorderColor[0] = sd.BorderColor[1] = sd.BorderColor[2] = sd.BorderColor[3] = 0;
    dev->CreateSamplerState(&sd, &samp[3]);
    sd.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR; sd.MaxAnisotropy = 1;
    sd.AddressU = sd.AddressV = sd.AddressW = D3D11_TEXTURE_ADDRESS_MIRROR;
    dev->CreateSamplerState(&sd, &samp[4]);

    D3D11_BUFFER_DESC bd = {};
    bd.Usage = D3D11_USAGE_DYNAMIC; bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER; bd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    bd.ByteWidth = sizeof(CommonCB);
    dev->CreateBuffer(&bd, nullptr, &cbCommon);
    bd.ByteWidth = 16 * 1024 + 256;
    dev->CreateBuffer(&bd, nullptr, &cbPass);

    D3D11_BLEND_DESC bl = {};
    bl.RenderTarget[0].BlendEnable = TRUE;
    bl.RenderTarget[0].SrcBlend = D3D11_BLEND_SRC_ALPHA; bl.RenderTarget[0].DestBlend = D3D11_BLEND_INV_SRC_ALPHA;
    bl.RenderTarget[0].BlendOp = D3D11_BLEND_OP_ADD;
    bl.RenderTarget[0].SrcBlendAlpha = D3D11_BLEND_ONE; bl.RenderTarget[0].DestBlendAlpha = D3D11_BLEND_INV_SRC_ALPHA;
    bl.RenderTarget[0].BlendOpAlpha = D3D11_BLEND_OP_ADD;
    bl.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
    dev->CreateBlendState(&bl, &blendAlpha);

    D3D11_RASTERIZER_DESC rd = {};
    rd.FillMode = D3D11_FILL_SOLID; rd.CullMode = D3D11_CULL_NONE; rd.DepthClipEnable = TRUE;
    dev->CreateRasterizerState(&rd, &raster);
    return true;
}

void Gpu::shutdown() {
    for (Tex* t : pool) { t->release(); delete t; }
    pool.clear();
    for (Tex* t : dummies) { t->release(); delete t; }
    dummies.clear();
    SafeRelease(vsFull); SafeRelease(vsDab);
    for (auto& s : samp) SafeRelease(s);
    SafeRelease(cbCommon); SafeRelease(cbPass); SafeRelease(blendAlpha); SafeRelease(raster);
}

Tex* Gpu::create(int w, int h, DXGI_FORMAT fmt, bool rt, const void* data, int pitch, bool mips) {
    Tex* t = new Tex();
    t->w = w; t->h = h; t->fmt = fmt;
    D3D11_TEXTURE2D_DESC td = {};
    td.Width = w; td.Height = h; td.MipLevels = mips ? 0 : 1; td.ArraySize = 1; td.Format = fmt;
    td.SampleDesc.Count = 1; td.Usage = D3D11_USAGE_DEFAULT;
    td.BindFlags = D3D11_BIND_SHADER_RESOURCE | ((rt || mips) ? D3D11_BIND_RENDER_TARGET : 0);
    td.MiscFlags = mips ? D3D11_RESOURCE_MISC_GENERATE_MIPS : 0;
    D3D11_SUBRESOURCE_DATA sd = { data, (UINT)pitch, 0 };
    HRESULT hr = dev->CreateTexture2D(&td, (data && !mips) ? &sd : nullptr, &t->tex);
    if (FAILED(hr)) { delete t; return nullptr; }
    dev->CreateShaderResourceView(t->tex, nullptr, &t->srv);
    if (rt || mips) dev->CreateRenderTargetView(t->tex, nullptr, &t->rtv);
    if (data && mips) {
        ctx->UpdateSubresource(t->tex, 0, nullptr, data, pitch, 0);
        ctx->GenerateMips(t->srv);
    }
    return t;
}

Tex* Gpu::acquire(int w, int h, DXGI_FORMAT fmt) {
    w = std::max(1, w); h = std::max(1, h);
    for (Tex* t : pool)
        if (!t->inUse && t->w == w && t->h == h && t->fmt == fmt) { t->inUse = true; t->lastUse = frame; return t; }
    Tex* t = create(w, h, fmt, true);
    if (!t) {
        // out of video memory: drop every idle texture and try once more
        trimPool(-1);
        t = create(w, h, fmt, true);
    }
    if (!t) {
        // still nothing: hand out a 1x1 stand-in so rendering degrades instead of crashing
        outOfMemory = true;
        for (Tex* d : dummies) if (d->fmt == fmt) return d;
        Tex* d = create(1, 1, fmt, true);
        if (!d) return nullptr;
        d->dummy = true;
        dummies.push_back(d);
        return d;
    }
    t->pooled = true; t->inUse = true; t->lastUse = frame;
    pool.push_back(t);
    return t;
}

void Gpu::release(Tex*& t) {
    if (!t) return;
    if (t->dummy) { t = nullptr; return; }
    if (t->pooled) t->inUse = false;
    else { t->release(); delete t; }
    t = nullptr;
}

static size_t BytesPerPixel(DXGI_FORMAT f) {
    switch (f) {
    case DXGI_FORMAT_R16G16B16A16_FLOAT: case DXGI_FORMAT_R16G16B16A16_UNORM: return 8;
    case DXGI_FORMAT_R32G32B32A32_FLOAT: return 16;
    case DXGI_FORMAT_R16_FLOAT: case DXGI_FORMAT_R16_UNORM: return 2;
    case DXGI_FORMAT_R8_UNORM: return 1;
    default: return 4;
    }
}

size_t Gpu::poolBytes(bool idleOnly) const {
    size_t n = 0;
    for (const Tex* t : pool) if (!idleOnly || !t->inUse) n += (size_t)t->w * t->h * BytesPerPixel(t->fmt);
    return n;
}

void Gpu::trimPool(int maxAge) {
    // Different crops / zoom levels produce textures of new sizes; without this the pool only grows.
    bool all = maxAge < 0 || poolBytes(true) > ((size_t)768 << 20);
    std::vector<Tex*> keep;
    for (Tex* t : pool) {
        if (!t->inUse && (all || frame - t->lastUse > maxAge)) { t->release(); delete t; }
        else keep.push_back(t);
    }
    pool.swap(keep);
}

void Gpu::unbindAll() {
    ID3D11ShaderResourceView* nulls[8] = {};
    ctx->PSSetShaderResources(0, 8, nulls);
    ID3D11RenderTargetView* nrt = nullptr;
    ctx->OMSetRenderTargets(1, &nrt, nullptr);
}

static void Upload(ID3D11DeviceContext* ctx, ID3D11Buffer* b, const void* data, size_t size) {
    D3D11_MAPPED_SUBRESOURCE m;
    if (SUCCEEDED(ctx->Map(b, 0, D3D11_MAP_WRITE_DISCARD, 0, &m))) {
        memcpy(m.pData, data, size);
        ctx->Unmap(b, 0);
    }
}

void Gpu::pass(ID3D11PixelShader* ps, Tex* out, std::initializer_list<ID3D11ShaderResourceView*> srvs,
               const void* cb, size_t cbSize, ID3D11BlendState* blend) {
    if (!ps || !out) return;
    unbindAll();
    common.size[0] = (float)out->w; common.size[1] = (float)out->h;
    common.inv[0] = 1.0f / out->w; common.inv[1] = 1.0f / out->h;
    Upload(ctx, cbCommon, &common, sizeof(common));
    if (cb && cbSize) Upload(ctx, cbPass, cb, std::min(cbSize, (size_t)16 * 1024 + 256));

    D3D11_VIEWPORT vp = { 0, 0, (float)out->w, (float)out->h, 0, 1 };
    ctx->RSSetViewports(1, &vp);
    ctx->RSSetState(raster);
    ctx->OMSetRenderTargets(1, &out->rtv, nullptr);
    float bf[4] = {0,0,0,0};
    ctx->OMSetBlendState(blend, bf, 0xffffffff);
    ctx->OMSetDepthStencilState(nullptr, 0);
    ctx->IASetInputLayout(nullptr);
    ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    ctx->VSSetShader(vsFull, nullptr, 0);
    ctx->GSSetShader(nullptr, nullptr, 0);
    ctx->PSSetShader(ps, nullptr, 0);
    ID3D11Buffer* cbs[2] = { cbCommon, cbPass };
    ctx->PSSetConstantBuffers(0, 2, cbs);
    ctx->VSSetConstantBuffers(0, 2, cbs);
    ctx->PSSetSamplers(0, 5, samp);
    ID3D11ShaderResourceView* arr[8] = {};
    int n = 0;
    for (auto* s : srvs) { if (n < 8) arr[n++] = s; }
    ctx->PSSetShaderResources(0, 8, arr);
    ctx->Draw(3, 0);
    unbindAll();
}

void Gpu::clear(Tex* t, float r, float g, float b, float a) {
    float c[4] = { r, g, b, a };
    ctx->ClearRenderTargetView(t->rtv, c);
}

static float HalfToFloat(uint16_t h) {
    uint32_t s = (h >> 15) & 1, e = (h >> 10) & 0x1f, m = h & 0x3ff;
    float v;
    if (e == 0) v = std::ldexp((float)m, -24);
    else if (e == 31) v = m ? NAN : INFINITY;
    else v = std::ldexp((float)(m | 0x400), (int)e - 25);
    return s ? -v : v;
}

bool Gpu::readPixels(Tex* t, int x, int y, int w, int h, std::vector<float>& out) {
    if (!t) return false;
    x = std::clamp(x, 0, t->w - 1); y = std::clamp(y, 0, t->h - 1);
    w = std::min(w, t->w - x); h = std::min(h, t->h - y);
    D3D11_TEXTURE2D_DESC td = {};
    td.Width = w; td.Height = h; td.MipLevels = 1; td.ArraySize = 1; td.Format = t->fmt;
    td.SampleDesc.Count = 1; td.Usage = D3D11_USAGE_STAGING; td.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    ID3D11Texture2D* st = nullptr;
    if (FAILED(dev->CreateTexture2D(&td, nullptr, &st))) return false;
    D3D11_BOX box = { (UINT)x, (UINT)y, 0, (UINT)(x + w), (UINT)(y + h), 1 };
    ctx->CopySubresourceRegion(st, 0, 0, 0, 0, t->tex, 0, &box);
    D3D11_MAPPED_SUBRESOURCE m;
    out.assign((size_t)w * h * 4, 0.0f);
    if (SUCCEEDED(ctx->Map(st, 0, D3D11_MAP_READ, 0, &m))) {
        for (int j = 0; j < h; j++) {
            const uint8_t* row = (const uint8_t*)m.pData + (size_t)j * m.RowPitch;
            for (int i = 0; i < w; i++) {
                float* o = &out[((size_t)j * w + i) * 4];
                if (t->fmt == DXGI_FORMAT_R16G16B16A16_FLOAT) {
                    const uint16_t* p = (const uint16_t*)row + i * 4;
                    for (int k = 0; k < 4; k++) o[k] = HalfToFloat(p[k]);
                } else if (t->fmt == DXGI_FORMAT_R16G16B16A16_UNORM) {
                    const uint16_t* p = (const uint16_t*)row + i * 4;
                    for (int k = 0; k < 4; k++) o[k] = p[k] / 65535.0f;
                } else if (t->fmt == DXGI_FORMAT_R8G8B8A8_UNORM || t->fmt == DXGI_FORMAT_R8G8B8A8_UNORM_SRGB) {
                    const uint8_t* p = row + i * 4;
                    for (int k = 0; k < 4; k++) o[k] = p[k] / 255.0f;
                } else if (t->fmt == DXGI_FORMAT_R16_FLOAT) {
                    o[0] = HalfToFloat(((const uint16_t*)row)[i]);
                } else if (t->fmt == DXGI_FORMAT_R16G16_FLOAT) {
                    const uint16_t* p = (const uint16_t*)row + i * 2;
                    o[0] = HalfToFloat(p[0]); o[1] = HalfToFloat(p[1]);
                }
            }
        }
        ctx->Unmap(st, 0);
    }
    st->Release();
    return true;
}

bool Gpu::readTexture(Tex* t, std::vector<uint8_t>& out, int& bpp) {
    if (!t) return false;
    bpp = (t->fmt == DXGI_FORMAT_R16G16B16A16_UNORM || t->fmt == DXGI_FORMAT_R16G16B16A16_FLOAT) ? 8 : 4;
    D3D11_TEXTURE2D_DESC td = {};
    td.Width = t->w; td.Height = t->h; td.MipLevels = 1; td.ArraySize = 1; td.Format = t->fmt;
    td.SampleDesc.Count = 1; td.Usage = D3D11_USAGE_STAGING; td.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    ID3D11Texture2D* st = nullptr;
    if (FAILED(dev->CreateTexture2D(&td, nullptr, &st))) return false;
    ctx->CopyResource(st, t->tex);
    D3D11_MAPPED_SUBRESOURCE m;
    bool ok = false;
    if (SUCCEEDED(ctx->Map(st, 0, D3D11_MAP_READ, 0, &m))) {
        out.resize((size_t)t->w * t->h * bpp);
        for (int j = 0; j < t->h; j++)
            memcpy(&out[(size_t)j * t->w * bpp], (const uint8_t*)m.pData + (size_t)j * m.RowPitch, (size_t)t->w * bpp);
        ctx->Unmap(st, 0);
        ok = true;
    }
    st->Release();
    return ok;
}
