"""Cache the assets supplied by Figma design context (no runtime remote URLs)."""

import concurrent.futures
import json
import pathlib
import re
import subprocess

ROOT = pathlib.Path(__file__).resolve().parent
data = json.loads((ROOT / "figma_context.json").read_text(encoding="utf-8"))
assets = ROOT / "assets"
assets.mkdir(exist_ok=True)
urls = sorted(
    set(
        re.findall(
            r'https://www.figma.com/api/mcp/asset/[^"\s]+',
            "\n".join(p["code"] for p in data["pages"]),
        )
    )
)


def fetch(url):
    dest = assets / url.rsplit("/", 1)[1]
    if not dest.exists() or not dest.stat().st_size:
        subprocess.run(
            [
                "curl.exe",
                "--fail",
                "--location",
                "--silent",
                "--show-error",
                "--retry",
                "2",
                "--max-time",
                "45",
                url,
                "--output",
                str(dest),
            ],
            check=True,
        )
    if not dest.stat().st_size:
        raise RuntimeError(f"Empty asset: {dest}")


with concurrent.futures.ThreadPoolExecutor(max_workers=8) as pool:
    list(pool.map(fetch, urls))
print(f"Cached {len(urls)} Figma assets")

# Open-source font sources for exact numeric typography and missing symbols.
fonts = ROOT / "fonts"
fonts.mkdir(exist_ok=True)
for family, filename in [
    ("inter", "Inter[opsz,wght].ttf"),
    ("notosanssymbols2", "NotoSansSymbols2-Regular.ttf"),
    ("inter", "OFL.txt"),
    ("notosanssc", "OFL.txt"),
    ("notosanssymbols2", "OFL.txt"),
]:
    dest = fonts / (family + "-OFL.txt" if filename == "OFL.txt" else filename)
    if not dest.exists() or not dest.stat().st_size:
        subprocess.run(
            [
                "curl.exe",
                "--globoff",
                "--fail",
                "--location",
                "--silent",
                "--show-error",
                "--retry",
                "2",
                "--max-time",
                "45",
                f"https://raw.githubusercontent.com/google/fonts/main/ofl/{family}/{filename}",
                "--output",
                str(dest),
            ],
            check=True,
        )
