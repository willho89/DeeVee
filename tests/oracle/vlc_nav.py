#!/usr/bin/env python3
# DeeVee — menu navigation oracle (CLAUDE.md M6c).
#
# Plays the same timed RetroPad/VLC key script in DeeVee (headless harness)
# and VLC (Flatpak, rc interface, --no-dvdnav-menu so both start at First
# Play), then diffs what each one went through: VTS/domain changes, cells
# (cellN, pgN, pgc_length, cell_start) and the highlighted button.
#
#   vlc_nav.py <iso> "<t>:<KEY>,<t>:<KEY>,..." [end_seconds] [--frames]
#
# ORACLE_OUT=<dir> (default build/vlc), ORACLE_RETRIES=<n> (default 2).
#
# KEY: UP DOWN LEFT RIGHT A (activate). t in seconds from start.
# --frames also compares the last picture (luma MSE) of both.
#
# Run from the repo root; needs build/harness, deevee_libretro.so and the
# org.videolan.VLC Flatpak. VLC's Flatpak has a private /tmp, so outputs go
# to build/vlc/.
import os
import re
import subprocess
import sys
import threading
import time

FPS = 60000 / 1001
VLC_KEYS = {"UP": "key-nav-up", "DOWN": "key-nav-down", "LEFT": "key-nav-left",
            "RIGHT": "key-nav-right", "A": "key-nav-activate"}
OUT = os.environ.get("ORACLE_OUT", "build/vlc")
RETRIES = int(os.environ.get("ORACLE_RETRIES", "2"))


def parse_script(text):
    steps = []
    for item in filter(None, text.split(",")):
        t, key = item.split(":")
        if key not in VLC_KEYS:
            sys.exit("unknown key " + key)
        steps.append((float(t), key))
    return steps


def run_deevee(iso, steps, end, frames):
    inp = ",".join("%d:%s" % (round(t * FPS), k) for t, k in steps)
    env = dict(os.environ, HARNESS_DEBUG="1", HARNESS_INPUT=inp)
    nframes = round(end * FPS)
    if frames:
        env["HARNESS_DUMP"] = OUT
        env["HARNESS_DUMP_FRAMES"] = str(nframes - 1)
    cmd = ["distrobox", "enter", "deevee-build", "--", "env"] + \
          ["%s=%s" % kv for kv in env.items() if kv[0].startswith("HARNESS")] + \
          ["build/harness", "./deevee_libretro.so", iso, str(nframes), "build/system"]
    r = subprocess.run(cmd, capture_output=True, text=True)
    log = r.stderr + r.stdout
    events = []
    for line in log.splitlines():
        m = re.search(r"\[f(\d+) .*VTS \d+ -> (\d+) \(domain (\d+)\)", line)
        if m:
            events.append((int(m[1]) / FPS, ("vts", int(m[2]), int(m[3]))))
        m = re.search(r"\[f(\d+) .*navstate: cellN=(\d+) pgN=(\d+) pgc_length=(\d+) cell_start=(\d+)", line)
        if m:
            events.append((int(m[1]) / FPS, ("cell",) + tuple(int(x) for x in m.groups()[1:])))
        m = re.search(r"\[f(\d+) .*highlight: button (\d+)", line)
        if m:
            events.append((int(m[1]) / FPS, ("button", int(m[2]))))
    return events, log, (OUT + "/frame_%06u.ppm" % (nframes - 1)) if frames else None


def run_vlc(iso, steps, end, frames):
    y4m = OUT + "/oracle.y4m"
    cmd = ["flatpak", "run", "--command=vlc", "org.videolan.VLC", "--ignore-config",
           "--no-qt-privacy-ask", "-I", "rc", "--rc-fake-tty", "--no-audio",
           "--no-video-title-show", "--verbose=2", "--no-dvdnav-menu"]
    if frames:
        cmd += ["--vout=yuv", "--yuv-file=" + y4m, "--yuv-yuv4mpeg2", "--yuv-chroma=I420"]
    else:
        cmd += ["--vout=vdummy"]
    cmd.append("dvd://" + os.path.abspath(iso))
    p = subprocess.Popen(cmd, stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                         stderr=subprocess.STDOUT, text=True, bufsize=1)
    # Time the script from VLC's first disc event (First Play's VTS change,
    # DeeVee's frame 0), not from process start: Flatpak start-up varies by
    # seconds, more so right after a previous instance, and keys sent before
    # a menu is up are dropped.
    lines, started = [], threading.Event()

    def reader():
        for line in p.stdout:
            lines.append(line)
            if not started.is_set() and "DVDNAV_VTS_CHANGE" in line:
                started.set()
        started.set()

    th = threading.Thread(target=reader, daemon=True)
    th.start()
    started.wait(timeout=60)
    t0 = time.time()
    for t, key in steps:
        time.sleep(max(0.0, t - (time.time() - t0)))
        p.stdin.write("key %s\n" % VLC_KEYS[key])
        p.stdin.flush()
    time.sleep(max(0.0, end - (time.time() - t0)))
    p.stdin.write("quit\n")
    p.stdin.flush()
    p.wait(timeout=30)
    th.join(timeout=10)
    log = "".join(lines)

    events, cur, pending, hopping = [], None, {}, False
    for line in log.splitlines():
        m = re.search(r"dvdnav demux debug: (DVDNAV_\w+)$", line)
        if m:
            cur = m[1]
            pending = {}
            # Between a jump and its first cell VLC reports the new PGC's
            # default button early; DeeVee reports it once on screen.
            if cur == "DVDNAV_HOP_CHANNEL":
                hopping = True
            elif cur == "DVDNAV_CELL_CHANGE":
                hopping = False
            continue
        m = re.search(r"dvdnav demux debug:\s+- (\w+)=(-?\d+)", line)
        if m and cur:
            pending[m[1]] = int(m[2])
            if cur == "DVDNAV_VTS_CHANGE" and "domain" in pending:
                events.append((0, ("vts", pending["vtsN"], pending["domain"])))
            if cur == "DVDNAV_CELL_CHANGE" and "pg_start" in pending:
                events.append((0, ("cell", pending["cellN"], pending["pgN"],
                                   pending["pgc_length"], pending["cell_start"])))
            continue
        m = re.search(r"dvdnav demux debug: buttonUpdate (\d+)$", line)
        if m and not hopping:
            events.append((0, ("button", int(m[1]))))
    return events, log, y4m if frames else None


def squash(events):
    """Drop repeated buttons (VLC re-reports the button at every PCI)."""
    out = []
    for _, e in events:
        if out and e[0] == "button" and out[-1] == e:
            continue
        out.append(e)
    return out


def luma(path):
    if path.endswith(".ppm"):
        cmd = ["ffmpeg", "-loglevel", "error", "-i", path]
    else:
        cmd = ["ffmpeg", "-loglevel", "error", "-sseof", "-0.5", "-i", path]
    cmd += ["-frames:v", "1", "-vf", "scale=720:480", "-pix_fmt", "gray", "-f", "rawvideo", "-"]
    return subprocess.run(cmd, capture_output=True).stdout


def main():
    args = [a for a in sys.argv[1:] if not a.startswith("--")]
    frames = "--frames" in sys.argv
    iso, steps = args[0], parse_script(args[1])
    end = float(args[2]) if len(args) > 2 else (steps[-1][0] + 8 if steps else 20)
    os.makedirs(OUT, exist_ok=True)

    d_ev, d_log, d_pic = run_deevee(iso, steps, end, frames)
    d = squash(d_ev)
    # VLC's rc interface occasionally drops a key (more so under load): its
    # trace then stops short as a strict prefix of DeeVee's. Retry those; a
    # real divergence differs somewhere instead.
    for attempt in range(RETRIES + 1):
        v_ev, v_log, v_pic = run_vlc(iso, steps, end, frames)
        v = squash(v_ev)
        if not (len(v) < len(d) and d[:len(v)] == v):
            break
        print("(vlc trace is a prefix of deevee's, retry %d)" % (attempt + 1), flush=True)
    open(OUT + "/oracle_deevee.log", "w").write(d_log)
    open(OUT + "/oracle_vlc.log", "w").write(v_log)

    n = max(len(d), len(v))
    first = None
    for i in range(n):
        a = d[i] if i < len(d) else None
        b = v[i] if i < len(v) else None
        mark = "  " if a == b else "!!"
        if a != b and first is None:
            first = i
        print("%s %-40s %s" % (mark, a, b))
    print("deevee %d events, vlc %d events, first divergence: %s" %
          (len(d), len(v), "none" if first is None else first))
    if frames:
        a, b = luma(d_pic), luma(v_pic)
        if a and b and len(a) == len(b):
            mse = sum((x - y) ** 2 for x, y in zip(a, b)) / len(a)
            print("last picture luma MSE %.1f" % mse)
    sys.exit(0 if first is None else 1)


if __name__ == "__main__":
    main()
