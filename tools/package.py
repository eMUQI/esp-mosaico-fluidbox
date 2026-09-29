#!/usr/bin/env python3
"""Merge a build into one full firmware image, flashed at 0x0, for the release page."""
import argparse
import json
from pathlib import Path
import subprocess
import sys

root = Path(__file__).resolve().parents[1]
parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument("--build-dir", type=Path, default=root / "build",
                    help="ESP-IDF build directory (default: build/)")
build = parser.parse_args().build_dir.resolve()
args = json.loads((build / "flasher_args.json").read_text())
project = json.loads((build / "project_description.json").read_text())
out = root / "dist" / f"mosaico-fluidbox-{project['project_version']}.bin"
out.parent.mkdir(parents=True, exist_ok=True)

merge = [sys.executable, "-m", "esptool", "--chip", "esp32s31", "merge-bin",
         "--output", str(out), *args["write_flash_args"]]
for offset, name in args["flash_files"].items():
    merge.extend([offset, str(build / name)])
subprocess.run(merge, check=True)
print(out)
