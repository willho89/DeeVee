#!/usr/bin/env python3
# DeeVee — play-out paths (CLAUDE.md M6c).
#
# For each key script from walk.py that starts a title, let the title play
# to its end and see where the disc returns (post-commands pick the menu
# and button). One script per distinct title, with the end time DeeVee
# measured plus a margin, printed as "<script> <end_seconds>" for
# vlc_nav.py.
#
#   playout.py <iso> <paths.txt> [max_title_seconds] > playout.txt
import re
import subprocess
import sys

FPS = 60000 / 1001


def run(iso, script, end):
    inp = ",".join("%d:%s" % (round(float(t) * FPS), k)
                   for t, k in (s.split(":") for s in script.split(",")))
    cmd = ["distrobox", "enter", "deevee-build", "--", "env", "HARNESS_DEBUG=1",
           "HARNESS_INPUT=" + inp, "build/harness", "./deevee_libretro.so", iso,
           str(round(end * FPS)), "build/system"]
    r = subprocess.run(cmd, capture_output=True, text=True)
    return r.stderr + r.stdout


def events(log):
    out = []
    for line in log.splitlines():
        m = re.search(r"\[f(\d+) .*VTS \d+ -> (\d+) \(domain (\d+)\)", line)
        if m:
            out.append((int(m[1]) / FPS, int(m[2]), int(m[3])))
    return out


def main():
    iso, paths = sys.argv[1], sys.argv[2]
    max_len = float(sys.argv[3]) if len(sys.argv) > 3 else 300.0
    scripts = [l.strip() for l in open(paths) if l.strip() and not l.startswith("#")]
    seen = set()
    for s in scripts:
        last = float(s.split(",")[-1].split(":")[0])
        ev = [e for e in events(run(iso, s, last + 6)) if e[0] >= last - 0.1]
        title = next((e for e in ev if e[2] == 2), None)
        if not title:
            continue
        # The title's first cell identifies it (several menus start the same one).
        log = run(iso, s, last + max_len)
        cells = re.findall(r"\[f(\d+) .*navstate: (cellN=\d+ pgN=\d+ pgc_length=\d+ cell_start=\d+)", log)
        first = next((c for f, c in cells if int(f) / FPS >= title[0] - 0.1), None)
        key = (title[1], first)
        if key in seen:
            continue
        seen.add(key)
        back = next((e for e in events(log) if e[0] > title[0] + 0.5 and e[2] != 2), None)
        if not back:
            print("# title vts %d longer than %g s: %s" % (title[1], max_len, s), file=sys.stderr)
            continue
        print("%s %d" % (s, int(back[0]) + 20), flush=True)


if __name__ == "__main__":
    main()
