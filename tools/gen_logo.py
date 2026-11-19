#!/usr/bin/env python3
"""Emit the comshu logo drafts in demos/logo as standalone SVG, plus a preview page.

comshu is "komşu", Turkish for neighbour, and every draft leans on that:

  window  the nosy neighbour: a house whose one window is an eye. A profiler
          is the neighbour who sees everything that goes on next door.
  line    two buildings with a line strung between their windows and a basket
          riding it, the Istanbul way of passing things to the flat across the
          street. Peer to peer, no middleman: what RDMA is.
  grid    the von Neumann neighbourhood: a rank and its four neighbours lit in
          a 3x3 grid. The HPC meaning of the word, and a nod to the rank-by-rank
          matrix comshu draws.

Each mark is a 64x64 box, drawn in two colours (ink + one accent) and also in
plain monochrome, which is the version that has to survive a favicon, a
terminal banner and a black-and-white paper figure.

    tools/gen_logo.py          # writes demos/logo/*.svg and demos/logo/index.html

The wordmark uses a system monospace face, so it renders slightly differently
per machine; outline it to paths once a draft is chosen.
"""

import os

OUT = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "demos", "logo")

INK = "#1d2a44"      # deep navy
ACCENT = "#d9622b"   # terracotta, the colour of the roofs it borrows from
PAPER = "#ffffff"
INK_DARK = "#e8ecf4"  # the ink on a dark ground

SW = 4  # stroke width at 64 px


def window(ink, accent):
    return f"""
  <path d="M8 29 L32 9 L56 29 V56 H8 Z" fill="none" stroke="{ink}" stroke-width="{SW}"
        stroke-linejoin="round"/>
  <path d="M17 42 Q32 28 47 42 Q32 56 17 42 Z" fill="none" stroke="{ink}" stroke-width="{SW}"
        stroke-linejoin="round"/>
  <circle cx="32" cy="42" r="5" fill="{accent}"/>"""


def line(ink, accent):
    return f"""
  <rect x="5" y="16" width="15" height="43" rx="1.5" fill="none" stroke="{ink}" stroke-width="{SW}"/>
  <rect x="44" y="7" width="15" height="52" rx="1.5" fill="none" stroke="{ink}" stroke-width="{SW}"/>
  <path d="M20 24 Q32 34 44 18" fill="none" stroke="{ink}" stroke-width="2.5" stroke-linecap="round"/>
  <path d="M31.5 27.6 V31" stroke="{ink}" stroke-width="2.5" stroke-linecap="round"/>
  <path d="M25 31 H39 L36.5 41 H27.5 Z" fill="{accent}" stroke="{accent}" stroke-width="1.5"
        stroke-linejoin="round"/>"""


def grid(ink, accent):
    cells = []
    for r in range(3):
        for c in range(3):
            x, y = 6 + c * 18, 6 + r * 18
            if (r, c) == (1, 1):
                cells.append(f'<rect x="{x}" y="{y}" width="16" height="16" rx="3" fill="{accent}"/>')
            elif r == 1 or c == 1:
                cells.append(f'<rect x="{x}" y="{y}" width="16" height="16" rx="3" fill="{ink}"/>')
            else:
                cells.append(f'<rect x="{x + 1.25}" y="{y + 1.25}" width="13.5" height="13.5" rx="2.5" '
                             f'fill="none" stroke="{ink}" stroke-width="2.5" opacity="0.45"/>')
    return "\n  " + "\n  ".join(cells)


MARKS = {"window": window, "line": line, "grid": grid}


def svg(body, w=64, h=64, bg=None):
    ground = f'\n  <rect width="{w}" height="{h}" fill="{bg}"/>' if bg else ""
    return (f'<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 {w} {h}" '
            f'width="{w}" height="{h}">{ground}{body}\n</svg>\n')


def wordmark(ink, accent, x0=0, y=40, size=40):
    # Monospace advance is ~0.6 em; the hook sits under the 's' (4th glyph), the
    # cedilla the English spelling dropped from komşu.
    adv = 0.6 * size
    sx = x0 + 3.5 * adv
    return f"""
  <text x="{x0}" y="{y}" font-family="ui-monospace, 'JetBrains Mono', Menlo, 'DejaVu Sans Mono', monospace"
        font-size="{size}" font-weight="600" fill="{ink}" letter-spacing="0">comshu</text>
  <path d="M{sx:.1f} {y + 1.5} q1.5 4.5 -3.5 7.5" fill="none" stroke="{accent}" stroke-width="3.2"
        stroke-linecap="round"/>"""


def lockup(mark, ink, accent):
    body = f'\n  <g transform="translate(0 4)">{MARKS[mark](ink, accent)}\n  </g>'
    body += wordmark(ink, accent, x0=80, y=48)
    return svg(body, w=240, h=72)


def main():
    os.makedirs(OUT, exist_ok=True)
    files = {}
    for name, draw in MARKS.items():
        files[f"{name}.svg"] = svg(draw(INK, ACCENT))
        files[f"{name}-mono.svg"] = svg(draw("#000000", "#000000"))
        files[f"{name}-dark.svg"] = svg(draw(INK_DARK, ACCENT))
        files[f"lockup-{name}.svg"] = lockup(name, INK, ACCENT)
        files[f"lockup-{name}-dark.svg"] = lockup(name, INK_DARK, ACCENT)
    files["wordmark.svg"] = svg(wordmark(INK, ACCENT, x0=2), w=150, h=56)
    for fn, text in files.items():
        with open(os.path.join(OUT, fn), "w") as f:
            f.write(text)

    rows = []
    for name in MARKS:
        sizes = "".join(f'<img src="{name}.svg" width="{s}" height="{s}" alt="">' for s in (128, 64, 32, 16))
        rows.append(f"""
<section>
  <h2>{name}</h2>
  <div class="row light">{sizes}<img src="{name}-mono.svg" width="64" height="64" alt="">
    <img src="lockup-{name}.svg" height="72" alt=""></div>
  <div class="row dark"><img src="{name}-dark.svg" width="64" height="64" alt="">
    <img src="{name}-dark.svg" width="16" height="16" alt="">
    <img src="lockup-{name}-dark.svg" height="72" alt=""></div>
</section>""")
    page = f"""<!doctype html>
<html lang="en"><head><meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<title>comshu logo drafts</title>
<style>
  body {{ font: 15px/1.5 system-ui, sans-serif; margin: 0 auto; max-width: 900px; padding: 24px 16px;
         color: {INK}; background: #f4f5f8; }}
  h1 {{ font-size: 22px; }} h2 {{ font-size: 16px; margin: 28px 0 8px; font-family: monospace; }}
  .row {{ display: flex; flex-wrap: wrap; align-items: center; gap: 20px; padding: 16px;
          border-radius: 8px; margin-bottom: 8px; }}
  .light {{ background: {PAPER}; }} .dark {{ background: #141a26; }}
</style></head><body>
<h1>comshu: logo drafts</h1>
<p>Generated by <code>tools/gen_logo.py</code>. Each mark at 128, 64, 32 and 16 px, monochrome,
then the lockup with the wordmark; second row on a dark ground.</p>
{''.join(rows)}
</body></html>
"""
    with open(os.path.join(OUT, "index.html"), "w") as f:
        f.write(page)
    print(f"wrote {len(files) + 1} files to demos/logo/")


if __name__ == "__main__":
    main()
