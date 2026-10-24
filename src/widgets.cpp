#include "widgets.h"
#include "imgui_internal.h"
#include <cmath>
#include <cstring>
#include <algorithm>
#include <cstdio>

namespace W {

ImU32 kAccent = IM_COL32(110, 168, 255, 255);

void SetupStyle(float dpi) {
    ImGuiStyle& s = ImGui::GetStyle();
    s = ImGuiStyle();
    s.WindowPadding = ImVec2(12, 10);
    s.FramePadding = ImVec2(8, 5);
    s.ItemSpacing = ImVec2(8, 6);
    s.ItemInnerSpacing = ImVec2(6, 4);
    s.ScrollbarSize = 8;
    s.GrabMinSize = 8;
    s.WindowRounding = 8; s.ChildRounding = 8; s.FrameRounding = 6; s.PopupRounding = 8;
    s.ScrollbarRounding = 4; s.GrabRounding = 4; s.TabRounding = 5;
    s.WindowBorderSize = 0; s.ChildBorderSize = 0; s.PopupBorderSize = 1; s.FrameBorderSize = 0;
    s.SeparatorTextBorderSize = 1;
    ImVec4* c = s.Colors;
    ImVec4 bg(0.075f, 0.078f, 0.085f, 1), panel(0.105f, 0.108f, 0.118f, 1), frame(0.16f, 0.165f, 0.18f, 1);
    ImVec4 acc(0.43f, 0.66f, 1.0f, 1);
    c[ImGuiCol_Text] = ImVec4(0.86f, 0.87f, 0.89f, 1);
    c[ImGuiCol_TextDisabled] = ImVec4(0.50f, 0.52f, 0.56f, 1);
    c[ImGuiCol_WindowBg] = bg;
    c[ImGuiCol_ChildBg] = panel;
    c[ImGuiCol_PopupBg] = ImVec4(0.12f, 0.125f, 0.135f, 0.98f);
    c[ImGuiCol_Border] = ImVec4(1, 1, 1, 0.07f);
    c[ImGuiCol_FrameBg] = frame;
    c[ImGuiCol_FrameBgHovered] = ImVec4(0.2f, 0.205f, 0.225f, 1);
    c[ImGuiCol_FrameBgActive] = ImVec4(0.23f, 0.24f, 0.26f, 1);
    c[ImGuiCol_TitleBg] = bg; c[ImGuiCol_TitleBgActive] = bg;
    c[ImGuiCol_MenuBarBg] = bg;
    c[ImGuiCol_ScrollbarBg] = ImVec4(0, 0, 0, 0);
    c[ImGuiCol_ScrollbarGrab] = ImVec4(1, 1, 1, 0.12f);
    c[ImGuiCol_ScrollbarGrabHovered] = ImVec4(1, 1, 1, 0.2f);
    c[ImGuiCol_ScrollbarGrabActive] = ImVec4(1, 1, 1, 0.28f);
    c[ImGuiCol_CheckMark] = acc;
    c[ImGuiCol_SliderGrab] = acc; c[ImGuiCol_SliderGrabActive] = acc;
    c[ImGuiCol_Button] = ImVec4(0.17f, 0.175f, 0.19f, 1);
    c[ImGuiCol_ButtonHovered] = ImVec4(0.22f, 0.225f, 0.245f, 1);
    c[ImGuiCol_ButtonActive] = ImVec4(0.27f, 0.28f, 0.3f, 1);
    c[ImGuiCol_Header] = ImVec4(0.15f, 0.155f, 0.17f, 1);
    c[ImGuiCol_HeaderHovered] = ImVec4(0.19f, 0.195f, 0.21f, 1);
    c[ImGuiCol_HeaderActive] = ImVec4(0.22f, 0.225f, 0.24f, 1);
    c[ImGuiCol_Separator] = ImVec4(1, 1, 1, 0.06f);
    c[ImGuiCol_Tab] = ImVec4(0, 0, 0, 0);
    c[ImGuiCol_TabHovered] = ImVec4(0.2f, 0.2f, 0.22f, 1);
    c[ImGuiCol_TabSelected] = ImVec4(0.18f, 0.185f, 0.2f, 1);
    c[ImGuiCol_TextSelectedBg] = ImVec4(acc.x, acc.y, acc.z, 0.35f);
    c[ImGuiCol_ModalWindowDimBg] = ImVec4(0, 0, 0, 0.55f);
    c[ImGuiCol_NavHighlight] = ImVec4(acc.x, acc.y, acc.z, 0.5f);
    s.ScaleAllSizes(dpi);
}

ImU32 HueColor(float h, float s, float v) {
    float r, g, b;
    ImGui::ColorConvertHSVtoRGB(std::fmod(h / 360.0f + 1.0f, 1.0f), s, v, r, g, b);
    return IM_COL32((int)(r * 255), (int)(g * 255), (int)(b * 255), 255);
}

void Hint(const char* text) {
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal)) ImGui::SetTooltip("%s", text);
}

bool Slider(const char* label, float* v, float mn, float mx, float def, const char* fmt, const ImU32* grad, int gradN) {
    ImGuiWindow* win = ImGui::GetCurrentWindow();
    if (win->SkipItems) return false;
    ImGuiContext& g = *GImGui;
    ImGuiID id = win->GetID(label);
    float fs = ImGui::GetFontSize();
    float w = ImGui::GetContentRegionAvail().x;
    float h = fs + fs * 0.95f;
    ImVec2 pos = win->DC.CursorPos;
    ImRect bb(pos, ImVec2(pos.x + w, pos.y + h));
    ImGui::ItemSize(bb);
    if (!ImGui::ItemAdd(bb, id)) return false;

    char vbuf[32];
    if (!fmt) fmt = (mn < 0) ? "%+.0f" : "%.0f";
    snprintf(vbuf, sizeof(vbuf), fmt, *v);
    if (strcmp(vbuf, "+0") == 0 || strcmp(vbuf, "-0") == 0) strcpy(vbuf, "0");
    if (strcmp(vbuf, "+0.00") == 0 || strcmp(vbuf, "-0.00") == 0) strcpy(vbuf, "0.00");

    bool hovered = ImGui::ItemHoverable(bb, id, 0);
    bool changed = false;
    bool tempInput = ImGui::TempInputIsActive(id);
    float pad = fs * 0.45f;
    float trackL = bb.Min.x + pad, trackR = bb.Max.x - pad;
    float trackY = bb.Min.y + fs + fs * 0.42f;
    float range = mx - mn;
    auto valToX = [&](float x) { return trackL + (std::clamp(x, mn, mx) - mn) / range * (trackR - trackL); };

    if (!tempInput) {
        bool clicked = hovered && ImGui::IsMouseClicked(0);
        if (clicked && g.IO.KeyCtrl) tempInput = true;
        else if (hovered && ImGui::IsMouseDoubleClicked(0)) {
            if (*v != def) { *v = def; changed = true; }
            ImGui::ClearActiveID();
        } else if (clicked) {
            ImGui::SetActiveID(id, win);
            ImGui::SetFocusID(id, win);
            ImGui::FocusWindow(win);
            float mx0 = g.IO.MousePos.x;
            if (g.IO.MousePos.y > bb.Min.y + fs * 0.9f && std::fabs(mx0 - valToX(*v)) > fs * 0.6f) {
                float nv = mn + (mx0 - trackL) / (trackR - trackL) * range;
                nv = std::clamp(nv, mn, mx);
                if (nv != *v) { *v = nv; changed = true; }
            }
        }
        if (g.ActiveId == id) {
            if (g.IO.MouseDown[0]) {
                float dx = g.IO.MouseDelta.x;
                if (dx != 0) {
                    float speed = range / std::max(1.0f, trackR - trackL) * (g.IO.KeyShift ? 0.15f : 1.0f);
                    float nv = std::clamp(*v + dx * speed, mn, mx);
                    if (nv != *v) { *v = nv; changed = true; }
                }
            } else ImGui::ClearActiveID();
        }
    }
    if (tempInput) {
        ImRect vr(ImVec2(bb.Max.x - fs * 4.5f, bb.Min.y), ImVec2(bb.Max.x, bb.Min.y + fs * 1.25f));
        float mnv = mn, mxv = mx;
        if (ImGui::TempInputScalar(vr, id, label, ImGuiDataType_Float, v, "%.2f", &mnv, &mxv)) changed = true;
    }

    ImDrawList* dl = win->DrawList;
    bool active = g.ActiveId == id;
    ImU32 textCol = ImGui::GetColorU32(ImGuiCol_Text);
    ImU32 dimCol = ImGui::GetColorU32(ImGuiCol_TextDisabled);
    const char* labelEnd = ImGui::FindRenderedTextEnd(label);
    dl->AddText(ImVec2(bb.Min.x + pad * 0.3f, bb.Min.y), (hovered || active) ? textCol : IM_COL32(190, 192, 198, 255), label, labelEnd);
    if (!tempInput) {
        ImVec2 ts = ImGui::CalcTextSize(vbuf);
        dl->AddText(ImVec2(bb.Max.x - ts.x - pad * 0.3f, bb.Min.y), (*v != def) ? kAccent : dimCol, vbuf);
    }
    float th = std::max(2.0f, fs * 0.16f);
    ImVec2 t0(trackL, trackY - th * 0.5f), t1(trackR, trackY + th * 0.5f);
    if (grad && gradN >= 2) {
        float segW = (trackR - trackL) / (gradN - 1);
        for (int i = 0; i < gradN - 1; i++) {
            ImVec2 a(trackL + segW * i, t0.y), b(trackL + segW * (i + 1), t1.y);
            dl->AddRectFilledMultiColor(a, b, grad[i], grad[i + 1], grad[i + 1], grad[i]);
        }
    } else {
        dl->AddRectFilled(t0, t1, IM_COL32(255, 255, 255, 28), th);
        float zx = valToX(std::clamp(def, mn, mx)), vx = valToX(*v);
        dl->AddRectFilled(ImVec2(std::min(zx, vx), t0.y), ImVec2(std::max(zx, vx), t1.y), (hovered || active) ? kAccent : IM_COL32(150, 160, 178, 200), th);
    }
    float kr = fs * (active ? 0.36f : hovered ? 0.33f : 0.28f);
    float kx = valToX(*v);
    dl->AddCircleFilled(ImVec2(kx, trackY), kr + 1.5f, IM_COL32(0, 0, 0, 90));
    dl->AddCircleFilled(ImVec2(kx, trackY), kr, IM_COL32(232, 234, 238, 255));
    if (changed) ImGui::MarkItemEdited(id);
    return changed;
}

bool Segmented(const char* id, int* v, const char* const* items, int n, float height) {
    ImGui::PushID(id);
    float w = ImGui::GetContentRegionAvail().x;
    float sp = 2;
    float bw = (w - sp * (n - 1)) / n;
    bool changed = false;
    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImVec2 p = ImGui::GetCursorScreenPos();
    float h = height > 0 ? height : ImGui::GetFrameHeight();
    dl->AddRectFilled(p, ImVec2(p.x + w, p.y + h), IM_COL32(255, 255, 255, 12), 6);
    for (int i = 0; i < n; i++) {
        if (i) ImGui::SameLine(0, sp);
        ImGui::PushID(i);
        ImVec2 bp = ImGui::GetCursorScreenPos();
        if (ImGui::InvisibleButton("##seg", ImVec2(bw, h))) { if (*v != i) { *v = i; changed = true; } }
        bool hov = ImGui::IsItemHovered();
        if (*v == i) dl->AddRectFilled(bp, ImVec2(bp.x + bw, bp.y + h), IM_COL32(255, 255, 255, 34), 6);
        else if (hov) dl->AddRectFilled(bp, ImVec2(bp.x + bw, bp.y + h), IM_COL32(255, 255, 255, 16), 6);
        ImVec2 ts = ImGui::CalcTextSize(items[i]);
        dl->AddText(ImVec2(bp.x + (bw - ts.x) * 0.5f, bp.y + (h - ts.y) * 0.5f),
                    *v == i ? IM_COL32(240, 242, 246, 255) : IM_COL32(150, 154, 162, 255), items[i]);
        ImGui::PopID();
    }
    ImGui::PopID();
    return changed;
}

void Histogram(const float hist[4][256], ImVec2 size, bool* clipLo, bool* clipHi) {
    ImVec2 p = ImGui::GetCursorScreenPos();
    ImGui::InvisibleButton("##hist", size);
    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->AddRectFilled(p, ImVec2(p.x + size.x, p.y + size.y), IM_COL32(0, 0, 0, 70), 6);
    float mxv = 1;
    for (int c = 0; c < 3; c++) for (int i = 2; i < 254; i++) mxv = std::max(mxv, hist[c][i]);
    ImU32 cols[3] = { IM_COL32(235, 70, 70, 120), IM_COL32(70, 220, 90, 110), IM_COL32(70, 120, 255, 130) };
    float bw = size.x / 256.0f;
    for (int c = 0; c < 3; c++) {
        for (int i = 0; i < 256; i++) {
            float hgt = std::min(1.0f, std::sqrt(hist[c][i] / mxv)) * (size.y - 6);
            if (hgt < 0.5f) continue;
            float x = p.x + i * bw;
            dl->AddRectFilled(ImVec2(x, p.y + size.y - 3 - hgt), ImVec2(x + bw + 0.6f, p.y + size.y - 3), cols[c]);
        }
    }
    // clipping toggles in the corners
    float tr = ImGui::GetFontSize() * 0.38f;
    auto tri = [&](ImVec2 c, bool on, bool left, bool* flag, ImU32 col) {
        ImVec2 a = left ? ImVec2(c.x, c.y) : ImVec2(c.x, c.y);
        ImVec2 q1 = left ? ImVec2(a.x + tr * 1.6f, a.y) : ImVec2(a.x - tr * 1.6f, a.y);
        ImVec2 q2 = ImVec2(a.x, a.y + tr * 1.6f);
        dl->AddTriangleFilled(a, q1, q2, on ? col : IM_COL32(255, 255, 255, 50));
        ImRect r(ImVec2(std::min(a.x, q1.x) - 2, a.y - 2), ImVec2(std::max(a.x, q1.x) + 2, q2.y + 2));
        if (ImGui::IsMouseHoveringRect(r.Min, r.Max) && ImGui::IsMouseClicked(0) && flag) *flag = !*flag;
    };
    bool lo = clipLo && *clipLo, hi = clipHi && *clipHi;
    tri(ImVec2(p.x + 5, p.y + 5), lo, true, clipLo, IM_COL32(80, 140, 255, 255));
    tri(ImVec2(p.x + size.x - 5, p.y + 5), hi, false, clipHi, IM_COL32(255, 80, 80, 255));
}

bool CurveEditor(const char* id, Curve& c, ImU32 color, const float* hist, float size) {
    ImGuiStorage* st = ImGui::GetStateStorage();
    ImGui::PushID(id);
    ImGuiID dragKey = ImGui::GetID("drag");
    ImVec2 p = ImGui::GetCursorScreenPos();
    ImVec2 sz(size, size);
    ImGui::InvisibleButton("##curve", sz, ImGuiButtonFlags_MouseButtonLeft | ImGuiButtonFlags_MouseButtonRight);
    bool hovered = ImGui::IsItemHovered();
    bool active = ImGui::IsItemActive();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->AddRectFilled(p, ImVec2(p.x + size, p.y + size), IM_COL32(0, 0, 0, 60), 6);
    if (hist) {
        float mxv = 1; for (int i = 2; i < 254; i++) mxv = std::max(mxv, hist[i]);
        for (int i = 0; i < 256; i++) {
            float hgt = std::min(1.0f, std::sqrt(hist[i] / mxv)) * size * 0.9f;
            float x = p.x + i * size / 256.0f;
            dl->AddRectFilled(ImVec2(x, p.y + size - hgt), ImVec2(x + size / 256.0f + 0.5f, p.y + size), IM_COL32(255, 255, 255, 14));
        }
    }
    for (int i = 1; i < 4; i++) {
        float t = i / 4.0f;
        dl->AddLine(ImVec2(p.x + t * size, p.y), ImVec2(p.x + t * size, p.y + size), IM_COL32(255, 255, 255, 18));
        dl->AddLine(ImVec2(p.x, p.y + t * size), ImVec2(p.x + size, p.y + t * size), IM_COL32(255, 255, 255, 18));
    }
    dl->AddLine(ImVec2(p.x, p.y + size), ImVec2(p.x + size, p.y), IM_COL32(255, 255, 255, 25));

    auto toScr = [&](float x, float y) { return ImVec2(p.x + x * size, p.y + (1 - y) * size); };
    ImVec2 m = ImGui::GetIO().MousePos;
    float mx = std::clamp((m.x - p.x) / size, 0.0f, 1.0f), myRaw = 1 - (m.y - p.y) / size, my = std::clamp(myRaw, 0.0f, 1.0f);
    bool changed = false;
    int drag = st->GetInt(dragKey, -1);
    auto nearest = [&]() {
        int best = -1; float bd = 1e9f;
        for (int i = 0; i < (int)c.pts.size(); i++) {
            ImVec2 s = toScr(c.pts[i].x, c.pts[i].y);
            float d = (s.x - m.x) * (s.x - m.x) + (s.y - m.y) * (s.y - m.y);
            if (d < bd) { bd = d; best = i; }
        }
        float r = ImGui::GetFontSize() * 0.6f;
        return bd < r * r ? best : -1;
    };
    if (hovered && ImGui::IsMouseClicked(1)) {
        int n = nearest();
        if (n > 0 && n < (int)c.pts.size() - 1) { c.pts.erase(c.pts.begin() + n); changed = true; }
    }
    if (hovered && ImGui::IsMouseDoubleClicked(0)) {
        int n = nearest();
        if (n > 0 && n < (int)c.pts.size() - 1) { c.pts.erase(c.pts.begin() + n); changed = true; drag = -1; }
    } else if (ImGui::IsItemActivated() && ImGui::IsMouseClicked(0)) {
        drag = nearest();
        if (drag < 0 && c.pts.size() < 16) {
            float lut[256]; BuildCurveLut(c, lut);
            float y = lut[(int)(mx * 255)];
            CurvePt np{ mx, y };
            auto it = std::upper_bound(c.pts.begin(), c.pts.end(), np, [](const CurvePt& a, const CurvePt& b) { return a.x < b.x; });
            drag = (int)(it - c.pts.begin());
            c.pts.insert(it, np);
            changed = true;
        }
    }
    if (active && drag >= 0 && drag < (int)c.pts.size()) {
        int n = (int)c.pts.size();
        float lo = drag > 0 ? c.pts[drag - 1].x + 0.01f : 0.0f;
        float hi = drag < n - 1 ? c.pts[drag + 1].x - 0.01f : 1.0f;
        bool endpoint = drag == 0 || drag == n - 1;
        if (!endpoint && (myRaw < -0.12f || myRaw > 1.12f)) {
            c.pts.erase(c.pts.begin() + drag); drag = -1; changed = true;
        } else {
            CurvePt np{ std::clamp(mx, lo, std::max(lo, hi)), my };
            if (np.x != c.pts[drag].x || np.y != c.pts[drag].y) { c.pts[drag] = np; changed = true; }
        }
    }
    if (!active) drag = -1;
    st->SetInt(dragKey, drag);

    float lut[256]; BuildCurveLut(c, lut);
    ImVec2 pts[128];
    for (int i = 0; i < 128; i++) { float x = i / 127.0f; pts[i] = toScr(x, lut[(int)(x * 255)]); }
    dl->PushClipRect(p, ImVec2(p.x + size, p.y + size), true);
    dl->AddPolyline(pts, 128, color, 0, 2.0f);
    dl->PopClipRect();
    int hov = hovered ? nearest() : -1;
    for (int i = 0; i < (int)c.pts.size(); i++) {
        ImVec2 s = toScr(c.pts[i].x, c.pts[i].y);
        float r = ImGui::GetFontSize() * (i == drag || i == hov ? 0.33f : 0.26f);
        dl->AddCircleFilled(s, r + 1.5f, IM_COL32(0, 0, 0, 160));
        dl->AddCircleFilled(s, r, IM_COL32(240, 240, 240, 255));
    }
    ImGui::PopID();
    return changed;
}

bool ColorWheel(const char* id, Wheel& w, float size) {
    ImGui::PushID(id);
    ImVec2 p = ImGui::GetCursorScreenPos();
    ImGui::InvisibleButton("##wheel", ImVec2(size, size));
    bool active = ImGui::IsItemActive();
    bool hovered = ImGui::IsItemHovered();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImVec2 c(p.x + size * 0.5f, p.y + size * 0.5f);
    float R = size * 0.5f - 4;
    const int N = 72;
    ImVec2 uv = ImGui::GetFontTexUvWhitePixel();
    dl->PrimReserve(N * 3, N * 3);
    ImU32 cc = IM_COL32(128, 128, 128, 255);
    for (int i = 0; i < N; i++) {
        float a0 = (float)i / N * 6.2831853f, a1 = (float)(i + 1) / N * 6.2831853f;
        ImU32 c0 = HueColor(a0 * 57.29578f, 0.85f, 0.85f), c1 = HueColor(a1 * 57.29578f, 0.85f, 0.85f);
        dl->PrimVtx(c, uv, cc);
        dl->PrimVtx(ImVec2(c.x + std::cos(a0) * R, c.y - std::sin(a0) * R), uv, c0);
        dl->PrimVtx(ImVec2(c.x + std::cos(a1) * R, c.y - std::sin(a1) * R), uv, c1);
    }
    dl->AddCircle(c, R, IM_COL32(0, 0, 0, 120), 64, 1.5f);
    dl->AddCircle(c, R * 0.5f, IM_COL32(255, 255, 255, 30), 48, 1.0f);
    bool changed = false;
    if (hovered && ImGui::IsMouseDoubleClicked(0)) { w.hue = 0; w.sat = 0; changed = true; }
    else if (active) {
        ImVec2 m = ImGui::GetIO().MousePos;
        float dx = m.x - c.x, dy = -(m.y - c.y);
        float d = std::sqrt(dx * dx + dy * dy);
        float h = std::atan2(dy, dx) * 57.29578f; if (h < 0) h += 360;
        float s = std::clamp(d / R, 0.0f, 1.0f) * 100;
        if (ImGui::GetIO().KeyShift) h = w.hue;
        if (h != w.hue || s != w.sat) { w.hue = h; w.sat = s; changed = true; }
    }
    float a = w.hue / 57.29578f;
    ImVec2 pk(c.x + std::cos(a) * R * w.sat / 100, c.y - std::sin(a) * R * w.sat / 100);
    dl->AddCircleFilled(pk, 6.5f, IM_COL32(0, 0, 0, 140));
    dl->AddCircleFilled(pk, 5.0f, IM_COL32(245, 245, 245, 255));
    ImGui::PopID();
    return changed;
}

ImFont* g_fontBold = nullptr;
ImFont* g_fontSmall = nullptr;
void SetFonts(ImFont* bold, ImFont* small_) { g_fontBold = bold; g_fontSmall = small_; }

static ImU32 Col(float r, float g, float b, float a) { return ImGui::GetColorU32(ImVec4(r, g, b, a)); }
static ImVec4 AccentV(float a) { ImVec4 c = ImGui::ColorConvertU32ToFloat4(kAccent); c.w = a; return c; }

bool Button(const char* label, ImVec2 size, int kind, bool selected) {
    ImGuiWindow* win = ImGui::GetCurrentWindow();
    if (win->SkipItems) return false;
    ImGuiContext& g = *GImGui;
    const ImGuiStyle& st = g.Style;
    ImGuiID id = win->GetID(label);
    const char* end = ImGui::FindRenderedTextEnd(label);
    ImVec2 ts = ImGui::CalcTextSize(label, end);
    ImVec2 sz = ImGui::CalcItemSize(size, ts.x + st.FramePadding.x * 2 + 4, ImGui::GetFrameHeight());
    ImVec2 pos = win->DC.CursorPos;
    ImRect bb(pos, ImVec2(pos.x + sz.x, pos.y + sz.y));
    ImGui::ItemSize(sz, st.FramePadding.y);
    if (!ImGui::ItemAdd(bb, id)) return false;
    bool hov, held;
    bool pressed = ImGui::ButtonBehavior(bb, id, &hov, &held);
    ImU32 bg = 0, border = 0, text = Col(0.88f, 0.89f, 0.91f, 1);
    float k = held ? 1.0f : hov ? 0.6f : 0.0f;
    switch (kind) {
    case BTN_PRIMARY: {
        ImVec4 a = AccentV(1);
        float l = held ? 0.92f : hov ? 1.08f : 1.0f;
        bg = ImGui::GetColorU32(ImVec4(std::min(1.f, a.x * l), std::min(1.f, a.y * l), std::min(1.f, a.z * l), 1));
        text = Col(0.06f, 0.08f, 0.12f, 1);
        break;
    }
    case BTN_GHOST:
        bg = Col(1, 1, 1, 0.07f * k + (selected ? 0.1f : 0));
        text = selected ? Col(1, 1, 1, 1) : Col(0.80f, 0.81f, 0.84f, 1);
        break;
    case BTN_DANGER:
        bg = hov || held ? Col(0.95f, 0.32f, 0.32f, 0.16f + 0.08f * k) : Col(1, 1, 1, 0.065f);
        border = hov || held ? Col(0.95f, 0.4f, 0.4f, 0.35f) : Col(1, 1, 1, 0.06f);
        text = hov || held ? Col(1.0f, 0.6f, 0.6f, 1) : Col(0.84f, 0.85f, 0.87f, 1);
        break;
    default:
        if (selected) { bg = ImGui::GetColorU32(AccentV(0.22f + 0.08f * k)); border = ImGui::GetColorU32(AccentV(0.55f)); text = Col(1, 1, 1, 1); }
        else { bg = Col(1, 1, 1, 0.065f + 0.05f * k); border = Col(1, 1, 1, 0.06f); }
    }
    ImDrawList* dl = win->DrawList;
    float r = st.FrameRounding;
    if (bg) dl->AddRectFilled(bb.Min, bb.Max, bg, r);
    if (border) dl->AddRect(bb.Min, bb.Max, border, r, 0, 1.0f);
    dl->AddText(ImVec2(bb.Min.x + (sz.x - ts.x) * 0.5f, bb.Min.y + (sz.y - ts.y) * 0.5f), text, label, end);
    return pressed;
}

bool IconButton(const char* icon, const char* tooltip, int kind, bool selected, float size) {
    float s = size > 0 ? size : ImGui::GetFrameHeight();
    char id[160];
    snprintf(id, sizeof(id), "%s##%s", icon, tooltip ? tooltip : "ib");
    bool r = Button(id, ImVec2(s, s), kind, selected);
    if (tooltip) Hint(tooltip);
    return r;
}

bool Toggle(const char* label, bool* v) {
    ImGuiWindow* win = ImGui::GetCurrentWindow();
    if (win->SkipItems) return false;
    ImGuiID id = win->GetID(label);
    float fs = ImGui::GetFontSize();
    float w = ImGui::GetContentRegionAvail().x, h = ImGui::GetFrameHeight();
    ImVec2 pos = win->DC.CursorPos;
    ImRect bb(pos, ImVec2(pos.x + w, pos.y + h));
    ImGui::ItemSize(bb, GImGui->Style.FramePadding.y);
    if (!ImGui::ItemAdd(bb, id)) return false;
    bool hov, held;
    bool pressed = ImGui::ButtonBehavior(bb, id, &hov, &held);
    if (pressed) { *v = !*v; ImGui::MarkItemEdited(id); }
    ImDrawList* dl = win->DrawList;
    const char* end = ImGui::FindRenderedTextEnd(label);
    ImVec2 ts = ImGui::CalcTextSize(label, end);
    dl->AddText(ImVec2(bb.Min.x + fs * 0.15f, bb.Min.y + (h - ts.y) * 0.5f), hov ? Col(1, 1, 1, 1) : Col(0.80f, 0.81f, 0.84f, 1), label, end);
    float sw = fs * 2.0f, sh = fs * 1.1f;
    ImVec2 a(bb.Max.x - sw - 2, bb.Min.y + (h - sh) * 0.5f), b(bb.Max.x - 2, a.y + sh);
    dl->AddRectFilled(a, b, *v ? kAccent : Col(1, 1, 1, hov ? 0.16f : 0.11f), sh * 0.5f);
    float kr = sh * 0.5f - 2.5f;
    float kx = *v ? b.x - sh * 0.5f : a.x + sh * 0.5f;
    dl->AddCircleFilled(ImVec2(kx, a.y + sh * 0.5f), kr, *v ? Col(0.06f, 0.08f, 0.12f, 1) : Col(0.85f, 0.86f, 0.88f, 1));
    return pressed;
}

static bool ComboRow(const char* label, int* v, const char* items, const char* const* arr, int n) {
    ImGui::PushID(label);
    float w = ImGui::GetContentRegionAvail().x;
    ImGui::AlignTextToFramePadding();
    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.62f, 0.64f, 0.68f, 1));
    ImGui::TextUnformatted(label, ImGui::FindRenderedTextEnd(label));
    ImGui::PopStyleColor();
    ImGui::SameLine(w * 0.42f);
    ImGui::SetNextItemWidth(-1);
    bool r = arr ? ImGui::Combo("##c", v, arr, n) : ImGui::Combo("##c", v, items);
    ImGui::PopID();
    return r;
}
bool Combo(const char* label, int* v, const char* items) { return ComboRow(label, v, items, nullptr, 0); }
bool Combo(const char* label, int* v, const char* const* items, int n) { return ComboRow(label, v, nullptr, items, n); }

bool Section(const char* title, bool defaultOpen, bool* reset) {
    ImGuiWindow* win = ImGui::GetCurrentWindow();
    if (win->SkipItems) return false;
    ImGuiID id = win->GetID(title);
    ImGuiStorage* stg = ImGui::GetStateStorage();
    bool open = stg->GetBool(id, defaultOpen);
    float fs = ImGui::GetFontSize();
    ImGui::Dummy(ImVec2(0, fs * 0.25f));
    float w = ImGui::GetContentRegionAvail().x, h = fs * 2.1f;
    ImVec2 pos = win->DC.CursorPos;
    ImRect bb(pos, ImVec2(pos.x + w, pos.y + h));
    ImGui::ItemSize(bb);
    if (!ImGui::ItemAdd(bb, id)) return open;
    bool hov, held;
    bool pressed = ImGui::ButtonBehavior(bb, id, &hov, &held);
    ImRect rr(ImVec2(bb.Max.x - h, bb.Min.y), bb.Max);
    bool overReset = reset && hov && rr.Contains(ImGui::GetIO().MousePos);
    if (pressed) {
        if (overReset) *reset = true;
        else { open = !open; stg->SetBool(id, open); }
    }
    ImDrawList* dl = win->DrawList;
    if (hov) dl->AddRectFilled(bb.Min, bb.Max, Col(1, 1, 1, 0.04f), 6);
    // chevron
    float cx = bb.Min.x + fs * 0.7f, cy = bb.Min.y + h * 0.5f, s = fs * 0.27f;
    ImU32 cc = Col(0.62f, 0.64f, 0.68f, 1);
    if (open) dl->AddTriangleFilled(ImVec2(cx - s, cy - s * 0.5f), ImVec2(cx + s, cy - s * 0.5f), ImVec2(cx, cy + s * 0.6f), cc);
    else dl->AddTriangleFilled(ImVec2(cx - s * 0.5f, cy - s), ImVec2(cx - s * 0.5f, cy + s), ImVec2(cx + s * 0.6f, cy), cc);
    if (g_fontBold) ImGui::PushFont(g_fontBold);
    ImVec2 ts = ImGui::CalcTextSize(title, ImGui::FindRenderedTextEnd(title));
    dl->AddText(ImVec2(bb.Min.x + fs * 1.5f, cy - ts.y * 0.5f), Col(0.92f, 0.93f, 0.95f, 1), title, ImGui::FindRenderedTextEnd(title));
    if (g_fontBold) ImGui::PopFont();
    if (reset && hov) {
        const char* ic = "↺";
        ImVec2 is = ImGui::CalcTextSize(ic);
        if (overReset) dl->AddRectFilled(ImVec2(rr.Min.x + 3, rr.Min.y + 3), ImVec2(rr.Max.x - 3, rr.Max.y - 3), Col(1, 1, 1, 0.08f), 5);
        dl->AddText(ImVec2(rr.Min.x + (h - is.x) * 0.5f, rr.Min.y + (h - is.y) * 0.5f), overReset ? Col(1, 1, 1, 1) : Col(0.6f, 0.62f, 0.66f, 1), ic);
    }
    if (!open) dl->AddLine(ImVec2(bb.Min.x + 4, bb.Max.y), ImVec2(bb.Max.x - 4, bb.Max.y), Col(1, 1, 1, 0.05f));
    return open;
}

void Caption(const char* text) {
    if (g_fontSmall) ImGui::PushFont(g_fontSmall);
    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.55f, 0.57f, 0.61f, 1));
    ImGui::TextWrapped("%s", text);
    ImGui::PopStyleColor();
    if (g_fontSmall) ImGui::PopFont();
}


} // namespace W
