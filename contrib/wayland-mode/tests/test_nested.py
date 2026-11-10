#!/usr/bin/env python3
"""Nested compositor tests: a real sway session inside "Xvnc --wayland".

sway runs with its wayland backend as the session, i.e. as a client of
Xvnc-wayland. The server must detect it, give its output the whole
screen (fullscreen, undecorated), follow resizes and multi-screen
layouts, and pass input and both selections through.

Run as an unprivileged user inside the test image. Results go to
$ARTIFACTS/results-nested.json, screenshots to $ARTIFACTS/nested-*.png.
"""

import glob
import json
import os
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from harness import Suite, wait_tcp, wait_file, sh, make_vnc_passwd
from rfbclient import RFBClient

PASSWORD = "nested-secret"
WORK = "/tmp/nestedtest"
BG = (0x33, 0x66, 0xaa)          # sway output background in sway.cfg
FOOT_BG = (0x24, 0x24, 0x24)     # foot's default background

s = Suite("nested")
os.makedirs(WORK, exist_ok=True)
passwd = os.path.join(WORK, "passwd")
make_vnc_passwd(passwd, PASSWORD)

SWAY_CFG = """\
output * bg #3366aa solid_color
default_border none
for_window [app_id="nested-foot"] border none
exec foot --app-id nested-foot bash --norc --noprofile
"""


def start(display, outputs, geometry="1024x768", extra=(), session=None):
    """Starts Xvnc --wayland with sway (or `session`) as the session.
    Returns (server process, environment for nested clients, runtime
    dir)."""
    runtime = os.path.join(WORK, "rt-%d" % display)
    os.makedirs(runtime, mode=0o700, exist_ok=True)
    cfg = os.path.join(WORK, "sway.cfg")
    with open(cfg, "w") as f:
        f.write(SWAY_CFG)
    sess = os.path.join(WORK, "session-%d.sh" % display)
    with open(sess, "w") as f:
        f.write("#!/bin/sh\nexport WLR_RENDERER=pixman WLR_WL_OUTPUTS=%d\n"
                "exec %s > %s/session-%d.log 2>&1\n"
                % (outputs, session or "sway -c " + cfg, s.logdir,
                   display))
    os.chmod(sess, 0o755)
    env = {k: v for k, v in os.environ.items()
           if k not in ("DISPLAY", "WAYLAND_DISPLAY", "XDG_SESSION_TYPE",
                        "SWAYSOCK")}
    env["XDG_RUNTIME_DIR"] = runtime
    cmd = ["Xvnc", "--wayland", ":%d" % display, "-geometry", geometry,
           "-rfbauth", passwd, "-SecurityTypes", "VncAuth", "-localhost"] + \
        list(extra) + ["--", sess]
    server = s.spawn("nested-server-%d" % display, cmd, env=env)
    return server, env, runtime


def stop(server):
    """Stops a server and its session before starting the next one, so
    that the servers don't compete for /dev/shm (64 MiB in Docker)."""
    import signal
    try:
        os.killpg(server.pid, signal.SIGTERM)
        server.wait(10)
    except (OSError, Exception):
        pass


def nested_env(env, runtime, own, sway=True):
    """Environment that talks to the nested compositor."""
    end = time.time() + 20
    while time.time() < end:
        socks = [p for p in glob.glob(runtime + "/wayland-*")
                 if not p.endswith(".lock") and
                 os.path.basename(p) != own]
        ipc = glob.glob(runtime + "/sway-ipc.*.sock")
        if socks and (ipc or not sway):
            e = dict(env)
            e["WAYLAND_DISPLAY"] = os.path.basename(socks[0])
            if ipc:
                e["SWAYSOCK"] = ipc[0]
            return e
        time.sleep(0.5)
    return None


def sway_outputs(env):
    rc, out = sh(["swaymsg", "-r", "-t", "get_outputs"], env=env)
    try:
        return [(o["name"], o["rect"]["x"], o["rect"]["y"],
                 o["current_mode"]["width"], o["current_mode"]["height"])
                for o in json.loads(out) if o.get("active")]
    except (ValueError, KeyError):
        return out


def foot_output(env):
    """Name of the sway output that shows foot."""
    rc, out = sh(["swaymsg", "-r", "-t", "get_tree"], env=env)
    try:
        tree = json.loads(out)
    except ValueError:
        return None

    def walk(node, output):
        if node.get("type") == "output":
            output = node.get("name")
        if node.get("app_id") == "nested-foot":
            return output
        for child in node.get("nodes", []) + node.get("floating_nodes", []):
            r = walk(child, output)
            if r:
                return r
        return None
    return walk(tree, None)


def server_log(display):
    try:
        return open(s.artifact("logs/nested-server-%d.log" % display)).read()
    except OSError:
        return ""


def fraction(c, colour, x0, y0, x1, y1, step=4):
    hit = tot = 0
    for y in range(y0, y1, step):
        for x in range(x0, x1, step):
            tot += 1
            if c.pixel(x, y) == colour:
                hit += 1
    return hit / max(tot, 1)


# ---------------------------------------------------------------- 1 output

server, env, runtime = start(1, 1)
PORT = 5901
nenv = None


def t_detect():
    global nenv
    if not wait_tcp(PORT, timeout=20):
        return False, "server not listening (exit=%s)" % server.poll()
    nenv = nested_env(env, runtime, "wayland-1")
    if not nenv:
        return False, "sway did not create its sockets"
    end = time.time() + 15
    while time.time() < end and "nested compositor serving" not in \
            server_log(1):
        time.sleep(0.5)
    log = server_log(1)
    line = [l.strip() for l in log.splitlines() if "nested compositor" in l]
    bridge = [l.strip() for l in log.splitlines()
              if "Clipboard bridge to nested compositor" in l]
    return bool(line) and bool(bridge), "server log: %r %r" % (line, bridge)


s.run("sway session detected as nested compositor, clipboard bridge "
      "connected", t_detect)
if not nenv:
    sys.exit(s.finish())

vnc = RFBClient(port=PORT, security="VncAuth", password=PASSWORD)


def t_fullscreen():
    # foot is sway's only window: tiled, it fills sway's output, which
    # must fill our whole screen without decorations
    end = time.time() + 15
    while time.time() < end:
        vnc.refresh()
        f = fraction(vnc, FOOT_BG, 0, 0, vnc.width, vnc.height)
        if f > 0.9:
            break
        time.sleep(1)
    shot = vnc.screenshot(s.artifact("nested-01-sway-foot-fullscreen.png"))
    outs = sway_outputs(nenv)
    corners = [vnc.pixel(x, y) for x, y in ((0, 0), (1023, 0), (0, 767),
                                             (1023, 767))]
    ok = f > 0.9 and outs == [("WL-1", 0, 0, 1024, 768)] and \
        all(px == FOOT_BG for px in corners[1:])
    return ok, ("foot background covers %.0f%% of 1024x768, corners %r, "
                "sway outputs %r" % (f * 100, corners, outs)), [shot]


s.run("nested sway output is fullscreen and undecorated (screenshot "
      "over VNC)", t_fullscreen)


def t_keyboard():
    vnc.click(500, 400)
    time.sleep(0.5)
    # SWAYSOCK only exists inside sway's session, TERM=foot inside foot
    vnc.type_text("echo \"nested-key-$((6*7))-$TERM-${SWAYSOCK:+sway}\" > "
                  "%s/key.txt\n" % WORK)
    ok = wait_file(WORK + "/key.txt", 5)
    content = open(WORK + "/key.txt").read().strip() if ok else ""
    vnc.refresh()
    shot = vnc.screenshot(s.artifact("nested-02-typed.png"))
    return content == "nested-key-42-foot-sway", \
        "file content %r" % content, [shot]


s.run("keyboard input reaches a client of the nested compositor (foot "
      "in sway)", t_keyboard)


def t_pointer():
    vnc.type_text("clear; echo NESTEDPTR\n")
    time.sleep(1)
    vnc.cut_texts.clear()
    # double click the first word in the top left corner
    x, y = 30, 7
    vnc.pointer(x, y, 0)
    time.sleep(0.05)
    for _ in range(2):
        vnc.pointer(x, y, 1)
        time.sleep(0.03)
        vnc.pointer(x, y, 0)
        time.sleep(0.03)
    time.sleep(1)
    rc, out = sh(["wl-paste", "--primary", "--no-newline"], env=nenv)
    txt = vnc.wait_cut_text(5)
    vnc.refresh()
    shot = vnc.screenshot(s.artifact("nested-03-selected.png"))
    return out == "NESTEDPTR" and txt == "NESTEDPTR", \
        ("primary in sway after double click: %r; viewer received %r "
         "(primary bridged to VNC)" % (out, txt)), [shot]


s.run("pointer reaches nested client (double click selects word), "
      "primary nested -> viewer", t_pointer)


def t_clip_nested_to_vnc():
    vnc.cut_texts.clear()
    s.spawn("wl-copy-nested", ["wl-copy", "--foreground",
                               "from-sway-123"], env=nenv)
    txt = vnc.wait_cut_text(5)
    return txt == "from-sway-123", "viewer received %r" % txt


def t_clip_vnc_to_nested():
    vnc.cut_text("from-vnc-to-sway-456")
    time.sleep(1)
    rc1, c = sh(["wl-paste", "--no-newline"], env=nenv)
    rc2, p = sh(["wl-paste", "--primary", "--no-newline"], env=nenv)
    return c == p == "from-vnc-to-sway-456", \
        "sway clipboard=%r primary=%r" % (c, p)


def t_paste_into_nested_foot():
    vnc.cut_text("echo pasted-$((40+2)) > %s/paste.txt" % WORK)
    time.sleep(0.5)
    vnc.click(500, 400)
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


s.run("clipboard nested -> viewer (wl-copy in sway -> ServerCutText)",
      t_clip_nested_to_vnc)
s.run("clipboard viewer -> nested (ClientCutText -> wl-paste and "
      "wl-paste --primary in sway)", t_clip_vnc_to_nested)
s.run("clipboard viewer -> paste into foot inside sway with "
      "Ctrl+Shift+V", t_paste_into_nested_foot)


def t_resize():
    vnc.set_desktop_size(1280, 720)
    end = time.time() + 5
    while time.time() < end and (vnc.width, vnc.height) != (1280, 720):
        vnc.request_update(True)
        vnc.pump(0.3)
    end = time.time() + 10
    outs = None
    while time.time() < end:
        outs = sway_outputs(nenv)
        if outs == [("WL-1", 0, 0, 1280, 720)]:
            break
        time.sleep(0.5)
    time.sleep(1)
    vnc.refresh()
    f = fraction(vnc, FOOT_BG, 0, 0, vnc.width, vnc.height)
    shot = vnc.screenshot(s.artifact("nested-04-resized-1280x720.png"))
    ok = (vnc.width, vnc.height) == (1280, 720) and \
        outs == [("WL-1", 0, 0, 1280, 720)] and f > 0.9 and \
        vnc.pixel(1279, 719) == FOOT_BG
    return ok, ("viewer fb %dx%d, sway outputs %r, foot covers %.0f%%"
                % (vnc.width, vnc.height, outs, f * 100)), [shot]


s.run("client resize: nested output follows (1024x768 -> 1280x720)",
      t_resize)
vnc.close()
stop(server)

# --------------------------------------------------- 2 outputs, 2 screens

server2, env2, runtime2 = start(2, 2)
PORT2 = 5902
nenv2 = None


def t_multi():
    global nenv2
    if not wait_tcp(PORT2, timeout=20):
        return False, "server not listening (exit=%s)" % server2.poll()
    nenv2 = nested_env(env2, runtime2, "wayland-2")
    if not nenv2:
        return False, "sway did not create its sockets"
    v = RFBClient(port=PORT2, security="VncAuth", password=PASSWORD)
    v.refresh()
    v.set_desktop_size(1600, 600, [(1, 0, 0, 800, 600, 0),
                                   (2, 800, 0, 800, 600, 0)])
    end = time.time() + 5
    while time.time() < end and (v.width, v.height) != (1600, 600):
        v.request_update(True)
        v.pump(0.3)
    end = time.time() + 10
    # Only the sizes are ours to decide; where sway places its outputs
    # in its own logical layout is sway's configuration
    want = [("WL-1", 800, 600), ("WL-2", 800, 600)]

    def sizes(o):
        return sorted((n, w, h) for n, x, y, w, h in o) \
            if isinstance(o, list) else o
    outs = None
    while time.time() < end:
        outs = sway_outputs(nenv2)
        if sizes(outs) == want:
            break
        time.sleep(0.5)
    time.sleep(1)
    v.refresh()
    # Each sway output must fill exactly its own screen: one shows foot,
    # the other only sway's background. Our output windows go to the
    # screens in creation order, so WL-1 is the left one.
    foot_out = foot_output(nenv2)
    end = time.time() + 10
    while True:
        fl, bl = (fraction(v, FOOT_BG, 0, 0, 800, 600),
                  fraction(v, BG, 0, 0, 800, 600))
        fr, br = (fraction(v, FOOT_BG, 800, 0, 1600, 600),
                  fraction(v, BG, 800, 0, 1600, 600))
        if (min(fl, br) > 0.9 or min(bl, fr) > 0.9) or time.time() > end:
            break
        time.sleep(1)
        v.refresh()
    shot = v.screenshot(s.artifact("nested-05-two-screens.png"))
    v.close()
    if foot_out == "WL-1":
        placed = fl > 0.9 and br > 0.9
    else:
        placed = foot_out == "WL-2" and bl > 0.9 and fr > 0.9
    ok = sizes(outs) == want and placed
    return ok, ("viewer fb %dx%d; sway outputs %r; foot is on %s; left "
                "screen foot %.0f%%/background %.0f%%, right screen foot "
                "%.0f%%/background %.0f%%"
                % (v.width, v.height, outs, foot_out, fl * 100, bl * 100,
                   fr * 100, br * 100)), [shot]


s.run("multi-monitor: two-screen client layout -> two nested outputs "
      "(sway WLR_WL_OUTPUTS=2)", t_multi)
stop(server2)

# ----------------------------------------------------------------- labwc

HOST_BG = (46, 51, 63)    # Xvnc-wayland's own background


def wlr_randr_size(env):
    import re
    rc, out = sh(["wlr-randr"], env=env)
    m = re.search(r"(\d+)x(\d+) px[^\n]*current", out)
    return (int(m.group(1)), int(m.group(2))) if m else out.strip()[-200:]


# labwc applies a new output size but only repaints once something on
# the output changes, so the test runs foot in it and types into it
# after each size change, as a user would interact with the desktop
server3, env3, runtime3 = start(3, 1, session="labwc -s foot")


def poke(v):
    v.tap(ord("x"))
    time.sleep(1)
    v.refresh()
PORT3 = 5903


def t_labwc():
    if not wait_tcp(PORT3, timeout=20):
        return False, "server not listening (exit=%s)" % server3.poll()
    nenv3 = nested_env(env3, runtime3, "wayland-3", sway=False)
    if not nenv3:
        return False, "labwc did not create its socket"
    end = time.time() + 15
    while time.time() < end and "nested compositor serving" not in \
            server_log(3):
        time.sleep(0.5)
    v = RFBClient(port=PORT3, security="VncAuth", password=PASSWORD)
    time.sleep(2)
    poke(v)
    host_bg = fraction(v, HOST_BG, 0, 0, v.width, v.height)
    size1 = wlr_randr_size(nenv3)
    shot = v.screenshot(s.artifact("nested-06-labwc.png"))
    v.cut_texts.clear()
    s.spawn("wl-copy-labwc", ["wl-copy", "--foreground", "from-labwc-77"],
            env=nenv3)
    txt = v.wait_cut_text(5)
    v.cut_text("to-labwc-88")
    time.sleep(1)
    rc, back = sh(["wl-paste", "--no-newline"], env=nenv3)
    v.set_desktop_size(1280, 720)
    end = time.time() + 10
    size2 = None
    while time.time() < end:
        size2 = wlr_randr_size(nenv3)
        if size2 == (1280, 720):
            break
        time.sleep(0.5)
    poke(v)
    host_bg2 = fraction(v, HOST_BG, 0, 0, v.width, v.height)
    shot2 = v.screenshot(s.artifact("nested-07-labwc-1280x720.png"))
    v.close()
    detected = "nested compositor serving" in server_log(3)
    ok = (detected and size1 == (1024, 768) and host_bg < 0.01 and
          txt == "from-labwc-77" and back == "to-labwc-88" and
          size2 == (1280, 720) and host_bg2 < 0.01)
    return ok, ("detected=%s; labwc output %r, our background visible on "
                "%.1f%%; clipboard labwc->viewer %r, viewer->labwc %r; "
                "after resize labwc output %r, our background %.1f%%"
                % (detected, size1, host_bg * 100, txt, back, size2,
                   host_bg2 * 100)), [shot, shot2]


s.run("labwc session: detected, output fills the screen, clipboard both "
      "ways, resize follows", t_labwc)
stop(server3)

# ------------------------------------------------- NestedCompositor=Off

server4, env4, runtime4 = start(4, 1, extra=["-NestedCompositor", "Off"])
PORT4 = 5904


def t_off():
    if not wait_tcp(PORT4, timeout=20):
        return False, "server not listening (exit=%s)" % server4.poll()
    nenv4 = nested_env(env4, runtime4, "wayland-4")
    time.sleep(4)
    v = RFBClient(port=PORT4, security="VncAuth", password=PASSWORD)
    v.refresh()
    host_bg = fraction(v, HOST_BG, 0, 0, v.width, v.height)
    shot = v.screenshot(s.artifact("nested-08-nestedcompositor-off.png"))
    v.close()
    outs = sway_outputs(nenv4) if nenv4 else None
    detected = "nested compositor serving" in server_log(4)
    # sway is then an ordinary (floating) window: its size is its own
    # choice and our background shows around it
    ok = not detected and isinstance(outs, list) and len(outs) == 1 and \
        outs[0][3:] != (1024, 768) and host_bg > 0.05
    return ok, ("detected=%s; sway output %r; our background visible on "
                "%.0f%%" % (detected, outs, host_bg * 100)), [shot]


s.run("NestedCompositor=Off: sway is managed as an ordinary window "
      "(floating WM fallback)", t_off)

sys.exit(s.finish())
