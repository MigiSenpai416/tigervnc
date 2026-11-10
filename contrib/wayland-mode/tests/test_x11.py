#!/usr/bin/env python3
"""X11 regression tests: plain "Xvnc" (and "Xvnc --x11") must behave like
upstream Xvnc. Run as an unprivileged user inside the test image."""

import os
import subprocess
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from harness import (Suite, wait_tcp, wait_file, sh, find_colour_bbox,
                     make_vnc_passwd, make_cert)
from rfbclient import RFBClient, RFBError

PASSWORD = "x11-secret"
WORK = "/tmp/x11test"
XT_X, XT_Y = 100, 100
XT_CLICK = (XT_X + 150, XT_Y + 80)

s = Suite("x11")
os.makedirs(WORK, exist_ok=True)
passwd = os.path.join(WORK, "passwd")
cert = os.path.join(WORK, "cert.pem")
key = os.path.join(WORK, "key.pem")
make_vnc_passwd(passwd, PASSWORD)
make_cert(cert, key)

env = {k: v for k, v in os.environ.items()
       if k not in ("DISPLAY", "WAYLAND_DISPLAY")}


def t_no_wlroots():
    rc, out = sh(["ldd", "/usr/bin/Xvnc"])
    bad = [l for l in out.splitlines() if "wlroots" in l or
           "wayland-server" in l]
    return rc == 0 and not bad, "Xvnc links wlroots/wayland-server: %r" % bad


def t_help():
    r = subprocess.run(["Xvnc", "-help"], capture_output=True, timeout=10)
    out = (r.stdout + r.stderr).decode()
    with open(s.artifact("logs/x11-Xvnc-help.txt"), "w") as f:
        f.write(out)
    ok = "--wayland" in out and "rfbport" in out and "-geometry" in out \
        and "-auth file" in out
    return ok, "Xvnc -help lists X server options, VNC parameters and " \
        "the --x11/--wayland lines (logs/x11-Xvnc-help.txt)"


def t_version():
    r = subprocess.run(["Xvnc", "-version"], capture_output=True, timeout=10)
    out = (r.stdout + r.stderr).decode()
    ok = "Xvnc TigerVNC" in out and "Underlying X server release" in out
    return ok, " / ".join(l for l in out.strip().splitlines()
                          if "TigerVNC" in l or "X server" in l)


s.run("Xvnc does not link wlroots", t_no_wlroots)
s.run("Xvnc -help (X server usage + mode flags)", t_help)
s.run("Xvnc -version", t_version)

server = s.spawn("x11-server", [
    "Xvnc", ":2", "-geometry", "1024x768", "-depth", "24",
    "-desktop", "x11-test", "-rfbauth", passwd,
    "-SecurityTypes", "VncAuth,X509Vnc", "-X509Cert", cert, "-X509Key", key,
    "-localhost"], env=env)
xenv = dict(env, DISPLAY=":2")


def t_start():
    ok = wait_tcp(5902, timeout=20)
    rc, out = sh("xdpyinfo | grep -E 'dimensions|depth of root'", env=xenv)
    return ok and "1024x768" in out, "port 5902 up=%s; %s" % (
        ok, " ".join(out.split()))


s.run("Xvnc :2 starts (no flag)", t_start)


def t_auth():
    c = RFBClient(port=5902, security="VncAuth", password=PASSWORD)
    c.close()
    try:
        RFBClient(port=5902, security="VncAuth", password="nope")
        return False, "wrong password accepted"
    except RFBError:
        pass
    c = RFBClient(port=5902, security="X509Vnc", password=PASSWORD)
    tls = c.tls_version
    c.close()
    return True, "VncAuth ok, wrong password rejected, X509Vnc ok (%s)" % tls


s.run("auth: VncAuth + X509Vnc, wrong password rejected", t_auth)

vnc = RFBClient(port=5902, security="VncAuth", password=PASSWORD)
s.spawn("x11-xterm", ["xterm", "-geometry", "60x12+%d+%d" % (XT_X, XT_Y),
                      "-bg", "white", "-fg", "black", "-e", "bash", "--norc",
                      "--noprofile"], env=xenv)
s.spawn("x11-xeyes", ["xeyes", "-geometry", "160x120+700+40"], env=xenv)


def t_render():
    end = time.time() + 20
    box = None
    while time.time() < end:
        time.sleep(1)
        vnc.refresh()
        box = find_colour_bbox(vnc, (255, 255, 255), region=(0, 0, 600, 600))
        if box:
            break
    shot = vnc.screenshot(s.artifact("x11-01-xterm-xeyes.png"))
    ok = box is not None and abs(box[0] - XT_X) <= 4 and \
        abs(box[1] - XT_Y) <= 4 and vnc.colour_count(700, 40, 160, 120) >= 3
    return ok, "xterm area %r, xeyes colours %d" % (
        box, vnc.colour_count(700, 40, 160, 120)), [shot]


def t_keyboard():
    vnc.click(*XT_CLICK)
    time.sleep(0.5)
    vnc.type_text("echo \"x11-key-ok-$((6*7))-$XTERM_VERSION\" > "
                  "%s/key.txt\n" % WORK)
    ok = wait_file(WORK + "/key.txt", 5)
    content = open(WORK + "/key.txt").read().strip() if ok else ""
    vnc.refresh()
    shot = vnc.screenshot(s.artifact("x11-02-typed.png"))
    return content.startswith("x11-key-ok-42-XTerm("), \
        "file content %r" % content, [shot]


def t_pointer():
    vnc.type_text("clear; echo PTRX11TEST\n")
    time.sleep(1)
    vnc.pointer(XT_X + 20, XT_Y + 8, 0)
    time.sleep(0.05)
    for _ in range(2):
        vnc.pointer(XT_X + 20, XT_Y + 8, 1)
        time.sleep(0.03)
        vnc.pointer(XT_X + 20, XT_Y + 8, 0)
        time.sleep(0.03)
    time.sleep(0.5)
    rc, sel = sh(["xclip", "-o", "-selection", "primary"], env=xenv)
    vnc.pointer(333, 444, 0)
    time.sleep(0.5)
    rc, loc = sh(["xdotool", "getmouselocation"], env=xenv)
    vnc.refresh()
    shot = vnc.screenshot(s.artifact("x11-03-selected.png"))
    return sel == "PTRX11TEST" and "x:333 y:444" in loc, \
        "PRIMARY=%r pointer=%r" % (sel, loc.strip()), [shot]


def t_clipboard():
    vnc.cut_text("x11-from-vnc")
    time.sleep(1)
    rc, c = sh(["xclip", "-o", "-selection", "clipboard"], env=xenv)
    vnc.cut_texts.clear()
    s.spawn("x11-xclip", ["sh", "-c", "printf x11-from-xclip | xclip -i "
                          "-loops 3 -selection clipboard"], env=xenv)
    txt = vnc.wait_cut_text(5)
    return c == "x11-from-vnc" and txt == "x11-from-xclip", \
        "viewer->X CLIPBOARD=%r, X->viewer=%r" % (c, txt)


def t_resize():
    vnc.set_desktop_size(1280, 720)
    end = time.time() + 5
    while time.time() < end and (vnc.width, vnc.height) != (1280, 720):
        vnc.request_update(True)
        vnc.pump(0.3)
    time.sleep(1)
    vnc.refresh()
    shot = vnc.screenshot(s.artifact("x11-04-resized-1280x720.png"))
    rc, out = sh("xdpyinfo | grep dimensions", env=xenv)
    return (vnc.width, vnc.height) == (1280, 720) and "1280x720" in out, \
        "viewer %dx%d, X screen %s" % (vnc.width, vnc.height,
                                       " ".join(out.split())), [shot]


s.run("X11 clients render (xterm, xeyes)", t_render)
s.run("keyboard input reaches xterm", t_keyboard)
s.run("pointer input reaches xterm (selection, pointer position)", t_pointer)
s.run("clipboard both ways (X11)", t_clipboard)
s.run("client resize (X11, RandR)", t_resize)
vnc.close()


def t_x11_flag():
    p = s.spawn("x11-flag-server", ["Xvnc", "--x11", ":3", "-geometry",
                                    "800x600", "-SecurityTypes", "VncAuth",
                                    "-rfbauth", passwd, "-localhost"],
                env=env)
    ok = wait_tcp(5903, timeout=20)
    rc, out = sh("xdpyinfo | grep -E 'vendor string|dimensions'",
                 env=dict(env, DISPLAY=":3"))
    c = RFBClient(port=5903, security="VncAuth", password=PASSWORD)
    c.refresh()
    shot = c.screenshot(s.artifact("x11-05-x11-flag-800x600.png"))
    c.close()
    os.killpg(p.pid, 15)
    return ok and "800x600" in out and (c.width, c.height) == (800, 600), \
        "Xvnc --x11 :3 -> %s" % " ".join(out.split()), [shot]


def t_both_flags():
    r = subprocess.run(["Xvnc", "--x11", "--wayland", ":4"],
                       capture_output=True, timeout=10)
    out = (r.stdout + r.stderr).decode()
    return r.returncode != 0 and "cannot be combined" in out, out.strip()


s.run("Xvnc --x11 runs the X server", t_x11_flag)
s.run("Xvnc --x11 --wayland is rejected", t_both_flags)
sys.exit(s.finish())
