"""
fetch_components.py - download ESP-IDF 3rd-party components (lvgl, esp_lvgl_port)
via GitHub raw.githubusercontent.com to bypass fastgithub 404s on archive/tar.gz endpoints.
"""
import json
import shutil
import subprocess
import sys
import urllib.request
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
COMP_DIR = ROOT / "components"

COMPONENTS = [
    ("lvgl",            "lvgl/lvgl",          "release/v9.2", ""),
    ("esp_lvgl_port",   "espressif/esp-bsp",  "master",        "components/esp_lvgl_port"),
]


def gh_get(url: str):
    req = urllib.request.Request(
        url,
        headers={"User-Agent": "curl/8", "Accept": "application/vnd.github+json"},
    )
    return urllib.request.urlopen(req, timeout=60)


def download_file(url: str, dest: Path) -> bool:
    try:
        with gh_get(url) as r:
            if r.status != 200:
                return False
            dest.parent.mkdir(parents=True, exist_ok=True)
            with open(dest, "wb") as f:
                f.write(r.read())
            return True
    except Exception as e:
        print(f"[!] {url}: {e}")
        return False


def fetch_subpath_via_tree(github_repo: str, ref: str, sub_path: str, out_dir: Path):
    """Use git/trees API to get the full tree, then download each file via raw.githubusercontent.com."""
    tree_url = (
        f"https://api.github.com/repos/{github_repo}/git/trees/{ref}?recursive=1"
    )
    print(f"[*] tree {tree_url}")
    tree = json.loads(gh_get(tree_url).read())
    if tree.get("truncated"):
        print("[!] repo tree truncated; results may be incomplete")
    count = 0
    for item in tree.get("tree", []):
        if item.get("type") != "blob":
            continue
        path = item["path"]
        if not path.startswith(sub_path + "/") and path != sub_path:
            continue
        rel = path[len(sub_path):].lstrip("/") if path != sub_path else ""
        if not rel:
            continue
        dest = out_dir / rel
        if dest.exists() and dest.stat().st_size == item.get("size", -1):
            count += 1
            continue
        raw = f"https://raw.githubusercontent.com/{github_repo}/{ref}/{path}"
        if download_file(raw, dest):
            count += 1
        else:
            print(f"[!] failed: {path}")
    print(f"[ok] fetched {count} files into {out_dir}")


def fetch_toplevel(github_repo: str, ref: str, out_dir: Path):
    """Fetch all files in the repo (lvgl style)."""
    tree_url = (
        f"https://api.github.com/repos/{github_repo}/git/trees/{ref}?recursive=1"
    )
    print(f"[*] tree {tree_url}")
    tree = json.loads(gh_get(tree_url).read())
    if tree.get("truncated"):
        print("[!] repo tree truncated; results may be incomplete")
    count = 0
    for item in tree.get("tree", []):
        if item.get("type") != "blob":
            continue
        rel = item["path"]
        dest = out_dir / rel
        if dest.exists() and dest.stat().st_size == item.get("size", -1):
            count += 1
            continue
        raw = f"https://raw.githubusercontent.com/{github_repo}/{ref}/{rel}"
        if download_file(raw, dest):
            count += 1
        else:
            print(f"[!] failed: {rel}")
    print(f"[ok] fetched {count} files into {out_dir}")


def main():
    p_comp = COMP_DIR
    p_comp.mkdir(exist_ok=True)

    for name, gh, ref, sub in COMPONENTS:
        out = p_comp / name
        if (out / "CMakeLists.txt").exists() and (out / "include").exists():
            print(f"[skip] {name} already present")
            continue
        if out.exists():
            shutil.rmtree(out)
        out.mkdir(exist_ok=True)
        if sub:
            fetch_subpath_via_tree(gh, ref, sub, out)
        else:
            fetch_toplevel(gh, ref, out)

    # 1. 写入 main/CMakeLists.txt EXTRA_COMPONENT_DIRS（已写过则跳过）
    cmakelists = ROOT / "main" / "CMakeLists.txt"
    txt = cmakelists.read_text(encoding="utf-8")
    extra_line = "set(EXTRA_COMPONENT_DIRS \"${CMAKE_CURRENT_SOURCE_DIR}/../components\")"
    if extra_line not in txt:
        lines = txt.splitlines()
        for i, ln in enumerate(lines):
            if "file(GLOB" in ln or "idf_component_register" in ln:
                lines.insert(i, extra_line)
                break
        else:
            lines.insert(0, extra_line)
        cmakelists.write_text("\n".join(lines) + "\n", encoding="utf-8")
        print(f"[ok] added EXTRA_COMPONENT_DIRS to {cmakelists}")

    # 2. 清空 idf_component.yml
    yml = ROOT / "main" / "idf_component.yml"
    yml.write_text(
        "## 组件通过 EXTRA_COMPONENT_DIRS 由本地 components__* 提供。\n"
        "## 本文件留空即可，不要写 dependencies: 块。\n",
        encoding="utf-8",
    )
    print(f"[ok] cleared {yml} dependencies")

    print("\n[DONE] Run `build.ps1 build` again.")


if __name__ == "__main__":
    main()