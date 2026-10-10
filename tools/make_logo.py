"""Generate the OpenAEDTrainer logo SVGs into the given folder.

Needs fontTools and the macOS Arial fonts. PNG exports use rsvg-convert
(brew install librsvg):

    python tools/make_logo.py docs
    rsvg-convert -w 512 docs/logo.svg -o docs/logo-512.png
    rsvg-convert -w 1280 docs/social-preview.svg -o docs/social-preview.png
"""
import sys
from fontTools.ttLib import TTFont
from fontTools.pens.svgPathPen import SVGPathPen
from fontTools.pens.transformPen import TransformPen

BLUE, RED, INK = "#2E5C9F", "#D92027", "#1F2A37"
FONTS = "/System/Library/Fonts/Supplemental/"

def text_path(font_file, text, x, baseline, cap_px):
    f = TTFont(FONTS + font_file); gs = f.getGlyphSet(); cmap = f.getBestCmap(); hm = f["hmtx"]
    cap = getattr(f["OS/2"], "sCapHeight", 0) or 0.716 * f["head"].unitsPerEm
    sc = cap_px / cap; pen = SVGPathPen(gs); adv = 0
    for ch in text:
        n = cmap[ord(ch)]
        gs[n].draw(TransformPen(pen, (sc, 0, 0, -sc, x + adv * sc, baseline)))
        adv += hm[n][0]
    return pen.getCommands(), adv * sc

# Symbol in a 512 box: rounded square, white heart, red bolt
HEART = ("M256 418 C 132 338 78 262 104 190 C 130 118 214 108 256 168 "
         "C 298 108 382 118 408 190 C 434 262 380 338 256 418 Z")
BOLT = "M282 158 L196 290 L248 290 L224 380 L318 238 L264 238 L296 158 Z"
def symbol(tx=0, ty=0, s=1.0):
    return (f'<g transform="translate({tx} {ty}) scale({s})">'
            f'<rect width="512" height="512" rx="112" fill="{BLUE}"/>'
            f'<path d="{HEART}" fill="#FFFFFF"/>'
            f'<path d="{BOLT}" fill="{RED}" stroke="{RED}" stroke-width="6" stroke-linejoin="round"/>'
            f'</g>')

out = sys.argv[1]
# 1) square icon
open(f"{out}/logo.svg", "w").write(
    '<svg xmlns="http://www.w3.org/2000/svg" width="512" height="512" viewBox="0 0 512 512">'
    + symbol() + "</svg>\n")

# 2) horizontal wordmark: symbol + "OpenAED" (blue) "Trainer" (ink)
H = 160; cap = 64; base = H / 2 + cap / 2
x0 = H + 36
d1, w1 = text_path("Arial Black.ttf", "OpenAED", x0, base, cap)
d2, w2 = text_path("Arial Bold.ttf", "Trainer", x0 + w1 + 6, base, cap)
W = int(x0 + w1 + 6 + w2 + 24)
open(f"{out}/logo-wordmark.svg", "w").write(
    f'<svg xmlns="http://www.w3.org/2000/svg" width="{W}" height="{H}" viewBox="0 0 {W} {H}">'
    + symbol(0, 0, H / 512)
    + f'<path fill="{BLUE}" d="{d1}"/><path fill="{INK}" d="{d2}"/></svg>\n')
open(f"{out}/logo-wordmark-dark.svg", "w").write(
    f'<svg xmlns="http://www.w3.org/2000/svg" width="{W}" height="{H}" viewBox="0 0 {W} {H}">'
    + symbol(0, 0, H / 512)
    + f'<path fill="#79A6EA" d="{d1}"/><path fill="#F0F3F6" d="{d2}"/></svg>\n')
# 3) GitHub social preview card, 1280x640: wordmark + tagline on white
CW, CH = 1280, 640
cap2, sym = 64, 164
d1b, w1b = text_path("Arial Black.ttf", "OpenAED", 0, 0, cap2)
d2b, w2b = text_path("Arial Bold.ttf", "Trainer", 0, 0, cap2)
gap = 40
total = sym + gap + w1b + 8 + w2b
x = (CW - total) / 2
yc = 270
tag = "Open source AED trainer board"
dt, wt = text_path("Arial Bold.ttf", tag, 0, 0, 34)
sub = "ESP32 Audio Kit  ·  voice prompts  ·  CPR metronome  ·  web UI"
ds, ws = text_path("Arial.ttf", sub, 0, 0, 24)
open(f"{out}/social-preview.svg", "w").write(
    f'<svg xmlns="http://www.w3.org/2000/svg" width="{CW}" height="{CH}" viewBox="0 0 {CW} {CH}">'
    f'<rect width="{CW}" height="{CH}" fill="#FFFFFF"/>'
    f'<rect y="{CH - 16}" width="{CW}" height="16" fill="{BLUE}"/>'
    + symbol(x, yc - sym / 2, sym / 512)
    + f'<g transform="translate({x + sym + gap} {yc + cap2 / 2})"><path fill="{BLUE}" d="{d1b}"/></g>'
    + f'<g transform="translate({x + sym + gap + w1b + 8} {yc + cap2 / 2})"><path fill="{INK}" d="{d2b}"/></g>'
    + f'<g transform="translate({(CW - wt) / 2} 470)"><path fill="{INK}" d="{dt}"/></g>'
    + f'<g transform="translate({(CW - ws) / 2} 530)"><path fill="#5B6573" d="{ds}"/></g>'
    + "</svg>\n")
print("wordmark width", W)
