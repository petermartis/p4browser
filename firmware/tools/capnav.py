import serial, time, glob, subprocess

port = '/dev/cu.usbmodem101'
if not glob.glob(port):
    port = glob.glob('/dev/cu.usbmodem*')[0]
print('using port', port)
s = serial.Serial(port, 921600, timeout=0.5)
buf = b''
t0 = time.time()
end = t0 + 80
navigated = False
state_done = False

def curl(url):
    r = subprocess.run(['curl', '-s', '-m', '10', url], capture_output=True, text=True)
    return r.stdout

try:
    while time.time() < end:
        d = s.read(4096)
        if d:
            buf += d
        if not navigated and time.time() - t0 > 3:
            curl('http://192.168.3.129/url?u=example.com')
            navigated = True
        if not state_done and time.time() - t0 > 18:
            open('/tmp/state_sync.txt', 'w').write(curl('http://192.168.3.129/state'))
            state_done = True
finally:
    s.close()

open('/tmp/c_sync.txt', 'wb').write(buf)
print('bytes', len(buf), 'nav', navigated, 'state', state_done)
