"""Upload an application .bin to the running ESP32 without BOOT/EN wiring.

Usage: python tools/serial_update.py COM5 .pio/build/esp32dev/firmware.bin
Requires pyserial. The first flash still needs the ROM loader.
"""
import argparse
import hashlib
from pathlib import Path
import time
import sys

TRACE = False
# Flash erases can pause UART interrupts. Keep each outstanding block within the
# ESP32's 128-byte hardware FIFO; a larger software RX buffer cannot prevent loss.
BLOCK_SIZE = 128


def wait_line(port, predicate, timeout=60):
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        line = port.readline().decode("utf-8", errors="replace").strip()
        if not line:
            continue
        if TRACE:
            print(f"RX {line}", flush=True)
        if line == "UPDATE ERROR":
            raise RuntimeError("设备拒绝更新（请检查固件大小、MD5 或分区）")
        if predicate(line):
            return line
    raise TimeoutError("等待设备响应超时，请检查串口和设备运行状态")


def upload(port, data, progress=None):
    if len(data) < 32 or data[0] != 0xE9:
        raise ValueError("需要 ESP32 应用固件 firmware.bin，不是合并烧录镜像")
    # Light-sleep UART wake loses the wake bytes. Send a separate preamble.
    port.write(b"\n\n\n\n")
    port.flush()
    time.sleep(0.3)
    port.write(b"\nUPDATE\n")
    wait_line(port, lambda line: line == "UPDATE MODE")
    digest = hashlib.md5(data).hexdigest()
    port.write(f"BEGIN {len(data)} {digest}\n".encode("ascii"))
    wait_line(port, lambda line: line == "UPDATE READY")
    for start in range(0, len(data), BLOCK_SIZE):
        block = data[start:start + BLOCK_SIZE]
        if port.write(block) != len(block):
            raise IOError("串口未完整发送数据")
        target = start + len(block)
        wait_line(port, lambda line: line.startswith("UPDATE RX ") and int(line[10:]) >= target)
        if progress:
            progress(target, len(data))
    wait_line(port, lambda line: line == "UPDATE OK")


def main():
    global TRACE
    sys.stdout.reconfigure(encoding="utf-8")
    import serial
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("port")
    parser.add_argument("firmware", type=Path)
    parser.add_argument("--verbose", action="store_true")
    args = parser.parse_args()
    TRACE = args.verbose
    data = args.firmware.read_bytes()
    # Construct unopened so DTR/RTS are disabled before Windows opens the port.
    port = serial.Serial(baudrate=115200, timeout=0.5, write_timeout=10)
    port.dtr = False
    port.rts = False
    port.port = args.port
    port.open()
    try:
        last_percent = [-1]
        def progress(done, total):
            percent = done * 100 // total
            if percent // 10 != last_percent[0] // 10:
                print(f"上传 {percent}%")
            last_percent[0] = percent
        upload(port, data, progress)
    finally:
        port.close()
    print("固件校验通过，设备正在重启；原配置和转发记录保留。")


if __name__ == "__main__":
    main()
