#!/usr/bin/env python3
"""Run the pinned official ESP-Mosaico CLI for this application workspace."""
from pathlib import Path
import sys

tool_root = Path(__file__).resolve().parent / "external/esp-mosaico-utils/mosaico-tools"
sys.path.insert(0, str(tool_root / "tools"))
from mosaico_cli.cli import main

if __name__ == "__main__":
    raise SystemExit(main(tool_root=tool_root))
