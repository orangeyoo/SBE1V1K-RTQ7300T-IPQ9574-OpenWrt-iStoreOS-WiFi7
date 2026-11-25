#!/usr/bin/env python3
"""Generate self-hosted badge SVGs for the repo README pages.

Why: shields.io badges backed by the GitHub API (release / downloads)
break intermittently when shields gets rate-limited, and GitHub camo caches
the error image. Self-hosted static SVGs have no external dependency and we
control CJK label widths (shields underestimates them).

Usage:
  python3 gen.py            # regenerate the static badges (idempotent)
  python3 gen.py --dynamic  # also refresh release-*.svg / downloads-*.svg
                            # from the live GitHub API (used by CI)

Files are written next to this script. Keep the filenames in sync with the
README.md / README-EN.md <img src> references.
"""
import json
import math
import os
import sys
import urllib.request

HERE = os.path.dirname(os.path.abspath(__file__))
REPO_API = "https://api.github.com/repos/orangeyoo/SBE1V1K-RTQ7300T-IPQ9574-OpenWrt-iStoreOS-WiFi7"

# ---------------------------------------------------------------- svg builder

NARROW = set(" .,:;'|!iljtfr()-")


def esc(s):
    return s.replace("&", "&amp;").replace("<", "&lt;").replace(">", "&gt;")


def text_w(s, fs, bold=False):
    """Estimated natural rendered width; CJK = em, latin per-class.

    Verdana bold is noticeably wider than regular, so the latin table is
    bold-aware (widths verified in-browser against getComputedTextLength()).
    """
    lo, up, dig, wm, sp = ((0.64, 0.74, 0.72, 0.95, 0.40) if bold
                           else (0.58, 0.68, 0.68, 0.88, 0.35))
    tot = 0.0
    for c in s:
        o = ord(c)
        if o >= 0x2E80:                      # CJK & fullwidth
            tot += fs
        elif c in "wm":
            tot += fs * wm
        elif c in "WM":
            tot += fs * (wm + 0.03)
        elif c in NARROW:
            tot += fs * 0.38
        elif c == " ":
            tot += fs * sp
        elif c.isupper():
            tot += fs * up
        elif c.isdigit():
            tot += fs * dig
        else:
            tot += fs * lo
    return tot


def badge(label, message, color, style="flat-square"):
    """shields.io-compatible flat-square / for-the-badge SVG.

    textLength is set ~12% above the natural width so renderers stretch
    spacing slightly instead of ever clipping (CJK labels included).
    """
    if style == "for-the-badge":
        fs, h, pad, y = 14, 28, 10, 192
        font = ' font-weight="bold"'
    else:
        fs, h, pad, y = 11, 20, 5, 150
        font = ""
    lt = math.ceil(text_w(label, fs, bold=(style == "for-the-badge")) * 1.18)
    mt = math.ceil(text_w(message, fs, bold=(style == "for-the-badge")) * 1.18)
    lw, mw = lt + 2 * pad, mt + 2 * pad
    total = lw + mw
    svg = (
        f'<svg xmlns="http://www.w3.org/2000/svg" width="{total}" height="{h}" '
        f'role="img" aria-label="{esc(label)}: {esc(message)}">\n'
        f"  <title>{esc(label)}: {esc(message)}</title>\n"
        f'  <g shape-rendering="crispEdges">\n'
        f'    <rect width="{lw}" height="{h}" fill="#555"/>\n'
        f'    <rect x="{lw}" width="{mw}" height="{h}" fill="#{color}"/>\n'
        f"  </g>\n"
        f'  <g fill="#fff" text-anchor="middle" '
        f'font-family="Verdana,Geneva,DejaVu Sans,sans-serif" '
        f'text-rendering="geometricPrecision" font-size="{fs * 10}">\n'
        f'    <text transform="scale(.1)" x="{lw * 10 / 2}" y="{y}" '
        f'textLength="{lt * 10}"{font}>{esc(label)}</text>\n'
        f'    <text transform="scale(.1)" x="{(lw + mw / 2) * 10}" y="{y}" '
        f'textLength="{mt * 10}"{font}>{esc(message)}</text>\n'
        f"  </g>\n"
        f"</svg>\n"
    )
    return svg


def write(name, svg):
    path = os.path.join(HERE, name)
    old = open(path, encoding="utf-8").read() if os.path.exists(path) else None
    if old != svg:
        open(path, "w", encoding="utf-8", newline="\n").write(svg)
        print(f"wrote {name}")
    else:
        print(f"unchanged {name}")


# ---------------------------------------------------------------- static set
# (filename, label, message, color, style)
STATIC = [
    ("buy-zh.svg",        "购买刷机密钥", "立即购买",      "ff5722", "for-the-badge"),
    ("download-zh.svg",   "下载",         "一键刷机工具",  "0e70c0", "for-the-badge"),
    ("group-zh.svg",      "会员群",       "1061612207",    "fe7d37", "flat-square"),
    ("wifi7-zh.svg",      "Wi-Fi 7",      "三频 QCN9224",  "8A2BE2", "flat-square"),
    ("soc-zh.svg",        "SoC",          "Qualcomm IPQ9574", "333333", "flat-square"),
    ("qsdk-zh.svg",       "QSDK 14",      "OpenWrt 24.10", "0099FF", "flat-square"),
    ("kernel-zh.svg",     "kernel",       "6.6.116 · ath12k", "97ca00", "flat-square"),
    ("10g-zh.svg",        "10G",          "实测 7.72 Gbps", "4c1",   "flat-square"),
    ("istoreos-zh.svg",   "iStoreOS",     "iStore 商店",   "FF6600", "flat-square"),
    ("license-zh.svg",    "许可证",       "GPL-2.0-or-later", "007ec6", "flat-square"),

    ("buy-en.svg",        "Buy Key",      "Purchase Now",  "ff5722", "for-the-badge"),
    ("download-en.svg",   "Download",     "AutoFlash Tool", "0e70c0", "for-the-badge"),
    ("group-en.svg",      "QQ Group",     "1061612207",    "fe7d37", "flat-square"),
    ("wifi7-en.svg",      "Wi-Fi 7",      "QCN9224 ×3 tri-band", "8A2BE2", "flat-square"),
    ("soc-en.svg",        "SoC",          "Qualcomm IPQ9574", "333333", "flat-square"),
    ("qsdk-en.svg",       "QSDK 14",      "OpenWrt 24.10", "0099FF", "flat-square"),
    ("kernel-en.svg",     "kernel",       "6.6.116 · ath12k", "97ca00", "flat-square"),
    ("10g-en.svg",        "10G",          "measured 7.72 Gbps", "4c1", "flat-square"),
    ("istoreos-en.svg",   "iStoreOS",     "iStore app store", "FF6600", "flat-square"),
    ("license-en.svg",    "license",      "GPL-2.0-or-later", "007ec6", "flat-square"),
]

# ------------------------------------------------------------- dynamic values

LATEST_FALLBACK = "v1.1.2"
DOWNLOADS_FALLBACK = "115"


def gh(path):
    import os
    headers = {"User-Agent": "badge-gen"}
    token = os.environ.get("GITHUB_TOKEN")
    if token:
        headers["Authorization"] = f"Bearer {token}"
    req = urllib.request.Request(REPO_API + path, headers=headers)
    with urllib.request.urlopen(req, timeout=30) as r:
        return json.load(r)


def dynamic():
    try:
        rels = gh("/releases?per_page=100")
        stable = [r for r in rels if not r.get("prerelease") and not r.get("draft")]
        latest = (stable or rels)[0].get("tag_name") or LATEST_FALLBACK
        total = sum(a.get("download_count", 0) for r in rels for a in r.get("assets", []))
        downloads = str(total) if total else DOWNLOADS_FALLBACK
    except Exception as ex:
        print(f"GitHub API unavailable ({ex}); using fallback values")
        latest, downloads = LATEST_FALLBACK, DOWNLOADS_FALLBACK
    write("release-zh.svg",   badge("最新版",  latest, "007ec6"))
    write("release-en.svg",   badge("release", latest, "007ec6"))
    write("downloads-zh.svg", badge("下载量",  downloads, "007ec6"))
    write("downloads-en.svg", badge("downloads", downloads, "007ec6"))


def main():
    for name, label, msg, color, style in STATIC:
        write(name, badge(label, msg, color, style))
    if "--dynamic" in sys.argv:
        dynamic()


if __name__ == "__main__":
    main()
