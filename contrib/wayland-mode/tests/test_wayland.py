#!/usr/bin/env python3
"""Wayland mode tests: "Xvnc --wayland" end to end through RFB.

Run as an unprivileged user inside the test image. Results go to
$ARTIFACTS/results-wayland.json, screenshots to $ARTIFACTS/wayland-*.png.
"""

import os
import socket
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from harness import (Suite, wait_tcp, wait_file, sh, read_env_file,
                     find_colour_bbox, make_vnc_passwd, make_cert)
from rfbclient import RFBClient, RFBError

DISPLAY_NUM = 1
PORT = 5900 + DISPLAY_NUM
PASSWORD = "wl-secret"
WORK = "/tmp/wltest"
FOOT_BG = (0x24, 0x24, 0x24)    # foot's default background
XTERM_GEOM = "50x10+560+430"    # X11 windows are placed where they ask
XTERM_X, XTERM_Y = 560, 430
# a point inside xterm that foot (centred, 72x16 cells) never covers
XTERM_CLICK = (XTERM_X + 200, XTERM_Y + 110)

s = Suite("wayland")
os.makedirs(WORK, exist_ok=True)
runtime = os.path.join(WORK, "xdg-runtime")
os.makedirs(runtime, mode=0o700, exist_ok=True)

passwd = os.path.join(WORK, "passwd")
cert = os.path.join(WORK, "cert.pem")
key = os.path.join(WORK, "key.pem")
make_vnc_passwd(passwd, PASSWORD)
make_cert(cert, key)

session_env = os.path.join(WORK, "session.env")
session_sh = os.path.join(WORK, "session.sh")
with open(session_sh, "w") as f:
    f.write("#!/bin/sh\nenv > %s.tmp && mv %s.tmp %s\nexec sleep infinity\n"
            % (session_env, session_env, session_env))
os.chmod(session_sh, 0o755)

# A clean, display-less environment: nothing graphical is running
base_env = {k: v for k, v in os.environ.items()
            if k not in ("DISPLAY", "WAYLAND_DISPLAY", "XDG_SESSION_TYPE")}
base_env["XDG_RUNTIME_DIR"] = runtime
base_env["HOME"] = os.environ.get("HOME", WORK)

server_cmd = ["Xvnc", "--wayland", ":%d" % DISPLAY_NUM,
              "-geometry", "1024x768", "-depth", "24",
              "-desktop", "wayland-test",
              "-rfbauth", passwd,
              "-SecurityTypes", "VncAuth,X509Vnc,TLSVnc",
              "-X509Cert", cert, "-X509Key", key,
              "-localhost",
              "--", session_sh]
with open(s.artifact("logs/wayland-server-cmd.txt"), "w") as f:
    f.write(" ".join(server_cmd) + "\n")
server = s.spawn("wayland-server", server_cmd, env=base_env)
log_path = s.artifact("logs/wayland-server.log")

app_env = None


def t_headless_start():
    global app_env
    if not wait_tcp(PORT, timeout=20):
        return False, "port %d not listening; server exit=%s" % (
            PORT, server.poll())
    if not wait_file(session_env, timeout=20):
        return False, "session command did not start"
    env = read_env_file(session_env)
    app_env = dict(base_env)
    for k in ("WAYLAND_DISPLAY", "DISPLAY", "XDG_SESSION_TYPE"):
        if k in env:
            app_env[k] = env[k]
    ok = (env.get("WAYLAND_DISPLAY") == "wayland-%d" % DISPLAY_NUM and
          env.get("DISPLAY", "").startswith(":") and
          env.get("XDG_SESSION_TYPE") == "wayland" and
          not os.path.exists("/dev/dri") and
          server.poll() is None)
    return ok, ("session env WAYLAND_DISPLAY=%s DISPLAY=%s "
                "XDG_SESSION_TYPE=%s; /dev/dri present=%s" %
                (env.get("WAYLAND_DISPLAY"), env.get("DISPLAY"),
                 env.get("XDG_SESSION_TYPE"), os.path.exists("/dev/dri")))


s.run("headless start (no GPU, no parent display), session started with "
      "WAYLAND_DISPLAY+DISPLAY", t_headless_start)
if app_env is None:
    sys.exit(s.finish())


def t_vncauth():
    c = RFBClient(port=PORT, security="VncAuth", password=PASSWORD)
    detail = "offered=%r name=%r size=%dx%d" % (c.offered_types, c.name,
                                                c.width, c.height)
    c.close()
    return (c.width, c.height) == (1024, 768) and c.name == "wayland-test", \
        detail


def t_vncauth_wrong():
    try:
        RFBClient(port=PORT, security="VncAuth", password="wrong-pw")
    except RFBError as e:
        return "authentication failed" in str(e), "rejected: %s" % e
    return False, "wrong password was accepted"


def t_x509vnc():
    c = RFBClient(port=PORT, security="X509Vnc", password=PASSWORD)
    detail = "subtypes=%r tls=%s cipher=%s cert=%d bytes" % (
        c.offered_subtypes, c.tls_version, c.tls_cipher[0],
        len(c.server_cert or b""))
    c.close()
    return bool(c.server_cert), detail


def t_tlsvnc():
    c = RFBClient(port=PORT, security="TLSVnc", password=PASSWORD)
    detail = "tls=%s cipher=%s" % (c.tls_version, c.tls_cipher[0])
    c.close()
    return True, detail


def t_none_refused():
    try:
        RFBClient(port=PORT, security="None")
    except RFBError as e:
        return "not offered" in str(e), "%s" % e
    return False, "security type None was accepted"


def t_localhost():
    # -localhost: not reachable on the container's external address
    ip = socket.gethostbyname(socket.gethostname())
    if ip.startswith("127."):
        return False, "could not determine a non-loopback address"
    try:
        socket.create_connection((ip, PORT), 2).close()
    except OSError as e:
        return True, "connect to %s:%d refused (%s)" % (ip, PORT, e)
    return False, "server reachable on %s" % ip


s.run("auth: VncAuth with correct password", t_vncauth)
s.run("auth: VncAuth with wrong password rejected", t_vncauth_wrong)
s.run("auth: X509Vnc (VeNCrypt, X.509 TLS + VNC password)", t_x509vnc)
s.run("auth: TLSVnc (VeNCrypt, anonymous TLS + VNC password)", t_tlsvnc)
s.run("auth: security type None not offered when not configured",
      t_none_refused)
s.run("-localhost: not reachable on external address", t_localhost)

# Main viewer connection used for the rest of the tests
vnc = RFBClient(port=PORT, security="X509Vnc", password=PASSWORD)
vnc.refresh()
vnc.screenshot(s.artifact("wayland-01-empty-desktop.png"))

foot_box = None


def t_foot_render():
    global foot_box
    s.spawn("foot", ["foot", "--title", "foot-test", "-W", "72x16",
                     "bash", "--norc", "--noprofile"], env=app_env)
    end = time.time() + 15
    while time.time() < end:
        time.sleep(1)
        vnc.refresh()
        foot_box = find_colour_bbox(vnc, FOOT_BG)
        if foot_box and (foot_box[2] - foot_box[0]) > 300:
            break
    shot = vnc.screenshot(s.artifact("wayland-02-foot.png"))
    if not foot_box:
        return False, "no foot window found in framebuffer", [shot]
    w, h = foot_box[2] - foot_box[0], foot_box[3] - foot_box[1]
    return w > 300 and h > 150, "foot content area %r (%dx%d)" % (
        foot_box, w, h), [shot]


def t_xterm_render():
    s.spawn("xterm", ["xterm", "-geometry", XTERM_GEOM, "-bg", "white",
                      "-fg", "black", "-e", "bash", "--norc",
                      "--noprofile"], env=app_env)
    s.spawn("xeyes", ["xeyes", "-geometry", "160x120+20+20"], env=app_env)
    t0 = time.time()
    end = t0 + 20
    while time.time() < end:
        time.sleep(1)
        vnc.refresh()
        white = find_colour_bbox(vnc, (255, 255, 255),
                                 region=(300, 300, vnc.width, vnc.height))
        if white and vnc.colour_count(20, 20, 160, 120) >= 3:
            break
    shot = vnc.screenshot(s.artifact("wayland-03-foot-xterm-xeyes.png"))
    rc, out = sh(["xdotool", "search", "--class", "XTerm"], env=app_env)
    ok_x = white is not None and abs(white[0] - XTERM_X) <= 4 and \
        abs(white[1] - XTERM_Y) <= 4
    # xeyes: something other than the desktop background in its area
    eyes = vnc.colour_count(20, 20, 160, 120)
    return ok_x and eyes >= 3 and rc == 0, \
        "xterm white area %r (want origin %d,%d), xdotool windows=%r, " \
        "xeyes colours=%d, appeared after %.1fs" % (
            white, XTERM_X, XTERM_Y, out.split(), eyes, time.time() - t0), \
        [shot]


s.run("Wayland client renders (foot)", t_foot_render)
s.run("X11 clients render via Xwayland (xterm, xeyes)", t_xterm_render)


def foot_point(dx, dy):
    return foot_box[0] + dx, foot_box[1] + dy


def t_wl_keyboard():
    # focus foot by clicking in an area xterm does not cover
    vnc.click(*foot_point(40, 40))
    time.sleep(0.5)
    # $TERM tells which terminal really ran the command
    vnc.type_text("cd /tmp && echo \"wl-key-ok-$((6*7))-$TERM\" > "
                  "%s/wl_key.txt\n" % WORK)
    ok = wait_file(WORK + "/wl_key.txt", 5)
    content = open(WORK + "/wl_key.txt").read().strip() if ok else ""
    vnc.refresh()
    shot = vnc.screenshot(s.artifact("wayland-04-foot-typed.png"))
    return content == "wl-key-ok-42-foot", "file content %r" % content, [shot]


def t_x_keyboard():
    vnc.click(*XTERM_CLICK)
    time.sleep(0.5)
    # XTERM_VERSION is only set inside xterm
    vnc.type_text("echo \"x-key-ok-$((6*7))-$XTERM_VERSION\" > "
                  "%s/x_key.txt\n" % WORK)
    ok = wait_file(WORK + "/x_key.txt", 5)
    content = open(WORK + "/x_key.txt").read().strip() if ok else ""
    vnc.refresh()
    shot = vnc.screenshot(s.artifact("wayland-05-xterm-typed.png"))
    return content.startswith("x-key-ok-42-XTerm("), \
        "file content %r" % content, [shot]


s.run("keyboard input reaches Wayland client (foot)", t_wl_keyboard)
s.run("keyboard input reaches X11 client (xterm via Xwayland)", t_x_keyboard)


def double_click(x, y):
    vnc.pointer(x, y, 0)
    time.sleep(0.05)
    for _ in range(2):
        vnc.pointer(x, y, 1)
        time.sleep(0.03)
        vnc.pointer(x, y, 0)
        time.sleep(0.03)


def t_wl_pointer():
    vnc.click(*foot_point(40, 40))
    time.sleep(0.3)
    vnc.type_text("clear; echo PTRWLTEST\n")
    time.sleep(1)
    vnc.cut_texts.clear()
    # first line of the terminal: double click selects the word
    double_click(*foot_point(25, 7))
    time.sleep(1)
    rc, out = sh(["wl-paste", "--primary", "--no-newline"], env=app_env)
    vnc.refresh()
    shot = vnc.screenshot(s.artifact("wayland-06-foot-selected.png"))
    return out == "PTRWLTEST", \
        "primary selection after double click in foot: %r" % out, [shot]


def t_x_pointer():
    vnc.click(*XTERM_CLICK)
    time.sleep(0.3)
    vnc.type_text("clear; echo PTRXTEST\n")
    time.sleep(1)
    double_click(XTERM_X + 20, XTERM_Y + 8)
    time.sleep(1)
    rc1, xsel = sh(["xclip", "-o", "-selection", "primary"], env=app_env)
    rc2, wsel = sh(["wl-paste", "--primary", "--no-newline"], env=app_env)
    vnc.pointer(XTERM_X + 77, XTERM_Y + 33, 0)
    time.sleep(0.5)
    rc3, loc = sh(["xdotool", "getmouselocation"], env=app_env)
    vnc.refresh()
    shot = vnc.screenshot(s.artifact("wayland-07-xterm-selected.png"))
    want_loc = "x:%d y:%d" % (XTERM_X + 77, XTERM_Y + 33)
    ok = xsel == "PTRXTEST" and wsel == "PTRXTEST" and want_loc in loc
    return ok, ("X PRIMARY=%r, Wayland primary=%r, X pointer=%r (want %s)"
                % (xsel, wsel, loc.strip(), want_loc)), [shot]


s.run("pointer motion+buttons reach Wayland client (foot word "
      "selection by double click)", t_wl_pointer)
s.run("pointer motion+buttons reach X11 client (xterm selection, "
      "xdotool pointer position)", t_x_pointer)


def t_clip_wl_to_vnc():
    vnc.cut_texts.clear()
    s.spawn("wl-copy", ["wl-copy", "--foreground", "from-wayland-123"],
            env=app_env)
    txt = vnc.wait_cut_text(5)
    return txt == "from-wayland-123", "viewer received %r" % txt


def t_primary_wl_to_vnc():
    vnc.cut_texts.clear()
    s.spawn("wl-copy-p", ["wl-copy", "--foreground", "--primary",
                          "primary-wayland-321"], env=app_env)
    txt = vnc.wait_cut_text(5)
    return txt == "primary-wayland-321", \
        "viewer received %r (SendPrimary default on)" % txt


def focus_xterm():
    # wlroots only lets X11 clients use the clipboard while an X11
    # window has keyboard focus, as when a user pastes into it
    vnc.click(*XTERM_CLICK)
    time.sleep(0.5)


def t_clip_vnc_to_wl():
    vnc.cut_text("from-vnc-456")
    time.sleep(1)
    rc1, c = sh(["wl-paste", "--no-newline"], env=app_env)
    rc2, p = sh(["wl-paste", "--primary", "--no-newline"], env=app_env)
    focus_xterm()
    rc3, xc = sh(["xclip", "-o", "-selection", "clipboard"], env=app_env)
    rc4, xp = sh(["xclip", "-o", "-selection", "primary"], env=app_env)
    ok = c == p == xc == xp == "from-vnc-456"
    return ok, ("Wayland clipboard=%r primary=%r; X11 CLIPBOARD=%r "
                "PRIMARY=%r (SetPrimary default on)" % (c, p, xc, xp))


def t_clip_x_to_vnc():
    focus_xterm()
    vnc.cut_texts.clear()
    s.spawn("xclip", ["sh", "-c", "printf from-x11-789 | xclip -i -loops 3 "
                      "-selection clipboard"], env=app_env)
    txt = vnc.wait_cut_text(5)
    return txt == "from-x11-789", "viewer received %r" % txt


def t_clip_paste_into_foot():
    # RFB cut text pasted into a Wayland app with the keyboard
    vnc.cut_text("echo pasted-$((40+2)) > %s/paste.txt" % WORK)
    time.sleep(0.5)
    vnc.click(*foot_point(40, 60))
    time.sleep(0.3)
    vnc.key(0xffe3, True)   # Ctrl+Shift+V: foot's paste binding
    vnc.key(0xffe1, True)
    vnc.tap(ord("V"))
    vnc.key(0xffe1, False)
    vnc.key(0xffe3, False)
    time.sleep(0.3)
    vnc.tap(0xff0d)
    ok = wait_file(WORK + "/paste.txt", 5)
    content = open(WORK + "/paste.txt").read().strip() if ok else ""
    return content == "pasted-42", "file content %r" % content


s.run("clipboard Wayland -> viewer (wl-copy -> ServerCutText)",
      t_clip_wl_to_vnc)
s.run("primary selection Wayland -> viewer (wl-copy --primary)",
      t_primary_wl_to_vnc)
s.run("clipboard viewer -> Wayland+X11 (ClientCutText -> wl-paste, "
      "wl-paste --primary, xclip)", t_clip_vnc_to_wl)
s.run("clipboard X11 app -> viewer (xclip via Xwayland -> "
      "ServerCutText)", t_clip_x_to_vnc)
s.run("clipboard viewer -> paste into foot with Ctrl+Shift+V",
      t_clip_paste_into_foot)


def output_mode():
    """Current wl_output mode as a Wayland client sees it."""
    import re
    rc, out = sh(["wayland-info", "-i", "wl_output"], env=app_env)
    m = re.search(r"width: (\d+) px, height: (\d+) px, refresh: [^\n]*\n\s*"
                  r"flags: current", out)
    if not m:
        return out[-300:]
    return (int(m.group(1)), int(m.group(2)))


def t_resize():
    other = RFBClient(port=PORT, security="VncAuth", password=PASSWORD)
    other.refresh()
    vnc.ext_desktop_size.clear()
    vnc.set_desktop_size(1280, 720)
    end = time.time() + 5
    while time.time() < end and (vnc.width, vnc.height) != (1280, 720):
        vnc.request_update(True)
        vnc.pump(0.3)
    replies = [r for r in vnc.ext_desktop_size if r[0] == 1]
    status = replies[0][1] if replies else None
    time.sleep(1)
    vnc.refresh()
    shot = vnc.screenshot(s.artifact("wayland-08-resized-1280x720.png"))
    other.request_update(True)
    other.pump(1.5)
    mode_line = output_mode()
    rc2, xd = sh("xdpyinfo | grep dimensions", env=app_env)
    ok = ((vnc.width, vnc.height) == (1280, 720) and status == 0 and
          (other.width, other.height) == (1280, 720) and
          mode_line == (1280, 720) and "1280x720" in xd)
    other.close()
    return ok, ("viewer fb %dx%d, ExtendedDesktopSize reply status=%r, "
                "second viewer %dx%d, wl_output mode %r, Xwayland %r" %
                (vnc.width, vnc.height, status, other.width, other.height,
                 mode_line, xd.strip())), [shot]


def t_resize_odd():
    vnc.set_desktop_size(1000, 700)
    end = time.time() + 5
    while time.time() < end and (vnc.width, vnc.height) != (1000, 700):
        vnc.request_update(True)
        vnc.pump(0.3)
    time.sleep(1)
    vnc.refresh()
    shot = vnc.screenshot(s.artifact("wayland-09-resized-1000x700.png"))
    mode = output_mode()
    box = find_colour_bbox(vnc, FOOT_BG)
    ok = (vnc.width, vnc.height) == (1000, 700) and mode == (1000, 700) \
        and box is not None
    return ok, "viewer fb %dx%d, wl_output mode %r, foot area %r" % (
        vnc.width, vnc.height, mode, box), [shot]


def t_input_after_resize():
    box = find_colour_bbox(vnc, FOOT_BG)
    vnc.click(box[0] + 40, box[1] + 40)
    time.sleep(0.3)
    vnc.type_text("echo after-resize > %s/resize_key.txt\n" % WORK)
    ok = wait_file(WORK + "/resize_key.txt", 5)
    content = open(WORK + "/resize_key.txt").read().strip() if ok else ""
    return content == "after-resize", "file content %r" % content


s.run("client resize 1024x768 -> 1280x720 changes the output "
      "(viewers, wl_output mode, Xwayland screen)", t_resize)
s.run("client resize to 1000x700", t_resize_odd)
s.run("input still works after resize", t_input_after_resize)


def t_weston_terminal():
    vnc.refresh()
    before_fb = bytes(vnc.fb)
    s.spawn("weston-terminal", ["weston-terminal", "--shell=/bin/bash"],
            env=app_env)
    time.sleep(4)
    vnc.request_update(True)
    vnc.pump(0.5)
    vnc.refresh()
    shot = vnc.screenshot(s.artifact("wayland-10-weston-terminal.png"))
    before = bytes(before_fb)
    changed = sum(1 for i in range(0, min(len(before), len(vnc.fb)), 16)
                  if before[i:i + 3] != vnc.fb[i:i + 3])
    rc, out = sh("ps -C weston-terminal -o pid=", env=app_env)
    return out.strip() != "" and changed > 2000, \
        "weston-terminal pid %s, %d sampled pixels changed" % (
            out.strip(), changed), [shot]


s.run("second Wayland client (weston-terminal) starts", t_weston_terminal)

vnc.close()


def t_server_alive_and_exit():
    alive = server.poll() is None
    os.killpg(server.pid, 15)
    try:
        server.wait(10)
    except Exception:
        return False, "server did not exit on SIGTERM"
    return alive, "server alive until SIGTERM, exit code %s" % server.returncode


s.run("server survived the whole run and exits on SIGTERM",
      t_server_alive_and_exit)


def t_inetd():
    import subprocess
    a, b = socket.socketpair()
    log = open(s.artifact("logs/wayland-inetd.log"), "wb")
    env = dict(base_env)
    p = subprocess.Popen(["Xvnc", "--wayland", ":7", "-inetd",
                          "-SecurityTypes", "VncAuth", "-rfbauth", passwd,
                          "-geometry", "640x480", "--", "sleep", "60"],
                         stdin=b, stdout=b, stderr=log, env=env,
                         start_new_session=True)
    b.close()
    try:
        c = RFBClient(sock=a, security="VncAuth", password=PASSWORD)
        c.refresh()
        shot = c.screenshot(s.artifact("wayland-11-inetd-640x480.png"))
        size = (c.width, c.height)
        c.close()
    finally:
        os.killpg(p.pid, 15)
        p.wait(10)
    return size == (640, 480), "RFB over inetd stdin/stdout, %dx%d" % \
        size, [shot]


s.run("-inetd (connection on stdin/stdout)", t_inetd)
sys.exit(s.finish())
