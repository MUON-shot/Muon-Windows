#include "imageio.h"
#include "i18n.h"
#include <shellapi.h>
#include <cstring>
#include <libraw/libraw.h>
#include <wincodec.h>
#include <shobjidl.h>
#include <shlwapi.h>
#include <algorithm>
#include <cstdio>
#include <cmath>
#include <memory>

static const wchar_t* kRawExt[] = { L"cr2", L"cr3", L"crw", L"nef", L"nrw", L"arw", L"srf", L"sr2", L"raf", L"orf", L"rw2", L"pef",
                                    L"dng", L"srw", L"x3f", L"3fr", L"fff", L"iiq", L"mos", L"mrw", L"erf", L"kdc", L"dcr", L"rwl", L"raw", L"mef" };
static const wchar_t* kStdExt[] = { L"jpg", L"jpeg", L"jpe", L"png", L"tif", L"tiff", L"bmp", L"webp", L"heic", L"heif", L"jxr", L"avif" };

std::wstring Utf8ToWide(const std::string& s) {
    if (s.empty()) return {};
    int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), nullptr, 0);
    std::wstring w(n, 0);
    MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), &w[0], n);
    return w;
}
std::string WideToUtf8(const std::wstring& w) {
    if (w.empty()) return {};
    int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), nullptr, 0, nullptr, nullptr);
    std::string s(n, 0);
    WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), &s[0], n, nullptr, nullptr);
    return s;
}

static std::wstring ExtOf(const std::wstring& p) {
    size_t d = p.find_last_of(L'.');
    if (d == std::wstring::npos) return {};
    std::wstring e = p.substr(d + 1);
    for (auto& c : e) c = (wchar_t)towlower(c);
    return e;
}
bool IsRawExt(const std::wstring& p) { auto e = ExtOf(p); for (auto x : kRawExt) if (e == x) return true; return false; }
bool IsSupportedExt(const std::wstring& p) {
    if (IsRawExt(p)) return true;
    auto e = ExtOf(p); for (auto x : kStdExt) if (e == x) return true; return false;
}

std::wstring ExeDir() {
    wchar_t buf[MAX_PATH * 2];
    GetModuleFileNameW(nullptr, buf, MAX_PATH * 2);
    PathRemoveFileSpecW(buf);
    return buf;
}

bool ReadTextFile(const std::wstring& path, std::string& out) {
    FILE* f = _wfopen(path.c_str(), L"rb");
    if (!f) return false;
    fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, 0, SEEK_SET);
    out.resize(n > 0 ? n : 0);
    if (n > 0) fread(&out[0], 1, n, f);
    fclose(f);
    return true;
}
bool WriteTextFile(const std::wstring& path, const std::string& s) {
    FILE* f = _wfopen(path.c_str(), L"wb");
    if (!f) return false;
    fwrite(s.data(), 1, s.size(), f);
    fclose(f);
    return true;
}

std::vector<std::wstring> ListFolderImages(const std::wstring& folder) {
    std::vector<std::wstring> r;
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW((folder + L"\\*").c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return r;
    do {
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
        std::wstring n = fd.cFileName;
        if (IsSupportedExt(n)) r.push_back(folder + L"\\" + n);
    } while (FindNextFileW(h, &fd));
    FindClose(h);
    std::sort(r.begin(), r.end(), [](const std::wstring& a, const std::wstring& b) { return StrCmpLogicalW(a.c_str(), b.c_str()) < 0; });
    return r;
}

// ------------------------------------------------------------------ WIC helpers
struct ComInit {
    HRESULT hr;
    ComInit() { hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED | COINIT_DISABLE_OLE1DDE); }
    ~ComInit() { if (SUCCEEDED(hr)) CoUninitialize(); }
};
template <class T> struct ComPtr {
    T* p = nullptr;
    ~ComPtr() { if (p) p->Release(); }
    T** operator&() { return &p; }
    T* operator->() { return p; }
    operator T*() { return p; }
};

static IWICImagingFactory* Factory() {
    IWICImagingFactory* f = nullptr;
    CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&f));
    return f;
}

static void ApplyOrientation(std::vector<uint8_t>& px, int& w, int& h, int orient) {
    if (orient <= 1 || orient > 8) return;
    std::vector<uint8_t> o(px.size());
    bool swap = orient >= 5;
    int nw = swap ? h : w, nh = swap ? w : h;
    for (int y = 0; y < h; y++) for (int x = 0; x < w; x++) {
        int dx = x, dy = y;
        switch (orient) {
        case 2: dx = w - 1 - x; break;
        case 3: dx = w - 1 - x; dy = h - 1 - y; break;
        case 4: dy = h - 1 - y; break;
        case 5: dx = y; dy = x; break;
        case 6: dx = h - 1 - y; dy = x; break;
        case 7: dx = h - 1 - y; dy = w - 1 - x; break;
        case 8: dx = y; dy = w - 1 - x; break;
        }
        memcpy(&o[((size_t)dy * nw + dx) * 4], &px[((size_t)y * w + x) * 4], 4);
    }
    px.swap(o); w = nw; h = nh;
}

static int ReadOrientation(IWICBitmapFrameDecode* frame) {
    ComPtr<IWICMetadataQueryReader> qr;
    if (FAILED(frame->GetMetadataQueryReader(&qr)) || !qr.p) return 1;
    PROPVARIANT v; PropVariantInit(&v);
    int o = 1;
    const wchar_t* q[] = { L"/app1/ifd/{ushort=274}", L"/ifd/{ushort=274}", L"System.Photo.Orientation" };
    for (auto s : q) {
        if (SUCCEEDED(qr->GetMetadataByName(s, &v))) {
            if (v.vt == VT_UI2) o = v.uiVal;
            PropVariantClear(&v);
            break;
        }
    }
    return o;
}

static bool DecodeWic(IWICImagingFactory* f, IWICBitmapDecoder* dec, int maxSize, std::vector<uint8_t>& rgba, int& w, int& h, int orientOverride = 0) {
    ComPtr<IWICBitmapFrameDecode> frame;
    if (FAILED(dec->GetFrame(0, &frame))) return false;
    UINT fw, fh; frame->GetSize(&fw, &fh);
    int orient = orientOverride ? orientOverride : ReadOrientation(frame);
    IWICBitmapSource* srcp = frame;
    ComPtr<IWICBitmapScaler> scaler;
    if (maxSize > 0 && (int)std::max(fw, fh) > maxSize) {
        float s = (float)maxSize / std::max(fw, fh);
        UINT sw = std::max(1u, (UINT)(fw * s)), sh = std::max(1u, (UINT)(fh * s));
        if (SUCCEEDED(f->CreateBitmapScaler(&scaler)) && SUCCEEDED(scaler->Initialize(frame, sw, sh, WICBitmapInterpolationModeFant))) {
            srcp = scaler; fw = sw; fh = sh;
        }
    }
    ComPtr<IWICFormatConverter> conv;
    if (FAILED(f->CreateFormatConverter(&conv))) return false;
    if (FAILED(conv->Initialize(srcp, GUID_WICPixelFormat32bppRGBA, WICBitmapDitherTypeNone, nullptr, 0, WICBitmapPaletteTypeCustom))) return false;
    w = fw; h = fh;
    rgba.resize((size_t)w * h * 4);
    if (FAILED(conv->CopyPixels(nullptr, w * 4, (UINT)rgba.size(), rgba.data()))) return false;
    for (size_t i = 3; i < rgba.size(); i += 4) rgba[i] = 255;
    ApplyOrientation(rgba, w, h, orient);
    return true;
}


// ------------------------------------------------------------------ embedded JPEG previews
// Some RAW variants cannot be decoded by LibRaw (for example Nikon "High Efficiency" NEFs, which use the
// closed TicoRAW codec). Almost every camera stores a full-size JPEG inside the file; we fall back to it.
struct JpegRef { uint64_t off, len; };

static void FindTiffJpegs(const std::vector<uint8_t>& d, std::vector<JpegRef>& out) {
    if (d.size() < 16) return;
    bool le = d[0] == 'I' && d[1] == 'I';
    bool be = d[0] == 'M' && d[1] == 'M';
    if (!le && !be) return;
    auto u16 = [&](size_t o) -> uint32_t { if (o + 2 > d.size()) return 0; return le ? (d[o] | d[o + 1] << 8) : (d[o] << 8 | d[o + 1]); };
    auto u32 = [&](size_t o) -> uint32_t {
        if (o + 4 > d.size()) return 0;
        return le ? (uint32_t)(d[o] | d[o + 1] << 8 | d[o + 2] << 16 | (uint32_t)d[o + 3] << 24)
                  : (uint32_t)((uint32_t)d[o] << 24 | d[o + 1] << 16 | d[o + 2] << 8 | d[o + 3]);
    };
    std::vector<uint32_t> todo = { u32(4) };
    std::vector<uint32_t> seen;
    while (!todo.empty() && seen.size() < 64) {
        uint32_t off = todo.back(); todo.pop_back();
        if (!off || off + 2 > d.size() || std::find(seen.begin(), seen.end(), off) != seen.end()) continue;
        seen.push_back(off);
        uint32_t n = u16(off);
        if (n == 0 || n > 512) continue;
        uint32_t jOff = 0, jLen = 0, sOff = 0, sLen = 0, comp = 0;
        for (uint32_t i = 0; i < n; i++) {
            size_t e = off + 2 + (size_t)i * 12;
            uint32_t tag = u16(e), type = u16(e + 2), cnt = u32(e + 4);
            uint32_t val = type == 3 && cnt == 1 ? u16(e + 8) : u32(e + 8);
            if (tag == 0x201) jOff = val;
            else if (tag == 0x202) jLen = val;
            else if (tag == 0x111 && cnt == 1) sOff = val;
            else if (tag == 0x117 && cnt == 1) sLen = val;
            else if (tag == 0x103) comp = val;
            else if (tag == 0x14a) {
                if (cnt == 1) todo.push_back(val);
                else for (uint32_t k = 0; k < cnt && k < 16; k++) todo.push_back(u32(val + k * 4));
            }
        }
        if (jOff && jLen) out.push_back({ jOff, jLen });
        if ((comp == 6 || comp == 7) && sOff && sLen) out.push_back({ sOff, sLen });
        uint32_t next = u32(off + 2 + (size_t)n * 12);
        if (next) todo.push_back(next);
    }
}

// Returns the bytes of the largest (or smallest above minBytes) embedded JPEG.
static bool ExtractEmbeddedJpeg(const std::wstring& path, bool largest, std::vector<uint8_t>& jpg, int& orientation) {
    FILE* f = _wfopen(path.c_str(), L"rb");
    if (!f) return false;
    _fseeki64(f, 0, SEEK_END);
    int64_t size = _ftelli64(f);
    _fseeki64(f, 0, SEEK_SET);
    std::vector<uint8_t> d((size_t)size);
    size_t got = fread(d.data(), 1, d.size(), f);
    fclose(f);
    if (got != d.size()) return false;
    std::vector<JpegRef> refs;
    orientation = 1;
    if (d.size() > 100 && memcmp(d.data(), "FUJIFILMCCD-RAW", 15) == 0) {
        // RAF: big-endian offset/length of the JPEG at bytes 84..91
        uint32_t o = d[84] << 24 | d[85] << 16 | d[86] << 8 | d[87], l = d[88] << 24 | d[89] << 16 | d[90] << 8 | d[91];
        refs.push_back({ o, l });
    } else FindTiffJpegs(d, refs);
    // full load: the largest preview; thumbnails: the smallest one that is still reasonably detailed
    auto better = [&](const JpegRef& a, const JpegRef& b) {
        if (largest) return a.len > b.len;
        bool aOk = a.len >= 60000, bOk = b.len >= 60000;
        if (aOk != bOk) return aOk;
        return aOk ? a.len < b.len : a.len > b.len;
    };
    const JpegRef* best = nullptr;
    for (auto& r : refs) {
        if (r.off + r.len > d.size() || r.len < 1024 || d[(size_t)r.off] != 0xFF || d[(size_t)r.off + 1] != 0xD8) continue;
        if (!best || better(r, *best)) best = &r;
    }
    if (!best) return false;
    jpg.assign(d.begin() + (size_t)best->off, d.begin() + (size_t)(best->off + best->len));
    // orientation of the camera: IFD0 tag 0x112
    bool le = d[0] == 'I';
    if (le || d[0] == 'M') {
        auto u16 = [&](size_t o) -> uint32_t { return le ? (d[o] | d[o + 1] << 8) : (d[o] << 8 | d[o + 1]); };
        uint32_t ifd0 = le ? (d[4] | d[5] << 8 | d[6] << 16 | (uint32_t)d[7] << 24) : ((uint32_t)d[4] << 24 | d[5] << 16 | d[6] << 8 | d[7]);
        if (ifd0 + 2 < d.size()) {
            uint32_t n = u16(ifd0);
            for (uint32_t i = 0; i < n && ifd0 + 2 + i * 12 + 12 <= d.size(); i++)
                if (u16(ifd0 + 2 + i * 12) == 0x112) orientation = (int)u16(ifd0 + 2 + i * 12 + 8);
        }
    }
    return true;
}

static bool DecodeJpegBytes(const std::vector<uint8_t>& jpg, int maxSize, int orientation, std::vector<uint8_t>& rgba, int& w, int& h) {
    IWICImagingFactory* f = Factory();
    if (!f) return false;
    bool ok = false;
    ComPtr<IWICStream> st;
    if (SUCCEEDED(f->CreateStream(&st)) && SUCCEEDED(st->InitializeFromMemory((BYTE*)jpg.data(), (DWORD)jpg.size()))) {
        ComPtr<IWICBitmapDecoder> dec;
        if (SUCCEEDED(f->CreateDecoderFromStream(st, nullptr, WICDecodeMetadataCacheOnDemand, &dec)))
            ok = DecodeWic(f, dec, maxSize, rgba, w, h, orientation < 1 || orientation > 8 ? 1 : orientation);
    }
    f->Release();
    return ok;
}

static float SrgbToLin(float v) { return v <= 0.04045f ? v / 12.92f : std::pow((v + 0.055f) / 1.055f, 2.4f); }

static void MakeSmall(LoadedImage& img) {
    int lw = std::max(img.w, img.h);
    float s = std::min(1.0f, 320.0f / lw);
    img.sw = std::max(1, (int)(img.w * s)); img.sh = std::max(1, (int)(img.h * s));
    img.preview.assign((size_t)img.sw * img.sh * 3, 0);
    float lin8[256];
    for (int i = 0; i < 256; i++) lin8[i] = SrgbToLin(i / 255.0f);
    for (int y = 0; y < img.sh; y++) {
        for (int x = 0; x < img.sw; x++) {
            int x0 = x * img.w / img.sw, x1 = std::max(x0 + 1, (x + 1) * img.w / img.sw);
            int y0 = y * img.h / img.sh, y1 = std::max(y0 + 1, (y + 1) * img.h / img.sh);
            double acc[3] = {}; int n = 0;
            int stepx = std::max(1, (x1 - x0) / 4), stepy = std::max(1, (y1 - y0) / 4);
            for (int yy = y0; yy < y1; yy += stepy) for (int xx = x0; xx < x1; xx += stepx) {
                size_t idx = ((size_t)yy * img.w + xx) * 4;
                for (int c = 0; c < 3; c++) acc[c] += img.isRaw ? img.rgba16[idx + c] / 65535.0 : lin8[img.rgba8[idx + c]];
                n++;
            }
            for (int c = 0; c < 3; c++) img.preview[((size_t)y * img.sw + x) * 3 + c] = (float)(acc[c] / n);
        }
    }
    // sRGB copy for the depth network (it was trained on ordinary photos, not linear RAW data)
    {
        float ds = std::min(1.0f, 770.0f / lw);
        img.dw = std::max(1, (int)(img.w * ds)); img.dh = std::max(1, (int)(img.h * ds));
        img.depthSrc.assign((size_t)img.dw * img.dh * 4, 255);
        for (int y = 0; y < img.dh; y++) {
            for (int x = 0; x < img.dw; x++) {
                int x0 = x * img.w / img.dw, x1 = std::max(x0 + 1, (x + 1) * img.w / img.dw);
                int y0 = y * img.h / img.dh, y1 = std::max(y0 + 1, (y + 1) * img.h / img.dh);
                double acc[3] = {}; int n = 0;
                int stepx = std::max(1, (x1 - x0) / 3), stepy = std::max(1, (y1 - y0) / 3);
                for (int yy = y0; yy < y1; yy += stepy) for (int xx = x0; xx < x1; xx += stepx) {
                    size_t idx = ((size_t)yy * img.w + xx) * 4;
                    for (int c = 0; c < 3; c++) acc[c] += img.isRaw ? img.rgba16[idx + c] / 65535.0 * 1.41 : img.rgba8[idx + c] / 255.0;
                    n++;
                }
                for (int c = 0; c < 3; c++) {
                    float v = (float)(acc[c] / n);
                    if (img.isRaw) v = v <= 0.0031308f ? 12.92f * v : 1.055f * std::pow(std::min(v, 1.0f), 1.0f / 2.4f) - 0.055f;
                    img.depthSrc[((size_t)y * img.dw + x) * 4 + c] = (uint8_t)std::clamp((int)(v * 255 + 0.5f), 0, 255);
                }
            }
        }
    }
}

static bool LoadRaw(const std::wstring& path, LoadedImage& out) {
    std::unique_ptr<LibRaw> rp(new LibRaw());
    auto& P = rp->imgdata.params;
    P.output_bps = 16;
    P.gamm[0] = 1.0; P.gamm[1] = 1.0;
    P.no_auto_bright = 1;
    P.use_camera_wb = 1;
    P.output_color = 1;
    P.highlight = 0;
    int r = rp->open_file(path.c_str());
    if (r != LIBRAW_SUCCESS) { out.error = std::string("RAW: ") + libraw_strerror(r); return false; }
    if ((r = rp->unpack()) != LIBRAW_SUCCESS) { out.error = std::string("RAW: ") + libraw_strerror(r); return false; }
    if ((r = rp->dcraw_process()) != LIBRAW_SUCCESS) { out.error = std::string("RAW: ") + libraw_strerror(r); return false; }
    int err = 0;
    libraw_processed_image_t* im = rp->dcraw_make_mem_image(&err);
    if (!im) { out.error = T("RAW: не удалось получить изображение", "RAW: could not develop the image"); return false; }
    out.w = im->width; out.h = im->height; out.isRaw = true;
    out.rgba16.resize((size_t)out.w * out.h * 4);
    const uint16_t* s = (const uint16_t*)im->data;
    size_t n = (size_t)out.w * out.h;
    if (im->colors == 3 && im->bits == 16) {
        for (size_t i = 0; i < n; i++) {
            out.rgba16[i * 4] = s[i * 3]; out.rgba16[i * 4 + 1] = s[i * 3 + 1]; out.rgba16[i * 4 + 2] = s[i * 3 + 2]; out.rgba16[i * 4 + 3] = 65535;
        }
    } else if (im->colors == 1) {
        for (size_t i = 0; i < n; i++) { uint16_t v = im->bits == 16 ? s[i] : (uint16_t)(im->data[i] * 257); out.rgba16[i * 4] = out.rgba16[i * 4 + 1] = out.rgba16[i * 4 + 2] = v; out.rgba16[i * 4 + 3] = 65535; }
    }
    LibRaw::dcraw_clear_mem(im);
    auto& id = rp->imgdata.idata;
    auto& ot = rp->imgdata.other;
    std::string inf = std::string(id.make) + " " + id.model;
    char b[64];
    if (ot.iso_speed > 0) { snprintf(b, sizeof(b), "  ·  ISO %.0f", ot.iso_speed); inf += b; }
    if (ot.shutter > 0) {
        if (ot.shutter < 1) snprintf(b, sizeof(b), T("  ·  1/%.0f с", "  ·  1/%.0f s"), 1.0 / ot.shutter); else snprintf(b, sizeof(b), T("  ·  %.1f с", "  ·  %.1f s"), ot.shutter);
        inf += b;
    }
    if (ot.aperture > 0) { snprintf(b, sizeof(b), "  ·  f/%.1f", ot.aperture); inf += b; }
    if (ot.focal_len > 0) { snprintf(b, sizeof(b), T("  ·  %.0f мм", "  ·  %.0f mm"), ot.focal_len); inf += b; }
    out.info = inf;
    rp->recycle();
    return true;
}

bool LoadImageFile(const std::wstring& path, LoadedImage& out) {
    ComInit com;
    out.path = path;
    bool ok = false;
    if (IsRawExt(path)) {
        ok = LoadRaw(path, out);
        if (!ok && ExtOf(path) == L"dng") { out.error.clear(); ok = false; }
    }
    if (!ok && IsRawExt(path)) {
        std::vector<uint8_t> jpg;
        int orient = 1;
        if (ExtractEmbeddedJpeg(path, true, jpg, orient) && DecodeJpegBytes(jpg, 0, orient, out.rgba8, out.w, out.h)) {
            ok = true;
            out.isRaw = false;
            out.error.clear();
            char buf[96]; snprintf(buf, sizeof(buf), T("встроенный JPEG  ·  %d × %d", "embedded JPEG  ·  %d × %d"), out.w, out.h);
            out.info = buf;
            out.notice = T("Этот RAW не читается (например, Nikon High Efficiency). Открыт встроенный полноразмерный JPEG из файла.",
                           "This RAW cannot be decoded (e.g. Nikon High Efficiency). Opened the full-size JPEG embedded in the file.");
        }
    }
    if (!ok && (!IsRawExt(path) || out.error.empty())) {
        IWICImagingFactory* f = Factory();
        if (f) {
            ComPtr<IWICBitmapDecoder> dec;
            if (SUCCEEDED(f->CreateDecoderFromFilename(path.c_str(), nullptr, GENERIC_READ, WICDecodeMetadataCacheOnDemand, &dec))) {
                ok = DecodeWic(f, dec, 0, out.rgba8, out.w, out.h);
                out.isRaw = false;
                char buf[64]; snprintf(buf, sizeof(buf), "%d × %d", out.w, out.h);
                out.info = buf;
            }
            f->Release();
        }
        if (!ok && out.error.empty()) out.error = T("Не удалось открыть файл: формат не поддерживается", "Could not open the file: unsupported format");
    }
    if (ok) MakeSmall(out);
    return ok;
}

bool LoadThumbnail(const std::wstring& path, int maxSize, std::vector<uint8_t>& rgba, int& w, int& h) {
    ComInit com;
    IWICImagingFactory* f = Factory();
    if (!f) return false;
    bool ok = false;
    if (IsRawExt(path)) {
        std::unique_ptr<LibRaw> rp(new LibRaw());
        if (rp->open_file(path.c_str()) == LIBRAW_SUCCESS && rp->unpack_thumb() == LIBRAW_SUCCESS) {
            int flip = rp->imgdata.sizes.flip;
            int orient = flip == 3 ? 3 : flip == 5 ? 8 : flip == 6 ? 6 : 1;
            auto& th = rp->imgdata.thumbnail;
            if (th.tformat == LIBRAW_THUMBNAIL_JPEG) {
                ComPtr<IWICStream> st;
                if (SUCCEEDED(f->CreateStream(&st)) && SUCCEEDED(st->InitializeFromMemory((BYTE*)th.thumb, th.tlength))) {
                    ComPtr<IWICBitmapDecoder> dec;
                    if (SUCCEEDED(f->CreateDecoderFromStream(st, nullptr, WICDecodeMetadataCacheOnDemand, &dec)))
                        ok = DecodeWic(f, dec, maxSize, rgba, w, h, orient);
                }
            } else if (th.tformat == LIBRAW_THUMBNAIL_BITMAP && th.tcolors == 3) {
                int tw = th.twidth, thh = th.theight;
                int step = std::max(1, std::max(tw, thh) / maxSize);
                w = tw / step; h = thh / step;
                rgba.resize((size_t)w * h * 4);
                for (int y = 0; y < h; y++) for (int x = 0; x < w; x++) {
                    const uint8_t* s = (const uint8_t*)th.thumb + ((size_t)y * step * tw + x * step) * 3;
                    uint8_t* d = &rgba[((size_t)y * w + x) * 4];
                    d[0] = s[0]; d[1] = s[1]; d[2] = s[2]; d[3] = 255;
                }
                ApplyOrientation(rgba, w, h, orient);
                ok = true;
            }
        }
        if (!ok) {
            std::vector<uint8_t> jpg;
            int orient = 1;
            if (ExtractEmbeddedJpeg(path, false, jpg, orient)) ok = DecodeJpegBytes(jpg, maxSize, orient, rgba, w, h);
        }
    } else {
        ComPtr<IWICBitmapDecoder> dec;
        if (SUCCEEDED(f->CreateDecoderFromFilename(path.c_str(), nullptr, GENERIC_READ, WICDecodeMetadataCacheOnDemand, &dec)))
            ok = DecodeWic(f, dec, maxSize, rgba, w, h);
    }
    f->Release();
    return ok;
}

// Decoders run on damaged files from anywhere; a fault inside LibRaw must not take the whole app down.
static bool LoadImageSEH(const std::wstring* path, LoadedImage* out) {
    __try { return LoadImageFile(*path, *out); }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
bool LoadImageFileSafe(const std::wstring& path, LoadedImage& out) {
    if (LoadImageSEH(&path, &out)) return true;
    if (out.error.empty()) out.error = T("Файл повреждён или не поддерживается", "The file is damaged or not supported");
    return false;
}
static bool LoadThumbSEH(const std::wstring* path, int maxSize, std::vector<uint8_t>* rgba, int* w, int* h) {
    __try { return LoadThumbnail(*path, maxSize, *rgba, *w, *h); }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
bool LoadThumbnailSafe(const std::wstring& path, int maxSize, std::vector<uint8_t>& rgba, int& w, int& h) {
    return LoadThumbSEH(&path, maxSize, &rgba, &w, &h);
}

// Encode 16-bit RGBA into a JPEG / PNG / TIFF byte stream.
bool EncodeImage(const EncodeOptions& o, const std::vector<uint16_t>& rgba, int w, int h, std::vector<uint8_t>& bytes) {
    ComInit com;
    IWICImagingFactory* f = Factory();
    if (!f) return false;
    bool ok = false;
    IStream* mem = nullptr;
    ComPtr<IWICBitmapEncoder> enc;
    ComPtr<IWICBitmapFrameEncode> frame;
    ComPtr<IPropertyBag2> props;
    GUID container = o.fmt == EXP_JPEG ? GUID_ContainerFormatJpeg : (o.fmt == EXP_TIFF ? GUID_ContainerFormatTiff : GUID_ContainerFormatPng);
    bool sixteen = o.bits16 && o.fmt != EXP_JPEG;
    do {
        if (FAILED(CreateStreamOnHGlobal(nullptr, TRUE, &mem))) break;
        if (FAILED(f->CreateEncoder(container, nullptr, &enc)) || FAILED(enc->Initialize(mem, WICBitmapEncoderNoCache))) break;
        if (FAILED(enc->CreateNewFrame(&frame, &props))) break;
        if (props.p) {
            if (o.fmt == EXP_JPEG) {
                PROPBAG2 opt[2] = {};
                opt[0].pstrName = (LPOLESTR)L"ImageQuality";
                opt[1].pstrName = (LPOLESTR)L"JpegYCrCbSubsampling";
                VARIANT v[2]; VariantInit(&v[0]); VariantInit(&v[1]);
                v[0].vt = VT_R4; v[0].fltVal = std::clamp(o.quality / 100.0f, 0.01f, 1.0f);
                v[1].vt = VT_UI1; v[1].bVal = o.chroma444 ? (BYTE)WICJpegYCrCbSubsampling444 : (BYTE)WICJpegYCrCbSubsampling420;
                props->Write(2, opt, v);
            } else if (o.fmt == EXP_TIFF) {
                PROPBAG2 opt = {}; opt.pstrName = (LPOLESTR)L"TiffCompressionMethod";
                VARIANT v; VariantInit(&v); v.vt = VT_UI1;
                v.bVal = o.tiffComp == 1 ? (BYTE)WICTiffCompressionLZW : o.tiffComp == 2 ? (BYTE)WICTiffCompressionZIP : (BYTE)WICTiffCompressionNone;
                props->Write(1, &opt, &v);
            }
        }
        if (FAILED(frame->Initialize(props))) break;
        if (FAILED(frame->SetSize(w, h))) break;
        WICPixelFormatGUID pf = sixteen ? GUID_WICPixelFormat48bppRGB : GUID_WICPixelFormat24bppBGR;
        if (FAILED(frame->SetPixelFormat(&pf))) break;
        bool isBgr = IsEqualGUID(pf, GUID_WICPixelFormat24bppBGR) != 0;
        bool is48 = IsEqualGUID(pf, GUID_WICPixelFormat48bppRGB) != 0;
        if (!isBgr && !is48) break;
        UINT stride = is48 ? w * 6 : ((w * 3 + 3) & ~3);
        std::vector<uint8_t> buf((size_t)stride * h);
        for (int y = 0; y < h; y++) {
            uint8_t* row = &buf[(size_t)y * stride];
            for (int x = 0; x < w; x++) {
                const uint16_t* s = &rgba[((size_t)y * w + x) * 4];
                if (is48) { uint16_t* d = (uint16_t*)row + x * 3; d[0] = s[0]; d[1] = s[1]; d[2] = s[2]; }
                else {
                    // round with a little ordered noise to avoid banding in 8 bit
                    float n = (((x * 7 + y * 13) & 15) / 16.0f - 0.5f) * 0.9f;
                    for (int c = 0; c < 3; c++) {
                        float v = s[2 - c] / 257.0f + n;
                        row[x * 3 + c] = (uint8_t)std::clamp((int)std::lround(v), 0, 255);
                    }
                }
            }
        }
        if (FAILED(frame->WritePixels(h, stride, (UINT)buf.size(), buf.data()))) break;
        if (FAILED(frame->Commit()) || FAILED(enc->Commit())) break;
        STATSTG stat = {};
        if (FAILED(mem->Stat(&stat, STATFLAG_NONAME))) break;
        bytes.resize((size_t)stat.cbSize.QuadPart);
        LARGE_INTEGER zero = {};
        mem->Seek(zero, STREAM_SEEK_SET, nullptr);
        ULONG got = 0;
        if (FAILED(mem->Read(bytes.data(), (ULONG)bytes.size(), &got)) || got != bytes.size()) break;
        ok = true;
    } while (false);
    if (mem) mem->Release();
    f->Release();
    return ok;
}

bool SaveImage(const std::wstring& path, const EncodeOptions& o, const std::vector<uint16_t>& rgba, int w, int h, int* usedQuality) {
    std::vector<uint8_t> bytes;
    EncodeOptions eo = o;
    if (o.fmt == EXP_JPEG && o.maxKB > 0) {
        // binary search for the highest quality that fits the size limit
        int lo = 5, hi = std::max(5, o.quality), best = -1;
        std::vector<uint8_t> tmp;
        while (lo <= hi) {
            int mid = (lo + hi) / 2;
            eo.quality = mid;
            if (!EncodeImage(eo, rgba, w, h, tmp)) return false;
            if (tmp.size() <= (size_t)o.maxKB * 1024) { best = mid; bytes.swap(tmp); lo = mid + 1; }
            else hi = mid - 1;
        }
        if (best < 0) { eo.quality = 5; if (!EncodeImage(eo, rgba, w, h, bytes)) return false; best = 5; }
        if (usedQuality) *usedQuality = best;
    } else {
        if (!EncodeImage(eo, rgba, w, h, bytes)) return false;
        if (usedQuality) *usedQuality = o.quality;
    }
    FILE* f = _wfopen(path.c_str(), L"wb");
    if (!f) return false;
    bool ok = fwrite(bytes.data(), 1, bytes.size(), f) == bytes.size();
    fclose(f);
    return ok;
}

void ApplyWatermark(std::vector<uint16_t>& rgba, int w, int h, const std::wstring& text, float sizePct, float opacity, int pos, bool dark) {
    if (text.empty() || opacity <= 0) return;
    int fontPx = std::max(8, (int)(std::min(w, h) * sizePct / 100.0f));
    HDC dc = CreateCompatibleDC(nullptr);
    HFONT font = CreateFontW(-fontPx, 0, 0, 0, FW_SEMIBOLD, 0, 0, 0, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                             ANTIALIASED_QUALITY, DEFAULT_PITCH, L"Segoe UI");
    HGDIOBJ oldFont = SelectObject(dc, font);
    SIZE sz = {};
    GetTextExtentPoint32W(dc, text.c_str(), (int)text.size(), &sz);
    int tw = sz.cx + 4, th = sz.cy + 4;
    BITMAPINFO bi = {};
    bi.bmiHeader.biSize = sizeof(bi.bmiHeader); bi.bmiHeader.biWidth = tw; bi.bmiHeader.biHeight = -th;
    bi.bmiHeader.biPlanes = 1; bi.bmiHeader.biBitCount = 32; bi.bmiHeader.biCompression = BI_RGB;
    void* bits = nullptr;
    HBITMAP bmp = CreateDIBSection(dc, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
    HGDIOBJ oldBmp = SelectObject(dc, bmp);
    memset(bits, 0, (size_t)tw * th * 4);
    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, RGB(255, 255, 255));
    TextOutW(dc, 2, 2, text.c_str(), (int)text.size());
    GdiFlush();
    int margin = (int)(std::min(w, h) * 0.03f);
    int col = pos % 3, row = pos / 3;
    int x0 = col == 0 ? margin : col == 1 ? (w - tw) / 2 : w - tw - margin;
    int y0 = row == 0 ? margin : row == 1 ? (h - th) / 2 : h - th - margin;
    const uint8_t* cov = (const uint8_t*)bits;
    uint16_t ink = dark ? 0 : 65535;
    for (int y = 0; y < th; y++) {
        int yy = y0 + y; if (yy < 0 || yy >= h) continue;
        for (int x = 0; x < tw; x++) {
            int xx = x0 + x; if (xx < 0 || xx >= w) continue;
            float a = cov[((size_t)y * tw + x) * 4 + 1] / 255.0f * opacity;
            if (a <= 0) continue;
            uint16_t* p = &rgba[((size_t)yy * w + xx) * 4];
            for (int c = 0; c < 3; c++) p[c] = (uint16_t)(p[c] * (1 - a) + ink * a);
        }
    }
    SelectObject(dc, oldFont); DeleteObject(font);
    SelectObject(dc, oldBmp); DeleteObject(bmp); DeleteDC(dc);
}

void RevealInExplorer(const std::wstring& path) {
    std::wstring args = L"/select,\"" + path + L"\"";
    ShellExecuteW(nullptr, L"open", L"explorer.exe", args.c_str(), nullptr, SW_SHOWNORMAL);
}

// ------------------------------------------------------------------ dialogs
static std::wstring RunDialog(IFileDialog* d, HWND owner) {
    std::wstring r;
    if (SUCCEEDED(d->Show(owner))) {
        IShellItem* it = nullptr;
        if (SUCCEEDED(d->GetResult(&it))) {
            PWSTR p = nullptr;
            if (SUCCEEDED(it->GetDisplayName(SIGDN_FILESYSPATH, &p))) { r = p; CoTaskMemFree(p); }
            it->Release();
        }
    }
    d->Release();
    return r;
}

std::wstring OpenFileDialog(HWND owner) {
    IFileOpenDialog* d = nullptr;
    if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&d)))) return {};
    COMDLG_FILTERSPEC spec[] = {
        { TW(L"Все изображения", L"All images"), L"*.cr2;*.cr3;*.crw;*.nef;*.nrw;*.arw;*.srf;*.sr2;*.raf;*.orf;*.rw2;*.pef;*.dng;*.srw;*.x3f;*.3fr;*.fff;*.iiq;*.mos;*.mrw;*.erf;*.kdc;*.dcr;*.rwl;*.raw;*.mef;*.jpg;*.jpeg;*.jpe;*.png;*.tif;*.tiff;*.bmp;*.webp;*.heic;*.heif;*.jxr;*.avif" },
        { L"RAW", L"*.cr2;*.cr3;*.crw;*.nef;*.nrw;*.arw;*.srf;*.sr2;*.raf;*.orf;*.rw2;*.pef;*.dng;*.srw;*.x3f;*.3fr;*.fff;*.iiq;*.mos;*.mrw;*.erf;*.kdc;*.dcr;*.rwl;*.raw;*.mef" },
        { L"JPEG / PNG / TIFF", L"*.jpg;*.jpeg;*.jpe;*.png;*.tif;*.tiff;*.bmp;*.webp" },
        { TW(L"Все файлы", L"All files"), L"*.*" },
    };
    d->SetFileTypes(4, spec);
    d->SetTitle(TW(L"Открыть фото", L"Open photo"));
    return RunDialog(d, owner);
}

std::wstring SaveFileDialog(HWND owner, const std::wstring& defName, int fmt) {
    IFileSaveDialog* d = nullptr;
    if (FAILED(CoCreateInstance(CLSID_FileSaveDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&d)))) return {};
    COMDLG_FILTERSPEC jpg = { L"JPEG", L"*.jpg" }, png = { L"PNG", L"*.png" }, tif = { L"TIFF", L"*.tif" };
    COMDLG_FILTERSPEC* s = fmt == EXP_JPEG ? &jpg : (fmt == EXP_TIFF ? &tif : &png);
    d->SetFileTypes(1, s);
    d->SetDefaultExtension(fmt == EXP_JPEG ? L"jpg" : (fmt == EXP_TIFF ? L"tif" : L"png"));
    d->SetFileName(defName.c_str());
    d->SetTitle(TW(L"Экспорт", L"Export"));
    return RunDialog(d, owner);
}

std::wstring PickFolderDialog(HWND owner) {
    IFileOpenDialog* d = nullptr;
    if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&d)))) return {};
    DWORD o = 0; d->GetOptions(&o); d->SetOptions(o | FOS_PICKFOLDERS);
    d->SetTitle(TW(L"Выберите папку", L"Choose a folder"));
    return RunDialog(d, owner);
}

static std::vector<std::wstring> RunMultiDialog(IFileOpenDialog* d, HWND owner) {
    std::vector<std::wstring> r;
    DWORD o = 0; d->GetOptions(&o); d->SetOptions(o | FOS_ALLOWMULTISELECT);
    if (SUCCEEDED(d->Show(owner))) {
        IShellItemArray* arr = nullptr;
        if (SUCCEEDED(d->GetResults(&arr))) {
            DWORD n = 0; arr->GetCount(&n);
            for (DWORD i = 0; i < n; i++) {
                IShellItem* it = nullptr;
                if (SUCCEEDED(arr->GetItemAt(i, &it))) {
                    PWSTR p = nullptr;
                    if (SUCCEEDED(it->GetDisplayName(SIGDN_FILESYSPATH, &p))) { r.push_back(p); CoTaskMemFree(p); }
                    it->Release();
                }
            }
            arr->Release();
        }
    }
    d->Release();
    return r;
}

std::vector<std::wstring> OpenFilesDialog(HWND owner) {
    IFileOpenDialog* d = nullptr;
    if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&d)))) return {};
    COMDLG_FILTERSPEC spec[] = {
        { TW(L"Все изображения", L"All images"), L"*.cr2;*.cr3;*.crw;*.nef;*.nrw;*.arw;*.srf;*.sr2;*.raf;*.orf;*.rw2;*.pef;*.dng;*.srw;*.x3f;*.3fr;*.fff;*.iiq;*.mos;*.mrw;*.erf;*.kdc;*.dcr;*.rwl;*.raw;*.mef;*.jpg;*.jpeg;*.jpe;*.png;*.tif;*.tiff;*.bmp;*.webp;*.heic;*.heif;*.jxr;*.avif" },
        { L"RAW", L"*.cr2;*.cr3;*.crw;*.nef;*.nrw;*.arw;*.srf;*.sr2;*.raf;*.orf;*.rw2;*.pef;*.dng;*.srw;*.x3f;*.3fr;*.fff;*.iiq;*.mos;*.mrw;*.erf;*.kdc;*.dcr;*.rwl;*.raw;*.mef" },
        { L"JPEG / PNG / TIFF", L"*.jpg;*.jpeg;*.jpe;*.png;*.tif;*.tiff;*.bmp;*.webp" },
        { TW(L"Все файлы", L"All files"), L"*.*" },
    };
    d->SetFileTypes(4, spec);
    d->SetTitle(TW(L"Открыть фото", L"Open photos"));
    return RunMultiDialog(d, owner);
}

std::vector<std::wstring> OpenPresetFilesDialog(HWND owner) {
    IFileOpenDialog* d = nullptr;
    if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&d)))) return {};
    COMDLG_FILTERSPEC spec[] = { { TW(L"Пресеты Muon", L"Muon presets"), L"*.muonpreset" }, { TW(L"Все файлы", L"All files"), L"*.*" } };
    d->SetFileTypes(2, spec);
    d->SetTitle(TW(L"Импорт пресетов", L"Import presets"));
    return RunMultiDialog(d, owner);
}

std::wstring SavePresetDialog(HWND owner, const std::wstring& defName) {
    IFileSaveDialog* d = nullptr;
    if (FAILED(CoCreateInstance(CLSID_FileSaveDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&d)))) return {};
    COMDLG_FILTERSPEC spec = { TW(L"Пресет Muon", L"Muon preset"), L"*.muonpreset" };
    d->SetFileTypes(1, &spec);
    d->SetDefaultExtension(L"muonpreset");
    d->SetFileName(defName.c_str());
    d->SetTitle(TW(L"Сохранить пресет", L"Save preset"));
    return RunDialog(d, owner);
}

bool IsDirectory(const std::wstring& p) {
    DWORD a = GetFileAttributesW(p.c_str());
    return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY);
}
