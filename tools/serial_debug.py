"""Send debug commands without toggling the USB adapter's DTR/RTS lines."""
import argparse
import time
import serial
import sys
sys.stdout.reconfigure(encoding='utf-8')

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('port')
parser.add_argument('commands', nargs='*', default=['IP', 'STATUS'])
parser.add_argument('--seconds', type=float, default=8)
args = parser.parse_args()
port = serial.Serial(port=None, baudrate=115200, timeout=0.3)
port.dtr = False
port.rts = False
port.port = args.port
port.open()
try:
    port.write(b'\n\n\n\n')
    time.sleep(0.4)
    port.write(('\n' + '\n'.join(args.commands) + '\n').encode('ascii'))
    end = time.monotonic() + args.seconds
    while time.monotonic() < end:
        data = port.read(4096)
        if data:
            print(data.decode('utf-8', errors='replace'), end='', flush=True)
finally:
    port.close()
