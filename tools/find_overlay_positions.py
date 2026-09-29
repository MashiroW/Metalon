"""Finds where each room overlay (environmental animation) sits on its room
picture, for every room of assets/levels, and writes the result to
data/rooms/<level>/<room>_overlays.cfg.

The overlays are the frames <name>.<n>.png of a room folder (the original
game's .smk videos, as exported in the Silver Blockouts). The original game
positions them from its compiled scripts (never decoded), but an overlay
is a pre-rendered PATCH of the scene itself -- it replaces the matching part
of the background -- so its position is where its frames match the room's
picture best (the method validated on the discs' .smk files, see
docs/PROGRESS.md). Every frame is matched (OpenCV, squared difference over
the opaque pixels) against the room picture and its variants (rep*.png of
the same folder); the frames vote for a position.

Needs only ./assets (the Silver Blockouts):  python tools/find_overlay_positions.py
Options: --room level/room   one room only, printing the details
         --cds PATH          folder holding CD1/ and CD2/ (the original discs' files):
                             each overlay's frame rate is read from its .smk there
                             (the PNG frames don't keep it); else 15 fps
"""
import struct
import argparse
import os
import re
import sys
from collections import Counter

import cv2
import numpy as np

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
LEVELS = os.path.join(ROOT, "assets", "levels")
FRAME_RE = re.compile(r"^(.+)\.(\d+)\.png$", re.I)


def load_rgba(path):
    img = cv2.imread(path, cv2.IMREAD_UNCHANGED)
    if img is None:
        return None
    if img.ndim == 2:
        img = cv2.cvtColor(img, cv2.COLOR_GRAY2BGRA)
    elif img.shape[2] == 3:
        img = cv2.cvtColor(img, cv2.COLOR_BGR2BGRA)
    return img


def match(bg, frame):
    """best (x, y, mean abs error per opaque pixel) of frame on bg"""
    fh, fw = frame.shape[:2]
    bh, bw = bg.shape[:2]
    if fw > bw or fh > bh:
        return None
    rgb = frame[:, :, :3].astype(np.float32)
    mask = (frame[:, :, 3] > 127).astype(np.float32)
    opaque = mask.sum()
    if opaque < 16:
        return None
    res = cv2.matchTemplate(bg[:, :, :3].astype(np.float32), rgb, cv2.TM_SQDIFF, mask=cv2.merge([mask] * 3))
    res = np.nan_to_num(res, nan=np.inf, posinf=np.inf)
    _, _, loc, _ = cv2.minMaxLoc(res)
    x, y = loc
    patch = bg[y:y + fh, x:x + fw, :3].astype(np.float32)
    err = (np.abs(patch - rgb).sum(axis=2) * mask).sum() / (opaque * 3)
    return x, y, float(err)


def edges(img):
    """soft edge map: compares shapes, whatever the brightness (glowing /
    animated overlays differ a lot from the still picture in colour)"""
    g = cv2.cvtColor(img[:, :, :3], cv2.COLOR_BGR2GRAY)
    return cv2.GaussianBlur(cv2.Canny(g, 40, 120).astype(np.float32), (5, 5), 0)


def edge_votes(bg, frames_paths):
    """second opinion for an uncertain overlay: frames vote by edge correlation"""
    e_bg = edges(bg)
    votes = Counter()
    for path in frames_paths:
        fr = load_rgba(path)
        if fr is None or fr.shape[0] > bg.shape[0] or fr.shape[1] > bg.shape[1]:
            continue
        e = edges(fr)
        if e.max() <= 0:
            continue
        res = cv2.matchTemplate(e_bg, e, cv2.TM_CCORR_NORMED)
        _, _, _, loc = cv2.minMaxLoc(res)
        votes[loc] += 1
    return votes


def room_overlays(level, room, verbose=False):
    folder = os.path.join(LEVELS, level, room)
    groups = {}
    for f in os.listdir(folder):
        m = FRAME_RE.match(f)
        if m:
            groups.setdefault(m.group(1).lower(), []).append((int(m.group(2)), f))
    backgrounds = {}
    for f in sorted(os.listdir(folder)):
        if f.lower().endswith(".png") and not FRAME_RE.match(f) and ".mask." not in f.lower():
            img = load_rgba(os.path.join(folder, f))
            if img is not None and img.shape[0] > 1:
                backgrounds[os.path.splitext(f)[0].lower()] = img
    out = []
    for name, frames in sorted(groups.items()):
        frames.sort()
        n = len(frames)
        # every frame when few, else a spread of them
        pick = frames if n <= 12 else [frames[round(i * (n - 1) / 11)] for i in range(12)]
        best = None
        for bgname, bg in backgrounds.items():
            votes = Counter()
            errs = {}
            for _, f in pick:
                fr = load_rgba(os.path.join(folder, f))
                r = match(bg, fr) if fr is not None else None
                if r is None:
                    continue
                votes[(r[0], r[1])] += 1
                errs.setdefault((r[0], r[1]), []).append(r[2])
            if not votes:
                continue
            pos, count = votes.most_common(1)[0]
            err = min(errs[pos])
            score = (count / len(pick), -err)
            if best is None or score > best[0]:
                best = (score, bgname, pos, count, err)
        fw = fh = 0
        fr0 = load_rgba(os.path.join(folder, frames[0][1]))
        if fr0 is not None:
            fh, fw = fr0.shape[:2]
        if best is None:
            out.append(dict(name=name, frames=n, w=fw, h=fh, x=0, y=0, bg="", votes=0, of=len(pick), err=999.0))
            continue
        _, bgname, pos, count, err = best
        o = dict(name=name, frames=n, w=fw, h=fh, x=pos[0], y=pos[1], bg=bgname, votes=count, of=len(pick), err=err, how="colour")
        if confidence(o) != "sure" and len(pick) >= 4:
            for bgn, bg in backgrounds.items():
                votes = edge_votes(bg, [os.path.join(folder, f) for _, f in pick])
                if not votes:
                    continue
                epos, ecount = votes.most_common(1)[0]
                if ecount * 4 >= len(pick) * 3 and ecount > o["votes"]:
                    o.update(x=epos[0], y=epos[1], bg=bgn, votes=ecount, err=0.0, how="edges")
                    break
        out.append(o)
        pos, bgname, count, err = (o["x"], o["y"]), o["bg"], o["votes"], o["err"]
        if verbose:
            print(f"  {name:12s} {n:3d} frames {fw}x{fh}  at {pos}  on {bgname}  votes {count}/{len(pick)}  err {err:.1f}")
    return out


def confidence(o):
    """sure: (almost) all the frames tested land on the same spot -- flames
    and water differ a lot from the still picture, but a dozen frames voting
    for one position is strong evidence -- or most of them do and one
    matches the picture almost exactly"""
    if o["votes"] == 0:
        return "none"
    if o["of"] >= 4 and o["votes"] * 4 >= o["of"] * 3:
        return "sure"
    if o["votes"] * 2 >= o["of"] and o["err"] < 6:
        return "sure"
    return "guess"


def smk_rates(cds):
    """'level/room/name' -> frames per second, from the discs' .smk headers"""
    rates = {}
    if not cds:
        return rates
    for cd in ("CD1", "CD2"):
        base = os.path.join(cds, cd, "SILVER", "levels")
        if not os.path.isdir(base):
            continue
        for dirpath, _, files in os.walk(base):
            for f in files:
                if not f.lower().endswith(".smk"):
                    continue
                with open(os.path.join(dirpath, f), "rb") as fh:
                    head = fh.read(20)
                if len(head) < 20 or head[:3] != b"SMK":
                    continue
                rate = struct.unpack("<i", head[16:20])[0]  # > 0: ms per frame, < 0: 1/100000 s per frame, 0: 10 fps
                fps = 1000.0 / rate if rate > 0 else (100000.0 / -rate if rate < 0 else 10.0)
                rel = os.path.relpath(os.path.join(dirpath, os.path.splitext(f)[0]), base).replace(os.sep, "/").lower()
                rates[rel] = fps
    return rates


def write_cfg(level, room, ovs):
    path = os.path.join(ROOT, "data", "rooms", level, room + "_overlays.cfg")
    if not ovs:
        if os.path.exists(path):
            os.remove(path)
        return
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, "w", newline="\n") as f:
        f.write("# Silver Remaster: this room's overlays (environmental animations) -- tools/find_overlay_positions.py\n")
        f.write("# overlay <name> <frames> <x> <y> <width> <height> <picture it matched> <sure|guess|none> <fps>\n")
        f.write("# frames: assets/levels/<level>/<room>/<name>.<0..frames-1>.png, drawn with their top-left corner at x y of the room picture\n")
        for o in ovs:
            f.write(f"overlay {o['name']} {o['frames']} {o['x']} {o['y']} {o['w']} {o['h']} {o['bg'] or '-'} {confidence(o)} {o['fps']:.2f}\n")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--room")
    ap.add_argument("--cds")
    args = ap.parse_args()
    rates = smk_rates(args.cds)
    if args.cds:
        print(f"{len(rates)} .smk frame rates read", file=sys.stderr)
    rooms = []
    if args.room:
        rooms = [tuple(args.room.lower().split("/"))]
    else:
        for level in sorted(os.listdir(LEVELS)):
            lp = os.path.join(LEVELS, level)
            if os.path.isdir(lp):
                rooms += [(level, r) for r in sorted(os.listdir(lp)) if os.path.isdir(os.path.join(lp, r))]
    stats = Counter()
    found = {}  # (level, room) -> overlays, for the fallback below
    for level, room in rooms:
        found[(level, room)] = room_overlays(level, room, verbose=bool(args.room))
    # an overlay fitting none of its room's pictures (its picture isn't in the
    # blockouts): the same overlay's place in another room of the level, if sure there
    for (level, room), ovs in found.items():
        for o in ovs:
            if o["votes"]:
                continue
            for (l2, r2), o2s in found.items():
                if l2 != level or r2 == room:
                    continue
                twin = next((t for t in o2s if t["name"] == o["name"] and confidence(t) == "sure"), None)
                if twin:
                    o.update(x=twin["x"], y=twin["y"], bg=f"{r2}/{twin['bg']}", votes=1, of=1, err=50.0)
                    break
    for level, room in rooms:
        ovs = found[(level, room)]
        for o in ovs:
            o["fps"] = rates.get(f"{level}/{room}/{o['name']}", 15.0)
            stats["with_fps" if f"{level}/{room}/{o['name']}" in rates else "no_fps"] += 1
            stats[confidence(o)] += 1
            if confidence(o) != "sure" and not args.room:
                print(f"{level}/{room}: {o['name']} {confidence(o)} (votes {o['votes']}/{o['of']}, err {o['err']:.1f})")
        write_cfg(level, room, ovs)
    print(f"{len(rooms)} rooms, overlays: {dict(stats)}", file=sys.stderr)


if __name__ == "__main__":
    main()
