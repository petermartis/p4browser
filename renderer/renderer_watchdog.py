#!/usr/bin/env python3
"""Supervisor: keep renderer.js alive. If it exits, restart after 3s.

Host-configurable via env (so the same file runs on the NAS and the Pi5):
  NODE, SCRIPT, PORT, RENDERER_LOG, WATCHDOG_LOG, CHROME_PATH,
  LD_LIBRARY_PATH, FONTCONFIG_FILE.
NAS defaults are used when unset; the NAS fontconfig/lib overrides are only
applied when those paths actually exist (the Pi5 skips both and uses system
defaults). On the Pi5 the launcher sets NODE/SCRIPT/CHROME_PATH to the Pi5
values.
"""
import subprocess, time, os, sys, signal, traceback

NODE = os.environ.get('NODE', '/volume1/hermes/node22/bin/node')
SCRIPT = os.environ.get('SCRIPT', '/volume1/hermes/renderer/renderer.js')
PORT = os.environ.get('PORT', '9100')
LOG = os.environ.get('RENDERER_LOG', '/tmp/renderer.log')
WDLOG = os.environ.get('WATCHDOG_LOG', '/tmp/watchdog.log')

env = dict(os.environ)
# CHROME_PATH rides through os.environ into renderer.js (system Chromium on the
# Pi5, portable build default on the NAS). The NAS lib/fontconfig overrides are
# added only when the launcher didn't set them AND the NAS paths exist.
if os.path.isdir('/volume1/hermes/renderer/lib'):
    env.setdefault('LD_LIBRARY_PATH', '/volume1/hermes/renderer/lib:/usr/lib:/lib')
if os.path.exists('/volume1/hermes/renderer/fontconf/fonts.conf'):
    env.setdefault('FONTCONFIG_FILE', '/volume1/hermes/renderer/fontconf/fonts.conf')
    env.setdefault('FONTCONFIG_PATH', '/volume1/hermes/renderer/fontconf')


def log(msg):
    with open(WDLOG, 'a') as f:
        f.write(f"{time.strftime('%H:%M:%S')} {msg}\n")
    print(msg, flush=True)


def _sig(signum, frame):
    # Catch the death: SIGTERM = systemd stop/restart (or a kill), SIGHUP =
    # session hangup. SIGKILL cannot be trapped — a death with NO log line
    # is by definition SIGKILL (OOM or external kill -9).
    try:
        name = signal.Signals(signum).name
    except ValueError:
        name = str(signum)
    log(f"watchdog received signal {signum} ({name}) — exiting")
    sys.exit(128 + signum)

signal.signal(signal.SIGTERM, _sig)
signal.signal(signal.SIGINT, _sig)
signal.signal(signal.SIGHUP, _sig)
sys.excepthook = lambda *exc: (
    log("watchdog FATAL EXCEPTION:\n" + "".join(traceback.format_exception(*exc))),
    sys.exit(1),
)


log("watchdog starting")
while True:
    lf = open(LOG, 'a')
    p = subprocess.Popen([NODE, SCRIPT, '--port', PORT], env=env,
                         stdout=lf, stderr=subprocess.STDOUT)
    log(f"renderer started pid {p.pid}")
    while True:
        try:
            rc = p.wait(timeout=600)
            break
        except subprocess.TimeoutExpired:
            log(f"watchdog alive (node pid {p.pid})")
    log(f"renderer exited rc={rc} — restarting in 3s")
    try:
        lf.close()
    except Exception:
        pass
    time.sleep(3)
