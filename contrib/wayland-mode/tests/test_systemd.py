#!/usr/bin/env python3
"""systemd + PAM integration (run as root inside the tigervnc-wl-systemd
container, where systemd is PID 1 and pam_systemd/logind are present):
vncserver@:1.service with mode=wayland gives a logind session of
Type=wayland; vncserver@:2.service without a mode stays Type=x11."""

import os
import pwd
import subprocess
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from harness import Suite, wait_tcp, wait_file, sh, make_vnc_passwd, \
    read_env_file, find_colour_bbox
from rfbclient import RFBClient

USER = "vncuser"
PASSWORD = "systemd-pw"
pw = pwd.getpwnam(USER)
HOME = pw.pw_dir
CONF = os.path.join(HOME, ".config/tigervnc")
s = Suite("systemd")


def write(path, text, mode=0o644, owner=None):
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, "w") as f:
        f.write(text)
    os.chmod(path, mode)
    if owner:
        os.chown(path, owner.pw_uid, owner.pw_gid)


write("/usr/share/wayland-sessions/vnc-test.desktop",
      "[Desktop Entry]\nName=VNC test (Wayland)\n"
      "Exec=/usr/local/bin/vnc-test-wayland-session\nType=Application\n")
write("/usr/local/bin/vnc-test-wayland-session",
      "#!/bin/sh\nenv > \"$HOME/wayland-session.env\"\n"
      "foot --title systemd-foot -W 70x14 &\n"
      "xterm -geometry 50x8+20+20 -bg white -fg black &\n"
      "exec sleep infinity\n", 0o755)
write("/usr/share/xsessions/vnc-test-x11.desktop",
      "[Desktop Entry]\nName=VNC test (X11)\n"
      "Exec=/usr/local/bin/vnc-test-x11-session\nType=Application\n")
write("/usr/local/bin/vnc-test-x11-session",
      "#!/bin/sh\nenv > \"$HOME/x11-session.env\"\n"
      "exec xterm -geometry 60x10+40+40 -bg white -fg black\n", 0o755)
os.makedirs(CONF, exist_ok=True)
for d in (os.path.join(HOME, ".config"), CONF):
    os.chown(d, pw.pw_uid, pw.pw_gid)
make_vnc_passwd(os.path.join(CONF, "passwd"), PASSWORD)
os.chown(os.path.join(CONF, "passwd"), pw.pw_uid, pw.pw_gid)
with open("/etc/tigervnc/vncserver.users", "w") as f:
    f.write(":1=%s\n:2=%s\n" % (USER, USER))


def set_config(text):
    write(os.path.join(CONF, "config"), text, 0o644, pw)


def logind_session_types():
    rc, out = sh(["loginctl", "list-sessions", "--no-legend"])
    types = []
    for line in out.splitlines():
        sid = line.split()[0]
        rc, props = sh(["loginctl", "show-session", sid, "-p", "Type", "-p",
                        "Class", "-p", "Name", "-p", "Service", "-p",
                        "State"])
        types.append(dict(l.split("=", 1) for l in props.splitlines()
                          if "=" in l))
    return types


def t_wayland_unit():
    set_config("mode=wayland\nsession=vnc-test\nsecuritytypes=VncAuth\n"
               "localhost\n")
    rc, out = sh(["systemctl", "start", "vncserver@:1.service"], timeout=60)
    up = wait_tcp(5901, timeout=30)
    rc2, st = sh(["systemctl", "is-active", "vncserver@:1.service"])
    sessions = logind_session_types()
    tig = [x for x in sessions if x.get("Service") == "tigervnc"]
    env_ok = wait_file(os.path.join(HOME, "wayland-session.env"), 20)
    senv = read_env_file(os.path.join(HOME, "wayland-session.env")) \
        if env_ok else {}
    c = RFBClient(port=5901, security="VncAuth", password=PASSWORD)
    end = time.time() + 20
    foot = xterm = None
    while time.time() < end:
        time.sleep(1)
        c.refresh()
        foot = find_colour_bbox(c, (0x24, 0x24, 0x24))
        xterm = find_colour_bbox(c, (255, 255, 255), region=(0, 0, 400, 200))
        if foot and xterm:
            break
    shot = c.screenshot(s.artifact("systemd-01-wayland-unit.png"))
    c.close()
    ok = (rc == 0 and up and st.strip() == "active" and foot and xterm and
          any(x.get("Type") == "wayland" and x.get("Class") == "user"
              for x in tig) and
          senv.get("XDG_RUNTIME_DIR", "").startswith("/run/user/"))
    return ok, ("systemctl start rc=%d, active=%s, logind sessions=%r, "
                "session XDG_RUNTIME_DIR=%r WAYLAND_DISPLAY=%r, foot %r, "
                "xterm %r" %
                (rc, st.strip(), tig, senv.get("XDG_RUNTIME_DIR"),
                 senv.get("WAYLAND_DISPLAY"), foot, xterm)), [shot]


def t_x11_unit():
    set_config("session=vnc-test-x11\nsecuritytypes=VncAuth\nlocalhost\n")
    rc, out = sh(["systemctl", "start", "vncserver@:2.service"], timeout=60)
    up = wait_tcp(5902, timeout=30)
    rc2, st = sh(["systemctl", "is-active", "vncserver@:2.service"])
    time.sleep(2)
    sessions = logind_session_types()
    tig = [x for x in sessions if x.get("Service") == "tigervnc"]
    c = RFBClient(port=5902, security="VncAuth", password=PASSWORD)
    end = time.time() + 20
    xterm = None
    while time.time() < end and not xterm:
        time.sleep(1)
        c.refresh()
        xterm = find_colour_bbox(c, (255, 255, 255))
    shot = c.screenshot(s.artifact("systemd-02-x11-unit.png"))
    c.close()
    ok = (rc == 0 and up and st.strip() == "active" and xterm and
          any(x.get("Type") == "x11" for x in tig))
    return ok, "active=%s, logind sessions=%r, xterm %r" % (
        st.strip(), tig, xterm), [shot]


def t_stop():
    sh(["systemctl", "stop", "vncserver@:1.service",
        "vncserver@:2.service"], timeout=60)
    time.sleep(2)
    rc, st = sh(["systemctl", "is-active", "vncserver@:1.service"])
    tig = [x for x in logind_session_types()
           if x.get("Service") == "tigervnc" and x.get("State") != "closing"]
    return st.strip() in ("inactive", "failed") and not tig, \
        "after stop: :1 %s, remaining tigervnc sessions %r" % (st.strip(),
                                                               tig)


s.run("vncserver@:1.service, mode=wayland: Wayland session, logind "
      "session Type=wayland", t_wayland_unit,
      needs=['foot'])
s.run("vncserver@:2.service, no mode: X11 session, logind Type=x11",
      t_x11_unit)
s.run("systemctl stop ends both sessions", t_stop)
code = s.finish()
sh("journalctl --no-pager -b > %s/logs/systemd-journal.txt 2>&1" %
   os.environ.get("ARTIFACTS", "/artifacts"), timeout=30)
sys.exit(code)
