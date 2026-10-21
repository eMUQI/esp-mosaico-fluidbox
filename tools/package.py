#!/usr/bin/env python3
"""Package a verified Iris application bundle and a complete wired BIN."""
import argparse
import hashlib
import json
from pathlib import Path
import subprocess
import sys
import tempfile

root = Path(__file__).resolve().parents[1]
parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument("--build-dir", type=Path, default=root / "build",
                    help="ESP-IDF build directory (default: build/)")
build = parser.parse_args().build_dir.resolve()
utils = root / "external/esp-mosaico-utils"
sys.path[:0] = [str(utils / "mosaico-tools/tools"),
               str(utils / "ESP-Iris/components/esp_iris/tools")]
from mosaico_cli.ideas_firmware import validate_firmware
from mosaico_cli.product_contract import validate_application_config
from iris_gateway.system_update import build_system_update_bundle

args = json.loads((build / "flasher_args.json").read_text())
project = json.loads((build / "project_description.json").read_text())
if project["target"] != "esp32s31":
    parser.error("packaging requires an ESP32-S31 build")
validate_application_config(build)

recovery = utils / "esp-mosaico-vibe-mode/firmware/vibe-mode/prebuilt/recovery"
entry = json.loads((recovery / "manifest.json").read_text())["images"]["recovery"]
vibe = recovery / entry["file"]
if hashlib.sha256(vibe.read_bytes()).hexdigest() != entry["sha256"]:
    parser.error("the reviewed Vibe Mode image does not match its manifest")

out = root / "dist"
out.mkdir(exist_ok=True)
name = f"mosaico-fluidbox-{project['project_version']}"
with tempfile.TemporaryDirectory(prefix="fluidbox-package-", dir=out) as directory:
    stage = Path(directory)
    subprocess.run([
        sys.executable, str(utils / "mosaico-tools/tools/prepare_system_update.py"),
        "--partition-csv", str(root / "partitions.csv"),
        "--partition-table", str(build / "partition_table/partition-table.bin"),
        "--application", str(build / project["app_bin"]),
        "--stage-dir", str(stage), "--release", project["project_version"],
    ], check=True)
    bundle = build_system_update_bundle(
        stage / f"{name}.irisfw", json.loads((stage / "manifest.json").read_text()), stage)
    validate_firmware(bundle.read_bytes())

    merged = stage / f"{name}.bin"
    merge = [sys.executable, "-m", "esptool", "--chip", "esp32s31", "merge-bin",
             "--output", str(merged), *args["write_flash_args"]]
    for offset, path in args["flash_files"].items():
        merge.extend([offset, str(build / path)])
    # The wired image includes retained Vibe Mode; Iris updates preserve it in place.
    merge.extend([entry["offset"], str(vibe)])
    subprocess.run(merge, check=True)
    for path in (bundle, merged):
        destination = out / path.name
        path.replace(destination)
        print(destination)
