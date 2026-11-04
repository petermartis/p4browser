#!/usr/bin/env python
# Mac SSH helper — runs a command / copies files on the Mac that holds the
# ESP32-P4's USB flashing port.
#
# Credentials come from the environment (never commit them):
#   export MAC_SSH_HOST=192.168.1.248
#   export MAC_SSH_USER=peter
#   export MAC_SSH_PASSWORD='***'
#
# Usage:
#   macssh.py exec '<shell command>'
#   macssh.py put <local> <remote>
#   macssh.py get <remote> <local>
import os, sys
import paramiko

HOST = os.environ.get("MAC_SSH_HOST", "192.168.1.248")
USER = os.environ.get("MAC_SSH_USER", "peter")
PWD  = os.environ.get("MAC_SSH_PASSWORD", "")

if not PWD:
    sys.exit("MAC_SSH_PASSWORD environment variable is not set")


def conn():
    c = paramiko.SSHClient()
    c.set_missing_host_key_policy(paramiko.AutoAddPolicy())
    c.connect(HOST, username=USER, password=PWD, timeout=15,
              allow_agent=False, look_for_keys=False)
    return c

def main():
    mode = sys.argv[1]
    c = conn()
    try:
        if mode == "exec":
            cmd = sys.argv[2]
            _, out, err = c.exec_command(cmd, timeout=900)
            sys.stdout.write(out.read().decode("utf-8", "replace"))
            e = err.read().decode("utf-8", "replace")
            if e: sys.stderr.write(e)
            sys.exit(0)
        elif mode == "put":
            sftp = c.open_sftp()
            sftp.put(sys.argv[2], sys.argv[3])
            sftp.close()
            print(f"PUT_OK {sys.argv[2]} -> {sys.argv[3]}")
        elif mode == "get":
            sftp = c.open_sftp()
            sftp.get(sys.argv[2], sys.argv[3])
            sftp.close()
            print(f"GET_OK {sys.argv[2]} -> {sys.argv[3]}")
        else:
            print("usage: macssh.py exec|put|get ..."); sys.exit(2)
    finally:
        c.close()

if __name__ == "__main__":
    main()
