#include "pipeline.h"
#include "imageio.h"
#include "effects.h"
#include <algorithm>
#include <cmath>
#include <chrono>
#include <cstring>
#include <future>

Pipeline g_pipe;

// =====================================================================================  HLSL
static const char* kGeo = R"HLSL(
cbuffer G : register(b1) {
    float4 crop; float2 canvasPx; float cosA; float sinA;
    float scl; float pv; float ph; float dist;
    float ca; float lvig; float rot90; float flipH;
    float flipV; float3 _gp;
};
float2 nToSrc(float2 n) {
    float m = max(canvasPx.x, canvasPx.y) * 0.5;
    float2 o = n * m / canvasPx + 0.5;
    if (flipH > 0.5) o.x = 1 - o.x;
    if (flipV > 0.5) o.y = 1 - o.y;
    int r = (int)(rot90 + 0.5);
    float2 s = o;
    if (r == 1) s = float2(o.y, 1 - o.x);
    else if (r == 2) s = float2(1 - o.x, 1 - o.y);
    else if (r == 3) s = float2(1 - o.y, o.x);
    return s;
}
float4 main(VSO i) : SV_Target {
    float2 c = crop.xy + i.uv * crop.zw;
    float m = max(canvasPx.x, canvasPx.y) * 0.5;
    float2 n = (c - 0.5) * canvasPx / m;
    n = float2(cosA * n.x + sinA * n.y, -sinA * n.x + cosA * n.y) / scl;
    float w = 1 + pv * n.y + ph * n.x;
    n = n / max(w, 0.05);
    float r2 = dot(n, n);
    float2 nd = n * (1 + dist * r2);
    float4 g = t0.Sample(sAniso, nToSrc(nd));
    float rr = t0.Sample(sAniso, nToSrc(nd * (1 + ca))).r;
    float bb = t0.Sample(sAniso, nToSrc(nd * (1 - ca))).b;
    float3 rgb = float3(rr, g.g, bb);
    rgb *= 1 + lvig * r2 * 0.6;
    return float4(rgb, g.a);
}
)HLSL";

static const char* kLumPrep = R"HLSL(
cbuffer LP : register(b1) { float4 gain; };
float4 main(VSO i) : SV_Target {
    float3 c = t0.SampleLevel(sPt, i.uv, 0).rgb;
    if (gain.w > 0.5) c = srgbDec(c);   // display-referred input
    c = max(c * gain.rgb, 0);
    return float4(log2(max(luma(c), 1e-5)), min(c.r, min(c.g, c.b)), 0, 1);
}
)HLSL";

static const char* kDown = R"HLSL(
float4 main(VSO i) : SV_Target { return t0.SampleLevel(sLin, i.uv, 0); }
)HLSL";

static const char* kBlur = R"HLSL(
cbuffer B : register(b1) { float2 dir; float sigma; float radius; };
float4 main(VSO i) : SV_Target {
    float4 acc = 0; float wsum = 0;
    int R = (int)radius;
    float k = -0.5 / (sigma * sigma);
    [loop] for (int j = -R; j <= R; j++) {
        float w = exp(j * j * k);
        acc += t0.SampleLevel(sLin, i.uv + dir * j, 0) * w;
        wsum += w;
    }
    return acc / wsum;
}
)HLSL";

// Noise reduction works in a square-root (variance stabilised) YCbCr space.
static const char* kNrPrep = R"HLSL(
float4 main(VSO i) : SV_Target {
    float4 s = t0.SampleLevel(sPt, i.uv, 0);
    float3 c = sqrt(max(s.rgb, 0));
    float Y = luma(c);
    return float4(Y, c.b - Y, c.r - Y, s.a);
}
)HLSL";

static const char* kDenoise = R"HLSL(
cbuffer N : register(b1) { float lumAmt; float detail; float colAmt; float smooth; };
static const float2 kP[5] = { float2(0, 0), float2(1, 0), float2(-1, 0), float2(0, 1), float2(0, -1) };
float4 main(VSO i) : SV_Target {
    float4 c0 = t0.SampleLevel(sPt, i.uv, 0);
    float Y = c0.r;
    float2 C = c0.gb;
    // luminance: non-local means over a 7x7 search window with a 5-tap cross patch
    if (lumAmt > 0) {
        float p0[5];
        [unroll] for (int k = 0; k < 5; k++) p0[k] = t0.SampleLevel(sPt, i.uv + kP[k] * uInv, 0).r;
        float h = lumAmt * 0.05 + 1e-4;
        float inv = 1.0 / (h * h);
        float sw = 0, sy = 0;
        [loop] for (int dy = -3; dy <= 3; dy++) {
            [loop] for (int dx = -3; dx <= 3; dx++) {
                float2 q = i.uv + float2(dx, dy) * uInv;
                float d = 0, center = 0;
                [unroll] for (int k2 = 0; k2 < 5; k2++) {
                    float v = t0.SampleLevel(sPt, q + kP[k2] * uInv, 0).r;
                    if (k2 == 0) center = v;
                    float e = v - p0[k2];
                    d += e * e;
                }
                float w = exp(-max(d * 0.2 - 0.0002, 0) * inv);
                sw += w; sy += center * w;
            }
        }
        float Yn = sy / sw;
        // "detail" puts back part of the removed fine texture
        Y = Yn + (Y - Yn) * detail * 0.45;
    }
    // colour: wide, edge-aware smoothing of the chroma planes
    if (colAmt > 0) {
        float st = max(1.0, (1.5 + smooth * 7.0) * uScale);
        float2 acc = 0; float ws = 0;
        [loop] for (int y2 = -3; y2 <= 3; y2++) {
            [loop] for (int x2 = -3; x2 <= 3; x2++) {
                float4 n = t0.SampleLevel(sLin, i.uv + float2(x2, y2) * uInv * st, 0);
                float w = exp(-(x2 * x2 + y2 * y2) / 10.0) * exp(-abs(n.r - c0.r) * (25 - smooth * 15));
                acc += n.gb * w; ws += w;
            }
        }
        C = lerp(C, acc / ws, saturate(colAmt * 1.25));
    }
    float r = C.y + Y, b = C.x + Y;
    float g = (Y - 0.2126 * r - 0.0722 * b) / 0.7152;
    float3 o = max(float3(r, g, b), 0);
    return float4(o * o, c0.a);
}
)HLSL";

static const char* kDevelop = R"HLSL(
cbuffer D : register(b1) {
    float4 hslH[2]; float4 hslS[2]; float4 hslL[2]; float4 bwMix[2];
    float4 gS; float4 gM; float4 gH; float4 gG;
    float4 wbGain;
    float shoulderW; float blackPt; float contrastA; float shadows;
    float highlights; float clarity; float texture_; float dehaze;
    float vibrance; float saturation_; float bw; float gradBlend;
    float gradBalance; float useLut; float satBase; float shoulderK;
};
float arr8(float4 a[2], int k) { return a[k >> 2][k & 3]; }
float shoulder(float y, float K, float W) {
    if (y <= K) return y;
    float u = (y - K) / (1 - K), Uw = max((W - K) / (1 - K), 0.05);
    float g = u * (1 + u / (Uw * Uw)) / (1 + u);
    return K + (1 - K) * g;
}
float3 contrastC(float3 x, float a, float p) {
    float3 lo = p * pow(saturate(x / p), a);
    float3 hi = 1 - (1 - p) * pow(saturate((1 - x) / (1 - p)), a);
    return float3(x.r < p ? lo.r : hi.r, x.g < p ? lo.g : hi.g, x.b < p ? lo.b : hi.b);
}
void hueWeights(float hdeg, out float w[8]) {
    static const float HC[9] = { 0, 30, 60, 120, 180, 225, 270, 315, 360 };
    [unroll] for (int k = 0; k < 8; k++) w[k] = 0;
    [unroll] for (int j = 0; j < 8; j++) {
        if (hdeg >= HC[j] && hdeg < HC[j + 1]) {
            float t = smoothstep(0, 1, (hdeg - HC[j]) / (HC[j + 1] - HC[j]));
            w[j] = 1 - t; w[(j + 1) & 7] = t;
        }
    }
}
float4 main(VSO i) : SV_Target {
    float4 s = t0.SampleLevel(sPt, i.uv, 0);
    float3 c = max(s.rgb, 0) * wbGain.rgb;
    float Y = max(luma(c), 1e-6);
    float lY = log2(Y);
    float2 bT = t1.SampleLevel(sLin, i.uv, 0).rg;
    float2 bC = t2.SampleLevel(sLin, i.uv, 0).rg;
    float2 bL = t3.SampleLevel(sLin, i.uv, 0).rg;

    // ---- local tone (log domain)
    float localL = lerp(lY, bL.r, 0.6);
    float pl = saturate(srgbEnc1(min(exp2(localL), 1)));
    float wS = 1 - smoothstep(0.05, 0.6, pl);
    float wH = smoothstep(0.4, 0.95, pl);
    float pp = saturate(srgbEnc1(min(Y, 1)));
    float midW = 1 - pow(abs(pp * 2 - 1), 2);
    float dl = shadows * 1.6 * wS + highlights * 1.6 * wH;
    dl += clarity * clamp(lY - bC.r, -2, 2) * 0.7 * (0.3 + 0.7 * midW);
    dl += texture_ * clamp(lY - bT.r, -1.5, 1.5) * 0.9;
    c *= exp2(dl);

    // ---- dehaze
    if (dehaze > 0) {
        float t = saturate(bL.g * 0.95) * dehaze * 0.9;
        c = max((c - t) / max(1 - t, 0.15), 0);
    } else if (dehaze < 0) {
        c = lerp(c, float3(0.75, 0.75, 0.75) * max(Y, 0.3), -dehaze * 0.45);
    }

    // ---- highlight shoulder + encode
    c = float3(shoulder(c.r, shoulderK, shoulderW), shoulder(c.g, shoulderK, shoulderW), shoulder(c.b, shoulderK, shoulderW));
    float3 x = srgbEnc(saturate(c));
    x = saturate((x - blackPt) / (1 - blackPt));
    x = contrastC(x, contrastA, 0.45);
    if (useLut > 0.5) {
        float3 lu = x * (255.0 / 256.0) + 0.5 / 256.0;
        x = float3(t4.SampleLevel(sLin, float2(lu.r, 0.5), 0).r, t4.SampleLevel(sLin, float2(lu.g, 0.5), 0).g, t4.SampleLevel(sLin, float2(lu.b, 0.5), 0).b);
    }

    // ---- HSL mixer
    float3 hsv = rgb2hsv(saturate(x));
    float w[8];
    hueWeights(hsv.x * 360, w);
    float dh = 0, ds = 0, dlm = 0, dbw = 0;
    [unroll] for (int k = 0; k < 8; k++) { dh += w[k] * arr8(hslH, k); ds += w[k] * arr8(hslS, k); dlm += w[k] * arr8(hslL, k); dbw += w[k] * arr8(bwMix, k); }
    float satW = smoothstep(0.02, 0.25, hsv.y);
    float3 hsvOrig = hsv;
    hsv.x = frac(hsv.x + dh / 360 * satW);
    hsv.y = saturate(hsv.y * (1 + ds));
    x = hsv2rgb(hsv);
    x *= 1 + dlm * 0.6 * satW;

    // ---- color grading
    float L = saturate(luma(x));
    float lb = saturate(L - gradBalance * 0.3);
    float span = 0.35 + 0.4 * gradBlend;
    float wSh = saturate(1 - lb / span);
    float wHi = saturate((lb - (1 - span)) / span);
    float wMd = saturate(1 - wSh - wHi);
    x += gS.rgb * wSh + gM.rgb * wMd + gH.rgb * wHi + gG.rgb;
    x += (gS.w * wSh + gM.w * wMd + gH.w * wHi + gG.w);

    // ---- vibrance / saturation
    float Lx = luma(x);
    float sat = max(x.r, max(x.g, x.b)) - min(x.r, min(x.g, x.b));
    float skin = w[1] * 0.5 + w[0] * 0.2;
    float vib = vibrance * (1 - saturate(sat * 1.6)) * (vibrance > 0 ? 1 - skin : 1);
    x = lerp(Lx.xxx, x, max(0, satBase * (1 + saturation_) + vib));

    // ---- black & white
    if (bw > 0.5) {
        float g = Lx * (1 + dbw * smoothstep(0.02, 0.3, hsvOrig.y) * 0.9);
        x = g.xxx;
    }
    return float4(saturate(x), s.a);
}
)HLSL";

static const char* kMaskW = R"HLSL(
cbuffer M : register(b1) {
    float4 crop; float4 lin; float4 rad;
    float radAngle; float feather; float invert; float shape;
    float range; float lumLo; float lumHi; float lumF;
    float hueC; float hueW; float hueF; float satMin;
    float asp; float useBrush; float2 _m;
};
float4 main(VSO i) : SV_Target {
    float2 c = crop.xy + i.uv * crop.zw;
    float2 q = float2(c.x * asp, c.y);
    float w = 1;
    int sh = (int)(shape + 0.5);
    if (sh == 1) {
        float2 a = float2(lin.x * asp, lin.y), b = float2(lin.z * asp, lin.w);
        float2 ab = b - a;
        float t = dot(q - a, ab) / max(dot(ab, ab), 1e-6);
        w = 1 - smoothstep(0, 1, saturate(t));
    } else if (sh == 2) {
        float2 d = rot2(q - float2(rad.x * asp, rad.y), -radAngle);
        float e = length(d / max(float2(rad.z * asp, rad.w), 1e-4));
        float f = max(feather, 0.002);
        w = 1 - smoothstep(1 - f, 1, e);
    } else if (sh == 3) {
        w = t1.SampleLevel(sLin, c, 0).r;
    }
    if (invert > 0.5) w = 1 - w;
    float3 img = t0.SampleLevel(sPt, i.uv, 0).rgb;
    int rg = (int)(range + 0.5);
    if (rg == 1) {
        float L = luma(img); float f = max(lumF, 1e-3);
        w *= smoothstep(lumLo - f, lumLo, L) * (1 - smoothstep(lumHi, lumHi + f, L));
    } else if (rg == 2) {
        float3 h = rgb2hsv(img);
        float d = abs(frac(h.x - hueC / 360 + 0.5) - 0.5) * 360;
        w *= (1 - smoothstep(hueW * 0.5, hueW * 0.5 + hueF + 1e-3, d)) * smoothstep(satMin * 0.5, satMin + 1e-3, h.y);
    } else if (rg == 3) {
        float N = t2.SampleLevel(sLin, i.uv, 0).r; float f = max(lumF, 1e-3);
        w *= smoothstep(lumLo - f, lumLo, N) * (1 - smoothstep(lumHi, lumHi + f, N));
    }
    return float4(w, 0, 0, 1);
}
)HLSL";

static const char* kLocal = R"HLSL(
cbuffer L : register(b1) {
    float amount; float exposure; float contrast; float highlights;
    float shadows; float whites; float blacks; float temp;
    float tint; float hue; float sat; float clarity;
    float dehaze; float sharp; float colorAmt; float _l;
    float4 color;
};
float3 contrastC(float3 x, float a, float p) {
    float3 lo = p * pow(saturate(x / p), a);
    float3 hi = 1 - (1 - p) * pow(saturate((1 - x) / (1 - p)), a);
    return float3(x.r < p ? lo.r : hi.r, x.g < p ? lo.g : hi.g, x.b < p ? lo.b : hi.b);
}
float sh1(float y) { const float K = 0.8; return y <= K ? y : K + (1 - K) * (1 - exp(-(y - K) / (1 - K))); }
float4 main(VSO i) : SV_Target {
    float4 s = t0.SampleLevel(sPt, i.uv, 0);
    float w = t1.SampleLevel(sPt, i.uv, 0).r * amount;
    if (w <= 0.0005) return s;
    float3 lin = srgbDec(s.rgb);
    float3 m = float3(exp(temp * 0.55), exp(-tint * 0.35), exp(-temp * 0.55));
    m /= luma(m);
    lin *= m * exp2(exposure);
    float Y = max(luma(lin), 1e-6), lY = log2(Y);
    float lB = t2.SampleLevel(sLin, i.uv, 0).r + exposure;
    float pl = srgbEnc1(min(exp2(lerp(lY, lB, 0.6)), 1));
    float wS = 1 - smoothstep(0.05, 0.6, pl), wH = smoothstep(0.4, 0.95, pl);
    float pp = srgbEnc1(min(Y, 1));
    float midW = 1 - pow(abs(pp * 2 - 1), 2);
    float dl = shadows * 1.4 * wS + highlights * 1.4 * wH + clarity * clamp(lY - lB, -2, 2) * 0.7 * (0.3 + 0.7 * midW);
    lin *= exp2(dl);
    lin = float3(sh1(lin.r), sh1(lin.g), sh1(lin.b));
    float3 x = srgbEnc(saturate(lin));
    float bp = -blacks * 0.08;
    x = saturate((x - bp) / (1 - bp));
    x = saturate(x * (1 + whites * 0.2));
    x = contrastC(x, exp(contrast * 0.75), 0.45);
    if (dehaze > 0) { x = saturate((x - dehaze * 0.12) / (1 - dehaze * 0.12)); x = lerp(luma(x).xxx, x, 1 + dehaze * 0.3); }
    else if (dehaze < 0) x = lerp(x, float3(0.8, 0.8, 0.8), -dehaze * 0.4);
    float3 b = t3.SampleLevel(sLin, i.uv, 0).rgb;
    if (sharp > 0) x += (s.rgb - b) * sharp * 2.0;
    else if (sharp < 0) x += (b - s.rgb) * -sharp;
    float3 h = rgb2hsv(saturate(x));
    h.x = frac(h.x + hue / 360 + 1);
    h.y = saturate(h.y * (1 + sat));
    x = hsv2rgb(h);
    x += (color.rgb - luma(color.rgb)) * colorAmt * 0.5;
    return float4(lerp(s.rgb, saturate(x), saturate(w)), s.a);
}
)HLSL";

static const char* kSharpen = R"HLSL(
cbuffer S : register(b1) { float amount; float detail; float masking; float _s; };
float4 main(VSO i) : SV_Target {
    float4 s = t0.SampleLevel(sPt, i.uv, 0);
    float3 b = t1.SampleLevel(sLin, i.uv, 0).rgb;
    float3 d = s.rgb - b;
    float lim = 0.02 + detail * 0.25;
    d = d / (1 + abs(d) / lim);
    float m = 1;
    if (masking > 0) {
        float2 o = uInv * 1.5;
        float gx = luma(t1.SampleLevel(sLin, i.uv + float2(o.x, 0), 0).rgb) - luma(t1.SampleLevel(sLin, i.uv - float2(o.x, 0), 0).rgb);
        float gy = luma(t1.SampleLevel(sLin, i.uv + float2(0, o.y), 0).rgb) - luma(t1.SampleLevel(sLin, i.uv - float2(0, o.y), 0).rgb);
        float e = length(float2(gx, gy));
        m = smoothstep(masking * 0.08, masking * 0.08 + 0.03, e);
    }
    float3 x = s.rgb + d * amount * 1.6 * m;
    return float4(saturate(x), s.a);
}
)HLSL";

static const char* kFinish = R"HLSL(
cbuffer F : register(b1) {
    float vigAmount; float vigMid; float vigRound; float vigFeather;
    float vigHigh; float grainAmount; float grainSize; float grainRough;
};
float4 main(VSO i) : SV_Target {
    float4 s = t0.SampleLevel(sPt, i.uv, 0);
    float3 x = s.rgb;
    if (vigAmount != 0) {
        float2 p = (i.uv - 0.5) * 2;
        float ar = uSize.x / uSize.y;
        float rr = vigRound;
        if (rr > 0) p.x *= lerp(1, ar, rr);
        float e = 2 + max(0, -rr) * 6;
        float d = pow(pow(abs(p.x), e) + pow(abs(p.y), e), 1 / e);
        float inner = 0.15 + vigMid * 1.1;
        float v = smoothstep(inner - vigFeather * 0.7 - 0.02, inner + 0.15 + vigFeather * 0.5, d);
        float L = luma(x);
        if (vigAmount < 0) { v *= 1 - vigHigh * smoothstep(0.5, 1.0, L); x *= 1 + vigAmount * v; }
        else x = lerp(x, 1, vigAmount * v * 0.9);
    }
    if (grainAmount > 0) {
        float gs = (0.6 + grainSize * 3.0) * max(uFull.x, uFull.y) / 4000;
        float2 q = i.uv * uFull / max(gs, 0.3) + uSeed;
        float n = (vnoise(q) - 0.5) * 1.6 + (vnoise(q * 2.3 + 7.1) - 0.5) * grainRough * 1.2 + (hash12(floor(q * 3.1)) - 0.5) * grainRough * 0.4;
        float L = luma(x);
        float wgt = 0.35 + 2.6 * L * (1 - L);
        x += n * grainAmount * 0.22 * wgt;
    }
    return float4(saturate(x), s.a);
}
)HLSL";

static const char* kOutput = R"HLSL(
cbuffer O : register(b1) { float clipWarn; float altView; float hasMask; float _o; };
float4 main(VSO i) : SV_Target {
    float4 s = t0.SampleLevel(sLin, i.uv, 0);
    float3 x = s.rgb * s.a;
    if (altView > 0.5 && altView < 1.5) {
        // highlights view: black, clipped channels light up (white = all channels clipped)
        return float4(saturate((s.rgb - 0.935) / 0.06) * s.a, 1);
    }
    if (altView > 1.5) {
        // shadows view: white, crushed channels go dark (black = all channels clipped)
        return float4(lerp(1, saturate((s.rgb - 0.005) / 0.06), s.a), 1);
    }
    if (clipWarn > 0.5) {
        if (max(s.r, max(s.g, s.b)) >= 0.998) x = float3(1, 0.1, 0.1);
        else if (max(s.r, max(s.g, s.b)) <= 0.002 && s.a > 0.5) x = float3(0.1, 0.3, 1);
    }
    if (hasMask > 0.5) {
        float m = t1.SampleLevel(sLin, i.uv, 0).r;
        x = lerp(x, float3(1, 0.15, 0.2), m * 0.55);
    }
    return float4(x, 1);
}
)HLSL";

static const char* kConvert = R"HLSL(
float4 main(VSO i) : SV_Target { float4 s = t0.SampleLevel(sPt, i.uv, 0); return float4(saturate(s.rgb * s.a), 1); }
)HLSL";

static const char* kDispLum = R"HLSL(
float4 main(VSO i) : SV_Target {
    float3 c = srgbDec(t0.SampleLevel(sPt, i.uv, 0).rgb);
    return float4(log2(max(luma(c), 1e-5)), 0, 0, 1);
}
)HLSL";

static const char* kDab = R"HLSL(
struct DO { float4 pos : SV_Position; float2 q : TEXCOORD0; float4 d : TEXCOORD1; };
float4 main(DO i) : SV_Target {
    float r = length(i.q);
    if (r > 1) discard;
    float f = max(i.d.y, 0.01);
    float a = 1 - smoothstep(1 - f, 1, r);
    return float4(i.d.z > 0.5 ? 0 : 1, 0, 0, a * i.d.x);
}
)HLSL";

// =====================================================================================  constant blocks
struct GeoCB { float crop[4]; float canvasPx[2]; float cosA, sinA, scl, pv, ph, dist, ca, lvig, rot90, flipH, flipV, pad[3]; };
struct BlurCB { float dir[2]; float sigma; float radius; };
struct LumCB { float gain[4]; };
struct NoiseCB { float lum, detail, col, smooth; };
struct DevCB {
    float hslH[8], hslS[8], hslL[8], bwMix[8];
    float gS[4], gM[4], gH[4], gG[4];
    float wbGain[4];
    float shoulderW, blackPt, contrastA, shadows;
    float highlights, clarity, texture, dehaze;
    float vibrance, saturation, bw, gradBlend;
    float gradBalance, useLut, satBase, shoulderK;
};
struct MaskCB {
    float crop[4], lin[4], rad[4];
    float radAngle, feather, invert, shape;
    float range, lumLo, lumHi, lumF;
    float hueC, hueW, hueF, satMin;
    float asp, useBrush, pad[2];
};
struct LocalCB {
    float amount, exposure, contrast, highlights;
    float shadows, whites, blacks, temp;
    float tint, hue, sat, clarity;
    float dehaze, sharp, colorAmt, pad;
    float color[4];
};
struct SharpCB { float amount, detail, masking, pad; };
struct FinishCB { float vigAmount, vigMid, vigRound, vigFeather, vigHigh, grainAmount, grainSize, grainRough; };
struct OutCB { float clipWarn, overlay, hasMask, pad; };
struct FxCB { float P[12]; float C[12]; float X[4]; };
struct CombineCB { float opacity, blend, target, invert, lo, hi, feather, hueC, hueW, hueF, satMin, pad; };

static void HueToRgb(float h, float rgb[3]) {
    float hh = std::fmod(h / 60.0f + 6.0f, 6.0f);
    float x = 1 - std::fabs(std::fmod(hh, 2.0f) - 1);
    float r = 0, g = 0, b = 0;
    if (hh < 1) { r = 1; g = x; } else if (hh < 2) { r = x; g = 1; } else if (hh < 3) { g = 1; b = x; }
    else if (hh < 4) { g = x; b = 1; } else if (hh < 5) { r = x; b = 1; } else { r = 1; b = x; }
    rgb[0] = r; rgb[1] = g; rgb[2] = b;
}

// =====================================================================================  setup
bool Pipeline::init() {
    struct Job { ID3D11PixelShader** out; const char* name; std::string src; };
    std::vector<Job> jobs = {
        { &psGeo, "geo", kGeo }, { &psLumPrep, "lumprep", kLumPrep }, { &psDown, "down", kDown }, { &psBlur, "blur", kBlur },
        { &psDenoise, "denoise", kDenoise }, { &psNrPrep, "nrprep", kNrPrep }, { &psDevelop, "develop", kDevelop }, { &psMaskW, "maskw", kMaskW },
        { &psLocal, "local", kLocal }, { &psSharpen, "sharpen", kSharpen }, { &psFinish, "finish", kFinish },
        { &psOutput, "output", kOutput }, { &psDab, "dab", kDab }, { &psCombine, "combine", FxCombineSource() },
        { &psConvert, "convert", kConvert }, { &psDispLum, "displum", kDispLum },
    };
    static const char* fxNames[FX_COUNT] = {};
    for (int t = 0; t < FX_COUNT; t++) jobs.push_back({ &psFx[t], GetFxDesc(t).name, FxShaderSource(t) });
    (void)fxNames;

    // Compile in parallel; D3DCompile is thread safe, object creation on the device is free-threaded.
    std::vector<std::future<void>> fut;
    for (auto& j : jobs) fut.push_back(std::async(std::launch::async, [&j]() { *j.out = g_gpu.compilePS(j.name, j.src); }));
    for (auto& f : fut) f.get();
    for (auto& j : jobs) if (!*j.out) return false;

    if (!initOptics()) return false;
    lut = g_gpu.create(256, 1, DXGI_FORMAT_R32G32B32A32_FLOAT, false);
    histSmall = nullptr;
    return lut != nullptr;
}

void Pipeline::shutdown() {
    releaseKeep();
    if (src) { src->release(); delete src; src = nullptr; }
    if (lut) { lut->release(); delete lut; lut = nullptr; }
    if (display) { display->release(); delete display; display = nullptr; }
    for (Tex* t : retired) { t->release(); delete t; }
    retired.clear();
    if (histSmall) { histSmall->release(); delete histSmall; histSmall = nullptr; }
    for (auto& b : brushes) if (b.second.tex) { b.second.tex->release(); delete b.second.tex; }
    brushes.clear();
    for (auto& g : glyphs) if (g.second.first) { g.second.first->release(); delete g.second.first; }
    glyphs.clear();
    shutdownOptics();
}

void Pipeline::releaseKeep() {
    for (Tex*& t : keep) g_gpu.release(t);
    keep.clear();
    lastGeo = lastDev = lastDepth = nullptr;
    lastMaskW.clear();
}

void Pipeline::setSource(const LoadedImage& img) {
    releaseKeep();
    if (src) { src->release(); delete src; src = nullptr; }
    for (auto& b : brushes) if (b.second.tex) { b.second.tex->release(); delete b.second.tex; }
    brushes.clear();
    clearDepth();
    g_gpu.trimPool();
    srcIsRaw = img.isRaw;
    srcW = img.w; srcH = img.h;
    if (img.isRaw) src = g_gpu.create(img.w, img.h, DXGI_FORMAT_R16G16B16A16_UNORM, false, img.rgba16.data(), img.w * 8, true);
    else src = g_gpu.create(img.w, img.h, DXGI_FORMAT_R8G8B8A8_UNORM_SRGB, false, img.rgba8.data(), img.w * 4, true);
}

Geo Pipeline::geometry(const Params& p, bool cropOverride) const {
    Geo g;
    bool swap = (p.rot90 & 1) != 0;
    g.canvasW = swap ? srcH : srcW;
    g.canvasH = swap ? srcW : srcH;
    if (!cropOverride) {
        g.cx0 = std::clamp(std::min(p.cropX0, p.cropX1), 0.0f, 1.0f);
        g.cy0 = std::clamp(std::min(p.cropY0, p.cropY1), 0.0f, 1.0f);
        g.cw = std::clamp(std::fabs(p.cropX1 - p.cropX0), 0.01f, 1.0f);
        g.ch = std::clamp(std::fabs(p.cropY1 - p.cropY0), 0.01f, 1.0f);
    }
    g.fullW = std::max(1, (int)std::lround(g.cw * g.canvasW));
    g.fullH = std::max(1, (int)std::lround(g.ch * g.canvasH));
    return g;
}

// =====================================================================================  helpers
Tex* Pipeline::blur(Tex* in, float sx, float sy, DXGI_FORMAT fmt) {
    Tex* cur = in;
    bool owned = false;
    while ((sx > 4.0f && cur->w > 8) || (sy > 4.0f && cur->h > 8)) {
        int nw = cur->w, nh = cur->h;
        if (sx > 4.0f && cur->w > 8) { nw = (cur->w + 1) / 2; sx *= 0.5f; }
        if (sy > 4.0f && cur->h > 8) { nh = (cur->h + 1) / 2; sy *= 0.5f; }
        Tex* d = g_gpu.acquire(nw, nh, fmt);
        g_gpu.pass(psDown, d, { cur->srv });
        if (owned) g_gpu.release(cur);
        cur = d; owned = true;
    }
    Tex* tmp = g_gpu.acquire(cur->w, cur->h, fmt);
    BlurCB b = { { 1.0f / cur->w, 0 }, std::max(sx, 0.3f), std::min(64.0f, std::ceil(std::max(sx, 0.3f) * 2.5f)) };
    g_gpu.pass(psBlur, tmp, { cur->srv }, &b, sizeof(b));
    Tex* out = owned ? cur : g_gpu.acquire(cur->w, cur->h, fmt);
    BlurCB b2 = { { 0, 1.0f / cur->h }, std::max(sy, 0.3f), std::min(64.0f, std::ceil(std::max(sy, 0.3f) * 2.5f)) };
    g_gpu.pass(psBlur, out, { tmp->srv }, &b2, sizeof(b2));
    g_gpu.release(tmp);
    return out;
}

void Pipeline::updateLut(const Params& p, bool& identity) {
    identity = p.curve[0].isIdentity() && p.curve[1].isIdentity() && p.curve[2].isIdentity() && p.curve[3].isIdentity();
    if (identity) return;
    float m[256], c[3][256];
    BuildCurveLut(p.curve[0], m);
    for (int k = 0; k < 3; k++) BuildCurveLut(p.curve[k + 1], c[k]);
    std::vector<float> data(256 * 4);
    auto eval = [](const float* l, float x) {
        float f = std::clamp(x, 0.0f, 1.0f) * 255.0f; int i = (int)f; int j = std::min(i + 1, 255); float t = f - i;
        return l[i] * (1 - t) + l[j] * t;
    };
    for (int i = 0; i < 256; i++) {
        float v = m[i];
        for (int k = 0; k < 3; k++) data[i * 4 + k] = eval(c[k], v);
        data[i * 4 + 3] = 1;
    }
    g_gpu.ctx->UpdateSubresource(lut->tex, 0, nullptr, data.data(), 256 * 16, 0);
}

void Pipeline::drawDabs(Tex* target, const std::vector<Dab>& dabs, size_t from, float aspect, ID3D11PixelShader* ps, ID3D11BlendState* blend) {
    auto* ctx = g_gpu.ctx;
    g_gpu.unbindAll();
    D3D11_VIEWPORT vp = { 0, 0, (float)target->w, (float)target->h, 0, 1 };
    ctx->RSSetViewports(1, &vp);
    ctx->RSSetState(g_gpu.raster);
    ctx->OMSetRenderTargets(1, &target->rtv, nullptr);
    float bf[4] = {};
    ctx->OMSetBlendState(blend, bf, 0xffffffff);
    ctx->IASetInputLayout(nullptr);
    ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    ctx->VSSetShader(g_gpu.vsDab, nullptr, 0);
    ctx->PSSetShader(ps, nullptr, 0);
    ID3D11Buffer* cbs[2] = { g_gpu.cbCommon, g_gpu.cbPass };
    ctx->VSSetConstantBuffers(0, 2, cbs);
    ctx->PSSetConstantBuffers(0, 2, cbs);
    struct DabCB { float a[512][4]; float b[512][4]; float info[4]; };
    static DabCB cb;
    size_t i = from;
    while (i < dabs.size()) {
        size_t n = std::min<size_t>(512, dabs.size() - i);
        for (size_t k = 0; k < n; k++) {
            const Dab& d = dabs[i + k];
            cb.a[k][0] = d.x; cb.a[k][1] = d.y; cb.a[k][2] = d.r; cb.a[k][3] = d.flow;
            cb.b[k][0] = d.feather; cb.b[k][1] = d.erase; cb.b[k][2] = 0; cb.b[k][3] = 0;
        }
        cb.info[0] = aspect;
        D3D11_MAPPED_SUBRESOURCE map;
        if (SUCCEEDED(ctx->Map(g_gpu.cbPass, 0, D3D11_MAP_WRITE_DISCARD, 0, &map))) { memcpy(map.pData, &cb, sizeof(cb)); ctx->Unmap(g_gpu.cbPass, 0); }
        ctx->DrawInstanced(6, (UINT)n, 0, 0);
        i += n;
    }
    ctx->OMSetBlendState(nullptr, bf, 0xffffffff);
    g_gpu.unbindAll();
}

Tex* Pipeline::brushTexture(const Mask& m, const Geo& g) {
    int lw = 2048;
    int bw, bh;
    if (g.canvasW >= g.canvasH) { bw = std::min(lw, g.canvasW); bh = std::max(1, (int)((float)bw * g.canvasH / g.canvasW)); }
    else { bh = std::min(lw, g.canvasH); bw = std::max(1, (int)((float)bh * g.canvasW / g.canvasH)); }
    BrushCache& bc = brushes[m.id];
    bool rebuild = !bc.tex || bc.w != bw || bc.h != bh || bc.rev != m.brushRev || m.dabs.size() < bc.count;
    if (rebuild) {
        if (bc.tex && (bc.w != bw || bc.h != bh)) { bc.tex->release(); delete bc.tex; bc.tex = nullptr; }
        if (!bc.tex) bc.tex = g_gpu.create(bw, bh, DXGI_FORMAT_R16_UNORM, true);
        bc.w = bw; bc.h = bh; bc.rev = m.brushRev; bc.count = 0;
        g_gpu.clear(bc.tex, 0, 0, 0, 0);
    }
    if (m.dabs.size() > bc.count) drawDabs(bc.tex, m.dabs, bc.count, (float)g.canvasW / g.canvasH, psDab, g_gpu.blendAlpha);
    bc.count = m.dabs.size();
    return bc.tex;
}

Tex* Pipeline::glyphAtlas(const std::string& chars, int& count) {
    auto it = glyphs.find(chars);
    if (it != glyphs.end()) { count = it->second.second; return it->second.first; }
    std::wstring w = Utf8ToWide(chars);
    if (w.empty()) w = L" .";
    count = (int)w.size();
    const int cw = 48, chh = 80;
    int W = cw * count, H = chh;
    HDC dc = CreateCompatibleDC(nullptr);
    BITMAPINFO bi = {};
    bi.bmiHeader.biSize = sizeof(bi.bmiHeader); bi.bmiHeader.biWidth = W; bi.bmiHeader.biHeight = -H;
    bi.bmiHeader.biPlanes = 1; bi.bmiHeader.biBitCount = 32; bi.bmiHeader.biCompression = BI_RGB;
    void* bits = nullptr;
    HBITMAP bmp = CreateDIBSection(dc, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
    HGDIOBJ oldBmp = SelectObject(dc, bmp);
    memset(bits, 0, (size_t)W * H * 4);
    HFONT font = CreateFontW(-72, 0, 0, 0, FW_BOLD, 0, 0, 0, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                             ANTIALIASED_QUALITY, FIXED_PITCH, L"Consolas");
    HGDIOBJ oldFont = SelectObject(dc, font);
    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, RGB(255, 255, 255));
    for (int k = 0; k < count; k++) {
        RECT r = { k * cw, 0, (k + 1) * cw, chh };
        DrawTextW(dc, &w[k], 1, &r, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOCLIP);
    }
    GdiFlush();
    std::vector<uint8_t> px((size_t)W * H);
    const uint8_t* src8 = (const uint8_t*)bits;
    for (size_t k = 0; k < px.size(); k++) px[k] = src8[k * 4 + 1];
    SelectObject(dc, oldFont); DeleteObject(font);
    SelectObject(dc, oldBmp); DeleteObject(bmp); DeleteDC(dc);
    Tex* t = g_gpu.create(W, H, DXGI_FORMAT_R8_UNORM, false, px.data(), W);
    glyphs[chars] = { t, count };
    return t;
}

Tex* Pipeline::applyEffect(const Effect& e, Tex* in, int W, int H, const std::vector<Tex*>& maskW) {
    FxCB cb = {};
    for (int k = 0; k < 12; k++) cb.P[k] = e.p[k];
    for (int k = 0; k < 3; k++) { cb.C[k * 4] = e.col[k][0]; cb.C[k * 4 + 1] = e.col[k][1]; cb.C[k * 4 + 2] = e.col[k][2]; cb.C[k * 4 + 3] = 1; }
    cb.X[0] = e.seed;
    const DXGI_FORMAT F = DXGI_FORMAT_R16G16B16A16_FLOAT;
    float U = std::max(g_gpu.common.full[0], g_gpu.common.full[1]) / 1500.0f;
    float pxPerUnit = U * g_gpu.common.scale;
    Tex* fx = g_gpu.acquire(W, H, F);
    ID3D11PixelShader* ps = psFx[e.type];
    switch (e.type) {
    case FX_ASCII: {
        int n = 1;
        int set = (int)e.p[1];
        std::string chars = set >= 0 && set < 5 ? kAsciiSets[set] : e.text;
        if (chars.empty()) chars = " .";
        Tex* atlas = glyphAtlas(chars, n);
        cb.X[1] = (float)n;
        g_gpu.pass(ps, fx, { in->srv, atlas ? atlas->srv : nullptr }, &cb, sizeof(cb));
        break;
    }
    case FX_HALATION: {
        Tex* bright = g_gpu.acquire(W, H, F);
        cb.X[3] = 0;
        g_gpu.pass(ps, bright, { in->srv }, &cb, sizeof(cb));
        float s = e.p[1] * pxPerUnit;
        Tex* bl = ((int)e.p[3] == 2) ? blur(bright, s * 4, std::max(0.5f, s * 0.08f), F) : blur(bright, s, s, F);
        cb.X[3] = 1;
        g_gpu.pass(ps, fx, { in->srv, bl->srv }, &cb, sizeof(cb));
        g_gpu.release(bl); g_gpu.release(bright);
        break;
    }
    case FX_NEON: {
        Tex* edges = g_gpu.acquire(W, H, F);
        cb.X[3] = 0;
        g_gpu.pass(ps, edges, { in->srv }, &cb, sizeof(cb));
        float s = 8 * pxPerUnit;
        Tex* glow = blur(edges, s, s, F);
        cb.X[3] = 1;
        g_gpu.pass(ps, fx, { in->srv, edges->srv, glow->srv }, &cb, sizeof(cb));
        g_gpu.release(glow); g_gpu.release(edges);
        break;
    }
    case FX_TILTSHIFT: {
        float s = e.p[3] * 0.4f * pxPerUnit;
        Tex* b2 = blur(in, s, s, F);
        Tex* b1 = blur(in, s * 0.35f, s * 0.35f, F);
        g_gpu.pass(ps, fx, { in->srv, b1->srv, b2->srv }, &cb, sizeof(cb));
        g_gpu.release(b1); g_gpu.release(b2);
        break;
    }
    default:
        g_gpu.pass(ps, fx, { in->srv }, &cb, sizeof(cb));
    }
    CombineCB c = {};
    c.opacity = e.opacity / 100.0f; c.blend = (float)e.blend; c.target = (float)e.target; c.invert = e.targetInvert ? 1.f : 0.f;
    c.lo = e.tLo; c.hi = e.tHi; c.feather = e.tFeather; c.hueC = e.tHue; c.hueW = e.tHueWidth; c.hueF = e.tHueFeather; c.satMin = e.tSatMin;
    Tex* mw = (e.target == TG_MASK && e.tMask >= 0 && e.tMask < (int)maskW.size()) ? maskW[e.tMask] : nullptr;
    Tex* out = g_gpu.acquire(W, H, F);
    g_gpu.pass(psCombine, out, { in->srv, fx->srv, mw ? mw->srv : nullptr, e.target == TG_DEPTH && lastDepth ? lastDepth->srv : nullptr }, &c, sizeof(c));
    g_gpu.release(fx);
    return out;
}

// =====================================================================================  main graph
Tex* Pipeline::renderCore(const Params& p, const Geo& g, int W, int H, const RenderOpts& o, std::vector<Tex*>& kept) {
    const DXGI_FORMAT F = DXGI_FORMAT_R16G16B16A16_FLOAT;
    const DXGI_FORMAT F2 = DXGI_FORMAT_R16G16_FLOAT;
    auto& cm = g_gpu.common;
    cm.full[0] = (float)g.fullW; cm.full[1] = (float)g.fullH;
    cm.scale = (float)W / g.fullW;
    cm.seed = 17.0f;
    float longFull = (float)std::max(g.fullW, g.fullH);
    float sc = cm.scale;

    // ---- geometry
    GeoCB gc = {};
    gc.crop[0] = g.cx0; gc.crop[1] = g.cy0; gc.crop[2] = g.cw; gc.crop[3] = g.ch;
    gc.canvasPx[0] = (float)g.canvasW; gc.canvasPx[1] = (float)g.canvasH;
    float a = p.straighten * 3.14159265f / 180.0f;
    gc.cosA = std::cos(a); gc.sinA = std::sin(a);
    float cw = (float)g.canvasW, ch = (float)g.canvasH;
    float ca = std::fabs(gc.cosA), sa = std::fabs(gc.sinA);
    float cover = std::max((cw * ca + ch * sa) / cw, (cw * sa + ch * ca) / ch);
    gc.scl = cover * p.geoScale / 100.0f;
    gc.pv = p.perspV / 100.0f * 0.35f; gc.ph = p.perspH / 100.0f * 0.35f;
    gc.dist = p.lensDist / 100.0f * 0.12f;
    gc.ca = p.lensCA / 100.0f * 0.002f;
    gc.lvig = p.lensVig / 100.0f;
    gc.rot90 = (float)(p.rot90 & 3); gc.flipH = p.flipH; gc.flipV = p.flipV;
    Tex* geo = g_gpu.acquire(W, H, F);
    g_gpu.pass(psGeo, geo, { src->srv }, &gc, sizeof(gc));
    kept.push_back(geo); lastGeo = geo;

    // ---- noise reduction (linear, before tone)
    Tex* base = geo;
    if (p.nrLuma > 0 || p.nrColor > 0) {
        NoiseCB nc = { p.nrLuma / 100.0f, p.nrDetail / 100.0f, p.nrColor / 100.0f, p.nrSmooth / 100.0f };
        Tex* prep = g_gpu.acquire(W, H, F);
        g_gpu.pass(psNrPrep, prep, { geo->srv });
        Tex* dn = g_gpu.acquire(W, H, F);
        g_gpu.pass(psDenoise, dn, { prep->srv }, &nc, sizeof(nc));
        g_gpu.release(prep);
        kept.push_back(dn);
        base = dn;
    }

    // ---- white balance / exposure
    float t = p.temp / 100.0f, ti = p.tint / 100.0f;
    float mr = std::exp(t * 0.55f), mg = std::exp(-ti * 0.35f), mb = std::exp(-t * 0.55f);
    float ml = 0.2126f * mr + 0.7152f * mg + 0.0722f * mb;
    mr /= ml; mg /= ml; mb /= ml;
    float baseline = 0, profC = 1, profS = 1;
    switch (p.profile) {
    case 0: baseline = srcIsRaw ? 0.5f : 0; profC = srcIsRaw ? 1.15f : 1.0f; profS = srcIsRaw ? 1.08f : 1.0f; break;
    case 1: baseline = srcIsRaw ? 0.5f : 0; break;
    case 2: baseline = srcIsRaw ? 0.5f : 0; profC = 1.25f; profS = 1.25f; break;
    case 3: baseline = srcIsRaw ? 0.6f : 0; profC = 0.8f; profS = 0.9f; break;
    }
    float gain = std::pow(2.0f, p.exposure + baseline);

    // ---- luminance helpers for local tone
    LumCB lc = { { mr * gain, mg * gain, mb * gain, 0 } };
    Tex* lum = g_gpu.acquire(W, H, F2);
    g_gpu.pass(psLumPrep, lum, { base->srv }, &lc, sizeof(lc));
    float sT = std::max(0.6f, 0.0006f * longFull * sc);
    float sC = 0.008f * longFull * sc;
    float sL = 0.025f * longFull * sc;
    Tex* bT = (p.texture != 0) ? blur(lum, sT, sT, F2) : nullptr;
    Tex* bC = (p.clarity != 0) ? blur(lum, sC, sC, F2) : nullptr;
    Tex* bL = blur(lum, sL, sL, F2);

    // ---- develop
    DevCB d = {};
    for (int k = 0; k < 8; k++) { d.hslH[k] = p.hslH[k] * 0.3f; d.hslS[k] = p.hslS[k] / 100.0f; d.hslL[k] = p.hslL[k] / 100.0f; d.bwMix[k] = p.bwMix[k] / 100.0f; }
    float* gw[4] = { d.gS, d.gM, d.gH, d.gG };
    for (int k = 0; k < 4; k++) {
        float rgb[3]; HueToRgb(p.grade[k].hue, rgb);
        float l = 0.2126f * rgb[0] + 0.7152f * rgb[1] + 0.0722f * rgb[2];
        float s = p.grade[k].sat / 100.0f * (k == 3 ? 0.15f : 0.25f);
        for (int c = 0; c < 3; c++) gw[k][c] = (rgb[c] - l) * s;
        gw[k][3] = p.grade[k].lum / 100.0f * 0.2f;
    }
    d.wbGain[0] = mr * gain; d.wbGain[1] = mg * gain; d.wbGain[2] = mb * gain;
    d.shoulderK = srcIsRaw ? 0.6f : 0.75f;
    float Wp = srcIsRaw ? gain : std::max(1.0f, gain);
    Wp *= std::pow(2.0f, -p.whites / 100.0f * 1.2f);
    d.shoulderW = std::max(Wp, d.shoulderK + 0.05f);
    d.blackPt = -p.blacks / 100.0f * 0.08f;
    d.contrastA = std::exp(p.contrast / 100.0f * 0.75f) * profC;
    d.shadows = p.shadows / 100.0f; d.highlights = p.highlights / 100.0f;
    d.clarity = p.clarity / 100.0f; d.texture = p.texture / 100.0f; d.dehaze = p.dehaze / 100.0f;
    d.vibrance = p.vibrance / 100.0f; d.saturation = p.saturation / 100.0f;
    d.bw = p.bw ? 1.f : 0.f; d.gradBlend = p.gradBlend / 100.0f; d.gradBalance = p.gradBalance / 100.0f;
    bool lutId = true;
    updateLut(p, lutId);
    d.useLut = lutId ? 0.f : 1.f;
    d.satBase = profS;
    Tex* dev = g_gpu.acquire(W, H, F);
    g_gpu.pass(psDevelop, dev, { base->srv, bT ? bT->srv : bL->srv, bC ? bC->srv : bL->srv, bL->srv, lut->srv }, &d, sizeof(d));
    if (bT) g_gpu.release(bT);
    if (bC) g_gpu.release(bC);
    g_gpu.release(bL);
    g_gpu.release(lum);
    kept.push_back(dev); lastDev = dev;

    // ---- depth map aligned with the output (same geometry, without lens colour effects)
    Tex* depth = nullptr;
    if (needsDepth(p)) {
        GeoCB dg = gc;
        dg.ca = 0; dg.lvig = 0;
        Tex* al = g_gpu.acquire(W, H, DXGI_FORMAT_R16_FLOAT);
        g_gpu.pass(psGeo, al, { (depthTex ? depthTex : flatDepth)->srv }, &dg, sizeof(dg));
        depth = refineDepth(p, g, al, dev, W, H);
        g_gpu.release(al);
        kept.push_back(depth); lastDepth = depth;
    }

    // ---- masks
    Tex* cur = dev;
    std::vector<Tex*> maskW(p.masks.size(), nullptr);
    bool anyMask = false;
    for (auto& m : p.masks) if (m.enabled) anyMask = true;
    if (anyMask) {
        LumCB lc2 = { { 1, 1, 1, 1 } };
        Tex* dl = g_gpu.acquire(W, H, F2);
        g_gpu.pass(psLumPrep, dl, { dev->srv }, &lc2, sizeof(lc2));
        Tex* dlB = blur(dl, sC, sC, F2);
        g_gpu.release(dl);
        float ss = std::max(0.8f, 1.2f * longFull / 4000.0f * sc);
        Tex* preview = blur(dev, ss, ss, F);
        for (size_t k = 0; k < p.masks.size(); k++) {
            const Mask& m = p.masks[k];
            if (!m.enabled) continue;
            MaskCB mc = {};
            mc.crop[0] = g.cx0; mc.crop[1] = g.cy0; mc.crop[2] = g.cw; mc.crop[3] = g.ch;
            mc.lin[0] = m.lx0; mc.lin[1] = m.ly0; mc.lin[2] = m.lx1; mc.lin[3] = m.ly1;
            mc.rad[0] = m.cx; mc.rad[1] = m.cy; mc.rad[2] = m.rx; mc.rad[3] = m.ry;
            mc.radAngle = m.angle * 3.14159265f / 180.0f; mc.feather = m.feather / 100.0f;
            mc.invert = m.invert; mc.shape = (float)m.shape; mc.range = (float)m.range;
            mc.lumLo = m.lumLo; mc.lumHi = m.lumHi; mc.lumF = m.lumFeather;
            mc.hueC = m.hueC; mc.hueW = m.hueWidth; mc.hueF = m.hueFeather; mc.satMin = m.satMin;
            mc.asp = (float)g.canvasW / g.canvasH;
            Tex* bt = m.shape == MS_BRUSH ? brushTexture(m, g) : nullptr;
            Tex* w = g_gpu.acquire(W, H, DXGI_FORMAT_R16_FLOAT);
            g_gpu.pass(psMaskW, w, { dev->srv, bt ? bt->srv : nullptr, depth ? depth->srv : nullptr }, &mc, sizeof(mc));
            maskW[k] = w;
            kept.push_back(w);
            LocalCB l = {};
            const LocalAdj& a2 = m.adj;
            l.amount = m.amount / 100.0f; l.exposure = a2.exposure; l.contrast = a2.contrast / 100.0f;
            l.highlights = a2.highlights / 100.0f; l.shadows = a2.shadows / 100.0f; l.whites = a2.whites / 100.0f; l.blacks = a2.blacks / 100.0f;
            l.temp = a2.temp / 100.0f; l.tint = a2.tint / 100.0f; l.hue = a2.hue; l.sat = a2.saturation / 100.0f;
            l.clarity = a2.clarity / 100.0f; l.dehaze = a2.dehaze / 100.0f; l.sharp = a2.sharpness / 100.0f; l.colorAmt = a2.colorAmt / 100.0f;
            l.color[0] = a2.color[0]; l.color[1] = a2.color[1]; l.color[2] = a2.color[2]; l.color[3] = 1;
            Tex* nx = g_gpu.acquire(W, H, F);
            g_gpu.pass(psLocal, nx, { cur->srv, w->srv, dlB->srv, preview->srv }, &l, sizeof(l));
            if (cur != dev) g_gpu.release(cur);
            cur = nx;
        }
        g_gpu.release(dlB);
        g_gpu.release(preview);
    }
    lastMaskW = maskW;

    // ---- sharpening
    if (p.sharpAmount > 0) {
        float s = std::max(0.5f, p.sharpRadius * sc);
        Tex* b = blur(cur, s, s, F);
        SharpCB sc2 = { p.sharpAmount / 100.0f, p.sharpDetail / 100.0f, p.sharpMask / 100.0f, 0 };
        Tex* nx = g_gpu.acquire(W, H, F);
        g_gpu.pass(psSharpen, nx, { cur->srv, b->srv }, &sc2, sizeof(sc2));
        g_gpu.release(b);
        if (cur != dev) g_gpu.release(cur);
        cur = nx;
    }

    // ---- vignette & grain
    if (p.vigAmount != 0 || p.grainAmount > 0) {
        FinishCB f = { p.vigAmount / 100.0f, p.vigMid / 100.0f, p.vigRound / 100.0f, p.vigFeather / 100.0f, p.vigHigh / 100.0f,
                       p.grainAmount / 100.0f, p.grainSize / 100.0f, p.grainRough / 100.0f };
        Tex* nx = g_gpu.acquire(W, H, F);
        g_gpu.pass(psFinish, nx, { cur->srv }, &f, sizeof(f));
        if (cur != dev) g_gpu.release(cur);
        cur = nx;
    }

    // ---- depth, light and glass
    if (depth && opticsActive(p)) {
        Tex* nx;
        if (p.showDepth) { nx = g_gpu.acquire(W, H, F); g_gpu.pass(psDepthView, nx, { cur->srv, depth->srv }); }
        else nx = applyOptics(p, g, cur, depth, W, H);
        if (nx != cur) { if (cur != dev) g_gpu.release(cur); cur = nx; }
    }

    // ---- experimental stack
    for (auto& e : p.effects) {
        if (!e.enabled || e.opacity <= 0) continue;
        Tex* nx = applyEffect(e, cur, W, H, maskW);
        if (cur != dev) g_gpu.release(cur);
        cur = nx;
    }
    if (cur == dev) {
        // make sure the result is a separate texture so "keep" ownership stays simple
        Tex* nx = g_gpu.acquire(W, H, F);
        g_gpu.pass(psDown, nx, { dev->srv });
        cur = nx;
    }
    return cur;
}

void Pipeline::renderDisplay(const Params& p, int workLong, const RenderOpts& o) {
    if (!src) return;
    auto t0 = std::chrono::high_resolution_clock::now();
    for (Tex* t : retired) { t->release(); delete t; }
    retired.clear();
    releaseKeep();
    Geo g = geometry(p, o.cropOverride);
    float s = std::min(1.0f, (float)workLong / std::max(g.fullW, g.fullH));
    int W = std::max(1, (int)std::lround(g.fullW * s));
    int H = std::max(1, (int)std::lround(g.fullH * s));
    Tex* fin;
    if (o.before) {
        Params n;
        n.profile = 1;
        n.rot90 = p.rot90; n.flipH = p.flipH; n.flipV = p.flipV;
        n.cropX0 = p.cropX0; n.cropY0 = p.cropY0; n.cropX1 = p.cropX1; n.cropY1 = p.cropY1;
        n.straighten = p.straighten; n.perspV = p.perspV; n.perspH = p.perspH; n.geoScale = p.geoScale;
        fin = renderCore(n, g, W, H, RenderOpts{ false, o.cropOverride, -1, false }, keep);
    } else {
        fin = renderCore(p, g, W, H, o, keep);
    }
    if (!display || display->w != W || display->h != H) {
        // the UI may still reference the old texture in this frame's draw list: free it on the next render
        if (display) retired.push_back(display);
        display = g_gpu.create(W, H, DXGI_FORMAT_R8G8B8A8_UNORM, true);
    }
    OutCB oc = { o.clipWarn ? 1.f : 0.f, o.before ? 0.f : (float)o.altView, 0, 0 };
    Tex* ov = nullptr;
    if (!o.before && o.overlayMask >= 0 && o.overlayMask < (int)lastMaskW.size()) ov = lastMaskW[o.overlayMask];
    if (ov) oc.hasMask = 1;
    g_gpu.pass(psOutput, display, { fin->srv, ov ? ov->srv : nullptr }, &oc, sizeof(oc));
    computeHistogram(fin);
    g_gpu.release(fin);
    dispW = W; dispH = H;
    g_gpu.frame++;
    g_gpu.trimPool(2);
    g_gpu.ctx->Flush();
    auto t1 = std::chrono::high_resolution_clock::now();
    lastRenderMs = std::chrono::duration<double, std::milli>(t1 - t0).count();
}

void Pipeline::computeHistogram(Tex* t) {
    int hw = 320, hh = std::max(1, (int)(320.0f * t->h / t->w));
    if (!histSmall || histSmall->w != hw || histSmall->h != hh) {
        if (histSmall) { histSmall->release(); delete histSmall; }
        histSmall = g_gpu.create(hw, hh, DXGI_FORMAT_R8G8B8A8_UNORM, true);
    }
    g_gpu.pass(psConvert, histSmall, { t->srv });
    std::vector<uint8_t> px; int bpp;
    memset(hist, 0, sizeof(hist));
    if (!g_gpu.readTexture(histSmall, px, bpp)) return;
    for (size_t i = 0; i + 3 < px.size(); i += 4) {
        hist[0][px[i]]++; hist[1][px[i + 1]]++; hist[2][px[i + 2]]++;
        int l = (int)(0.2126f * px[i] + 0.7152f * px[i + 1] + 0.0722f * px[i + 2]);
        hist[3][std::min(l, 255)]++;
    }
}

bool Pipeline::renderExport(const Params& p, int outW, int outH, float outSharpen, std::vector<uint16_t>& rgba, int& w, int& h) {
    if (!src) return false;
    releaseKeep();
    Geo g = geometry(p, false);
    int W = std::max(1, outW > 0 ? outW : g.fullW);
    int H = std::max(1, outH > 0 ? outH : g.fullH);
    if (std::max(W, H) > 16384) { float k = 16384.0f / std::max(W, H); W = std::max(1, (int)(W * k)); H = std::max(1, (int)(H * k)); }
    RenderOpts o;
    Tex* fin = renderCore(p, g, W, H, o, keep);
    if (!fin) return false;
    if (outSharpen > 0) {
        // output sharpening: compensates for downscaling / print softness
        const DXGI_FORMAT F = DXGI_FORMAT_R16G16B16A16_FLOAT;
        float r = std::max(0.6f, std::min(W, H) / 2500.0f);
        Tex* b = blur(fin, r, r, F);
        SharpCB sc = { outSharpen, 0.35f, 0.0f, 0 };
        Tex* nx = g_gpu.acquire(W, H, F);
        g_gpu.pass(psSharpen, nx, { fin->srv, b->srv }, &sc, sizeof(sc));
        g_gpu.release(b);
        g_gpu.release(fin);
        fin = nx;
    }
    Tex* out = g_gpu.create(W, H, DXGI_FORMAT_R16G16B16A16_UNORM, true);
    g_gpu.pass(psConvert, out, { fin->srv });
    g_gpu.release(fin);
    std::vector<uint8_t> raw; int bpp;
    bool ok = g_gpu.readTexture(out, raw, bpp);
    out->release(); delete out;
    releaseKeep();
    g_gpu.frame++;
    g_gpu.trimPool(0);
    if (!ok) return false;
    rgba.resize((size_t)W * H * 4);
    memcpy(rgba.data(), raw.data(), rgba.size() * 2);
    w = W; h = H;
    return true;
}

static bool SampleTex(Tex* t, float u, float v, float rgb[3]) {
    if (!t) return false;
    int x = (int)(u * t->w), y = (int)(v * t->h);
    std::vector<float> px;
    if (!g_gpu.readPixels(t, x - 2, y - 2, 5, 5, px) || px.empty()) return false;
    double s[3] = {};
    size_t n = px.size() / 4;
    for (size_t k = 0; k < n; k++) for (int c = 0; c < 3; c++) s[c] += px[k * 4 + c];
    for (int c = 0; c < 3; c++) rgb[c] = (float)(s[c] / n);
    return true;
}
bool Pipeline::sampleSource(float u, float v, float rgb[3]) { return SampleTex(lastGeo, u, v, rgb); }
bool Pipeline::sampleDeveloped(float u, float v, float rgb[3]) { return SampleTex(lastDev, u, v, rgb); }
bool Pipeline::sampleDepth(float u, float v, float& n) {
    float rgb[3];
    if (!SampleTex(lastDepth, u, v, rgb)) return false;
    n = rgb[0];
    return true;
}
