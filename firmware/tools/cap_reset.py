import sys, time
try:
    from serial import Serial
except Exception as e:
    print("NO_SERIAL_LIB", e); sys.exit(1)
port = "/dev/cu.usbserial-10"
b = int(sys.argv[1]) if len(sys.argv) > 1 else 115200
d = int(sys.argv[2]) if len(sys.argv) > 2 else 25
s = Serial(port, b, timeout=1)
time.sleep(0.5)
n = 0; last = 0
end = time.time() + d
while time.time() < end:
    data = s.read(4096)
    if data:
        n += len(data); last = time.time()
        sys.stdout.write(data.decode("utf-8", "replace"))
        sys.stdout.flush()
print("\n===CAP bytes=%d last_rx=%.1fs ago===" % (n, time.time() - last))
