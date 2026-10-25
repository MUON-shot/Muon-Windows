#pragma once
#include <string>
#include <vector>
#include <cstdint>
#include <windows.h>

struct LoadedImage {
    int w = 0, h = 0;
    bool isRaw = false;
    std::vector<uint16_t> rgba16;   // RAW: linear RGB 16 bit
    std::vector<uint8_t> rgba8;     // other formats: sRGB 8 bit
    std::vector<float> preview;       // linear RGB preview for auto tools
    int sw = 0, sh = 0;
    std::vector<uint8_t> depthSrc;  // sRGB RGBA8, long side up to 770, input for the depth network
    int dw = 0, dh = 0;
    std::string info;               // camera / exposure summary
    std::wstring path;
    std::string error;
    std::string notice;             // shown to the user after loading (e.g. RAW fell back to the embedded JPEG)
};

bool LoadImageFile(const std::wstring& path, LoadedImage& out);
bool LoadThumbnail(const std::wstring& path, int maxSize, std::vector<uint8_t>& rgba, int& w, int& h);
// Same, but a crash inside a decoder (damaged file) just reports failure.
bool LoadImageFileSafe(const std::wstring& path, LoadedImage& out);
bool LoadThumbnailSafe(const std::wstring& path, int maxSize, std::vector<uint8_t>& rgba, int& w, int& h);

enum ExportFormat { EXP_JPEG = 0, EXP_PNG, EXP_TIFF };
struct EncodeOptions {
    int fmt = EXP_JPEG;
    bool bits16 = false;     // PNG / TIFF
    int quality = 92;        // JPEG
    bool chroma444 = false;  // JPEG: full colour resolution
    int tiffComp = 1;        // 0 none, 1 LZW, 2 ZIP
    int maxKB = 0;           // JPEG: limit file size (0 = off)
};
bool EncodeImage(const EncodeOptions& o, const std::vector<uint16_t>& rgba, int w, int h, std::vector<uint8_t>& bytes);
bool SaveImage(const std::wstring& path, const EncodeOptions& o, const std::vector<uint16_t>& rgba, int w, int h, int* usedQuality = nullptr);
void ApplyWatermark(std::vector<uint16_t>& rgba, int w, int h, const std::wstring& text, float sizePct, float opacity, int pos, bool dark);
void RevealInExplorer(const std::wstring& path);

bool IsSupportedExt(const std::wstring& path);
bool IsRawExt(const std::wstring& path);
std::vector<std::wstring> ListFolderImages(const std::wstring& folder);

std::wstring OpenFileDialog(HWND owner);
std::wstring SaveFileDialog(HWND owner, const std::wstring& defName, int fmt);
std::wstring PickFolderDialog(HWND owner);
std::vector<std::wstring> OpenFilesDialog(HWND owner);
std::vector<std::wstring> OpenPresetFilesDialog(HWND owner);
std::wstring SavePresetDialog(HWND owner, const std::wstring& defName);
bool IsDirectory(const std::wstring& p);

std::wstring Utf8ToWide(const std::string& s);
std::string WideToUtf8(const std::wstring& s);
std::wstring ExeDir();
bool ReadTextFile(const std::wstring& path, std::string& out);
bool WriteTextFile(const std::wstring& path, const std::string& s);
