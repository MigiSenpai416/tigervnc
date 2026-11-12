#!/usr/bin/env python3
"""Wayland mode tests: "Xvnc --wayland" end to end through RFB.

Run as an unprivileged user inside the test image. Results go to
$ARTIFACTS/results-wayland.json, screenshots to $ARTIFACTS/wayland-*.png.
"""

import os
import shutil
import socket
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from harness import (Suite, wait_tcp, wait_file, sh, read_env_file,
                     find_colour_bbox, make_vnc_passwd, make_cert)
from rfbclient import RFBClient, RFBError, XK_TAB, XK_ESCAPE, XK_SHIFT_L

XK_ALT_L = 0xffe9

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


s.run("Wayland client renders (foot)", t_foot_render,
      needs=['foot'])
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


s.run("keyboard input reaches Wayland client (foot)", t_wl_keyboard,
      needs=['foot'])
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
      "selection by double click)", t_wl_pointer,
      needs=['foot', 'wl-paste'])
s.run("pointer motion+buttons reach X11 client (xterm selection, "
      "xdotool pointer position)", t_x_pointer,
      needs=['wl-paste'])


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
      t_clip_wl_to_vnc,
      needs=['wl-copy'])
s.run("primary selection Wayland -> viewer (wl-copy --primary)",
      t_primary_wl_to_vnc,
      needs=['wl-copy'])
s.run("clipboard viewer -> Wayland+X11 (ClientCutText -> wl-paste, "
      "wl-paste --primary, xclip)", t_clip_vnc_to_wl,
      needs=['wl-paste'])
s.run("clipboard X11 app -> viewer (xclip via Xwayland -> "
      "ServerCutText)", t_clip_x_to_vnc)
s.run("clipboard viewer -> paste into foot with Ctrl+Shift+V",
      t_clip_paste_into_foot,
      needs=['foot'])


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
      "(viewers, wl_output mode, Xwayland screen)", t_resize,
      needs=['wayland-info'])
s.run("client resize to 1000x700", t_resize_odd,
      needs=['foot', 'wayland-info'])
s.run("input still works after resize", t_input_after_resize,
      needs=['foot'])


def t_weston_terminal():
    vnc.refresh()
    before_fb = bytes(vnc.fb)
    # WAYLAND_DEBUG: the protocol log shows which globals it binds
    s.spawn("weston-terminal", ["weston-terminal", "--shell=/bin/bash"],
            env=dict(app_env, WAYLAND_DEBUG="client"))
    time.sleep(4)
    vnc.request_update(True)
    vnc.pump(0.5)
    vnc.refresh()
    shot = vnc.screenshot(s.artifact("wayland-10-weston-terminal.png"))
    before = bytes(before_fb)
    changed = sum(1 for i in range(0, min(len(before), len(vnc.fb)), 16)
                  if before[i:i + 3] != vnc.fb[i:i + 3])
    rc, out = sh("ps -C weston-terminal -o pid=", env=app_env)
    ok = out.strip() != "" and changed > 2000
    detail = "weston-terminal pid %s, %d sampled pixels changed" % (
        out.strip(), changed)
    if not ok:
        # An old weston-terminal (weston < 5) only knows zxdg_shell_v6 /
        # wl_shell, which wlroots 0.19 does not offer: SKIP only when its
        # own protocol log proves it saw xdg_wm_base and did not bind it
        import re
        log = open(s.artifact("logs/weston-terminal.log"),
                   errors="replace").read()
        offered = set(re.findall(r'wl_registry@\d+\.global\(\d+, "(\w+)"',
                                 log))
        bound = set(re.findall(r'-> wl_registry@\d+\.bind\(\d+, "(\w+)"',
                               log))
        if "xdg_wm_base" in offered and "wl_compositor" in bound and \
                "xdg_wm_base" not in bound:
            ver = sh("dpkg-query -W -f '${Version}' weston")[1].strip()
            return "SKIP", ("weston-terminal (weston %s) does not speak "
                            "xdg_wm_base: it was offered but not bound "
                            "(WAYLAND_DEBUG: bound %s; %s)" %
                            (ver or "?", sorted(bound), detail))
        detail += "; offered xdg_wm_base=%s, bound %s" % (
            "xdg_wm_base" in offered, sorted(bound))
    return ok, detail, [shot]


s.run("second Wayland client (weston-terminal) starts", t_weston_terminal,
      needs=['weston-terminal'])


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


def start_small(num, extra, session, env=None):
    import subprocess
    log = open(s.artifact("logs/wayland-server-%d.log" % num), "wb")
    return subprocess.Popen(["Xvnc", "--wayland", ":%d" % num,
                             "-SecurityTypes", "VncAuth", "-rfbauth",
                             passwd, "-localhost", "-geometry",
                             "640x480"] + extra + ["--"] + session,
                            stdout=log, stderr=subprocess.STDOUT,
                            env=env or base_env, start_new_session=True)


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


s.run("-depth 16: native RGB565 framebuffer, picture correct", t_depth16,
      needs=['foot'])


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
        keymap = keypad_shift_cancels_numlock(env, num)
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
    return presses, leds_on, keymap


def keypad_shift_cancels_numlock(env, num):
    """Whether the live keymap (dumped from Xwayland with xkbcomp) lets
    Shift cancel NumLock on the keypad (type KEYPAD uses Shift).
    xkeyboard-config >= 2.47 dropped that mapping (Shift no longer
    reaches KP_Left etc. with NumLock on), and stock Xvnc then falls
    back to the plain arrow key even with -AvoidShiftNumLock=0."""
    import re
    import shutil
    import subprocess
    xkbcomp = shutil.which("xkbcomp") or "/usr/libexec/tigervnc/xkbcomp"
    dump = s.artifact("logs/keymap-%d.xkb" % num)
    subprocess.run([xkbcomp, "-xkb", env["DISPLAY"], dump], env=env,
                   capture_output=True, timeout=20)
    text = open(dump).read() if os.path.exists(dump) else ""
    m = re.search(r'type "KEYPAD" \{(.*?)\};', text, re.S)
    if not m:
        return None
    # xkbcomp leaves out map[...] = Level1 entries, so look at the type's
    # modifiers: with Shift among them, Shift+NumLock is Level1 (KP_Left)
    mods = re.search(r"modifiers\s*=\s*([^;]*);", m.group(1))
    return mods is not None and "Shift" in mods.group(1).split("+")


def t_avoid_shift_numlock():
    on, leds, _ = xev_keys(7, [])
    off, _, shift_cancels = xev_keys(8, ["-AvoidShiftNumLock=0"])
    nl = ("77", "Num_Lock")
    want_on = [("13", "4"), nl, ("113", "Left"), nl]
    if shift_cancels:
        want_off = [("13", "4"), nl, ("50", "Shift_L"), ("83", "KP_Left"),
                    nl]
        why = "fake Shift + keypad key, like Xvnc"
    else:
        # xkeyboard-config >= 2.47: no Shift level reaches KP_Left with
        # NumLock on, so Xvnc (same data) sends the plain arrow key too
        want_off = want_on
        why = ("keymap KEYPAD type ignores Shift (xkeyboard-config "
               ">= 2.47): plain arrow key, like Xvnc with this data")
    # ledNumLock is bit 1 of the LED state pseudo-encoding
    ok = shift_cancels is not None and on == want_on and \
        off == want_off and leds is not None and (leds & 2) != 0
    return ok, ("XK_KP_4 (NumLock off), Num_Lock, XK_KP_Left (NumLock "
                "on), Num_Lock: default -> X11 key presses %r (want %r: "
                "the arrow key instead of a fake Shift); "
                "-AvoidShiftNumLock=0 -> %r (want %r: %s); keymap KEYPAD "
                "type uses Shift: %r; LED state after Num_Lock %r" %
                (on, want_on, off, want_off, why, shift_cancels, leds))


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
      "fullscreen on one screen (Wayland and X11)", t_multiscreen_wm,
      needs=['foot'])


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
      t_output_management,
      needs=['wlr-randr'])
def xwin_geometry(env, wid):
    import re
    rc, out = sh(["xdotool", "getwindowgeometry", wid], env=env)
    m = re.search(r"Position: (-?\d+),(-?\d+).*?Geometry: (\d+)x(\d+)",
                  out, re.S)
    return tuple(int(g) for g in m.groups()) if m else None


def misc_fixed_glyphs():
    """6x13 glyph bitmaps (rows of 6 bits) from the X.Org misc-fixed font
    file, independent of the server's built-in copy."""
    import gzip
    from PIL import PcfFontFile
    f = PcfFontFile.PcfFontFile(gzip.open(
        "/usr/share/fonts/X11/misc/6x13-ISO8859-1.pcf.gz"))
    glyphs = {}
    for code in range(32, 256):
        g = f.glyph[code] if code < len(f.glyph) else None
        rows = [[False] * 6 for _ in range(13)]
        if g:
            _, (x0, y0, _, _), _, im = g
            for yy in range(im.size[1]):
                for xx in range(im.size[0]):
                    X, Y = x0 + xx, 11 + y0 + yy
                    if im.getpixel((xx, yy)) and 0 <= X < 6 and 0 <= Y < 13:
                        rows[Y][X] = True
        glyphs[code] = rows
    return glyphs


# X11 title-bar buttons, Windows 11 style (compositor.c: DECO_BTN_W 32,
# DECO_GLYPH 10): minimize, maximize/restore, close; each a full-height
# 32 px cell at the right of the bar with a thin 1 px glyph centred in it.
# Cells are drawn only when hovered: lighter for minimize / maximize,
# #c42b1c with a white glyph for close.
BTN_W = 32
BAR_ACTIVE = (51, 84, 135)
HOVER_ACTIVE = (88, 115, 157)
CLOSE_HOVER = (196, 43, 28)
GLYPH_ACTIVE = (242, 242, 242)
GLYPH_WHITE = (255, 255, 255)
BUTTONS = ("min", "max", "close")


def button_cell(g, which, T=22):
    """Top-left of the cell of button `which` for window geometry g."""
    x, y, w, _ = g
    return x + w - (3 - BUTTONS.index(which)) * BTN_W, y - T


def button_centre(g, which, T=22):
    bx, by = button_cell(g, which, T)
    return bx + BTN_W // 2, by + T // 2


def button_ok(c, g, which, bg, glyph, min_glyph, max_glyph=60, T=22):
    """The cell's corners are `bg`; the glyph (inside the centred 10x10
    box only) has min_glyph..max_glyph pixels of `glyph`."""
    bx, by = button_cell(g, which, T)
    for dx, dy in ((1, 1), (BTN_W - 2, 1), (1, T - 2), (BTN_W - 2, T - 2)):
        px = c.pixel(bx + dx, by + dy)
        if not near(px, bg, 3):
            return False, "%s: cell corner %d,%d is %r, want ~%r" % \
                (which, dx, dy, px, bg)
    inside = outside = 0
    for dy in range(T):
        for dx in range(BTN_W):
            if near(c.pixel(bx + dx, by + dy), glyph, 3):
                if 11 <= dx < 21 and 6 <= dy < 16:
                    inside += 1
                else:
                    outside += 1
    if not (min_glyph <= inside <= max_glyph) or outside:
        return False, "%s: %d glyph pixels in the glyph box (want %d..%d), " \
            "%d outside" % (which, inside, min_glyph, max_glyph, outside)
    return True, "%s: bg ~%r, %d glyph pixels" % (which, bg, inside)


def x11_state(env, wid):
    """'Normal', 'Iconic' or None from WM_STATE."""
    rc, out = sh(["xprop", "-id", wid, "WM_STATE"], env=env)
    for st in ("Normal", "Iconic", "Withdrawn"):
        if "window state: " + st in out:
            return st
    return None


def active_window(env):
    rc, out = sh(["xprop", "-root", "_NET_ACTIVE_WINDOW"], env=env)
    try:
        return int(out.split()[-1], 16)
    except (ValueError, IndexError):
        return None


def title_text_matches(c, g, T, text, glyphs):
    """Compares the title bar's white text pixels with `text` rendered in
    6x13 at (6, (T-13)//2) in the bar; returns (match, mismatches)."""
    x0, y0 = g[0] + 6, g[1] - T + (T - 13) // 2
    bad = 0
    width = 6 * (len(text) + 2)          # also check nothing follows
    for y in range(13):
        for x in range(width):
            i = x // 6
            want = i < len(text) and glyphs[ord(text[i])][y][x % 6]
            px = c.pixel(x0 + x, y0 + y)
            white = px is not None and min(px) >= 240
            if want != white:
                bad += 1
    return bad == 0, bad


def t_x11_title_bars():
    import subprocess
    T = 22                            # title bar height
    BAR = (51, 84, 135)               # focused title bar colour
    envf, sess = small_session(11)
    p = start_small(11, ["-geometry", "800x600"], sess)
    shots, notes = [], []
    ok = True

    def check(cond, note):
        nonlocal ok
        ok = ok and bool(cond)
        notes.append(("" if cond else "FAILED: ") + note)

    try:
        glyphs = misc_fixed_glyphs()
        wait_tcp(5911, timeout=20)
        env = small_app_env(envf)
        c = RFBClient(port=5911, security="VncAuth", password=PASSWORD)
        # sh: bash's prompt would set its own title
        xt = subprocess.Popen(["xterm", "-bg", "white", "-T", "Title Test 42",
                               "-geometry", "40x8+100+100", "-e", "sh"],
                              env=env)
        rc, wid = sh(["xdotool", "search", "--sync", "--class", "XTerm"],
                     env=env, timeout=15)
        wid = wid.split()[0] if wid.split() else "0"
        time.sleep(1.5)
        c.refresh()
        g = xwin_geometry(env, wid)
        shots.append(c.screenshot(s.artifact("wayland-17-x11-title-bar.png")))
        # between the title text and the buttons
        bar = c.pixel(g[0] + g[2] - 3 * BTN_W - 4, g[1] - T // 2) \
            if g else None
        check(g is not None and near(bar, BAR, 3),
              "xterm at %r, title bar pixel %r" % (g, bar))

        # three flat buttons: cells in the bar colour, thin light glyphs
        # (minimize: a 10 px dash; maximize: a 10x10 box; close: an X)
        if g is not None:
            c.pointer(0, 0)
            c.refresh()
            shots.append(c.screenshot(s.artifact(
                "wayland-17b-x11-buttons.png")))
            for which, lo, hi in (("min", 10, 10), ("max", 36, 36),
                                  ("close", 18, 20)):
                r, n = button_ok(c, g, which, BAR_ACTIVE, GLYPH_ACTIVE,
                                 lo, hi, T)
                check(r, "normal " + n)
            # hover: lighter cell (min, max), red cell + white X (close)
            for which, bg, gl, lo, hi in (
                    ("min", HOVER_ACTIVE, GLYPH_ACTIVE, 10, 10),
                    ("max", HOVER_ACTIVE, GLYPH_ACTIVE, 36, 36),
                    ("close", CLOSE_HOVER, GLYPH_WHITE, 18, 20)):
                c.pointer(*button_centre(g, which, T))
                time.sleep(0.3)
                c.refresh()
                if which == "close":
                    shots.append(c.screenshot(s.artifact(
                        "wayland-17c-x11-close-hover.png")))
                r, n = button_ok(c, g, which, bg, gl, lo, hi, T)
                check(r, "hovered " + n)
                # the other cells stay flat
                others = [b for b in BUTTONS if b != which]
                flat = all(button_ok(c, g, o, BAR_ACTIVE,
                                     GLYPH_ACTIVE, 1, 60, T)[0]
                           for o in others)
                check(flat, "hovering %s leaves %s flat" % (which, others))
            c.pointer(0, 0)
            time.sleep(0.3)
            c.refresh()
            r, n = button_ok(c, g, "close", BAR_ACTIVE, GLYPH_ACTIVE, 18,
                             20, T)
            check(r, "hover cleared when the pointer leaves: " + n)

        # title text: the window's title, pixel exact in misc-fixed 6x13
        m, bad = title_text_matches(c, g, T, "Title Test 42", glyphs)
        check(m, "title text 'Title Test 42' drawn (%d pixels differ)" % bad)
        sh(["xdotool", "set_window", "--name", "Renamed-xterm", wid],
           env=env)
        time.sleep(1)
        c.refresh()
        m, bad = title_text_matches(c, g, T, "Renamed-xterm", glyphs)
        shots.append(c.screenshot(s.artifact(
            "wayland-20-x11-title-renamed.png")))
        check(m, "after renaming, title text 'Renamed-xterm' drawn "
              "(%d pixels differ)" % bad)

        # resize with the pointer on the (invisible) borders
        def resize(x0, y0, dx, dy, name, want):
            c.drag(x0, y0, x0 + dx, y0 + dy)
            time.sleep(1)
            got = xwin_geometry(env, wid)
            check(got == want, "%s drag by %+d,%+d -> %r (want %r)" %
                  (name, dx, dy, got, want))
            return got
        x, y, w, h = g
        g1 = resize(x + w + 2, y + h // 2, 100, 0, "right edge",
                    (x, y, w + 100, h))
        x, y, w, h = g1 or g
        g1 = resize(x + w + 2, y + h + 2, 50, 40, "bottom-right corner",
                    (x, y, w + 50, h + 40))
        x, y, w, h = g1 or g
        g1 = resize(x - 3, y + h // 2, -60, 0, "left edge",
                    (x - 60, y, w + 60, h))
        x, y, w, h = g1 or g
        g1 = resize(x + w // 2, y - T - 3, 0, -30, "top edge (above the bar)",
                    (x, y - 30, w, h + 30))
        x, y, w, h = g1 or g
        g1 = resize(x - 3, y - T - 3, 20, 20, "top-left corner",
                    (x + 20, y + 20, w - 20, h - 20))
        c.refresh()
        shots.append(c.screenshot(s.artifact(
            "wayland-21-x11-title-bar-resized.png")))
        g = g1 or g
        # the title follows the new width (still drawn, bar wider)
        m, bad = title_text_matches(c, g, T, "Renamed-xterm", glyphs)
        check(m, "title text still correct after resizing (%d pixels "
              "differ)" % bad)

        # drag the title bar by (+200, +150), grabbing it on the text
        bx, by = g[0] + 20, g[1] - T // 2
        c.drag(bx, by, bx + 200, by + 150)
        time.sleep(1)
        g2 = xwin_geometry(env, wid)
        c.refresh()
        shots.append(c.screenshot(s.artifact(
            "wayland-18-x11-title-bar-moved.png")))
        moved = g2 is not None and g2 == (g[0] + 200, g[1] + 150, g[2], g[3])
        check(moved, "after dragging the bar (on the title text) by "
              "+200,+150: %r" % (g2,))

        # maximize button (second from the right)
        c.click(*button_centre(g2, "max", T))
        time.sleep(1.5)
        g3 = xwin_geometry(env, wid)
        c.refresh()
        shots.append(c.screenshot(s.artifact(
            "wayland-19-x11-title-bar-maximized.png")))
        maxed = g3 is not None and (g3[0], g3[1]) == (0, T) and \
            g3[2] >= 790 and g3[3] >= 600 - T - 20
        notes.append("maximize button -> %r (want 0,%d ~800x%d)" %
                     (g3, T, 600 - T))
        ok = ok and maxed
        if g3 is not None:
            c.pointer(0, 0)
            time.sleep(0.3)
            c.refresh()
            # restore glyph: two overlapping boxes, more pixels than the
            # maximize box
            ok_r, n_r = button_ok(c, g3, "max", BAR_ACTIVE, GLYPH_ACTIVE,
                                  38, 50, T)
            check(ok_r, "restore glyph on the maximized window: " + n_r)

        # again: restore
        c.click(*button_centre(g3, "max", T))
        time.sleep(1.5)
        g4 = xwin_geometry(env, wid)
        restored = g4 is not None and g4 == g2
        notes.append("maximize again -> %r (want %r)" % (g4, g2))
        ok = ok and restored

        # minimize button: the window is hidden and iconic in X11 terms
        inside = (g4[0] + g4[2] // 2, g4[1] + g4[3] // 2)
        c.click(*button_centre(g4, "min", T))
        time.sleep(1)
        c.refresh()
        hidden = not near(c.pixel(*inside), (255, 255, 255), 8)
        st = x11_state(env, wid)
        shots.append(c.screenshot(s.artifact(
            "wayland-19b-x11-minimized.png")))
        check(hidden and st == "Iconic",
              "minimize button: hidden=%s, WM_STATE %s" % (hidden, st))
        # Alt+Tab (the only window) brings it back
        c.key(XK_ALT_L, True)
        c.tap(XK_TAB)
        time.sleep(0.5)
        c.key(XK_ALT_L, False)
        time.sleep(1)
        c.refresh()
        shown = near(c.pixel(*inside), (255, 255, 255), 8)
        st = x11_state(env, wid)
        check(shown and st == "Normal" and xwin_geometry(env, wid) == g4,
              "Alt+Tab restores it: shown=%s, WM_STATE %s" % (shown, st))

        # close button (rightmost): xterm gets WM_DELETE_WINDOW and exits
        c.click(*button_centre(g4, "close", T))
        try:
            xt.wait(5)
            closed = True
        except Exception:
            closed = False
            xt.kill()
        notes.append("close button -> xterm exited=%s" % closed)
        ok = ok and closed
        c.close()
    except Exception as e:
        ok = False
        notes.append("error: %r" % e)
    finally:
        os.killpg(p.pid, 15)
        p.wait(10)
    return ok, "; ".join(notes), shots


s.run("X11 title bars: window title drawn as text (pixel exact, follows "
      "renames), resize by edges and corners, drag moves; minimize, "
      "maximize/restore and close buttons (flat glyphs, hover colours) work",
      t_x11_title_bars)


# Alt+Tab window switcher (compositor.c: SW_PAD 10, SW_ROW_H 44, SW_ICON 32)
SW_BG = (43, 43, 43)
SW_SEL = (74, 77, 82)
SW_PAD, SW_ROW_H, SW_ICON = 10, 44, 32


def switcher_box(c):
    """(x0, y0, x1, y1) of the switcher popup, or None."""
    return find_colour_bbox(c, SW_BG, tolerance=1, step=1, min_run=60)


def switcher_rows(c, box, n):
    """For n rows: (selected, icon centre pixel) of each row."""
    x0, y0 = box[0] - 1, box[1] - 1       # bbox of the body; 1px outline
    rows = []
    for i in range(n):
        ry = y0 + SW_PAD + i * SW_ROW_H
        sel = near(c.pixel(x0 + SW_PAD + 3, ry + SW_ROW_H // 2), SW_SEL, 2)
        icon = c.pixel(x0 + SW_PAD + 10 + SW_ICON // 2,
                       ry + (SW_ROW_H - SW_ICON) // 2 + SW_ICON // 2)
        rows.append((sel, icon))
    return rows


def alt_tab(c, taps=1, shift=False, release=True, shot=None):
    c.key(XK_ALT_L, True)
    if shift:
        c.key(XK_SHIFT_L, True)
    for _ in range(taps):
        c.tap(XK_TAB)
        time.sleep(0.15)
    time.sleep(0.4)
    c.refresh()
    box = switcher_box(c)
    if shot:
        shot = c.screenshot(s.artifact(shot))
    if shift:
        c.key(XK_SHIFT_L, False)
    if release:
        c.key(XK_ALT_L, False)
        time.sleep(0.8)
        c.refresh()
    return box, shot


def t_switcher():
    import subprocess
    envf, sess = small_session(13)
    p = start_small(13, ["-geometry", "800x600"], sess)
    shots, notes = [], []
    ok = True
    procs = []

    def check(cond, note):
        nonlocal ok
        ok = ok and bool(cond)
        notes.append(("" if cond else "FAILED: ") + note)

    # three xterms on top of each other; the colour at PROBE tells which
    # one is on top
    cols = {"A": (204, 34, 34), "B": (34, 170, 34), "C": (34, 68, 204)}
    PROBE = (150, 120)

    def top():
        c.refresh()
        px = c.pixel(*PROBE)
        for k, v in cols.items():
            if near(px, v, 6):
                return k
        return px

    try:
        wait_tcp(5913, timeout=20)
        env = small_app_env(envf)
        c = RFBClient(port=5913, security="VncAuth", password=PASSWORD)
        wids = {}
        for k in "ABC":
            procs.append(subprocess.Popen(
                ["xterm", "-bg", "#%02x%02x%02x" % cols[k], "-T", "SW-" + k,
                 "-class", "SwTest" + k, "-geometry", "40x10+60+60",
                 "-e", "sh"], env=env))
            rc, out = sh(["xdotool", "search", "--sync", "--class",
                          "SwTest" + k], env=env, timeout=15)
            wids[k] = out.split()[0] if out.split() else "0"
            time.sleep(0.8)
        check(top() == "C", "C (newest) on top: %r" % (top(),))

        # one tap: the popup lists C, B, A (MRU) with B selected
        box, shot = alt_tab(c, 1, release=False,
                            shot="wayland-26-switcher-popup.png")
        shots.append(shot)
        rows = switcher_rows(c, box, 3) if box else []
        check(box is not None and [r[0] for r in rows] == [False, True, False],
              "Alt+Tab: popup at %r, selection %r" % (box, [r[0] for r in rows]))
        if box:
            h = box[3] - box[1] + 3
            check(h == 2 * SW_PAD + 3 * SW_ROW_H,
                  "popup height %d for 3 windows" % h)
            cx = (box[0] + box[2]) // 2
            check(abs(cx - 400) <= 2, "popup centred (x %d)" % cx)
        c.key(XK_ALT_L, False)
        time.sleep(0.8)
        check(top() == "B" and c.pixel(*PROBE) and switcher_box(c) is None,
              "Alt released: B raised (%r), popup gone" % (top(),))
        check(active_window(env) == int(wids["B"]),
              "B focused (_NET_ACTIVE_WINDOW %r)" % active_window(env))

        # MRU is now B, C, A: two taps select A
        box, _ = alt_tab(c, 2)
        check(top() == "A", "Alt+Tab+Tab: A raised (%r)" % (top(),))
        # Alt+Shift+Tab goes backwards: from A, B, C the last one, C
        box, _ = alt_tab(c, 1, shift=True)
        check(top() == "C", "Alt+Shift+Tab: C raised (%r)" % (top(),))
        # Tab wraps around: four taps on C, A, B select A
        box, _ = alt_tab(c, 4)
        check(top() == "A", "Alt+Tab x4 wraps: A raised (%r)" % (top(),))

        # Esc cancels: nothing changes, focus stays on A
        c.key(XK_ALT_L, True)
        c.tap(XK_TAB)
        time.sleep(0.3)
        c.tap(XK_ESCAPE)
        time.sleep(0.3)
        c.refresh()
        gone = switcher_box(c) is None
        c.key(XK_ALT_L, False)
        time.sleep(0.8)
        check(gone and top() == "A" and active_window(env) == int(wids["A"]),
              "Esc: popup closed=%s, A still on top and focused (%r)" %
              (gone, top()))

        # minimized windows are listed (at the end) and come back
        rc, _ = sh(["xdotool", "windowminimize", wids["A"]], env=env)
        time.sleep(1)
        st = x11_state(env, wids["A"])
        t = top()
        check(st == "Iconic" and t != "A" and t in cols,
              "client request (WM_CHANGE_STATE Iconic): A hidden, now %r "
              "on top, WM_STATE %s" % (t, st))
        check(active_window(env) == int(wids[t]) if t in wids else False,
              "focus moved to %r" % (t,))
        box, shot = alt_tab(c, 2, release=False,
                            shot="wayland-27-switcher-minimized.png")
        shots.append(shot)
        rows = switcher_rows(c, box, 3) if box else []
        check([r[0] for r in rows] == [False, False, True],
              "popup lists the minimized window last, selected: %r" %
              ([r[0] for r in rows],))
        c.key(XK_ALT_L, False)
        time.sleep(1)
        st = x11_state(env, wids["A"])
        check(top() == "A" and st == "Normal" and
              active_window(env) == int(wids["A"]),
              "Alt released: A un-minimized, raised, focused (WM_STATE %s)"
              % st)

        # a Wayland client's minimize request (foot's own minimize button);
        # distros without foot (18.04) only run the X11 part above
        have_foot = shutil.which("foot") is not None
        if have_foot:
            foot = subprocess.Popen(["foot", "-W", "30x6"], env=env)
            procs.append(foot)
        else:
            notes.append("xdg request_minimize part not run: needs foot, "
                         "not installed on this distro")
        fb = None
        end = time.time() + 10
        while have_foot and time.time() < end and not fb:
            time.sleep(0.5)
            c.refresh()
            fb = find_colour_bbox(c, FOOT_BG, min_run=50)
        if fb:
            time.sleep(1)
            c.refresh()
            fb = find_colour_bbox(c, FOOT_BG, min_run=50)
            # foot's title bar (26 px) has close, maximize, minimize from
            # the right, 26 px each
            c.click(fb[2] - 26 * 2 - 13, fb[1] - 13)
            time.sleep(1)
            c.refresh()
            shots.append(c.screenshot(s.artifact(
                "wayland-28-foot-minimized.png")))
            hidden = find_colour_bbox(c, FOOT_BG, min_run=50) is None
            box, _ = alt_tab(c, 3)
            back = find_colour_bbox(c, FOOT_BG, min_run=50) is not None
            check(hidden and back,
                  "foot (xdg request_minimize): hidden=%s, back after "
                  "Alt+Tab x3=%s" % (hidden, back))
        elif have_foot:
            check(False, "foot window not found")
        c.close()
    except Exception as e:
        ok = False
        notes.append("error: %r" % e)
    finally:
        for pr in procs:
            pr.kill()
        os.killpg(p.pid, 15)
        p.wait(10)
    return ok, "; ".join(notes), shots


s.run("Alt+Tab window switcher: popup centred, MRU order, Tab / Shift+Tab / "
      "wrap, Alt release raises and focuses, Esc cancels; minimize requests "
      "(X11 WM_CHANGE_STATE, xdg request_minimize) hide windows that stay "
      "listed and come back", t_switcher)


def make_icon_theme(root):
    """XDG_DATA_HOME with a PNG icon named in a .desktop file (found by
    StartupWMClass) and an SVG icon named like a WM_CLASS."""
    from PIL import Image
    d = os.path.join(root, "icons/hicolor/48x48/apps")
    os.makedirs(d, exist_ok=True)
    Image.new("RGBA", (48, 48), (0, 200, 0, 255)).save(
        os.path.join(d, "tvnc-test-green.png"))
    d = os.path.join(root, "icons/hicolor/scalable/apps")
    os.makedirs(d, exist_ok=True)
    with open(os.path.join(d, "tvnc-test-blue.svg"), "w") as f:
        f.write('<svg xmlns="http://www.w3.org/2000/svg" width="48" '
                'height="48"><rect x="0" y="0" width="48" height="48" '
                'fill="#0000ff"/></svg>\n')
    d = os.path.join(root, "applications")
    os.makedirs(d, exist_ok=True)
    with open(os.path.join(d, "tvnc-png-test.desktop"), "w") as f:
        f.write("[Desktop Entry]\nType=Application\nName=PNG test\n"
                "Exec=true\nIcon=tvnc-test-green\n"
                "StartupWMClass=TvncPngTest\n")


def t_switcher_icons():
    import subprocess
    data = os.path.join(WORK, "icon-data")
    make_icon_theme(data)
    env0 = dict(base_env)
    env0["XDG_DATA_HOME"] = data
    envf, sess = small_session(14)
    p = start_small(14, ["-geometry", "800x600"], sess, env=env0)
    shots, notes = [], []
    ok = True
    procs = []

    def check(cond, note):
        nonlocal ok
        ok = ok and bool(cond)
        notes.append(("" if cond else "FAILED: ") + note)

    try:
        wait_tcp(5914, timeout=20)
        env = small_app_env(envf)
        c = RFBClient(port=5914, security="VncAuth", password=PASSWORD)
        log = open(s.artifact("logs/wayland-server-14.log")).read()
        check("icon formats PNG, SVG" in log,
              "server log: %r" % [l.strip() for l in log.splitlines()
                                  if "switcher" in l])

        def xterm(cls, name, title):
            procs.append(subprocess.Popen(
                ["xterm", "-class", cls, "-name", name, "-T", title,
                 "-geometry", "30x5+%d+40" % (40 + 60 * len(procs)),
                 "-e", "sh"], env=env))
            rc, out = sh(["xdotool", "search", "--sync", "--class", cls],
                         env=env, timeout=15)
            time.sleep(0.8)
            return out.split()[0] if out.split() else "0"

        # A: _NET_WM_ICON (7x7 red: xprop sets at most 64 values) beats
        # the theme's xterm icon
        wa = xterm("TvncNetIcon", "xterm", "icon-net")
        rc, _ = sh(["xprop", "-id", wa, "-f", "_NET_WM_ICON", "32" + "c" * 51,
                    "-set", "_NET_WM_ICON",
                    ",".join(["7", "7"] + ["0xffff0000"] * 49)], env=env)
        # B: PNG named by a .desktop file's Icon= (StartupWMClass match)
        xterm("TvncPngTest", "tvncpng", "icon-png")
        # C: SVG theme icon named like the WM_CLASS instance
        xterm("TvncSvgTest", "tvnc-test-blue", "icon-svg")
        # D: nothing: the built-in fallback icon
        xterm("TvncNoIcon", "tvncnoicon", "icon-none")

        box, shot = alt_tab(c, 1, release=False,
                            shot="wayland-29-switcher-icons.png")
        shots.append(shot)
        rows = switcher_rows(c, box, 4) if box else []
        icons = [r[1] for r in rows]
        # MRU: D, C, B, A
        want = [None, (0, 0, 255), (0, 200, 0), (255, 0, 0)]
        for i, (name, w) in enumerate(zip(("fallback", "SVG", "PNG",
                                           "_NET_WM_ICON"), want)):
            got = icons[i] if i < len(icons) else None
            if w is None:
                good = got is not None and not near(got, SW_BG, 2) and \
                    not near(got, SW_SEL, 2)
            else:
                good = near(got, w, 3)
            check(good, "%s icon: row %d centre %r (want %r)" %
                  (name, i, got, w or "drawn"))
        # the whole 32x32 icon is filled for the scaled 7x7 one
        if box:
            x0, y0 = box[0] - 1, box[1] - 1
            ry = y0 + SW_PAD + 3 * SW_ROW_H + (SW_ROW_H - SW_ICON) // 2
            ix = x0 + SW_PAD + 10
            red = sum(1 for y in range(SW_ICON) for x in range(SW_ICON)
                      if near(c.pixel(ix + x, ry + y), (255, 0, 0), 3))
            check(red == SW_ICON * SW_ICON,
                  "_NET_WM_ICON 7x7 scaled to 32x32: %d red pixels" % red)
        c.tap(XK_ESCAPE)
        c.key(XK_ALT_L, False)
        c.close()
    except Exception as e:
        ok = False
        notes.append("error: %r" % e)
    finally:
        for pr in procs:
            pr.kill()
        os.killpg(p.pid, 15)
        p.wait(10)
    return ok, "; ".join(notes), shots


s.run("Alt+Tab icons: _NET_WM_ICON first, then the icon theme (PNG via a "
      ".desktop StartupWMClass, SVG by WM_CLASS), else a built-in icon",
      t_switcher_icons)
def t_vncconfig():
    import subprocess, stat
    envf, sess = small_session(12)
    p = start_small(12, ["-desktop", "before"], sess)
    notes, ok = [], True

    def vc(*args):
        rc, out = sh(["sh", "-c", "vncconfig \"$@\" 2>&1", "vncconfig"] +
                     list(args), env=env)
        return rc, out.strip()

    def check(cond, note):
        nonlocal ok
        ok = ok and bool(cond)
        notes.append(("" if cond else "FAILED: ") + note)

    try:
        wait_tcp(5912, timeout=20)
        env = small_app_env(envf)
        sock = os.path.join(runtime, "tigervnc-wayland-12.vncconfig")
        mode = stat.S_IMODE(os.stat(sock).st_mode) if os.path.exists(sock) \
            else None
        # only the owner may connect (the server checks the peer uid too)
        check(mode is not None and mode & 0o077 == 0,
              "control socket %s mode %s" %
              (sock, oct(mode) if mode is not None else None))

        rc, out = vc("-list")
        check(rc == 0 and "SendCutText" in out.split() and
              "AllowOverride" in out.split(),
              "-list rc=%d, %d names" % (rc, len(out.split())))
        rc, out = vc("-get", "SendCutText")
        check(rc == 0 and out == "on", "-get SendCutText -> %r" % out)
        rc, out = vc("-get", "Password")
        check(rc != 0, "-get Password refused (rc=%d %r)" % (rc, out))
        rc, out = vc("-desc", "SendCutText")
        check(rc == 0 and len(out) > 10, "-desc SendCutText -> %r" % out)

        c = RFBClient(port=5912, security="VncAuth", password=PASSWORD)
        c.refresh()
        rc, out = vc("-set", "SendCutText=0")
        rc2, out2 = vc("-get", "SendCutText")
        check(rc == 0 and out2 == "off", "-set SendCutText=0 rc=%d, now %r" %
              (rc, out2))
        c.cut_texts.clear()
        cp = subprocess.Popen(["wl-copy", "--foreground", "not-sent-1"],
                              env=env)
        got = c.wait_cut_text(3)
        cp.terminate()
        check(got is None, "with SendCutText=0 the viewer got %r" % got)
        rc, out = vc("SendCutText=1")        # bare form, like vncconfig
        c.cut_texts.clear()
        cp = subprocess.Popen(["wl-copy", "--foreground", "sent-2"], env=env)
        got = c.wait_cut_text(5)
        cp.terminate()
        check(rc == 0 and got == "sent-2",
              "SendCutText=1 rc=%d, then the viewer got %r" % (rc, got))

        rc, out = vc("-set", "desktop=after-rename")
        c2 = RFBClient(port=5912, security="VncAuth", password=PASSWORD)
        check(rc == 0 and c2.name == "after-rename",
              "-set desktop=after-rename rc=%d, new viewer sees %r" %
              (rc, c2.name))
        c2.close()

        rc, out = vc("-set", "rfbport=1")
        check(rc != 0 and "AllowOverride" in out,
              "-set rfbport=1 refused (rc=%d %r)" % (rc, out))

        rc, out = vc("-disconnect")
        gone = False
        try:
            c.pump(2)
            c.request_update(True)
            c.pump(1)
        except Exception:
            gone = True
        check(rc == 0 and gone, "-disconnect rc=%d, viewer disconnected=%s"
              % (rc, gone))

        # reverse connection to a listening viewer
        lsock = socket.socket()
        lsock.bind(("127.0.0.1", 0))
        lsock.listen(1)
        lport = lsock.getsockname()[1]
        rc, out = vc("-connect", "127.0.0.1:%d" % lport)
        lsock.settimeout(5)
        try:
            conn, _ = lsock.accept()
            conn.settimeout(5)
            banner = conn.recv(12)
            conn.close()
        except Exception as e:
            banner = repr(e).encode()
        lsock.close()
        check(rc == 0 and banner.startswith(b"RFB 003.008"),
              "-connect 127.0.0.1:%d rc=%d, listening viewer got %r" %
              (lport, rc, banner))

        # "vncconfig -nowin &" (as in xstartup files) keeps running, like
        # on Xvnc, as the QueryConnect prompt (see t_query_connect)
        vp = subprocess.Popen(["vncconfig", "-nowin"], env=env,
                              stdout=subprocess.DEVNULL,
                              stderr=subprocess.DEVNULL)
        time.sleep(2)
        running = vp.poll() is None
        vp.terminate()
        vp.wait(5)
        check(running, "vncconfig -nowin keeps running in the background "
              "(running after 2 s: %s)" % running)
    except Exception as e:
        ok = False
        notes.append("error: %r" % e)
    finally:
        os.killpg(p.pid, 15)
        p.wait(10)
    return ok, "; ".join(notes)


s.run("vncconfig works in Wayland mode (control socket): -list -get "
      "-desc -set/AllowOverride, desktop rename, -disconnect, -connect",
      t_vncconfig,
      needs=['wl-copy'])


def t_query_connect():
    """QueryConnect with vncconfig's prompt, as on Xvnc: vncconfig runs as
    an X client of Xwayland; connections are prompted while it runs and
    rejected when it doesn't. The observing viewer is a reverse connection
    (vncconfig -connect), which Xvnc lets bypass the query."""
    import subprocess, threading
    T = 22
    envf, sess = small_session(13)
    p = start_small(13, ["-geometry", "800x600", "-desktop", "qc-test",
                         "-QueryConnect", "-QueryConnectTimeout", "4",
                         "-AcceptCutText=0"], sess)
    shots, notes = [], []
    ok = True
    vcp = None

    def check(cond, note):
        nonlocal ok
        ok = ok and bool(cond)
        notes.append(("" if cond else "FAILED: ") + note)

    def connect():
        try:
            v = RFBClient(port=5913, security="VncAuth", password=PASSWORD,
                          timeout=20)
            name = v.name
            v.close()
            return "connected:" + name
        except Exception as e:
            return "error:" + str(e)

    def connect_bg():
        res = {}
        t = threading.Thread(target=lambda: res.update(r=connect()))
        t.start()
        return t, res

    def window(name, timeout=10):
        rc, out = sh(["xdotool", "search", "--sync", "--onlyvisible",
                      "--name", name], env=env, timeout=timeout)
        ids = out.split()
        return xwin_geometry(env, ids[-1]) if ids else None

    def dark(x, y, w, h):
        return sum(1 for yy in range(y, y + h) for xx in range(x, x + w)
                   if max(c.pixel(xx, yy)) < 90)

    try:
        wait_tcp(5913, timeout=20)
        env = small_app_env(envf)

        r = connect()
        check(r == "error:authentication failed: Unable to query the local "
              "user to accept the connection.",
              "no prompt running: viewer -> %r" % r)

        lsock = socket.socket()
        lsock.bind(("127.0.0.1", 0))
        lsock.listen(1)
        sh(["vncconfig", "-connect", "127.0.0.1:%d" %
            lsock.getsockname()[1]], env=env)
        lsock.settimeout(10)
        conn, _ = lsock.accept()
        lsock.close()
        c = RFBClient(sock=conn, security="VncAuth", password=PASSWORD)
        c.refresh()
        notes.append("observer: reverse connection (no query, as on Xvnc)")

        vcp = subprocess.Popen(["vncconfig"], env=env,
                               stdout=subprocess.DEVNULL,
                               stderr=subprocess.DEVNULL)
        g = window("VNC config")
        time.sleep(1.5)
        c.refresh()
        shots.append(c.screenshot(s.artifact("wayland-22-vncconfig-window.png")))
        check(g is not None, "vncconfig window on screen at %r" % (g,))
        # The four checkboxes (12, 29, 46, 63 px down): AcceptCutText=0 at
        # start, the rest on; a ticked box has dark pixels inside
        rows = {"AcceptCutText": 12, "SetPrimary": 29, "SendCutText": 46,
                "SendPrimary": 63}
        xoff = {"AcceptCutText": 12, "SetPrimary": 22, "SendCutText": 12,
                "SendPrimary": 22}
        ticks = {k: dark(g[0] + xoff[k], g[1] + v - 3, 6, 6)
                 for k, v in rows.items()}
        check(ticks["AcceptCutText"] == 0 and ticks["SendCutText"] > 4 and
              ticks["SendPrimary"] > 4,
              "checkboxes show the server's settings (dark pixels in boxes "
              "%r)" % ticks)
        # clicking a checkbox changes the server parameter
        c.click(g[0] + 40, g[1] + rows["SendCutText"])
        time.sleep(1)
        rc, v1 = sh(["vncconfig", "-get", "SendCutText"], env=env)
        c.click(g[0] + 40, g[1] + rows["AcceptCutText"])
        time.sleep(1)
        rc, v2 = sh(["vncconfig", "-get", "AcceptCutText"], env=env)
        c.refresh()
        shots.append(c.screenshot(s.artifact(
            "wayland-23-vncconfig-toggled.png")))
        check(v1.strip() == "off" and v2.strip() == "on",
              "clicking 'Send clipboard' -> SendCutText=%s, 'Accept "
              "clipboard' -> AcceptCutText=%s" % (v1.strip(), v2.strip()))
        c.click(g[0] + 40, g[1] + rows["SendCutText"])
        time.sleep(1)
        rc, v3 = sh(["vncconfig", "-get", "SendCutText"], env=env)
        check(v3.strip() == "on", "clicking it again -> SendCutText=%s" %
              v3.strip())

        def answer(button, shot=None):
            d = window("Accept connection")
            if d is None:
                return None
            time.sleep(1)
            # The prompt's countdown label redraws every second, so the
            # screen never settles while it is open: bound the refresh
            # (QueryConnectTimeout is only 4 seconds here)
            c.refresh(max_time=1.0)
            if shot:
                shots.append(c.screenshot(s.artifact(shot)))
            # the prompt (TX default background 0xbbbbbb) must be what the
            # observer sees at the position the X server reports
            grey = sum(1 for yy in range(d[1], d[1] + d[3])
                       for xx in range(d[0], d[0] + d[2])
                       if c.pixel(xx, yy) == (0xbb, 0xbb, 0xbb))
            check(grey * 2 > d[2] * d[3],
                  "%s prompt visible to the observer (%d of %d pixels)" %
                  (button, grey, d[2] * d[3]))
            if button == "Accept":
                c.click(d[0] + 34, d[1] + d[3] - 14)
            else:
                c.click(d[0] + d[2] - 34, d[1] + d[3] - 14)
            return d

        t, res = connect_bg()
        d = answer("Accept", "wayland-24-queryconnect-dialog.png")
        t.join(25)
        check(d is not None and res.get("r") == "connected:qc-test",
              "prompt shown at %r, Accept clicked -> viewer %r" %
              (d, res.get("r")))

        t, res = connect_bg()
        d = answer("Reject")
        t.join(25)
        check(d is not None and res.get("r") ==
              "error:authentication failed: Connection rejected by local "
              "user", "Reject clicked -> viewer %r" % res.get("r"))

        t, res = connect_bg()
        window("Accept connection")
        r2 = connect()
        check(r2 == "error:authentication failed: Another connection is "
              "currently being queried.",
              "second viewer while a query is open -> %r" % r2)
        t.join(25)
        check(res.get("r") == "error:authentication failed: The attempt "
              "to prompt the user to accept the connection failed",
              "no answer within QueryConnectTimeout=4 -> viewer %r" %
              res.get("r"))

        vcp.terminate()
        vcp.wait(5)
        vcp = None
        time.sleep(1)
        r = connect()
        check(r == "error:authentication failed: Unable to query the local "
              "user to accept the connection.",
              "after vncconfig exits: viewer -> %r" % r)

        # -nowin: no window, but still the prompt
        vcp = subprocess.Popen(["vncconfig", "-nowin"], env=env,
                               stdout=subprocess.DEVNULL,
                               stderr=subprocess.DEVNULL)
        time.sleep(2)
        rc, out = sh(["xdotool", "search", "--onlyvisible", "--name",
                      "VNC config"], env=env)
        t, res = connect_bg()
        d = answer("Accept", "wayland-25-queryconnect-nowin.png")
        t.join(25)
        check(out.strip() == "" and d is not None and
              res.get("r") == "connected:qc-test",
              "vncconfig -nowin: no window (%r), prompt shown, Accept -> %r"
              % (out.strip(), res.get("r")))
        c.close()
    except Exception as e:
        ok = False
        notes.append("error: %r" % e)
    finally:
        if vcp:
            vcp.kill()
        os.killpg(p.pid, 15)
        p.wait(10)
    return ok, "; ".join(notes), shots


s.run("QueryConnect with the vncconfig prompt (as on Xvnc): rejected "
      "without vncconfig, Accept/Reject/busy/timeout with it, -nowin; "
      "vncconfig window shows and sets the clipboard parameters",
      t_query_connect)
sys.exit(s.finish())
