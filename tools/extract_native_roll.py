"""
Regenerates data/rooms/<level>/<room>_camera.cfg from the ORIGINAL game.

The game's camera header (level .PAK, section out_2, offset 0x1C) stores
each room's camera roll in degrees. The blockouts' glTF export applied that value
as radians with the opposite sign, which tilts 43 rooms (upside down for
some). The game corrects the camera at load time from this value; this
script is the only thing in the project that needs the original CDs.

Usage (from anywhere):
    python tools/extract_native_roll.py <CD1 SILVER folder> [<CD2 SILVER folder>]
e.g.
    python tools/extract_native_roll.py "D:/CD1/SILVER" "D:/CD2/Silver"
"""
import struct
import sys
from pathlib import Path

from silver_archives import extract_entry, parse_pak, rnc_unpack, room_paks

ROOT = Path(__file__).resolve().parent.parent


def main():
    if len(sys.argv) < 2:
        print(__doc__)
        return 1
    rooms = room_paks(sys.argv[1:])
    if not rooms:
        print("no fat*.nob / data*.nob found in", sys.argv[1:])
        return 1
    written = cleared = 0
    for label, (data_path, entry) in sorted(rooms.items()):
        try:
            pak = parse_pak(rnc_unpack(extract_entry(data_path, entry)))
        except Exception as ex:
            print(f"  skip {label}: {ex}")
            continue
        roll = struct.unpack_from("<f", pak["out_2"], 0x1C)[0]
        level, room = label.split("/")
        out = ROOT / "data" / "rooms" / level / f"{room}_camera.cfg"
        if abs(roll) < 1e-6:
            if out.exists():
                out.unlink()
                cleared += 1
            continue
        out.parent.mkdir(parents=True, exist_ok=True)
        out.write_text("# Camera roll stored in the original game's camera header (degrees), see README\n"
                       f"native_roll {roll:.6f}\n")
        written += 1
    print(f"{len(rooms)} rooms read, {written} camera files written, {cleared} removed (roll 0)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
