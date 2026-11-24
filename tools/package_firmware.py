"""Package an ESP32 application image and a combined first-flash image."""
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys

root = Path(__file__).resolve().parents[1]
build = root / ".pio" / "build" / "esp32dev"
core = Path(os.environ.get("PLATFORMIO_CORE_DIR", Path.home() / ".platformio"))
packages = core / "packages"
esptool = packages / "tool-esptoolpy" / "esptool.py"
boot_app = packages / "framework-arduinoespressif32" / "tools" / "partitions" / "boot_app0.bin"
out = root / "dist"
required = [build / "firmware.bin", build / "bootloader.bin", build / "partitions.bin", esptool, boot_app]
for path in required:
    if not path.is_file():
        raise SystemExit(f"文件不存在，请先运行 platformio run：{path}")
out.mkdir(exist_ok=True)
shutil.copy2(build / "firmware.bin", out / "firmware.bin")
subprocess.run([
    sys.executable, str(esptool), "--chip", "esp32", "merge_bin",
    "-o", str(out / "factory.bin"), "--flash_mode", "dio", "--flash_freq", "40m", "--flash_size", "4MB",
    "0x1000", str(build / "bootloader.bin"),
    "0x8000", str(build / "partitions.bin"),
    "0xe000", str(boot_app), "0x10000", str(build / "firmware.bin"),
], check=True)
manifest = {}
for name in ["firmware.bin", "factory.bin"]:
    data = (out / name).read_bytes()
    manifest[name] = {"bytes": len(data), "sha256": hashlib.sha256(data).hexdigest()}
(out / "manifest.json").write_text(json.dumps(manifest, indent=2) + "\n", encoding="utf-8")
print("dist/factory.bin: first flash at 0x0; dist/firmware.bin: application serial update.")
if (root / "include" / "secrets.h").is_file():
    print("A local secrets.h exists. Keep these images private if a webhook was compiled in.")
