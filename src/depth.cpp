#include "depth.h"
#include "imageio.h"
#include "gpu.h"
#define ORT_API_MANUAL_INIT
#include "ort/onnxruntime_c_api.h"
#include "ort/ort_dml.h"
#include <windows.h>
#include <algorithm>
#include <cmath>
#include <mutex>
#include <cstdio>

static const wchar_t* kModelFile = L"depth_anything_v2_small_fp16.onnx";

static std::mutex s_mx;
static HMODULE s_dll = nullptr;
static const OrtApi* s_api = nullptr;
static OrtEnv* s_env = nullptr;
static OrtSession* s_session = nullptr;
static std::string s_device;
static bool s_gpuBroken = false;    // DirectML failed once: stay on the CPU for this session

std::wstring DepthModuleDir() {
    std::wstring cands[2];
    cands[0] = ExeDir() + L"\\depth";
    wchar_t buf[MAX_PATH];
    if (GetEnvironmentVariableW(L"LOCALAPPDATA", buf, MAX_PATH)) cands[1] = std::wstring(buf) + L"\\Muon\\depth";
    for (auto& d : cands) {
        if (d.empty()) continue;
        if (GetFileAttributesW((d + L"\\onnxruntime.dll").c_str()) != INVALID_FILE_ATTRIBUTES &&
            GetFileAttributesW((d + L"\\" + kModelFile).c_str()) != INVALID_FILE_ATTRIBUTES) return d;
    }
    return L"";
}

bool DepthModuleAvailable() { return !DepthModuleDir().empty(); }

static std::string TakeError(OrtStatus* st) {
    std::string m = s_api->GetErrorMessage(st);
    s_api->ReleaseStatus(st);
    return m;
}

// Creates the session once: DirectML on the most powerful GPU, CPU if that is not possible.
static bool EnsureSession(std::string& err) {
    if (s_session) return true;
    std::wstring dir = DepthModuleDir();
    if (dir.empty()) { err = "depth module not found"; return false; }
    if (!s_dll) {
        // altered search path: DirectML.dll is taken from the same folder, not the older copy in System32
        s_dll = LoadLibraryExW((dir + L"\\onnxruntime.dll").c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
        if (!s_dll) { err = "cannot load onnxruntime.dll"; return false; }
        auto getBase = (const OrtApiBase*(ORT_API_CALL*)())GetProcAddress(s_dll, "OrtGetApiBase");
        if (!getBase) { err = "onnxruntime.dll is not valid"; return false; }
        s_api = getBase()->GetApi(ORT_API_VERSION);
        if (!s_api) { err = "onnxruntime.dll is too old"; return false; }
        OrtStatus* st = s_api->CreateEnv(ORT_LOGGING_LEVEL_ERROR, "muon", &s_env);
        if (st) { err = TakeError(st); return false; }
    }
    std::wstring model = dir + L"\\" + kModelFile;
    for (int gpu = s_gpuBroken ? 0 : 1; gpu >= 0 && !s_session; gpu--) {
        OrtSessionOptions* so = nullptr;
        if (s_api->CreateSessionOptions(&so)) continue;
        bool ok = true;
        if (gpu) {
            const OrtDmlApi* dml = nullptr;
            OrtDmlDeviceOptions opt{ OrtDmlPerformancePreference::HighPerformance, OrtDmlDeviceFilter::Gpu };
            OrtStatus* st = s_api->DisableMemPattern(so);
            if (!st) st = s_api->SetSessionExecutionMode(so, ORT_SEQUENTIAL);
            if (!st) st = s_api->GetExecutionProviderApi("DML", ORT_API_VERSION, (const void**)&dml);
            if (!st) st = dml->SessionOptionsAppendExecutionProvider_DML2(so, &opt);
            if (!st) st = s_api->SetSessionGraphOptimizationLevel(so, ORT_ENABLE_ALL);
            if (st) { err = TakeError(st); ok = false; }
        } else {
            // the fp16 model trips one of ORT's CPU fusions, basic optimisation is safe
            OrtStatus* st = s_api->SetSessionGraphOptimizationLevel(so, ORT_ENABLE_BASIC);
            if (st) { err = TakeError(st); ok = false; }
        }
        if (ok) {
            OrtStatus* st = s_api->CreateSession(s_env, model.c_str(), so, &s_session);
            if (st) { err = TakeError(st); s_session = nullptr; }
            else s_device = gpu ? "GPU (DirectML)" : "CPU";
        }
        s_api->ReleaseSessionOptions(so);
    }
    return s_session != nullptr;
}

static bool EstimateLocked(const std::vector<uint8_t>& rgba, int w, int h, std::vector<float>& out, int& ow, int& oh, std::string& error) {
    if (!EnsureSession(error)) return false;
    // network input: long side 518, both sides multiples of 14 (the ViT patch size)
    float s = 518.0f / std::max(w, h);
    int iw = std::max(14, (int)std::lround(w * s / 14.0f) * 14), ih = std::max(14, (int)std::lround(h * s / 14.0f) * 14);
    static const float mean[3] = { 0.485f, 0.456f, 0.406f }, stdv[3] = { 0.229f, 0.224f, 0.225f };
    std::vector<float> in((size_t)3 * iw * ih);
    for (int y = 0; y < ih; y++) {
        float fy = std::min((y + 0.5f) * h / ih - 0.5f, (float)h - 1); fy = std::max(fy, 0.0f);
        int y0 = (int)fy, y1 = std::min(y0 + 1, h - 1); float ty = fy - y0;
        for (int x = 0; x < iw; x++) {
            float fx = std::min((x + 0.5f) * w / iw - 0.5f, (float)w - 1); fx = std::max(fx, 0.0f);
            int x0 = (int)fx, x1 = std::min(x0 + 1, w - 1); float tx = fx - x0;
            for (int c = 0; c < 3; c++) {
                auto px = [&](int xx, int yy) { return rgba[((size_t)yy * w + xx) * 4 + c] / 255.0f; };
                float v = (px(x0, y0) * (1 - tx) + px(x1, y0) * tx) * (1 - ty) + (px(x0, y1) * (1 - tx) + px(x1, y1) * tx) * ty;
                in[(size_t)c * iw * ih + (size_t)y * iw + x] = (v - mean[c]) / stdv[c];
            }
        }
    }
    int64_t shape[4] = { 1, 3, ih, iw };
    OrtMemoryInfo* mi = nullptr;
    OrtValue* tin = nullptr;
    OrtValue* tout = nullptr;
    bool ok = false;
    do {
        OrtStatus* st = s_api->CreateCpuMemoryInfo(OrtArenaAllocator, OrtMemTypeDefault, &mi);
        if (!st) st = s_api->CreateTensorWithDataAsOrtValue(mi, in.data(), in.size() * sizeof(float), shape, 4, ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT, &tin);
        const char* inNames[] = { "pixel_values" };
        const char* outNames[] = { "predicted_depth" };
        if (!st) st = s_api->Run(s_session, nullptr, inNames, &tin, 1, outNames, 1, &tout);
        float* d = nullptr;
        if (!st) st = s_api->GetTensorMutableData(tout, (void**)&d);
        if (st) { error = TakeError(st); break; }
        // the model returns relative disparity: bigger = closer. Normalise robustly to 0..1.
        size_t n = (size_t)iw * ih;
        std::vector<float> sorted(d, d + n);
        size_t lo = n / 100, hi = n - 1 - n / 200;
        std::nth_element(sorted.begin(), sorted.begin() + lo, sorted.end());
        float vlo = sorted[lo];
        std::nth_element(sorted.begin(), sorted.begin() + hi, sorted.end());
        float vhi = sorted[hi];
        float k = 1.0f / std::max(vhi - vlo, 1e-6f);
        out.resize(n);
        for (size_t i = 0; i < n; i++) out[i] = std::clamp((d[i] - vlo) * k, 0.0f, 1.0f);
        ow = iw; oh = ih;
        ok = true;
    } while (false);
    if (tout) s_api->ReleaseValue(tout);
    if (tin) s_api->ReleaseValue(tin);
    if (mi) s_api->ReleaseMemoryInfo(mi);
    return ok;
}

// A fault inside the GPU driver must not take the editor down with it.
struct EstimateArgs { const std::vector<uint8_t>* rgba; int w, h; std::vector<float>* out; int* ow; int* oh; std::string* error; };
static bool EstimateSEH(EstimateArgs* a) {
    __try { return EstimateLocked(*a->rgba, a->w, a->h, *a->out, *a->ow, *a->oh, *a->error); }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

bool DepthEstimate(const std::vector<uint8_t>& rgba, int w, int h, std::vector<float>& out, int& ow, int& oh,
                   std::string& error, std::string* device) {
    std::lock_guard<std::mutex> lk(s_mx);
    EstimateArgs a{ &rgba, w, h, &out, &ow, &oh, &error };
    bool ok = EstimateSEH(&a);
    if (!ok && !s_gpuBroken && s_device != "CPU") {
        // the GPU path failed (driver reset, out of memory, crash): drop it and try once more on the processor
        LogLine("depth on GPU failed, retrying on CPU: " + error);
        s_gpuBroken = true;
        s_session = nullptr;   // not released: after a fault its state cannot be trusted
        error.clear();
        ok = EstimateSEH(&a);
    }
    if (ok && device) *device = s_device;
    return ok;
}

void DepthShutdown() {
    std::lock_guard<std::mutex> lk(s_mx);
    if (s_session) { s_api->ReleaseSession(s_session); s_session = nullptr; }
    if (s_env) { s_api->ReleaseEnv(s_env); s_env = nullptr; }
    // the DLL stays loaded until exit: unloading ORT while DirectML threads wind down is not safe
}

// ------------------------------------------------------------------ cache
static std::wstring CachePath(const std::wstring& photo) {
    wchar_t buf[MAX_PATH];
    if (!GetEnvironmentVariableW(L"LOCALAPPDATA", buf, MAX_PATH)) return L"";
    std::wstring d = std::wstring(buf) + L"\\Muon";
    CreateDirectoryW(d.c_str(), nullptr);
    d += L"\\depthcache";
    CreateDirectoryW(d.c_str(), nullptr);
    WIN32_FILE_ATTRIBUTE_DATA fa = {};
    GetFileAttributesExW(photo.c_str(), GetFileExInfoStandard, &fa);
    uint64_t hsh = 1469598103934665603ull;
    auto mix = [&](uint64_t v) { for (int i = 0; i < 8; i++) { hsh ^= (v >> (i * 8)) & 0xff; hsh *= 1099511628211ull; } };
    for (wchar_t c : photo) mix((uint64_t)towlower(c));
    mix(((uint64_t)fa.nFileSizeHigh << 32) | fa.nFileSizeLow);
    mix(((uint64_t)fa.ftLastWriteTime.dwHighDateTime << 32) | fa.ftLastWriteTime.dwLowDateTime);
    wchar_t name[40]; swprintf(name, 40, L"\\%016llx.depth", (unsigned long long)hsh);
    return d + name;
}

bool DepthCacheLoad(const std::wstring& photo, std::vector<float>& d, int& w, int& h) {
    std::wstring p = CachePath(photo);
    FILE* f = p.empty() ? nullptr : _wfopen(p.c_str(), L"rb");
    if (!f) return false;
    char magic[4]; int32_t dims[2];
    bool ok = fread(magic, 1, 4, f) == 4 && memcmp(magic, "MDP1", 4) == 0 && fread(dims, 4, 2, f) == 2 &&
              dims[0] > 0 && dims[1] > 0 && dims[0] < 8192 && dims[1] < 8192;
    if (ok) {
        w = dims[0]; h = dims[1];
        d.resize((size_t)w * h);
        ok = fread(d.data(), 4, d.size(), f) == d.size();
    }
    fclose(f);
    return ok;
}

void DepthCacheSave(const std::wstring& photo, const std::vector<float>& d, int w, int h) {
    std::wstring p = CachePath(photo);
    FILE* f = p.empty() ? nullptr : _wfopen(p.c_str(), L"wb");
    if (!f) return;
    int32_t dims[2] = { w, h };
    fwrite("MDP1", 1, 4, f);
    fwrite(dims, 4, 2, f);
    fwrite(d.data(), 4, d.size(), f);
    fclose(f);
}
