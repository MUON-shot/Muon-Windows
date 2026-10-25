#pragma once
#include "params.h"
#include <string>
#include <vector>
#include <cstdint>

// Groups of settings. Used by presets and by copy / paste of settings between photos.
enum SettingsGroup : uint32_t {
    SG_WB       = 1u << 0,   // temperature, tint
    SG_EXPOSURE = 1u << 1,
    SG_TONE     = 1u << 2,   // contrast, highlights, shadows, whites, blacks
    SG_PRESENCE = 1u << 3,   // texture, clarity, dehaze, vibrance, saturation, profile
    SG_CURVE    = 1u << 4,
    SG_HSL      = 1u << 5,
    SG_GRADE    = 1u << 6,
    SG_BW       = 1u << 7,
    SG_DETAIL   = 1u << 8,   // sharpening, noise reduction
    SG_LENS     = 1u << 9,
    SG_VIGGRAIN = 1u << 10,
    SG_EFFECTS  = 1u << 11,  // experimental stack
    SG_CROP     = 1u << 12,  // crop, rotation, perspective
    SG_MASKS    = 1u << 13,
    SG_OPTICS   = 1u << 14,  // glass, lights, depth corrections, fog, depth of field
    SG_COUNT    = 15,
    SG_DEVELOP  = (1u << 12) - 1,       // everything except crop and masks
    SG_ALL      = (1u << 15) - 1,
    SG_LOOK     = SG_TONE | SG_PRESENCE | SG_CURVE | SG_HSL | SG_GRADE | SG_BW | SG_VIGGRAIN,
};

const char* GroupName(int bit);           // in the current UI language
void CopyGroups(Params& dst, const Params& src, uint32_t groups);

// A shareable preset (.muonpreset file or a text code).
struct Preset {
    std::string name, author, description;
    uint32_t groups = SG_DEVELOP;
    Params params;
    std::wstring file;                    // where it is stored (user presets)
    bool builtin = false;
};

std::string PresetToText(const Preset& p);
bool PresetFromText(const std::string& text, Preset& out, std::string* error = nullptr);
std::string PresetToCode(const Preset& p);                 // "MUON1-..." single line for messengers
bool PresetFromCode(const std::string& code, Preset& out, std::string* error = nullptr);
bool LooksLikePresetCode(const std::string& s);

bool ClipboardSetText(const std::string& utf8);
std::string ClipboardGetText();

std::vector<Preset> BuiltinPresetList();
