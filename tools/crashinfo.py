"""Where a Hello Neighbor crash happened: the faulting module + offset from the newest UE4 minidump, and the return
addresses on the crashing thread's stack that point into the game, UE4SS, our mod or ReShade.

    python tools/crashinfo.py [minidump]
"""
import glob
import os
import struct
import sys

from minidump.minidumpfile import MinidumpFile

INTERESTING = ("main.dll", "ue4ss.dll", "helloneighbor-win64-shipping.exe", "dxgi.dll", "neighborcraftmod.dll")


def newest_dump():
    dumps = glob.glob(os.path.expandvars(r"%LOCALAPPDATA%\HelloNeighbor\Saved\Crashes\*\UE4Minidump.dmp"))
    return max(dumps, key=os.path.getmtime)


def main(path):
    mf = MinidumpFile.parse(path)
    mods = mf.modules.modules

    def where(a):
        for m in mods:
            if m.baseaddress <= a < m.endaddress:
                return os.path.basename(m.name.replace("\\", "/")), a - m.baseaddress
        return None

    ex = mf.exception.exception_records[0]
    rec = ex.ExceptionRecord
    print(path)
    print("exception", rec.ExceptionCode, "at", hex(rec.ExceptionAddress), where(rec.ExceptionAddress))
    thread = next(t for t in mf.threads.threads if t.ThreadId == ex.ThreadId)
    ctx = thread.ContextObject
    print("rip", hex(ctx.Rip), where(ctx.Rip))
    reader = mf.get_reader()
    data = b""
    for off in range(0, 0x8000, 0x100):
        try:
            data += reader.read(ctx.Rsp + off, 0x100)
        except Exception:
            break
    shown = 0
    for i in range(0, len(data) - 7, 8):
        w = where(struct.unpack_from("<Q", data, i)[0])
        if w and w[0].lower() in INTERESTING:
            print(f"  rsp+{i:#06x} {w[0]} +{w[1]:#x}")
            shown += 1
            if shown > 40:
                break


if __name__ == "__main__":
    main(sys.argv[1] if len(sys.argv) > 1 else newest_dump())
