"""Procedural landscape used for README screenshots (no third-party photos in the repo).
Usage: python make_demo_image.py out.png [seed]"""
import math, struct, sys, zlib, random

W, H = 1800, 1100
random.seed(int(sys.argv[2]) if len(sys.argv) > 2 else 7)

def lerp(a, b, t): return a + (b - a) * t
def mix(c1, c2, t): return tuple(lerp(a, b, t) for a, b in zip(c1, c2))
def smooth(t): return t * t * (3 - 2 * t)

# 1D value noise for ridge lines
rnd = [random.random() for _ in range(4096)]
def noise1(x):
    i = int(math.floor(x)); f = x - i
    a, b = rnd[i % 4096], rnd[(i + 1) % 4096]
    return lerp(a, b, smooth(f))
def fbm1(x, oct=5):
    s, a, f = 0.0, 0.5, 1.0
    for _ in range(oct):
        s += a * noise1(x * f); f *= 2.03; a *= 0.5
    return s

horizon = int(H * 0.62)
layers = [  # (base height, amplitude, frequency, colour near, colour far haze)
    (0.40, 0.16, 0.004, (64, 72, 104), 0.55),
    (0.47, 0.12, 0.007, (44, 50, 78), 0.35),
    (0.54, 0.09, 0.012, (26, 30, 48), 0.15),
]
ridges = []
for li, (base, amp, freq, col, haze) in enumerate(layers):
    off = random.random() * 1000
    ridges.append([H * (base + amp * (fbm1(x * freq + off) - 0.5) * 2) for x in range(W)])

sun = (W * (0.25 + random.random() * 0.5), H * (0.30 + random.random() * 0.15))
rows = []
for y in range(H):
    row = bytearray()
    for x in range(W):
        if y < horizon:
            t = y / horizon
            sky = mix((24, 38, 92), (250, 160, 110), smooth(t) ** 1.4)
            d = math.hypot(x - sun[0], y - sun[1])
            glow = math.exp(-(d / 260) ** 2) * 0.85 + math.exp(-(d / 40) ** 2)
            c = [min(255, sky[k] + glow * (255, 210, 150)[k]) for k in range(3)]
            for li, (base, amp, freq, col, haze) in enumerate(layers):
                if y >= ridges[li][x]:
                    hz = haze * (1 - (y - ridges[li][x]) / (H * 0.4))
                    c = list(mix(col, (240, 150, 120), max(0.0, hz)))
            # thin clouds
            cl = fbm1(x * 0.004 + y * 0.02) * fbm1(y * 0.05 + 3.3)
            if y < horizon * 0.55 and cl > 0.22:
                c = list(mix(c, (255, 200, 180), min(0.6, (cl - 0.22) * 3)))
        else:
            # lake: mirrored sky with ripples
            yy = horizon - (y - horizon) * 0.9
            ripple = math.sin(y * 0.35 + math.sin(x * 0.01) * 2) * (y - horizon) * 0.02
            sx = int(min(W - 1, max(0, x + ripple)))
            t = max(0.0, yy / horizon)
            c = list(mix((24, 38, 92), (250, 160, 110), smooth(min(1, t)) ** 1.4))
            for li, (base, amp, freq, col, haze) in enumerate(layers):
                if yy >= ridges[li][sx]:
                    c = list(col)
            d = math.hypot(x - sun[0], yy - sun[1])
            c = [min(255, c[k] + (math.exp(-(d / 260) ** 2) * 0.6) * (255, 210, 150)[k]) for k in range(3)]
            dark = 0.55 + 0.25 * (1 - (y - horizon) / (H - horizon))
            c = [v * dark for v in c]
        n = (random.random() - 0.5) * 6
        row += bytes(int(max(0, min(255, v + n))) for v in c)
    rows.append(b'\x00' + bytes(row))

def chunk(tag, data):
    return struct.pack('>I', len(data)) + tag + data + struct.pack('>I', zlib.crc32(tag + data) & 0xffffffff)
png = b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', struct.pack('>IIBBBBB', W, H, 8, 2, 0, 0, 0)) + \
      chunk(b'IDAT', zlib.compress(b''.join(rows), 9)) + chunk(b'IEND', b'')
open(sys.argv[1] if len(sys.argv) > 1 else 'demo.png', 'wb').write(png)
print('ok')
