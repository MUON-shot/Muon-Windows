#pragma once
#include "params.h"
#include <vector>
#include <string>

enum ParamKind { PK_SLIDER = 0, PK_TOGGLE, PK_COMBO };

struct FxParam {
    const char* label;
    float mn, mx, def;
    int kind = PK_SLIDER;
    const char* options = nullptr;  // ImGui combo string ("a\0b\0")
};

struct FxColor { const char* label; float r, g, b; };

struct FxDesc {
    const char* name;
    const char* desc;
    std::vector<FxParam> params;
    std::vector<FxColor> colors;
    int defBlend = 0;
};

const FxDesc& GetFxDesc(int type);
Effect MakeEffect(int type);
void RandomizeEffect(Effect& e);
std::string FxShaderSource(int type);
std::string FxCombineSource();

const char* BlendNames();             // combo string in the current language
extern const char* kAsciiSets[];     // built-in charsets (dark -> bright)
