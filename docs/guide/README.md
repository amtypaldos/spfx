# SP-1 FX quick-reference guide

Files
- `sp1fx-guide.html` - editable hand-authored source (HTML/CSS + inline SVG device artwork). The effects table rows sit between `<!--ROWS-->` markers and were generated from `companion/data/fx_table.json`.
- `sp1fx-guide.png` - 2400x3502 render.
- `sp1fx-guide.pdf` - single A4 page (print-to-PDF of the same HTML).

Regenerate (headless Chrome, run in this directory)
    C="/Applications/Google Chrome.app/Contents/MacOS/Google Chrome"
    "$C" --headless --hide-scrollbars --force-device-scale-factor=1.7 --window-size=1412,2060 --screenshot=sp1fx-guide.png file://$PWD/sp1fx-guide.html
    "$C" --headless --no-pdf-header-footer --print-to-pdf=sp1fx-guide.pdf file://$PWD/sp1fx-guide.html

Style
Layout, palette, typography and callout-card style follow the Marisko firmware guide image (`download.png` in jackiscool123123121/marisko, commit `315d5046`, MIT): card callouts with dotted leader lines to a flat device illustration, warm off-white background, single red accent. Only the layout idea is used. All artwork is redrawn from scratch; no pixels, text or branding are copied. Nothing from softmodded/marisko (no licence) is used.

Fonts: the HTML uses system fonts only ("Helvetica Neue", Helvetica, Arial, sans-serif) and loads nothing external. The PDF was printed on macOS and embeds subsets of the system fonts Helvetica Neue and Arial; the PNG has no embedded fonts.

Status: development firmware. Not yet tested on hardware.
