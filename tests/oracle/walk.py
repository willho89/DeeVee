#!/usr/bin/env python3
# DeeVee — menu tree walker (CLAUDE.md M6c).
#
# Explores a disc's menus with the headless harness: from the first menu,
# reach every button over the PCI link table (logged by dv_nav at DEBUG),
# activate it, and recurse into the menu it leads to. Prints one key script
# per button, in vlc_nav.py's format, for replaying against VLC.
#
#   walk.py <iso> [first_key_seconds] [max_depth] > paths.txt
import collections
import os
import re
import subprocess
import sys

FPS = 60000 / 1001
STEP = 2.0      # seconds between key presses
SETTLE = 4.0    # after an activation, before the next key


def run(iso, script, end):
    inp = ",".join("%d:%s" % (round(t * FPS), k) for t, k in script)
    cmd = ["distrobox", "enter", "deevee-build", "--", "env", "HARNESS_DEBUG=1",
           "HARNESS_INPUT=" + inp, "build/harness", "./deevee_libretro.so", iso,
           str(round(end * FPS)), "build/system"]
    r = subprocess.run(cmd, capture_output=True, text=True)
    return r.stderr + r.stdout


def last_menu(log, after):
    """(button table, selected button, fingerprint) of the menu on screen
    after time 'after' (seconds), or None if no buttons appeared."""
    table, sel, cells, domain = None, None, [], None
    for line in log.splitlines():
        m = re.match(r"\[f(\d+) ", line)
        if not m:
            continue
        t = int(m[1]) / FPS
        m2 = re.search(r"VTS \d+ -> (\d+) \(domain (\d+)\)", line)
        if m2:
            domain = (int(m2[1]), int(m2[2]))
        m2 = re.search(r"navstate: (.*)", line)
        if m2:
            cells.append(m2[1])
        if t < after:
            continue
        m2 = re.search(r"buttons \(u/d/l/r\): (.*)", line)
        if m2:
            table = {}
            for item in m2[1].split():
                b, links = item.split(":")
                auto = links.endswith("a")
                u, d, l, r = (int(x) for x in links.rstrip("a").split("/"))
                table[int(b)] = (u, d, l, r, auto)
        m2 = re.search(r"highlight: button (\d+)", line)
        if m2:
            sel = int(m2[1])
    if not table or not sel:
        return None, domain
    fp = (domain, cells[-1] if cells else None, tuple(sorted(table.items())))
    return (table, sel, fp), domain


def routes(table, start):
    """Shortest key sequence from the start button to every button. Moving
    onto an auto-action button activates it, so don't route through one."""
    dirs = ("UP", "DOWN", "LEFT", "RIGHT")
    best = {start: []}
    q = collections.deque([start])
    while q:
        b = q.popleft()
        if b != start and table[b][4]:
            continue
        for i, key in enumerate(dirs):
            n = table[b][i]
            if n in table and n not in best:
                best[n] = best[b] + [key]
                q.append(n)
    return best


def main():
    iso = sys.argv[1]
    first = float(sys.argv[2]) if len(sys.argv) > 2 else 14.0
    max_depth = int(sys.argv[3]) if len(sys.argv) > 3 else 4
    seen = set()
    out = []

    def explore(script, t, depth):
        log = run(iso, script, t + 1.0)
        # The menu reached by the last key (activation or auto-action move).
        menu, _ = last_menu(log, script[-1][0] - 0.1 if script else 0.0)
        if not menu:
            return
        table, sel, fp = menu
        if fp in seen:
            return
        seen.add(fp)
        for b, keys in sorted(routes(table, sel).items()):
            s, tt = list(script), t
            for k in keys:
                s.append((tt, k))
                tt += STEP
            if not table[b][4]:
                s.append((tt, "A"))
                tt += STEP
            out.append(s)
            print(",".join("%g:%s" % (a, k) for a, k in s), flush=True)
            if depth + 1 < max_depth:
                explore(s, tt + SETTLE, depth + 1)

    explore([], first, 0)
    print("# %d paths, %d menus" % (len(out), len(seen)), file=sys.stderr)


if __name__ == "__main__":
    main()
