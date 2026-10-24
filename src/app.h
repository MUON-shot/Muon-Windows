#pragma once
#include "params.h"
#include "preset.h"
#include "gpu.h"
#include "imageio.h"
#include <windows.h>
#include <string>
#include <vector>
#include <memory>
#include <future>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <deque>
#include <atomic>

struct ImFont;

struct ThumbEntry {
    Tex* tex = nullptr;
    int w = 0, h = 0;
    bool requested = false, failed = false;
    int lastUse = 0;
};

struct ThumbResult { std::wstring path; std::vector<uint8_t> rgba; int w, h; bool ok; };

enum Tool { TOOL_EDIT = 0, TOOL_CROP, TOOL_MASK };

struct ExportSettings {
    int fmt = 0;                // EXP_JPEG / EXP_PNG / EXP_TIFF
    bool bits16 = false;
    int quality = 92;
    bool chroma444 = false;
    int tiffComp = 1;           // 0 none, 1 LZW, 2 ZIP
    bool limitSize = false;
    int maxKB = 2000;
    int sizeMode = 0;           // 0 original, 1 long edge, 2 short edge, 3 width, 4 height, 5 megapixels, 6 percent
    int sizeValue = 2048;
    float megapixels = 12;
    int percent = 50;
    bool noUpscale = true;
    int sharpen = 0;            // 0 off, 1 screen, 2 matte paper, 3 glossy paper
    int sharpenAmt = 1;         // 0 low, 1 standard, 2 high
    int dest = 0;               // 0 ask, 1 next to original, 2 chosen folder
    std::wstring folder;
    char nameTpl[128] = "{name}";
    bool overwrite = false;
    bool reveal = false;
    bool watermark = false;
    char wmText[128] = "";
    int wmPos = 8;
    float wmSize = 3.0f;
    float wmOpacity = 70;
    bool wmDark = false;
};
enum Pick { PICK_NONE = 0, PICK_WB, PICK_MASK_COLOR, PICK_MASK_LUMA, PICK_FX_COLOR, PICK_FX_LUMA };
enum BottomTab { TAB_LIBRARY = 0, TAB_CLASSIC, TAB_EXPERIMENTS };
enum AltView { ALT_NONE = 0, ALT_HIGHLIGHTS, ALT_SHADOWS };

struct SettingsClip { Params p; uint32_t groups = 0; bool valid = false; };

struct App {
    HWND hwnd = nullptr;
    float dpi = 1.0f;
    ImFont* fontMain = nullptr;
    ImFont* fontBold = nullptr;
    ImFont* fontBig = nullptr;
    ImFont* fontSmall = nullptr;

    // document
    Params params;
    std::vector<Params> history;
    int histPos = -1;
    uint32_t rev = 1, committedRev = 0, savedRev = 0, renderedRev = 0;
    std::wstring curPath;
    std::string info;
    bool isRaw = false;
    LoadedImage* current = nullptr;          // kept for auto tools (small preview)
    std::future<std::shared_ptr<LoadedImage>> loader;
    bool loading = false;
    std::wstring loadingPath;

    // working collection (the filmstrip) and its selection
    std::vector<std::wstring> collection;
    std::vector<char> collSel;
    int collAnchor = -1;
    int curIndex = -1;
    uint32_t thumbRev = 0;                   // committedRev the current thumbnail was made from
    double lastCommitTime = 0;

    // library tab
    std::wstring libFolder;
    std::vector<std::wstring> libFiles;
    std::vector<char> libSel;
    int libAnchor = -1;
    float libThumb = 170;
    bool libOpenAfterImport = true;

    // thumbnail worker
    std::thread thumbThread;
    std::mutex thumbMx;
    std::condition_variable thumbCv;
    std::deque<std::wstring> thumbQueue;
    std::vector<ThumbResult> thumbDone;
    std::atomic<bool> quit{ false };

    // view
    bool fit = true;
    float zoom = 1.0f;          // screen px per full-res px
    float panX = 0, panY = 0;
    int workLong = 0;
    int bottomTab = TAB_CLASSIC;
    int tool = TOOL_EDIT;
    int selMask = -1;
    int selEffect = -1;
    bool showOverlay = true;
    bool before = false;
    bool clipLo = false, clipHi = false;
    int altView = ALT_NONE;     // Alt + drag on exposure / tone sliders
    int pick = PICK_NONE;
    bool needRender = true;
    bool showHelp = false;
    bool showFilmstrip = true;

    // brush
    float brushSize = 0.06f, brushFeather = 0.6f, brushFlow = 0.6f;
    bool brushErase = false;
    bool painting = false;
    float lastDabX = -1, lastDabY = -1;

    // panel state
    int curveCh = 0, hslTab = 0, gradeTab = 0;

    // export
    bool exportOpen = false;
    ExportSettings exp;
    std::vector<std::wstring> exportTargets;   // photos the export dialog was opened for (empty = current)
    // batch export
    bool batchActive = false;
    std::vector<std::wstring> batchFiles;
    size_t batchIndex = 0;
    std::wstring batchOut, batchReturn;
    int batchDone = 0;

    // settings clipboard and presets
    SettingsClip clip;
    std::wstring copySource;                   // photo for the "copy selected settings" dialog
    bool copyDialogOpen = false;
    uint32_t copyGroups = 0;
    std::vector<Preset> userPresets;
    int hoverPreset = -1;                      // live preview while a preset is hovered
    int hoverPresetPrev = -1;
    bool newPresetOpen = false;

    // misc
    std::string status;
    double statusTime = 0;
    double lastSave = 0;
    bool noPersist = false;   // --uitest: never write sidecars or settings
};

extern App g_app;

void AppInit();
void AppShutdown();
void AppFrame();                     // builds UI and renders the preview when needed
void AppOpenFile(const std::wstring& path);
void AppDropFiles(const std::vector<std::wstring>& paths);
void SetStatus(const std::string& s);
void ApplyLanguage();                // window title etc. after g_lang changes
void AppEmergencySave();             // save edits right now (GPU reset, shutdown)
std::wstring AppCurrentPath();

// GPU choice (main.cpp / ui.cpp)
extern std::vector<std::wstring> g_gpuNames;
extern int g_gpuUsed;
std::wstring ReadGpuPreference();    // adapter description, empty = most powerful
void WriteGpuPreference(const std::wstring& name);
