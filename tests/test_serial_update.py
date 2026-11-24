import hashlib
import importlib.util
from pathlib import Path
import unittest
from unittest.mock import patch

spec = importlib.util.spec_from_file_location("serial_update", Path(__file__).parents[1] / "tools" / "serial_update.py")
uploader = importlib.util.module_from_spec(spec)
spec.loader.exec_module(uploader)


class Device:
    """Stop-and-wait device simulator with partial acknowledgements and MD5 validation."""
    def __init__(self, expected):
        self.expected = expected
        self.data = bytearray()
        self.lines = [b"Wi-Fi IP: 192.168.8.10\n"]
        self.writes = []
        self.phase = "command"

    def write(self, data):
        self.writes.append(data)
        if self.phase == "data":
            if len(data) > 128:
                raise BufferError("Flash erase can pause UART interrupts: hardware FIFO overflow")
            self.data.extend(data)
            count = len(self.data)
            self.lines += [f"UPDATE RX {count - 1}\n".encode(), f"UPDATE RX {count}\n".encode()]
            if count == len(self.expected):
                self.lines.append(b"UPDATE OK\n" if self.data == self.expected else b"UPDATE ERROR\n")
        elif b"UPDATE\n" in data:
            self.lines.append(b"UPDATE MODE\n")
            self.phase = "header"
        elif data.startswith(b"BEGIN "):
            self.assert_header(data)
            self.lines.append(b"UPDATE READY\n")
            self.phase = "data"
        return len(data)

    def assert_header(self, data):
        expected = f"BEGIN {len(self.expected)} {hashlib.md5(self.expected).hexdigest()}\n".encode()
        if data != expected:
            raise AssertionError("Wrong size or MD5")

    def flush(self):
        pass

    def readline(self):
        return self.lines.pop(0) if self.lines else b""


class Tests(unittest.TestCase):
    @patch.object(uploader.time, "sleep")
    def test_upload_multiple_chunks_with_trailing_partial_chunk(self, _):
        data = b"\xe9" + bytes(range(256))
        device = Device(data)
        progress = []
        uploader.upload(device, data, lambda sent, total: progress.append((sent, total)))
        self.assertEqual(device.data, data)
        self.assertEqual(progress, [(128, len(data)), (256, len(data)), (len(data), len(data))])
        self.assertEqual(device.writes[0], b"\n\n\n\n")

    @patch.object(uploader.time, "sleep")
    def test_single_chunk(self, _):
        data = b"\xe9" + b"x" * 40
        device = Device(data)
        uploader.upload(device, data)
        self.assertEqual(device.data, data)

    def test_reject_invalid_image_before_serial_write(self):
        for data in [b"", b"\xe9", b"\xff" * 1024]:
            device = Device(data)
            with self.assertRaises(ValueError):
                uploader.upload(device, data)
            self.assertFalse(device.writes)

    def test_device_error_is_not_success(self):
        device = Device(b"")
        device.lines = [b"UPDATE ERROR\n"]
        with self.assertRaises(RuntimeError):
            uploader.wait_line(device, lambda _: True)

    def test_timeout(self):
        with patch.object(uploader.time, "monotonic", side_effect=[0, 1, 65]):
            with self.assertRaises(TimeoutError):
                uploader.wait_line(Device(b""), lambda _: False)

    @patch.object(uploader.time, "sleep")
    def test_partial_write_is_failure(self, _):
        device = Device(b"\xe9" + b"x" * 40)
        original = device.write
        def write(data):
            result = original(data)
            return result - 1 if device.phase == "data" and not data.startswith(b"BEGIN") else result
        device.write = write
        with self.assertRaises(IOError):
            uploader.upload(device, device.expected)


if __name__ == "__main__":
    unittest.main()
