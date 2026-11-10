#!/usr/bin/env python3
"""vncsession tests (run as root in the test image): "mode=wayland" in
the user's vncserver config starts a Wayland session, no mode keeps the
X11 behaviour. Uses the real vncsession -> PAM -> vncserver -> Xvnc path
(without systemd as PID 1)."""

import glob
import os
import pwd
import signal
import socket
import subprocess
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from harness import (Suite, wait_tcp, wait_file, sh, read_env_file,
                     make_vnc_passwd, find_colour_bbox)
from rfbclient import RFBClient

USER = "vncuser"
PASSWORD = "session-pw"
pw = pwd.getpwnam(USER)
HOME = pw.pw_dir
CONF_DIR = os.path.join(HOME, ".config/tigervnc")
HOST = socket.gethostname()

s = Suite("vncsession")


def write(path, text, mode=0o644, owner=None):
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, "w") as f:
        f.write(text)
    os.chmod(path, mode)
    if owner:
        os.chown(path, owner.pw_uid, owner.pw_gid)


# Session definitions
write("/usr/share/wayland-sessions/vnc-test.desktop",
      "[Desktop Entry]\nName=VNC test (Wayland)\n"
      "Exec=/usr/local/bin/vnc-test-wayland-session %U\n"
      "DesktopNames=VncTest\nType=Application\n")
write("/usr/local/bin/vnc-test-wayland-session",
      "#!/bin/sh\nenv > \"$HOME/wayland-session.env\"\n"
      "foot --title session-foot -W 70x14 &\n"
      "xterm -geometry 50x8+20+20 -bg white -fg black &\n"
      "exec sleep infinity\n", 0o755)
write("/usr/share/xsessions/vnc-test-x11.desktop",
      "[Desktop Entry]\nName=VNC test (X11)\n"
      "Exec=/usr/local/bin/vnc-test-x11-session\nType=Application\n")
write("/usr/local/bin/vnc-test-x11-session",
      "#!/bin/sh\nenv > \"$HOME/x11-session.env\"\n"
      "exec xterm -geometry 60x10+40+40 -bg white -fg black\n", 0o755)

os.makedirs(CONF_DIR, exist_ok=True)
for d in (os.path.join(HOME, ".config"), CONF_DIR):
    os.chown(d, pw.pw_uid, pw.pw_gid)
make_vnc_passwd(os.path.join(CONF_DIR, "passwd"), PASSWORD)
os.chown(os.path.join(CONF_DIR, "passwd"), pw.pw_uid, pw.pw_gid)


def set_config(text):
    write(os.path.join(CONF_DIR, "config"), text, 0o644, pw)


def proc_env(pid, user=None):
    """Initial environment of a process (what it was exec'ed with)."""
    if user:
        data = subprocess.run(["runuser", "-u", user, "--", "cat",
                               "/proc/%d/environ" % pid],
                              capture_output=True).stdout
    else:
        with open("/proc/%d/environ" % pid, "rb") as f:
            data = f.read()
    return dict(i.decode().split("=", 1) for i in data.split(b"\0")
                if b"=" in i)


def server_procs():
    """(pid, command name, user) of running Xvnc / Xvnc-wayland"""
    rc, out = sh(["ps", "-eo", "pid=,user:20=,comm="])
    procs = []
    for line in out.splitlines():
        pid, user, comm = line.split(None, 2)
        if comm in ("Xvnc", "Xvnc-wayland"):
            procs.append((int(pid), comm, user))
    return procs


def start_session(display):
    r = subprocess.run(["vncsession", USER, display], capture_output=True,
                       timeout=30)
    pidfile = "/run/vncsession-%s.pid" % display
    wait_file(pidfile, 10)
    pid = int(open(pidfile).read()) if os.path.exists(pidfile) else None
    return r.returncode, pid, (r.stdout + r.stderr).decode()


def stop_session(pid, display):
    for p, comm, user in server_procs():
        os.kill(p, signal.SIGTERM)
    end = time.time() + 10
    while time.time() < end and os.path.exists("/proc/%d" % pid):
        time.sleep(0.2)
    return not os.path.exists("/run/vncsession-%s.pid" % display)


# ------------------------------------------------------------- Wayland
set_config("mode=wayland\nsession=vnc-test\n"
           "securitytypes=VncAuth,TLSVnc\ngeometry=1024x768\nlocalhost\n")

state = {}


def t_wayland_start():
    rc, pid, out = start_session(":1")
    state["pid"] = pid
    up = wait_tcp(5901, timeout=20)
    procs = server_procs()
    srv = [p for p in procs if p[1] == "Xvnc-wayland" and p[2] == USER]
    # The server's initial environment comes from vncsession (vncserver
    # does not set XDG_SESSION_TYPE), i.e. the value PAM was opened with
    env = proc_env(srv[0][0], USER) if srv else {}
    ok = (rc == 0 and pid and up and srv and
          env.get("XDG_SESSION_TYPE") == "wayland" and
          env.get("XDG_SESSION_CLASS") == "user")
    return ok, ("vncsession rc=%d pid=%s, port 5901 up=%s, servers=%r, "
                "server env from vncsession: XDG_SESSION_TYPE=%r "
                "XDG_SESSION_CLASS=%r %s" %
                (rc, pid, up, procs, env.get("XDG_SESSION_TYPE"),
                 env.get("XDG_SESSION_CLASS"), out))


def t_wayland_session_env():
    path = os.path.join(HOME, "wayland-session.env")
    if not wait_file(path, 20):
        return False, "session script did not run"
    env = read_env_file(path)
    keys = ("USER", "XDG_SESSION_TYPE", "WAYLAND_DISPLAY", "DISPLAY",
            "XDG_CURRENT_DESKTOP", "XDG_SESSION_DESKTOP",
            "DBUS_SESSION_BUS_ADDRESS")
    ok = (env.get("USER") == USER and
          env.get("XDG_SESSION_TYPE") == "wayland" and
          env.get("WAYLAND_DISPLAY") == "wayland-1" and
          env.get("DISPLAY", "").startswith(":") and
          env.get("XDG_CURRENT_DESKTOP") == "VncTest" and
          env.get("DBUS_SESSION_BUS_ADDRESS"))
    return ok, ", ".join("%s=%s" % (k, env.get(k)) for k in keys)


def t_wayland_view():
    c = RFBClient(port=5901, security="TLSVnc", password=PASSWORD)
    end = time.time() + 20
    while time.time() < end:
        time.sleep(1)
        c.refresh()
        foot = find_colour_bbox(c, (0x24, 0x24, 0x24))
        xterm = find_colour_bbox(c, (255, 255, 255), region=(0, 0, 400, 200))
        if foot and xterm:
            break
    shot = c.screenshot(s.artifact("vncsession-01-wayland-session.png"))
    c.close()
    return bool(foot and xterm), "foot area %r, xterm area %r" % (
        foot, xterm), [shot]


def t_wayland_log():
    log = os.path.join(HOME, ".local/state/tigervnc/%s:1.log" % HOST)
    text = open(log).read() if os.path.exists(log) else ""
    with open(s.artifact("logs/vncsession-wayland-user.log"), "w") as f:
        f.write(text)
    ok = ("Starting Wayland desktop session vnc-test" in text and
          "Wayland display wayland-1" in text)
    return ok, "%s (copied to logs/vncsession-wayland-user.log)" % log


def t_wayland_stop():
    ok = stop_session(state["pid"], ":1")
    return ok, "vncsession exited and removed its pid file after the " \
        "server stopped"


s.run("vncsession with mode=wayland starts Xvnc-wayland as the user, "
      "XDG_SESSION_TYPE=wayland for PAM", t_wayland_start)
s.run("Wayland session environment", t_wayland_session_env)
s.run("Wayland session visible over VNC (foot + xterm)", t_wayland_view)
s.run("session log", t_wayland_log)
s.run("stop", t_wayland_stop)

# ------------------------------------------------------------- X11
set_config("session=vnc-test-x11\nsecuritytypes=VncAuth\n"
           "geometry=1024x768\nlocalhost\n")


def t_x11_default():
    rc, pid, out = start_session(":2")
    state["pid2"] = pid
    up = wait_tcp(5902, timeout=20)
    procs = server_procs()
    srv = [p for p in procs if p[1] == "Xvnc" and p[2] == USER]
    env = proc_env(srv[0][0], USER) if srv else {}
    path = os.path.join(HOME, "x11-session.env")
    wait_file(path, 20)
    senv = read_env_file(path) if os.path.exists(path) else {}
    c = RFBClient(port=5902, security="VncAuth", password=PASSWORD)
    time.sleep(2)
    c.refresh()
    shot = c.screenshot(s.artifact("vncsession-02-x11-default.png"))
    c.close()
    ok = (rc == 0 and up and env.get("XDG_SESSION_TYPE") == "x11" and
          bool(srv) and senv.get("DISPLAY") == ":2" and
          "WAYLAND_DISPLAY" not in senv)
    return ok, ("no mode: server env XDG_SESSION_TYPE=%r, servers=%r, "
                "session DISPLAY=%r" % (env.get("XDG_SESSION_TYPE"), procs,
                                        senv.get("DISPLAY"))), [shot]


s.run("vncsession without mode keeps X11 (Xvnc + Xsession)", t_x11_default)
stop_session(state.get("pid2") or 0, ":2")


# ------------------------------------------- stock sway session, nested
def sway_ipc():
    for pat in ("/run/user/%d/sway-ipc.*.sock" % pw.pw_uid,
                "/tmp/runtime-%d/sway-ipc.*.sock" % pw.pw_uid):
        found = glob.glob(pat)
        if found:
            return found[0]
    return None


def swaymsg(*args):
    sock = sway_ipc()
    if not sock:
        return None
    r = subprocess.run(["runuser", "-u", USER, "--", "env",
                        "SWAYSOCK=" + sock, "swaymsg", "-r"] + list(args),
                       capture_output=True, timeout=10)
    return r.stdout.decode()


def sway_output_sizes():
    import json
    try:
        return [(o["name"], o["current_mode"]["width"],
                 o["current_mode"]["height"])
                for o in json.loads(swaymsg("-t", "get_outputs") or "")
                if o.get("active")]
    except (ValueError, KeyError):
        return None


def t_sway_session():
    set_config("mode=wayland\nsession=sway\nsecuritytypes=VncAuth\n"
               "geometry=1024x768\nlocalhost\n")
    rc, pid, out = start_session(":6")
    state["pid6"] = pid
    up = wait_tcp(5906, timeout=20)
    end = time.time() + 20
    outs = None
    while time.time() < end:
        outs = sway_output_sizes()
        if outs:
            break
        time.sleep(0.5)
    log = os.path.join(HOME, ".local/state/tigervnc/%s:6.log" % HOST)
    text = open(log).read() if os.path.exists(log) else ""
    with open(s.artifact("logs/vncsession-sway-user.log"), "w") as f:
        f.write(text)
    nested = [l.strip() for l in text.splitlines()
              if "nested compositor" in l]
    c = RFBClient(port=5906, security="VncAuth", password=PASSWORD)
    time.sleep(2)
    c.refresh()
    shot = c.screenshot(s.artifact("vncsession-03-sway-session.png"))
    c.close()
    ok = (rc == 0 and up and outs == [("WL-1", 1024, 768)] and
          "Starting Wayland desktop session sway" in text and bool(nested))
    return ok, ("stock sway.desktop via vncsession: rc=%d, sway outputs "
                "%r, server log %r (log copied to "
                "logs/vncsession-sway-user.log)" % (rc, outs, nested)), \
        [shot]


def t_sway_session_input():
    # sway's default config: Super+Return opens its terminal (foot)
    keyfile = os.path.join(HOME, "sway-key.txt")
    c = RFBClient(port=5906, security="VncAuth", password=PASSWORD)
    c.refresh()
    c.pointer(500, 400, 0)
    c.key(0xffeb, True)
    c.tap(0xff0d)
    c.key(0xffeb, False)
    end = time.time() + 15
    while time.time() < end and "foot" not in (swaymsg("-t", "get_tree")
                                               or ""):
        time.sleep(0.5)
    time.sleep(1)
    c.type_text("echo \"sway-session-$((6*7))-${SWAYSOCK:+sway}\" > %s\n"
                % keyfile)
    ok = wait_file(keyfile, 5)
    content = open(keyfile).read().strip() if ok else ""
    c.refresh()
    shot = c.screenshot(s.artifact("vncsession-04-sway-terminal.png"))
    c.set_desktop_size(1280, 800)
    end = time.time() + 10
    outs = None
    while time.time() < end:
        outs = sway_output_sizes()
        if outs == [("WL-1", 1280, 800)]:
            break
        time.sleep(0.5)
    c.close()
    ok = content == "sway-session-42-sway" and outs == [("WL-1", 1280, 800)]
    return ok, ("Super+Return + typed command in sway's terminal: %r; "
                "after resize to 1280x800 sway outputs %r"
                % (content, outs)), [shot]


s.run("vncsession starts the stock sway session (sway.desktop) as a "
      "nested compositor", t_sway_session)
s.run("stock sway session: Super+Return opens foot, typed command runs "
      "inside sway, resize follows", t_sway_session_input)
stop_session(state.get("pid6") or 0, ":6")


def t_explicit_x11():
    set_config("mode=x11\nsession=vnc-test-x11\nsecuritytypes=VncAuth\n"
               "geometry=1024x768\nlocalhost\n")
    rc, pid, out = start_session(":3")
    up = wait_tcp(5903, timeout=20)
    procs = server_procs()
    srv = [p for p in procs if p[1] == "Xvnc" and p[2] == USER]
    env = proc_env(srv[0][0], USER) if srv else {}
    rc2, args = sh(["ps", "-o", "args=", "-p", str(srv[0][0])]) if srv \
        else (1, "")
    ok = (rc == 0 and up and bool(srv) and
          env.get("XDG_SESSION_TYPE") == "x11" and
          "--wayland" not in args and "-mode" not in args)
    stop_session(pid or 0, ":3")
    return ok, ("mode=x11: servers=%r, XDG_SESSION_TYPE=%r, Xvnc args "
                "%r" % (procs, env.get("XDG_SESSION_TYPE"), args.strip()))


def t_invalid_mode():
    set_config("mode=wayland2\nsession=vnc-test\nsecuritytypes=VncAuth\n"
               "localhost\n")
    rc, pid, out = start_session(":4")
    time.sleep(2)
    procs = server_procs()
    ok = rc != 0 and not procs and \
        not os.path.exists("/run/vncsession-:4.pid")
    # vncserver itself (e.g. run by hand) validates the same way
    r = subprocess.run(["runuser", "-u", USER, "--", "env",
                        "HOME=" + HOME, "USER=" + USER,
                        "/usr/libexec/vncserver", ":4"],
                       capture_output=True, timeout=20)
    msg = (r.stdout + r.stderr).decode().strip()
    ok = ok and r.returncode != 0 and "Invalid mode 'wayland2'" in msg
    return ok, ("vncsession rc=%d, servers=%r; vncserver rc=%d: %r" %
                (rc, procs, r.returncode, msg))


def t_mode_case():
    set_config("mode = Wayland \nsession=vnc-test\n"
               "securitytypes=VncAuth\nlocalhost\n")
    rc, pid, out = start_session(":5")
    up = wait_tcp(5905, timeout=20)
    procs = server_procs()
    srv = [p for p in procs if p[1] == "Xvnc-wayland"]
    env = proc_env(srv[0][0], USER) if srv else {}
    stop_session(pid or 0, ":5")
    return (rc == 0 and up and bool(srv) and
            env.get("XDG_SESSION_TYPE") == "wayland"), \
        "'mode = Wayland ': servers=%r XDG_SESSION_TYPE=%r" % (
            procs, env.get("XDG_SESSION_TYPE"))


s.run("explicit mode=x11 behaves like the default", t_explicit_x11)
s.run("invalid mode is rejected by vncsession and vncserver",
      t_invalid_mode)
s.run("mode value is case-insensitive and whitespace-trimmed", t_mode_case)
sys.exit(s.finish())
