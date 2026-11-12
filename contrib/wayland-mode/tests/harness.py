"""Small test harness shared by the Wayland mode test scripts."""

import json
import os
import socket
import subprocess
import sys
import time
import traceback

ARTIFACTS = os.environ.get("ARTIFACTS", "/artifacts")


class Suite:
    def __init__(self, name):
        self.name = name
        self.results = []
        self.procs = []
        os.makedirs(ARTIFACTS, exist_ok=True)
        self.logdir = os.path.join(ARTIFACTS, "logs")
        os.makedirs(self.logdir, exist_ok=True)

    def artifact(self, name):
        return os.path.join(ARTIFACTS, name)

    def check(self, test, ok, detail="", artifacts=()):
        status = "PASS" if ok else "FAIL"
        self.results.append({"suite": self.name, "test": test,
                             "status": status, "detail": detail,
                             "artifacts": list(artifacts)})
        print("[%s] %s: %s %s" % (status, self.name, test, detail),
              flush=True)
        return ok

    def missing(self, tools):
        """The test tools (programs) of `tools` that are not installed."""
        import shutil
        return [t for t in tools if shutil.which(t) is None]

    def skip_all(self, reason):
        """Every following run() is recorded as SKIP with `reason`."""
        self.skip_reason = reason

    @property
    def skipping(self):
        return bool(getattr(self, "skip_reason", None))

    def skip(self, test, reason):
        self.results.append({"suite": self.name, "test": test,
                             "status": "SKIP", "detail": reason,
                             "artifacts": []})
        print("[SKIP] %s: %s %s" % (self.name, test, reason), flush=True)
        return True

    def run(self, test, func, needs=()):
        """Run func(); it returns (ok, detail[, artifacts]), or ("SKIP",
        reason) when it proved the test does not apply. `needs`: test
        tools (Wayland clients) the test drives; if one is not installed
        on this distro the test is recorded as SKIP, never as PASS."""
        if self.skipping:
            return self.skip(test, self.skip_reason)
        gone = self.missing(needs)
        if gone:
            return self.skip(test, "needs %s, not installed on this "
                             "distro" % ", ".join(gone))
        try:
            r = func()
            if r[0] == "SKIP":   # the test proved it cannot apply here
                return self.skip(test, r[1])
            if len(r) == 2:
                return self.check(test, r[0], r[1])
            return self.check(test, r[0], r[1], r[2])
        except Exception as e:  # report, never hide
            tb = traceback.format_exc()
            return self.check(test, False, "exception: %r\n%s" % (e, tb))

    def spawn(self, name, cmd, env=None, **kw):
        log = open(os.path.join(self.logdir, name + ".log"), "wb")
        p = subprocess.Popen(cmd, env=env, stdout=log,
                             stderr=subprocess.STDOUT,
                             start_new_session=True, **kw)
        self.procs.append(p)
        return p

    def finish(self):
        for p in reversed(self.procs):
            if p.poll() is None:
                try:
                    os.killpg(p.pid, 15)
                except OSError:
                    pass
        time.sleep(1)
        for p in self.procs:
            if p.poll() is None:
                try:
                    os.killpg(p.pid, 9)
                except OSError:
                    pass
        path = os.path.join(ARTIFACTS, "results-%s.json" % self.name)
        with open(path, "w") as f:
            json.dump(self.results, f, indent=2)
        failed = [r for r in self.results if r["status"] == "FAIL"]
        skipped = [r for r in self.results if r["status"] == "SKIP"]
        print("== %s: %d passed, %d failed%s" %
              (self.name, len(self.results) - len(failed) - len(skipped),
               len(failed),
               ", %d skipped" % len(skipped) if skipped else ""))
        return 1 if failed else 0


def wait_tcp(port, host="127.0.0.1", timeout=20.0):
    end = time.time() + timeout
    while time.time() < end:
        try:
            with socket.create_connection((host, port), 0.5):
                return True
        except OSError:
            time.sleep(0.2)
    return False


def wait_file(path, timeout=20.0):
    end = time.time() + timeout
    while time.time() < end:
        if os.path.exists(path) and os.path.getsize(path) > 0:
            return True
        time.sleep(0.2)
    return False


def sh(cmd, env=None, timeout=10, check=False, input=None):
    r = subprocess.run(cmd, shell=isinstance(cmd, str), env=env,
                       capture_output=True, timeout=timeout, input=input)
    out = r.stdout.decode("utf-8", "replace")
    if check and r.returncode != 0:
        raise RuntimeError("%s failed (%d): %s %s" %
                           (cmd, r.returncode, out,
                            r.stderr.decode("utf-8", "replace")))
    return r.returncode, out


def read_env_file(path):
    env = {}
    with open(path) as f:
        for line in f:
            if "=" in line:
                k, v = line.rstrip("\n").split("=", 1)
                env[k] = v
    return env


def find_colour_bbox(client, colour, tolerance=0, step=2, min_run=100,
                     region=None):
    """Bounding box (x0, y0, x1, y1) of the area filled with the given
    colour. Only rows and columns with at least `min_run` matching pixels
    count, so text and antialiasing elsewhere are ignored."""
    w, h = client.width, client.height
    rx0, ry0, rx1, ry1 = region or (0, 0, w, h)
    fb = client.fb
    r0, g0, b0 = colour
    rows, cols = {}, {}
    for y in range(ry0, ry1, step):
        base = y * w * 4
        for x in range(rx0, rx1, step):
            off = base + x * 4
            if (abs(fb[off + 2] - r0) <= tolerance and
                    abs(fb[off + 1] - g0) <= tolerance and
                    abs(fb[off] - b0) <= tolerance):
                rows[y] = rows.get(y, 0) + step
                cols[x] = cols.get(x, 0) + step
    ys = [y for y, n in rows.items() if n >= min_run]
    xs = [x for x, n in cols.items() if n >= min_run]
    if not xs or not ys:
        return None
    return (min(xs), min(ys), max(xs), max(ys))


def make_vnc_passwd(path, password):
    r = subprocess.run(["vncpasswd", "-f"], input=(password + "\n").encode(),
                       capture_output=True, check=True)
    with open(path, "wb") as f:
        f.write(r.stdout)
    os.chmod(path, 0o600)


def make_cert(certpath, keypath):
    subprocess.run(["openssl", "req", "-x509", "-newkey", "rsa:2048",
                    "-nodes", "-keyout", keypath, "-out", certpath,
                    "-days", "30", "-subj", "/CN=localhost"],
                   check=True, capture_output=True)
