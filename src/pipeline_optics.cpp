// Depth map, light sources and glass optics (part of Pipeline).
#include "pipeline.h"
#include <algorithm>
#include <cmath>
#include <cstring>

// =====================================================================  HLSL
// Depth aligned to the output, refined along image edges (joint bilateral) plus hand corrections.
static const char* kDepthRefine = R"HLSL(
cbuffer R : register(b1) { float4 crop; float stepPx; float hasEdit; float2 _r; };
float4 main(VSO i) : SV_Target {
    float3 g0 = t1.SampleLevel(sPt, i.uv, 0).rgb;
    float sum = 0, ws = 0;
    [unroll] for (int y = -2; y <= 2; y++) {
        [unroll] for (int x = -2; x <= 2; x++) {
            float2 uv = i.uv + float2(x, y) * uInv * stepPx;
            float d = t0.SampleLevel(sLin, uv, 0).r;
            float3 g = t1.SampleLevel(sLin, uv, 0).rgb;
            float3 df = g - g0;
            float w = exp(-(x * x + y * y) / 12.0) * exp(-dot(df, df) * 90);   // wide: samples past the soft edge of the map count too
            sum += d * w; ws += w;
        }
    }
    float d = sum / max(ws, 1e-5);
    if (hasEdit > 0.5) d += t2.SampleLevel(sLin, crop.xy + i.uv * crop.zw, 0).r;
    return float4(saturate(d), 0, 0, 1);
}
)HLSL";

// Signed brush dabs for depth corrections (additive blending).
static const char* kDabAdd = R"HLSL(
struct DO { float4 pos : SV_Position; float2 q : TEXCOORD0; float4 d : TEXCOORD1; };
float4 main(DO i) : SV_Target {
    float r = length(i.q);
    if (r > 1) discard;
    float f = max(i.d.y, 0.01);
    float a = 1 - smoothstep(1 - f, 1, r);
    return float4((i.d.z > 0.5 ? -1 : 1) * a * i.d.x, 0, 0, 1);
}
)HLSL";

// Max-filtered smaller copy of the depth map: a long shadow ray can take big steps and still not miss a thin occluder.
static const char* kDepthMax = R"HLSL(
float4 main(VSO i) : SV_Target {
    float m = 0;
    [unroll] for (int y = 0; y < 4; y++) {
        [unroll] for (int x = 0; x < 4; x++) m = max(m, t0.SampleLevel(sLin, i.uv + (float2(x, y) / 3.0 - 0.5) * uInv, 0).r);
    }
    return float4(m, 0, 0, 1);
}
)HLSL";

static const char* kDepthView = R"HLSL(
float4 main(VSO i) : SV_Target {
    float n = t1.SampleLevel(sLin, i.uv, 0).r;
    float v = pow(saturate(n), 0.85);
    return float4(v, v, v * 1.04, 1);
}
)HLSL";

static const char* kFogDof = R"HLSL(
cbuffer FD : register(b1) { float fog; float fogStart; float dof; float focus; float range; float3 fogCol; };
float4 main(VSO i) : SV_Target {
    float4 s = t0.SampleLevel(sPt, i.uv, 0);
    float n = t1.SampleLevel(sLin, i.uv, 0).r;
    float3 c = s.rgb;
    if (dof > 0) {
        float coc = saturate((abs(n - focus) - range) / 0.3) * saturate(dof * 1.5);
        float3 b1 = t2.SampleLevel(sLin, i.uv, 0).rgb, b2 = t3.SampleLevel(sLin, i.uv, 0).rgb;
        c = coc < 0.5 ? lerp(c, b1, coc * 2) : lerp(b1, b2, coc * 2 - 1);
    }
    float3 lin = srgbDec(c);
    if (fog > 0) {
        float f = fog * smoothstep(fogStart, 1.0, 1 - n);
        lin = lerp(lin, srgbDec(fogCol), f);
    }
    return float4(srgbEnc(saturate(lin)), s.a);
}
)HLSL";

static const char* kOpticsCommon = R"HLSL(
float depthAt(float2 uv) { return t1.SampleLevel(sLin, uv, 0).r; }
float shoulder3(float x) { return x < 0.8 ? x : 0.8 + 0.2 * (1 - exp(-(x - 0.8) / 0.2)); }
float3 softClip(float3 c) { return float3(shoulder3(c.r), shoulder3(c.g), shoulder3(c.b)); }
float3 spectrumRGB(float t) { float3 c = hsv2rgb(float3(saturate(t) * 0.78, 1, 1)); return c * c; }   // 0 red .. 1 violet
)HLSL";

// Light sources, in two steps: the smooth parts (soft shadows, glow, light shafts, ambient occlusion) at a reduced
// resolution, then shading with surface normals and the result at full resolution.
static const char* kLightsCB = R"HLSL(
cbuffer LT : register(b1) {
    float4 LP[8];   // x, y (aspect space), distance in front of the surface below, kind (0 point, 1 spot, 2 beam)
    float4 LC[8];   // colour * intensity, range
    float4 LS[8];   // shadow softness, glow, shafts, gloss
    float4 LD[8];   // direction x, y, cos(cone) or beam width, cone softness
    float4 PR[4];   // prisms: centre x, y, half length, half width
    float4 PR2[4];  // angle, ior, dispersion, unused
    float nL; float nP; float relief; float aspect;
    float ambientLevel; float micro; float mode; float darken;
    float2 dstep; float2 _lp;
};
float2 toUV(float2 A) { return float2(A.x / aspect, A.y); }
)HLSL";

// A light hovers in front of whatever is below it, so it sits inside the scene instead of above a flat picture.
static const char* kLightPos = R"HLSL(
float3 lightPos(int l) {
    float2 uv = float2(LP[l].x / aspect, LP[l].y);
    float2 r = float2(0.03 / aspect, 0.03);
    float d = depthAt(uv) * 0.4 + (depthAt(uv + float2(r.x, 0)) + depthAt(uv - float2(r.x, 0)) + depthAt(uv + float2(0, r.y)) + depthAt(uv - float2(0, r.y))) * 0.15;
    return float3(LP[l].xy, d * relief + LP[l].z);
}
)HLSL";

static const char* kLightsLow = R"HLSL(
// depth around uv seen at a footprint of fp low-resolution pixels (max over the footprint)
// t1 is the base level here
float maxDepthAt(float2 uv, float fp) {
    if (fp <= 1) return t1.SampleLevel(sLin, uv, 0).r;
    if (fp <= 2) return t2.SampleLevel(sLin, uv, 0).r;
    if (fp <= 4) return t3.SampleLevel(sLin, uv, 0).r;
    if (fp <= 8) return t4.SampleLevel(sLin, uv, 0).r;
    return t5.SampleLevel(sLin, uv, 0).r;
}
float4 main(VSO i) : SV_Target {
    float2 A = float2(i.uv.x * aspect, i.uv.y);
    float h = depthAt(i.uv) * relief;
    float3 P3 = float3(A, h);
    if (mode < 1.5) {
        // soft shadows of four lights: walk the depth relief towards each light
        float4 vis = 1;
        int base = mode < 0.5 ? 0 : 4;
        [loop] for (int j = 0; j < 4; j++) {
            int l = base + j;
            if (l >= (int)nL) break;
            if (LP[l].w > 1.5) continue;
            float3 Ld = lightPos(l) - P3;
            float soft = LS[l].x, sh = 1;
            // skip the first couple of depth texels: the soft ramp of an outline must not shadow itself
            float ts = saturate(length(dstep * float2(aspect, 1)) * 2.5 / max(length(Ld.xy), 1e-4));
            // enough steps that each covers at most 12 pixels, and each step looks at the max depth over its length
            float lenPx = length(Ld.xy) * (1 - ts) * uSize.y;
            int n = clamp((int)ceil(lenPx / 12), 12, 96);
            float fp = lenPx / n;
            float jit = 0.5;
            float dz = Ld.z * (1 - ts) / n;
            [loop] for (int k = 0; k < n; k++) {
                float t = lerp(ts, 1, (k + jit) / n);
                float3 q = P3 + Ld * t;
                // blocked wherever the ray runs below the surface over the whole step
                float occHi = maxDepthAt(toUV(q.xy), fp) * relief - 0.012 - (q.z - abs(dz) * 0.5);
                sh = min(sh, saturate(1 - occHi / (0.004 + soft * 0.09 * t)));
            }
            if (j == 0) vis.x = sh; else if (j == 1) vis.y = sh; else if (j == 2) vis.z = sh; else vis.w = sh;
        }
        return vis;
    }
    // glow around the sources and light shafts in the air
    float3 add = 0;
    [loop] for (int l = 0; l < (int)nL; l++) {
        if (LP[l].w > 1.5) continue;
        float3 L3 = lightPos(l);
        float3 col = LC[l].rgb;
        float d2 = length(A - L3.xy);
        // small hot core and a wide soft halo, like a real lamp seen through a lens
        float gr = 0.004 + 0.012 * LS[l].y;
        add += col * LS[l].y * (0.6 * exp(-(d2 / gr) * (d2 / gr)) + 0.08 * exp(-d2 / (gr * 6)) + 0.03 * exp(-d2 * 6));
        if (LS[l].z > 0) {
            float acc = 0, wsum = 0;
            [loop] for (int k = 0; k < 32; k++) {
                float t = (k + 0.5) / 32.0;
                // air between the camera and the scene glows where the lamp sees it: what stands in front of the lamp blocks it
                float2 qa = lerp(A, L3.xy, t);
                float open = 1 - smoothstep(L3.z - 0.02, L3.z + 0.02, depthAt(toUV(qa)) * relief);
                float w = 0.3 + t;
                acc += open * w; wsum += w;
            }
            float cone = 1;
            if (LP[l].w > 0.5) {
                float3 S = normalize(float3(LD[l].xy * 0.4, -max(LP[l].z, 0.05)));
                cone = smoothstep(LD[l].z - 0.1, LD[l].z + 0.04 + LD[l].w, dot(normalize(float3(A, h) - L3), S));
            }
            add += col * LS[l].z * 0.22 * (acc / wsum) * exp(-d2 * d2 * 40 - d2 * 4) * cone;
        }
    }
    // ambient occlusion: how much nearer relief surrounds this point
    float occ = 0;
    [unroll] for (int k = 0; k < 8; k++) {
        float ang = k * 0.7854 + 0.39;
        float2 dir = float2(cos(ang), sin(ang));
        [unroll] for (int r = 1; r <= 3; r++) {
            float rad = 0.01 * r * r;
            float dh = depthAt(toUV(A + dir * rad)) * relief - h;
            // only relief close in depth occludes; an object far in front does not darken what is behind it
            float range = saturate(rad * 3 / max(dh, 1e-4));
            occ += saturate(dh / rad * 0.6 - 0.05) * range * range / r;
        }
    }
    return float4(add, saturate(occ / (8 * 1.83) * 1.6));
}
)HLSL";

static const char* kLightsFull = R"HLSL(
bool prismHit(float2 O, float2 d, int k, out float tIn, out float tOut) {
    float2 o = rot2(O - PR[k].xy, -PR2[k].x), dd = rot2(d, -PR2[k].x);
    float2 hs = PR[k].zw;
    float2 inv = float2(abs(dd.x) < 1e-5 ? 1e5 : 1 / dd.x, abs(dd.y) < 1e-5 ? 1e5 : 1 / dd.y);
    float2 t1 = (-hs - o) * inv, t2 = (hs - o) * inv;
    float2 tmin = min(t1, t2), tmax = max(t1, t2);
    tIn = max(tmin.x, tmin.y); tOut = min(tmax.x, tmax.y);
    return tOut > max(tIn, 0);
}
float lumAt(float2 uv) { return log2(luma(srgbDec(t0.SampleLevel(sLin, uv, 0).rgb)) + 0.02); }
float4 main(VSO i) : SV_Target {
    float4 s = t0.SampleLevel(sPt, i.uv, 0);
    float3 x = srgbDec(s.rgb);
    float2 A = float2(i.uv.x * aspect, i.uv.y);
    float h = depthAt(i.uv) * relief;
    float3 P3 = float3(A, h);
    // surface normal: large shapes from the depth map, fine relief from the picture itself (lighter reads as raised)
    // one-sided differences, taking the gentler side: across an outline each pixel keeps the slope of its own surface
    float d0 = depthAt(i.uv);
    float dxp = depthAt(i.uv + float2(dstep.x, 0)) - d0, dxm = d0 - depthAt(i.uv - float2(dstep.x, 0));
    float dyp = depthAt(i.uv + float2(0, dstep.y)) - d0, dym = d0 - depthAt(i.uv - float2(0, dstep.y));
    float gx = (abs(dxp) < abs(dxm) ? dxp : dxm) * relief / (dstep.x * aspect);
    float gy = (abs(dyp) < abs(dym) ? dyp : dym) * relief / dstep.y;
    // a jump in the depth map is an outline, not a steep wall: cap the slope so outlines do not look embossed
    // real surfaces have gentle slopes; the much steeper ramps of an outline are faded out instead of shading like a bevel
    float2 gd = float2(gx, gy) * 2.5;
    float gm = length(gd);
    float q = gm / 1.6;
    gd *= 1 / (1 + q * q * q * q);
    float2 e = uInv * 1.5;
    float2 gl = float2(lumAt(i.uv + float2(e.x, 0)) - lumAt(i.uv - float2(e.x, 0)), lumAt(i.uv + float2(0, e.y)) - lumAt(i.uv - float2(0, e.y)));
    // only fine texture: strong brightness edges (outlines, horizon) and depth outlines are left flat
    float ge = length(gl) / 0.35;
    gl *= micro * 0.6 / (1 + ge * ge * ge);
    float gc = (abs(dxp + dxm) / dstep.x + abs(dyp + dym) / dstep.y) * relief * 0.05;
    gl *= 1 / (1 + gc * gc * 4);
    float3 N = normalize(float3(-gd - gl, 1));
    float4 visA = t2.SampleLevel(sLin, i.uv, 0), visB = t3.SampleLevel(sLin, i.uv, 0);
    float4 low = t4.SampleLevel(sLin, i.uv, 0);
    // the photo already contains its own lighting: shadowed areas are dark but not black, so a new lamp brightens them more
    float Lb = luma(srgbDec(t5.SampleLevel(sLin, i.uv, 0).rgb));
    float3 albedo = x * lerp(1, clamp(0.22 / (Lb + 0.03), 0.6, 3.0), 0.55);
    float shine = 0.25 + 0.75 * sqrt(saturate(luma(x)));
    float3 light = 0, spec = 0, add = low.rgb;
    [loop] for (int l = 0; l < (int)nL; l++) {
        int kind = (int)(LP[l].w + 0.5);
        float3 col = LC[l].rgb;
        if (kind == 2) {
            // a narrow beam; when it meets a prism it leaves as a fan of colours
            float2 O = LP[l].xy, d = normalize(LD[l].xy);
            float w = LD[l].z;
            float tStop = 1e3, tOut = 0; int hit = -1;
            [loop] for (int k = 0; k < (int)nP; k++) {
                float a, b;
                if (prismHit(O, d, k, a, b) && a < tStop && a > 0) { tStop = a; tOut = b; hit = k; }
            }
            float2 v = A - O;
            float along = dot(v, d), perp = abs(v.x * d.y - v.y * d.x);
            float on = smoothstep(0, 0.01, along) * (along < tStop ? 1 : 0);
            add += col * (exp(-(perp / w) * (perp / w)) * 0.9 + 0.125 * exp(-(perp / (w * 4)) * (perp / (w * 4)))) * on;
            if (hit >= 0) {
                float ior = PR2[hit].y, disp = PR2[hit].z;
                float2 X = O + d * tOut;
                float2 dm = rot2(d, (ior - 1) * 0.9);
                float spread = 0.03 + disp * 0.4;
                float2 vx = A - X;
                float al = dot(vx, dm);
                if (al > 0) {
                    float ang = atan2(dm.x * vx.y - dm.y * vx.x, al);
                    float t = ang / spread + 0.5;
                    float wgt = smoothstep(0, 0.12, t) * smoothstep(1, 0.88, t);
                    add += spectrumRGB(1 - t) * col * wgt * 1.1 / (1 + al * 2.0) / (1 + spread * 3);
                }
            }
            continue;
        }
        float3 L3 = lightPos(l);
        float3 Ld = L3 - P3;
        float3 Lv = normalize(Ld);
        float ndl = dot(N, Lv);
        float diff = saturate((ndl + 0.12) / 1.12);
        // distance in depth counts fully: things far behind or in front of the lamp get much less of it
        float att = 1 / (1 + dot(Ld, Ld) / (LC[l].w * LC[l].w) * 3);
        float cone = 1;
        if (kind == 1) {
            float3 S = normalize(float3(LD[l].xy * 0.4, -max(LP[l].z, 0.05)));
            cone = smoothstep(LD[l].z, LD[l].z + 0.04 + LD[l].w, dot(-Lv, S));
        }
        float4 vv = l < 4 ? visA : visB;
        int vj = l & 3;
        float vis = vj == 0 ? vv.x : vj == 1 ? vv.y : vj == 2 ? vv.z : vv.w;
        float k = att * cone * vis;
        light += col * diff * k;
        float3 H = normalize(Lv + float3(0, 0, 1));
        float gloss = LS[l].w;
        spec += col * pow(saturate(dot(N, H)), lerp(6, 140, gloss)) * gloss * 0.55 * shine * k * saturate(ndl * 4);
    }
    float ao = 1 - low.a * (0.2 + 0.8 * darken);
    x = x * ambientLevel * ao + albedo * light + spec + add;
    return float4(srgbEnc(softClip(max(x, 0))), s.a);
}
)HLSL";

static const char* kGlass = R"HLSL(
cbuffer GL : register(b1) {
    float4 g0;     // centre x, y (aspect space), half width, half length
    float4 g1;     // angle, kind, variant, glass height
    float4 g2;     // ior, dispersion, curvature, roughness
    float4 g3;     // reflection, caustics, bevel, tint amount
    float4 g4;     // tint rgb, seed
    float4 LP[8];  // lights: x, y, z, kind
    float4 LC[8];  // colour * intensity, range
    float nL; float relief; float aspect; float _g;
};
//LIGHTPOS
static const float WL[7] = { 0.42, 0.46, 0.50, 0.54, 0.58, 0.62, 0.66 };
static const float3 WC[7] = { float3(0.30, 0.00, 0.95) / float3(3.05, 2.85, 2.6), float3(0.05, 0.10, 1.00) / float3(3.05, 2.85, 2.6),
                              float3(0.00, 0.60, 0.60) / float3(3.05, 2.85, 2.6), float3(0.10, 1.00, 0.05) / float3(3.05, 2.85, 2.6),
                              float3(0.80, 0.85, 0.00) / float3(3.05, 2.85, 2.6), float3(1.00, 0.30, 0.00) / float3(3.05, 2.85, 2.6),
                              float3(0.80, 0.00, 0.00) / float3(3.05, 2.85, 2.6) };
float iorAt(float lambda) { return g2.x + g2.y * 0.02 * (1 / (lambda * lambda) - 1 / 0.3025); }

// Footprint (0..1), surface height above the glass plane and its gradient, all in aspect-space units.
void profile(float2 A, out float inside, out float hgt, out float2 grad, out float edge) {
    float2 q = rot2(A - g0.xy, -g1.x);
    float s = g0.z, L = g0.w;
    int kind = (int)(g1.y + 0.5), variant = (int)(g1.z + 0.5);
    float c = g2.z, aa = 1.5 * uInv.y;
    float2 gq = 0;
    hgt = 0; inside = 0; edge = 0;
    float2 outward = 0;
    float bevelDist = 1;
    if (kind == 2 || (kind == 1 && variant < 2)) {
        float2 e = q / float2(kind == 2 ? s : L, s);
        float rr = length(e);
        inside = 1 - smoothstep(1 - aa / s, 1, rr);
        edge = rr;
        outward = rr > 1e-4 ? e / rr : 0;
        bevelDist = (1 - rr) * s;
        if (kind == 2) {
            float z = sqrt(saturate(1 - rr * rr));
            hgt = z * s;
            gq = -e / max(z, 0.08);
        } else {
            float k = c * 0.7 * s;
            float2 de = 2 * e / float2(L, s);
            if (variant == 0) { hgt = k * (1 - rr * rr); gq = -k * de; }
            else { hgt = k * rr * rr; gq = k * de; }
        }
    } else {
        float2 e = abs(q) / float2(L, s);
        inside = (1 - smoothstep(1 - aa / L, 1, e.x)) * (1 - smoothstep(1 - aa / s, 1, e.y));
        edge = max(e.x, e.y);
        bevelDist = min(L - abs(q.x), s - abs(q.y));
        outward = (L - abs(q.x)) < (s - abs(q.y)) ? float2(sign(q.x), 0) : float2(0, sign(q.y));
        if (kind == 0) {                       // prism: two faces meeting in a ridge along its length
            float k = c * 1.2 * s;
            hgt = k * (1 - e.y);
            gq = float2(0, -k * sign(q.y) / s);
        } else if (kind == 1) {                // cylindrical lens
            float k = c * 0.7 * s, yy = q.y / s;
            hgt = k * (1 - yy * yy);
            gq = float2(0, -2 * k * yy / s);
        } else if (kind == 3) {                // glass panel
            if (variant == 1) {                // ribbed / fluted
                float f = 3.14159 * 9 / s;
                float a = c * 0.6 / f;
                hgt = a * (1 + cos(q.x * f));
                gq = float2(-a * f * sin(q.x * f), 0);
            } else if (variant == 2) {         // hammered
                float f = 7 / s, e2 = 0.02 / f;
                float2 p = q * f + g4.w;
                float h0 = vnoise(p) + 0.5 * vnoise(p * 2.1 + 3.7);
                float hx = vnoise(p + float2(e2 * f, 0)) + 0.5 * vnoise((p + float2(e2 * f, 0)) * 2.1 + 3.7);
                float hy = vnoise(p + float2(0, e2 * f)) + 0.5 * vnoise((p + float2(0, e2 * f)) * 2.1 + 3.7);
                float a = c * 0.25 / f;
                hgt = a * h0;
                gq = a * float2(hx - h0, hy - h0) / e2;
            }
        } else {                               // water drops on a sheet
            float cell = s * 0.24;
            float2 id = floor(q / cell);
            float2 f = q / cell - id - 0.5;
            float2 rnd = hash22(id + g4.w);
            float rad = 0.16 + rnd.x * 0.3;
            float2 dq = f - (rnd - 0.5) * 0.3;
            float rr = length(dq) / rad;
            float present = rnd.y > 0.22 ? 1 : 0;
            float z = sqrt(saturate(1 - rr * rr));
            float k = c * 1.6;
            hgt = z * rad * cell * k * present;
            gq = -dq / max(z, 0.12) / rad * k * present * (rr < 1 ? 1 : 0);
            inside *= present * (1 - smoothstep(0.9, 1, rr));
            edge = rr;
        }
    }
    // bevelled outline catches light and bends the view at the edges
    float bw = g3.z * 0.12 * s + 1e-5;
    if (kind != 4 && bevelDist < bw) gq -= outward * (1 - bevelDist / bw) * 1.4;
    grad = rot2(gq, g1.x);
}

float3 sampleScene(float2 uv, float rough) {
    float3 a = srgbDec(t0.SampleLevel(sMirror, uv, 0).rgb), b = srgbDec(t2.SampleLevel(sMirror, uv, 0).rgb);
    return lerp(a, b, rough);
}

// Where a view ray leaves the glass and lands on the scene, for refractive index n.
float2 landing(float2 A, float3 N, float hgt, float n, float hb) {
    int kind = (int)(g1.y + 0.5);
    float hg = g1.w;
    if (kind == 2) {
        // full glass ball: refract in, cross the sphere, refract out (it flips the view like a real ball lens)
        float s = g0.z;
        float2 u = rot2(A - g0.xy, -g1.x) / s;
        float3 E = float3(u, sqrt(saturate(1 - dot(u, u))));
        float3 T1 = refract(float3(0, 0, -1), E, 1 / n);
        float t = -2 * dot(E, T1);
        float3 X = E + t * T1;
        float3 T2 = refract(T1, -X, n);
        if (dot(T2, T2) < 1e-4) T2 = reflect(T1, -X);
        float down = max(-T2.z, 0.08);
        float2 l = X.xy + T2.xy / down * (X.z + max(hg - hb, 0.004) / s);
        return g0.xy + rot2(l * s, g1.x);
    }
    float3 T1 = refract(float3(0, 0, -1), N, 1 / n);
    if (dot(T1, T1) < 1e-4) T1 = reflect(float3(0, 0, -1), N);
    float3 T2 = refract(T1, float3(0, 0, 1), n);
    if (dot(T2, T2) < 1e-4) T2 = T1;
    float inGlass = hgt + 0.01 * g0.z;
    return A + T1.xy / max(-T1.z, 0.08) * inGlass + T2.xy / max(-T2.z, 0.08) * max(hg - hb, 0.004);
}

// Light from each source bent by the glass: bright caustics where it focuses, a soft shadow where it spreads.
// Where light from L3 that passes the glass plane at G lands on a receiver at height hb.
float2 lightLanding(float2 G, float3 L3, float hb, float n, out float inside) {
    float hgt, edge; float2 grad;
    profile(G, inside, hgt, grad, edge);
    float hg = g1.w;
    float3 N = normalize(float3(-grad, 1));
    float3 Din = normalize(float3(G - L3.xy, hg - L3.z));
    float3 T = refract(Din, N, 1 / n);
    T = dot(T, T) < 1e-4 ? Din : refract(T, float3(0, 0, 1), n);
    if (dot(T, T) < 1e-4) T = Din;
    return G + T.xy / max(-T.z, 0.08) * max(hg - hb, 0.004);
}
float3 caustics(float2 A, float hb, float nearness) {
    float3 acc = 0;
    float hg = g1.w;
    float3 tint = lerp(1, srgbDec(g4.rgb), g3.w);
    float F0 = pow((g2.x - 1) / (g2.x + 1), 2);
    // far surfaces (sky, distant hills) do not catch a visible caustic
    float recv = smoothstep(0.05, 0.3, nearness);
    if (recv <= 0) return 0;
    float e = 1.5 * uInv.y;
    static const float wl3[3] = { 0.61, 0.55, 0.46 };
    [loop] for (int l = 0; l < (int)nL; l++) {
        if (LP[l].w > 1.5) continue;
        float3 L3 = lightPos(l);
        if (L3.z <= hg + 0.01) continue;     // beams and lights below the glass do not cast caustics
        float k = (L3.z - hg) / max(L3.z - hb, 1e-3);
        float2 G = L3.xy + (A - L3.xy) * k;
        float3 fac;
        float inside = 0;
        [unroll] for (int ch = 0; ch < 3; ch++) {
            float n = iorAt(wl3[ch]);
            float i0, i1, i2;
            float2 l0 = lightLanding(G, L3, hb, n, i0);
            float2 l1 = lightLanding(G + float2(e * k, 0), L3, hb, n, i1);
            float2 l2 = lightLanding(G + float2(0, e * k), L3, hb, n, i2);
            float2 d1 = l1 - l0, d2 = l2 - l0;
            float ratio = (e * e) / max(abs(d1.x * d2.y - d1.y * d2.x), 1e-14);
            fac[ch] = clamp(ratio, 0, 4);
            inside = max(inside, i0);
        }
        fac = lerp(fac, 1, saturate(g2.w * 1.5));
        float3 dl = L3 - float3(A, hb);
        float att = 1 / (1 + dot(dl, dl) / (LC[l].w * LC[l].w) * 3);
        acc += LC[l].rgb * att * (fac * tint * (1 - F0) - 1) * inside * g3.y * 0.5 * recv;
    }
    return acc;
}

float4 main(VSO i) : SV_Target {
    float4 base = t0.SampleLevel(sPt, i.uv, 0);
    float3 x = srgbDec(base.rgb);
    float2 A = float2(i.uv.x * aspect, i.uv.y);
    float hb = depthAt(i.uv) * relief;
    float hg = g1.w;
    float3 outc = x;
    if (g3.y > 0 && (int)(g1.y + 0.5) != 4) outc = max(outc + caustics(A, hb, depthAt(i.uv)) * outc, 0);
    float inside, hgt, edge; float2 grad;
    profile(A, inside, hgt, grad, edge);
    [branch] if (inside > 0.001) {
        float occ = smoothstep(hg - 0.005, hg + 0.02, hb);          // the scene is in front of the glass here
        float3 N = normalize(float3(-grad, 1));
        float rough = saturate(g2.w + ((int)(g1.y + 0.5) == 3 && (int)(g1.z + 0.5) == 3 ? 0.85 : 0));
        float3 trans = 0;
        if (g2.y < 0.01) {
            float2 l = landing(A, N, hgt, g2.x, hb);
            trans = sampleScene(float2(l.x / aspect, l.y), rough);
        } else {
            [unroll] for (int k = 0; k < 7; k++) {
                float2 l = landing(A, N, hgt, iorAt(WL[k]), hb);
                trans += sampleScene(float2(l.x / aspect, l.y), rough) * WC[k];
            }
        }
        float cosi = saturate(N.z);
        float F0 = pow((g2.x - 1) / (g2.x + 1), 2);
        float F = F0 + (1 - F0) * pow(1 - cosi, 5);
        float3 R = reflect(float3(0, 0, -1), N);
        float3 env = srgbDec(t2.SampleLevel(sMirror, i.uv - R.xy * float2(0.35 / aspect, 0.35), 0).rgb);
        float3 spec = 0;
        [loop] for (int l = 0; l < (int)nL; l++) {
            if (LP[l].w > 1.5) continue;   // beams have no glint of their own
            float3 L3 = lightPos(l);
            float3 Lv = normalize(L3 - float3(A, hg + hgt));
            float3 H = normalize(Lv + float3(0, 0, 1));
            float att = 1 / (1 + dot(L3.xy - A, L3.xy - A) / (LC[l].w * LC[l].w) * 2);
            spec += LC[l].rgb * pow(saturate(dot(N, H)), lerp(500, 25, rough)) * (1.6 - rough) * att;
        }
        float3 tint = lerp(1, srgbDec(g4.rgb), g3.w);
        float3 glass = trans * tint * (1 - F * g3.x) + env * F * g3.x * 1.4 + spec * 0.9;
        glass *= 1 - 0.18 * smoothstep(0.86, 1.0, edge);
        outc = lerp(outc, glass, inside * (1 - occ));
    }
    return float4(srgbEnc(softClip(max(outc, 0))), base.a);
}
)HLSL";

// =====================================================================  C++ side
struct RefineCB { float crop[4]; float stepPx, hasEdit, pad[2]; };
struct FogDofCB { float fog, fogStart, dof, focus, range, fogCol[3]; };
struct LightsCB {
    float LP[8][4], LC[8][4], LS[8][4], LD[8][4];
    float PR[4][4], PR2[4][4];
    float nL, nP, relief, aspect;
    float ambientLevel, micro, mode, darken;
    float dstep[2], pad[2];
};
struct GlassCB {
    float g0[4], g1[4], g2[4], g3[4], g4[4];
    float LP[8][4], LC[8][4];
    float nL, relief, aspect, pad;
};

bool Pipeline::initOptics() {
    std::string common = kOpticsCommon;
    psDepthRefine = g_gpu.compilePS("depthrefine", kDepthRefine);
    psDabAdd = g_gpu.compilePS("dabadd", kDabAdd);
    psDepthView = g_gpu.compilePS("depthview", kDepthView);
    psDepthMax = g_gpu.compilePS("depthmax", kDepthMax);
    psFogDof = g_gpu.compilePS("fogdof", kFogDof);
    std::string lights = common + kLightsCB + kLightPos;
    psLightsLow = g_gpu.compilePS("lightslow", lights + kLightsLow);
    psLightsFull = g_gpu.compilePS("lightsfull", lights + kLightsFull);
    std::string glass = common + kGlass;
    glass.replace(glass.find("//LIGHTPOS"), 10, kLightPos);
    psGlass = g_gpu.compilePS("glass", glass);
    float half = 0.35f;   // depth used when no map has been built yet: a flat scene
    flatDepth = g_gpu.create(1, 1, DXGI_FORMAT_R32_FLOAT, false, &half, 4);
    return psDepthRefine && psDabAdd && psDepthView && psFogDof && psDepthMax && psLightsLow && psLightsFull && psGlass && flatDepth;
}

void Pipeline::shutdownOptics() {
    if (depthTex) { depthTex->release(); delete depthTex; depthTex = nullptr; }
    if (flatDepth) { flatDepth->release(); delete flatDepth; flatDepth = nullptr; }
    if (depthEdit.tex) { depthEdit.tex->release(); delete depthEdit.tex; depthEdit = BrushCache(); }
}

void Pipeline::setDepth(const std::vector<float>& d, int w, int h) {
    if (depthTex) { depthTex->release(); delete depthTex; depthTex = nullptr; }
    if (!d.empty()) depthTex = g_gpu.create(w, h, DXGI_FORMAT_R32_FLOAT, false, d.data(), w * 4);
    depthW_ = w; depthH_ = h;
}

void Pipeline::clearDepth() {
    if (depthTex) { depthTex->release(); delete depthTex; depthTex = nullptr; }
    if (depthEdit.tex) { depthEdit.tex->release(); delete depthEdit.tex; depthEdit = BrushCache(); }
}

bool Pipeline::needsDepth(const Params& p) const {
    if (p.showDepth || p.fogAmount > 0 || p.dofAmount > 0) return true;
    for (auto& o : p.optics) if (o.enabled) return true;
    for (auto& m : p.masks) if (m.enabled && m.range == MR_DEPTH) return true;
    for (auto& e : p.effects) if (e.enabled && e.target == TG_DEPTH) return true;
    return false;
}

bool Pipeline::opticsActive(const Params& p) const {
    if (p.fogAmount > 0 || p.dofAmount > 0 || p.showDepth) return true;
    for (auto& o : p.optics) if (o.enabled) return true;
    return false;
}

// Hand corrections of the depth map, rendered in canvas space like a brush mask.
Tex* Pipeline::depthEditTexture(const Params& p, const Geo& g) {
    if (p.depthDabs.empty()) return nullptr;
    int lw = 1024, bw, bh;
    if (g.canvasW >= g.canvasH) { bw = std::min(lw, g.canvasW); bh = std::max(1, (int)((float)bw * g.canvasH / g.canvasW)); }
    else { bh = std::min(lw, g.canvasH); bw = std::max(1, (int)((float)bh * g.canvasW / g.canvasH)); }
    BrushCache& bc = depthEdit;
    bool rebuild = !bc.tex || bc.w != bw || bc.h != bh || bc.rev != p.depthRev || p.depthDabs.size() < bc.count;
    if (rebuild) {
        if (bc.tex && (bc.w != bw || bc.h != bh)) { bc.tex->release(); delete bc.tex; bc.tex = nullptr; }
        if (!bc.tex) bc.tex = g_gpu.create(bw, bh, DXGI_FORMAT_R16_FLOAT, true);
        if (!bc.tex) return nullptr;
        bc.w = bw; bc.h = bh; bc.rev = p.depthRev; bc.count = 0;
        g_gpu.clear(bc.tex, 0, 0, 0, 0);
    }
    if (p.depthDabs.size() > bc.count) drawDabs(bc.tex, p.depthDabs, bc.count, (float)g.canvasW / g.canvasH, psDabAdd, g_gpu.blendAdd);
    bc.count = p.depthDabs.size();
    return bc.tex;
}

Tex* Pipeline::refineDepth(const Params& p, const Geo& g, Tex* aligned, Tex* guide, int W, int H) {
    RefineCB rc = {};
    rc.crop[0] = g.cx0; rc.crop[1] = g.cy0; rc.crop[2] = g.cw; rc.crop[3] = g.ch;
    // sample spacing close to one depth-map texel at the working resolution
    float texel = depthTex ? (float)W / std::max(1, (int)(depthW_ * g.cw)) : 1.0f;
    rc.stepPx = std::clamp(texel * 1.1f, 1.0f, 14.0f);
    Tex* edit = depthEditTexture(p, g);
    rc.hasEdit = edit ? 1.f : 0.f;
    Tex* out = g_gpu.acquire(W, H, DXGI_FORMAT_R16_FLOAT);
    g_gpu.pass(psDepthRefine, out, { aligned->srv, guide->srv, edit ? edit->srv : nullptr }, &rc, sizeof(rc));
    return out;
}

Tex* Pipeline::applyOptics(const Params& p, const Geo& g, Tex* cur, Tex* depth, int W, int H) {
    const DXGI_FORMAT F = DXGI_FORMAT_R16G16B16A16_FLOAT;
    float aspect = (float)W / H;
    auto ax = [&](float x) { return (x - g.cx0) / g.cw * aspect; };
    auto ay = [&](float y) { return (y - g.cy0) / g.ch; };
    float relief = p.relief / 100.0f * 0.7f;
    Tex* c = cur;
    bool owned = false;
    auto replace = [&](Tex* nx) { if (owned) g_gpu.release(c); c = nx; owned = true; };

    // light sources (and prisms that beams can pass through)
    LightsCB lc = {};
    int nl = 0, np = 0;
    float ambient = 0;
    for (auto& o : p.optics) {
        if (!o.enabled) continue;
        if (o.kind == OP_PRISM && np < 4) {
            float s = o.size / g.ch;
            lc.PR[np][0] = ax(o.x); lc.PR[np][1] = ay(o.y); lc.PR[np][2] = s * o.stretch; lc.PR[np][3] = s;
            lc.PR2[np][0] = o.angle * 3.14159265f / 180; lc.PR2[np][1] = o.ior; lc.PR2[np][2] = o.dispersion / 100.0f;
            np++;
        }
        if (!OpticIsLight(o.kind) || nl >= 8) continue;
        float k = o.intensity / 100.0f * 3.0f;
        int kind = o.kind == OP_LIGHT ? 0 : o.kind == OP_SPOT ? 1 : 2;
        lc.LP[nl][0] = ax(o.x); lc.LP[nl][1] = ay(o.y); lc.LP[nl][2] = 0.03f + o.height / 100.0f * 0.6f; lc.LP[nl][3] = (float)kind;
        lc.LC[nl][0] = o.color[0] * k; lc.LC[nl][1] = o.color[1] * k; lc.LC[nl][2] = o.color[2] * k; lc.LC[nl][3] = 0.05f + o.range / 100.0f * 1.2f;
        lc.LS[nl][0] = o.softness / 100.0f; lc.LS[nl][1] = o.glow / 100.0f; lc.LS[nl][2] = o.rays / 100.0f; lc.LS[nl][3] = o.gloss / 100.0f;
        float a = o.dir * 3.14159265f / 180;
        lc.LD[nl][0] = std::cos(a); lc.LD[nl][1] = std::sin(a);
        lc.LD[nl][2] = kind == 2 ? 0.002f + o.width / 100.0f * 0.04f : std::cos(o.cone * 3.14159265f / 180);
        lc.LD[nl][3] = o.softness / 100.0f * 0.2f;
        if (kind != 2) ambient = std::max(ambient, o.ambient / 100.0f);
        nl++;
    }
    if (nl > 0) {
        lc.nL = (float)nl; lc.nP = (float)np; lc.relief = relief; lc.aspect = aspect;
        lc.ambientLevel = 1 - ambient * 0.9f;
        lc.darken = ambient;
        lc.micro = p.microRelief / 100.0f;
        // normals from the depth map are taken over about one of its texels, so its grid never shows in the shading
        float dl = depthTex ? 1.5f * std::max(g.canvasW, g.canvasH) / std::max(1, std::max(depthW_, depthH_)) : 2.0f;
        lc.dstep[0] = std::max(1.5f / W, dl / (g.cw * g.canvasW));
        lc.dstep[1] = std::max(1.5f / H, dl / (g.ch * g.canvasH));
        // shadows, glow, shafts and occlusion are smooth: a reduced resolution is enough and keeps big exports fast
        float k = std::min(1.0f, 1280.0f / std::max(W, H));
        int lw = std::max(1, (int)(W * k)), lh = std::max(1, (int)(H * k));
        // max-depth pyramid at 1 to 16 low-resolution pixels for the shadow rays
        const DXGI_FORMAT R = DXGI_FORMAT_R16_FLOAT;
        Tex* lv[5] = {};
        lv[0] = g_gpu.acquire(lw, lh, R);
        g_gpu.pass(psDepthMax, lv[0], { depth->srv });
        for (int i = 1; i < 5; i++) {
            lv[i] = g_gpu.acquire(std::max(1, (lv[i - 1]->w + 1) / 2), std::max(1, (lv[i - 1]->h + 1) / 2), R);
            g_gpu.pass(psDepthMax, lv[i], { lv[i - 1]->srv });
        }
        lc.mode = 0;
        Tex* visA = g_gpu.acquire(lw, lh, F);
        g_gpu.pass(psLightsLow, visA, { nullptr, lv[0]->srv, lv[1]->srv, lv[2]->srv, lv[3]->srv, lv[4]->srv }, &lc, sizeof(lc));
        Tex* visB = nullptr;
        if (nl > 4) {
            lc.mode = 1;
            visB = g_gpu.acquire(lw, lh, F);
            g_gpu.pass(psLightsLow, visB, { nullptr, lv[0]->srv, lv[1]->srv, lv[2]->srv, lv[3]->srv, lv[4]->srv }, &lc, sizeof(lc));
        }
        for (Tex*& t : lv) g_gpu.release(t);
        lc.mode = 2;
        Tex* low = g_gpu.acquire(lw, lh, F);
        g_gpu.pass(psLightsLow, low, { c->srv, depth->srv }, &lc, sizeof(lc));
        float sb = std::max(1.0f, 0.015f * std::max(W, H));
        Tex* bl = blur(c, sb, sb, F);
        Tex* nx = g_gpu.acquire(W, H, F);
        g_gpu.pass(psLightsFull, nx, { c->srv, depth->srv, visA->srv, (visB ? visB : visA)->srv, low->srv, bl->srv }, &lc, sizeof(lc));
        g_gpu.release(visA);
        if (visB) g_gpu.release(visB);
        g_gpu.release(low);
        g_gpu.release(bl);
        replace(nx);
    }

    // atmosphere: depth of field and fog
    if (p.fogAmount > 0 || p.dofAmount > 0) {
        float sc = g_gpu.common.scale, lf = (float)std::max(g.fullW, g.fullH);
        float s1 = std::max(0.6f, (0.002f + 0.006f * p.dofAmount / 100.0f) * lf * sc);
        float s2 = std::max(0.8f, (0.004f + 0.022f * p.dofAmount / 100.0f) * lf * sc);
        Tex* b1 = p.dofAmount > 0 ? blur(c, s1, s1, F) : nullptr;
        Tex* b2 = p.dofAmount > 0 ? blur(c, s2, s2, F) : nullptr;
        FogDofCB fc = { p.fogAmount / 100.0f, p.fogStart / 100.0f, p.dofAmount / 100.0f, p.dofFocus / 100.0f, p.dofRange / 100.0f * 0.5f,
                        { p.fogColor[0], p.fogColor[1], p.fogColor[2] } };
        Tex* nx = g_gpu.acquire(W, H, F);
        g_gpu.pass(psFogDof, nx, { c->srv, depth->srv, b1 ? b1->srv : c->srv, b2 ? b2->srv : c->srv }, &fc, sizeof(fc));
        if (b1) g_gpu.release(b1);
        if (b2) g_gpu.release(b2);
        replace(nx);
    }

    // glass objects, in list order (later ones see the earlier ones through them)
    for (auto& o : p.optics) {
        if (!o.enabled || OpticIsLight(o.kind)) continue;
        GlassCB gc = {};
        float s = o.size / g.ch;
        gc.g0[0] = ax(o.x); gc.g0[1] = ay(o.y); gc.g0[2] = s;
        gc.g0[3] = s * ((o.kind == OP_SPHERE || (o.kind == OP_LENS && o.variant < 2)) && o.kind != OP_LENS ? 1.0f : o.stretch);
        if (o.kind == OP_LENS && o.variant < 2) gc.g0[3] = s * std::max(0.3f, o.stretch);
        gc.g1[0] = o.angle * 3.14159265f / 180; gc.g1[1] = (float)o.kind; gc.g1[2] = (float)o.variant;
        gc.g1[3] = o.height / 100.0f * 0.6f;
        gc.g2[0] = o.ior; gc.g2[1] = o.dispersion / 100.0f; gc.g2[2] = o.curvature / 100.0f; gc.g2[3] = o.roughness / 100.0f;
        gc.g3[0] = o.reflection / 100.0f; gc.g3[1] = o.caustics / 100.0f; gc.g3[2] = o.bevel / 100.0f; gc.g3[3] = o.tintAmt / 100.0f;
        gc.g4[0] = o.tint[0]; gc.g4[1] = o.tint[1]; gc.g4[2] = o.tint[2]; gc.g4[3] = (float)(o.id % 97);
        for (int l = 0; l < nl; l++) { memcpy(gc.LP[l], lc.LP[l], 16); memcpy(gc.LC[l], lc.LC[l], 16); }
        gc.nL = (float)nl; gc.relief = relief; gc.aspect = aspect;
        float sb = std::max(1.0f, 0.012f * H);
        Tex* bl = blur(c, sb, sb, F);
        Tex* nx = g_gpu.acquire(W, H, F);
        g_gpu.pass(psGlass, nx, { c->srv, depth->srv, bl->srv }, &gc, sizeof(gc));
        g_gpu.release(bl);
        replace(nx);
    }
    return c;
}
