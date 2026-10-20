"""Builds res/icons.ttf: the Fluent UI System Icons the app uses, embedded in FileTools.exe.

The glyphs are placed at the Segoe Fluent Icons code points the source already uses, so the
C++ code keeps referring to icons by those numbers.

Usage (needs `pip install fonttools`):
    python make-icon-font.py FluentSystemIcons-Regular.ttf FluentSystemIcons-Regular.json [icons.ttf]

Font and JSON: https://github.com/microsoft/fluentui-system-icons/tree/main/fonts (MIT License).
"""
import json
import os
import sys

from fontTools import subset
from fontTools.ttLib import TTFont
from fontTools.ttLib.tables._c_m_a_p import CmapSubtable

# Segoe Fluent Icons code point used in src/ -> Fluent System Icons name (24 px, regular).
ICONS = {
    # Wheel (src/actions.cpp)
    0xE73F: "arrow_minimize",          # COMPRIMIR
    0xE9E9: "options",                 # EDITAR / Ajustes
    0xE70F: "edit",                    # ANOTAR
    0xEB9F: "image",                   # FONDO
    0xE7A8: "crop",                    # RECORTAR
    0xED1A: "eye_off",                 # CENSURAR
    0xE7AD: "arrow_clockwise",         # ROTAR
    0xE8D2: "text_t",                  # TEXTO
    0xE8C6: "cut",                     # DIVIDIR / SILENCIOS
    0xE71B: "link",                    # UNIR / RUTA
    0xE74F: "speaker_mute",            # SILENCIAR
    0xE8D6: "music_note_2",            # AUDIO
    0xE786: "gif",                     # GIF
    0xE722: "camera",                  # FOTOGRAMA
    0xE995: "speaker_2",               # NORMALIZAR
    0xE838: "folder_open",             # EXTRAER
    0xE8C8: "copy",                    # COPIAR
    0xE8DE: "folder_arrow_right",      # MOVER
    0xF012: "folder_zip",              # ZIP
    0xE756: "code",                    # TERMINAL
    0xE840: "pin",                     # FLOTAR
    0xE72D: "share",                   # COMPARTIR
    0xE7AC: "open",                    # ABRIR CON
    0xE7F5: "speaker_1",               # MONO
    0xE928: "fingerprint",             # HASH
    0xE8EF: "number_symbol",           # CONTAR / TAMAÑO
    0xE946: "info",                    # INFO
    0xE75C: "eraser",                  # LIMPIAR
    0xE771: "wand",                    # SIN FONDO
    0xED14: "qr_code",                 # ENVIAR
    0xE740: "arrow_maximize",          # ESCALAR / Tamaño
    # Editor (src/editor.cpp)
    0xE74E: "save",
    0xE8AB: "flip_horizontal",
    0xE72C: "arrow_reset",
    0xEE56: "pen",
    0xE7E6: "highlight",
    0xE72A: "arrow_up_right",
    0xE739: "rectangle_landscape",
    0xEA3A: "circle",
    0xE7A7: "arrow_undo",
    0xE74D: "delete",
    # Floating shelf (src/floatshelf.cpp)
    0xE8BB: "dismiss",
}

FAMILY = "FileTools Icons"


def main():
    src_ttf, src_json = sys.argv[1], sys.argv[2]
    out = sys.argv[3] if len(sys.argv) > 3 else os.path.join(os.path.dirname(os.path.abspath(__file__)), "icons.ttf")
    with open(src_json, encoding="utf-8") as f:
        names = json.load(f)
    font = TTFont(src_ttf)
    by_code = font.getBestCmap()

    mapping = {}
    missing = []
    for cp, name in ICONS.items():
        key = f"ic_fluent_{name}_24_regular"
        if key not in names or names[key] not in by_code:
            missing.append(key)
            continue
        mapping[cp] = by_code[names[key]]
    if missing:
        sys.exit("No están en la fuente: " + ", ".join(missing))

    opts = subset.Options()
    opts.layout_features = []
    opts.name_IDs = ["*"]
    opts.notdef_outline = True
    opts.drop_tables += ["GSUB", "GPOS", "GDEF"]
    sub = subset.Subsetter(opts)
    sub.populate(glyphs=sorted(set(mapping.values())))
    sub.subset(font)

    table = CmapSubtable.newSubtable(4)
    table.platformID, table.platEncID, table.language = 3, 1, 0
    table.cmap = mapping
    font["cmap"].tables = [table]

    name = font["name"]
    name.names = []
    for nid, value in ((1, FAMILY), (2, "Regular"), (4, FAMILY), (6, FAMILY.replace(" ", "") + "-Regular"),
                       (5, "Version 1.0"), (0, "Copyright (c) 2020 Microsoft Corporation. MIT License.")):
        name.setName(value, nid, 3, 1, 0x409)
    font.save(out)
    print(f"{out}: {len(mapping)} iconos, {os.path.getsize(out)} bytes")


if __name__ == "__main__":
    main()
