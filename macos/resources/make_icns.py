"""Aqulify.icns a macOS alkalmazáshoz: a Windows/Linux ikon rajza (windows/resources/make_icon.py), a macOS
ikonrácsához igazítva (1024 px vásznon 824 px-es lekerekített négyzet). Futtatás: python3 make_icns.py"""
import os
import runpy
import tempfile

from PIL import Image

HERE = os.path.dirname(os.path.abspath(__file__))
SRC = os.path.join(HERE, "..", "..", "windows", "resources", "make_icon.py")

# A rajzoló szkript a futási mappába ír (aether.ico, aether_256.png): átmeneti mappában fut
cwd = os.getcwd()
with tempfile.TemporaryDirectory() as tmp:
    os.chdir(tmp)
    try:
        art = runpy.run_path(SRC)["img"]
    finally:
        os.chdir(cwd)

S, MARGIN, BODY = 1024, 100, 824
body = art.crop((40, 40, S - 40, S - 40)).resize((BODY, BODY), Image.LANCZOS)
icon = Image.new("RGBA", (S, S), (0, 0, 0, 0))
icon.paste(body, (MARGIN, MARGIN), body)
icon.save(os.path.join(HERE, "Aqulify.icns"),
          sizes=[(16, 16), (32, 32), (64, 64), (128, 128), (256, 256), (512, 512), (1024, 1024)])
print("Aqulify.icns kész")
