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
    for k in ("WAYLAND_DISPLAY", "DISPLAY", "XDG_SESSION_TYPE",
              "XAUTHORITY"):
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


def t_xauth():
    xa = app_env.get("XAUTHORITY", "")
    st = os.stat(xa) if xa and os.path.exists(xa) else None
    rc1, out1 = sh(["xdpyinfo"], env=app_env)
    bad = dict(app_env)
    bad["XAUTHORITY"] = os.path.join(WORK, "no-such-file")
    rc2, out2 = sh(["xdpyinfo"], env=bad)
    rc3, args = sh("ps -C Xwayland -o args=")
    ok = (st is not None and (st.st_mode & 0o077) == 0 and rc1 == 0 and
          rc2 != 0 and "-auth " + xa in args)
    return ok, ("session XAUTHORITY=%s mode=%s; xdpyinfo with it rc=%d, "
                "without it rc=%d (%s); Xwayland args %r" % (
                    xa, oct(st.st_mode & 0o777) if st else None, rc1, rc2,
                    out2.strip().splitlines()[0] if out2.strip() else "",
                    args.strip()))


s.run("Xwayland uses a private X authority file (XAUTHORITY for the "
      "session, clients without the cookie refused)", t_xauth)

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


def start_small(num, extra, session):
    import subprocess
    log = open(s.artifact("logs/wayland-server-%d.log" % num), "wb")
    return subprocess.Popen(["Xvnc", "--wayland", ":%d" % num,
                             "-SecurityTypes", "VncAuth", "-rfbauth",
                             passwd, "-localhost", "-geometry",
                             "640x480"] + extra + ["--"] + session,
                            stdout=log, stderr=subprocess.STDOUT,
                            env=base_env, start_new_session=True)


def t_persist():
    p = start_small(3, [], ["sh", "-c", "exit 3"])
    try:
        up = wait_tcp(5903, timeout=20)
        time.sleep(4)   # the session is long gone by now
        alive = p.poll() is None
        c = RFBClient(port=5903, security="VncAuth", password=PASSWORD)
        c.refresh()
        size = (c.width, c.height)
        c.close()
    finally:
        os.killpg(p.pid, 15)
        p.wait(10)
    log = open(s.artifact("logs/wayland-server-3.log")).read()
    ok = up and alive and size == (640, 480) and \
        "Session exited with status 3" in log and "keeps running" in log
    return ok, ("session exited (status 3), server still running=%s, "
                "viewer connected afterwards at %dx%d" % (alive, *size))


def t_exit_with_session():
    p = start_small(4, ["-ExitWithSession"], ["sh", "-c", "sleep 3"])
    up = wait_tcp(5904, timeout=20)
    try:
        p.wait(20)
    except Exception:
        os.killpg(p.pid, 9)
        return False, "server still running 20 s after the session ended"
    return up and p.returncode == 0, \
        "-ExitWithSession: server exited with %s after the session" % \
        p.returncode


s.run("display persists after the session command exits (like Xvnc)",
      t_persist)
s.run("-ExitWithSession stops the server with the session (used by "
      "vncserver)", t_exit_with_session)


def t_depth16():
    p = start_small(5, ["-depth", "16"], ["foot", "-W", "40x10"])
    try:
        wait_tcp(5905, timeout=20)
        c = RFBClient(port=5905, security="VncAuth", password=PASSWORD)
        pf = c.server_pf
        end = time.time() + 15
        box = None
        while time.time() < end and not box:
            time.sleep(1)
            c.refresh()
            # foot's background through RGB565 and back
            box = find_colour_bbox(c, FOOT_BG, tolerance=8, min_run=60)
        shot = c.screenshot(s.artifact("wayland-12-depth16.png"))
        corner = c.pixel(2, 2)
        c.close()
    finally:
        os.killpg(p.pid, 15)
        p.wait(10)
    ok = pf[0] == 16 and pf[1] == 16 and box is not None and \
        all(abs(a - b) <= 8 for a, b in zip(corner, (46, 51, 63)))
    return ok, ("server pixel format bpp=%d depth=%d; foot found at %r; "
                "background %r" % (pf[0], pf[1], box, corner)), [shot]


s.run("-depth 16: native RGB565 framebuffer, picture correct", t_depth16)


def t_ignored_x_options():
    p = start_small(6, ["-dpi", "96", "-ac", "-fp", "/nonexistent",
                        "-noreset", "-pn"], ["sleep", "30"])
    try:
        up = wait_tcp(5906, timeout=20)
    finally:
        os.killpg(p.pid, 15)
        p.wait(10)
    log = open(s.artifact("logs/wayland-server-6.log")).read()
    warned = [o for o in ("-dpi", "-ac", "-fp", "-noreset", "-pn")
              if "option '%s' has no effect" % o in log]
    return up and len(warned) == 5, \
        "server started=%s, warnings for %r" % (up, warned)


s.run("X-only options (-dpi -ac -fp -noreset -pn) are ignored with a "
      "warning instead of refused", t_ignored_x_options)
def small_session(num):
    """Session that saves its environment, for starting apps in it."""
    envf = os.path.join(WORK, "session-%d.env" % num)
    if os.path.exists(envf):
        os.unlink(envf)
    return envf, ["sh", "-c", "env > %s.tmp && mv %s.tmp %s; "
                  "exec sleep infinity" % (envf, envf, envf)]


def small_app_env(envf):
    if not wait_file(envf, 20):
        return None
    env = read_env_file(envf)
    e = dict(base_env)
    for k in ("WAYLAND_DISPLAY", "DISPLAY", "XAUTHORITY"):
        if k in env:
            e[k] = env[k]
    return e


def near(a, b, tol=4):
    return a is not None and b is not None and \
        all(abs(x - y) <= tol for x, y in zip(a, b))


def xev_keys(num, extra):
    """Keypad keysyms that need a modifier change; returns the key
    presses an X11 app saw."""
    import subprocess
    envf, sess = small_session(num)
    p = start_small(num, extra, sess)
    xev_log = s.artifact("logs/xev-%d.txt" % num)
    try:
        wait_tcp(5900 + num, timeout=20)
        env = small_app_env(envf)
        xev = subprocess.Popen(["stdbuf", "-oL", "xev", "-event", "keyboard", "-geometry",
                                "300x200+0+0"], env=env,
                               stdout=open(xev_log, "w"),
                               stderr=subprocess.STDOUT)
        # a viewer with LED state support, like vncviewer: the server
        # then does not insert its own NumLock/CapsLock sync presses
        c = RFBClient(port=5900 + num, security="VncAuth",
                      password=PASSWORD, led_state=True)
        time.sleep(3)
        c.click(100, 100)
        time.sleep(0.5)
        c.tap(0xffb4)       # XK_KP_4 with NumLock off
        time.sleep(0.3)
        c.tap(0xff7f)       # Num_Lock on
        time.sleep(0.3)
        c.request_update(True)  # LED state comes with an update
        c.pump(0.5)
        leds_on = c.leds
        c.tap(0xff96)       # XK_KP_Left with NumLock on: needs Shift
        time.sleep(0.3)
        c.tap(0xff7f)       # Num_Lock off again
        time.sleep(1)
        c.close()
        xev.terminate()
        xev.wait(5)
    finally:
        os.killpg(p.pid, 15)
        p.wait(10)
    import re
    out = open(xev_log).read()
    presses = re.findall(r"KeyPress event.*?\n.*?\n\s+state 0x[0-9a-f]+, "
                         r"keycode (\d+) \(keysym 0x[0-9a-f]+, (\w+)\)", out)
    return presses, leds_on


def t_avoid_shift_numlock():
    on, leds = xev_keys(7, [])
    off, _ = xev_keys(8, ["-AvoidShiftNumLock=0"])
    nl = ("77", "Num_Lock")
    want_on = [("13", "4"), nl, ("113", "Left"), nl]
    want_off = [("13", "4"), nl, ("50", "Shift_L"), ("83", "KP_Left"), nl]
    # ledNumLock is bit 1 of the LED state pseudo-encoding
    ok = on == want_on and off == want_off and leds is not None and \
        (leds & 2) != 0
    return ok, ("XK_KP_4 (NumLock off), Num_Lock, XK_KP_Left (NumLock "
                "on), Num_Lock: default -> X11 key presses %r (want %r: "
                "the arrow key instead of a fake Shift); "
                "-AvoidShiftNumLock=0 -> %r (want %r: fake Shift + keypad "
                "key, like Xvnc); LED state after Num_Lock %r" %
                (on, want_on, off, want_off, leds))


s.run("AvoidShiftNumLock: no fake Shift on keypad keys (xev); keypad "
      "keysyms unreachable without NumLock use the plain key",
      t_avoid_shift_numlock)


def t_multiscreen_wm():
    import subprocess
    envf, sess = small_session(9)
    p = start_small(9, ["-geometry", "1600x600"], sess)
    shots = []
    try:
        wait_tcp(5909, timeout=20)
        env = small_app_env(envf)
        c = RFBClient(port=5909, security="VncAuth", password=PASSWORD)
        c.set_desktop_size(1600, 600, [(1, 0, 0, 800, 600, 0),
                                       (2, 800, 0, 800, 600, 0)])
        c.pump(1)
        # pointer on the right screen: fullscreen foot opens there
        c.pointer(1200, 300)
        time.sleep(0.3)
        f1 = subprocess.Popen(["foot", "-F"], env=env)
        box_fs = None
        end = time.time() + 15
        while time.time() < end:
            time.sleep(1)
            c.refresh()
            box_fs = find_colour_bbox(c, FOOT_BG, min_run=300)
            if near(box_fs, (800, 0, 1598, 598)):
                break
        shots.append(c.screenshot(s.artifact(
            "wayland-13-two-screens-fullscreen-right.png")))
        f1.terminate()
        f1.wait(5)
        time.sleep(1)
        # pointer on the left screen: maximized foot opens there
        c.pointer(300, 300)
        time.sleep(0.3)
        f2 = subprocess.Popen(["foot", "-m"], env=env)
        box_max = None
        end = time.time() + 15
        while time.time() < end:
            time.sleep(1)
            c.refresh()
            box_max = find_colour_bbox(c, FOOT_BG, min_run=300)
            if box_max and box_max[2] < 800 and box_max[2] > 700:
                break
        shots.append(c.screenshot(s.artifact(
            "wayland-14-two-screens-maximized-left.png")))
        f2.terminate()
        f2.wait(5)
        # an X11 window on the right screen maximizes on that screen
        # an X11 window placed on the right screen goes fullscreen
        # (xterm -fullscreen asks with _NET_WM_STATE) on that screen only
        xt = subprocess.Popen(["xterm", "-bg", "white", "-geometry",
                               "40x10+900+100", "-fullscreen"], env=env)
        box_x = None
        end = time.time() + 10
        while time.time() < end:
            time.sleep(1)
            c.refresh()
            box_x = find_colour_bbox(c, (255, 255, 255), min_run=300)
            if near(box_x, (800, 0, 1598, 598), 6):
                break
        shots.append(c.screenshot(s.artifact(
            "wayland-15-two-screens-xterm-fullscreen-right.png")))
        xt.terminate()
        c.close()
    finally:
        os.killpg(p.pid, 15)
        p.wait(10)
    # foot -m: client-side decorations (title bar) take some of the
    # height, so only the horizontal extent is exact
    ok = near(box_fs, (800, 0, 1598, 598)) and box_max is not None and \
        box_max[0] <= 4 and 700 < box_max[2] < 800 and \
        near(box_x, (800, 0, 1598, 598), 6)
    return ok, ("screens 0+0 800x600 and 800+0 800x600: fullscreen foot "
                "(pointer right) %r; maximized foot (pointer left) %r; "
                "xterm -fullscreen at +900+100 %r" % (box_fs, box_max,
                                                     box_x)), shots


s.run("built-in WM with two screens: windows open, maximize and go "
      "fullscreen on one screen (Wayland and X11)", t_multiscreen_wm)


def t_output_management():
    import re
    envf, sess = small_session(10)
    p = start_small(10, [], sess)
    try:
        wait_tcp(5910, timeout=20)
        env = small_app_env(envf)
        c = RFBClient(port=5910, security="VncAuth", password=PASSWORD)
        c.refresh()
        rc0, before = sh(["wlr-randr"], env=env)
        out_name = before.split()[0] if before.split() else "?"
        rc1, err1 = sh(["wlr-randr", "--output", out_name,
                        "--custom-mode", "800x600"], env=env)
        end = time.time() + 5
        while time.time() < end and (c.width, c.height) != (800, 600):
            c.request_update(True)
            c.pump(0.3)
        rc2, after = sh(["wlr-randr"], env=env)
        rc3, err3 = sh(["wlr-randr", "--output", out_name,
                        "--scale", "2"], env=env)
        rc4, xd = sh("xdpyinfo | grep dimensions", env=env)
        c.refresh()
        shot = c.screenshot(s.artifact("wayland-16-wlr-randr-800x600.png"))
        size = (c.width, c.height)
        c.close()
    finally:
        os.killpg(p.pid, 15)
        p.wait(10)
    cur = re.search(r"(\d+)x(\d+) px[^\n]*current", after)
    cur = (int(cur.group(1)), int(cur.group(2))) if cur else None
    ok = rc1 == 0 and size == (800, 600) and cur == (800, 600) and \
        rc3 != 0 and "800x600" in xd
    return ok, ("wlr-randr --custom-mode 800x600 rc=%d -> viewer %dx%d, "
                "wlr-randr current %r, Xwayland %r; --scale 2 rejected "
                "rc=%d %r" % (rc1, size[0], size[1], cur, xd.strip(), rc3,
                              err3.strip()[-80:])), [shot]


s.run("wlr-output-management: wlr-randr --custom-mode resizes the "
      "desktop for all viewers; unsupported changes are refused",
      t_output_management)
sys.exit(s.finish())
