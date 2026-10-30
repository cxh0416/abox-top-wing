"""Receive-only CANalyst-II capture through the manufacturer's ControlCAN DLL.

CH1 is CAN index 0. The controller always starts in listen-only mode; this
script has no transmit operation and does not acknowledge bus traffic.
"""
import argparse
import ctypes as C
from datetime import datetime, timezone
import json
from pathlib import Path
import time


class BoardInfo(C.Structure):
    _fields_ = [(n, C.c_ushort) for n in ("hw", "fw", "driver", "interface", "irq")] + [
        ("channels", C.c_ubyte), ("serial", C.c_char * 20),
        ("model", C.c_char * 40), ("reserved", C.c_ushort * 4)]


class Config(C.Structure):
    _fields_ = [("AccCode", C.c_uint32), ("AccMask", C.c_uint32),
                ("Reserved", C.c_uint32), ("Filter", C.c_ubyte),
                ("Timing0", C.c_ubyte), ("Timing1", C.c_ubyte), ("Mode", C.c_ubyte)]


class Frame(C.Structure):
    _fields_ = [("ID", C.c_uint32), ("TimeStamp", C.c_uint32),
                ("TimeFlag", C.c_ubyte), ("SendType", C.c_ubyte),
                ("RemoteFlag", C.c_ubyte), ("ExternFlag", C.c_ubyte),
                ("DataLen", C.c_ubyte), ("Data", C.c_ubyte * 8),
                ("Reserved", C.c_ubyte * 3)]


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("--dll", required=True)
    p.add_argument("--channel", type=int, choices=[1, 2], default=1)
    p.add_argument("--bitrate", type=int, choices=[250000, 500000], default=500000)
    p.add_argument("--seconds", type=float, default=120)
    p.add_argument("--output", type=Path, required=True)
    args = p.parse_args()
    if args.seconds <= 0:
        p.error("--seconds must be positive")
    args.output.parent.mkdir(parents=True, exist_ok=True)
    dll = C.WinDLL(str(Path(args.dll).resolve()))
    for name, params in {
        "VCI_OpenDevice": [C.c_uint32] * 3,
        "VCI_CloseDevice": [C.c_uint32] * 2,
        "VCI_ReadBoardInfo": [C.c_uint32, C.c_uint32, C.POINTER(BoardInfo)],
        "VCI_InitCAN": [C.c_uint32] * 3 + [C.POINTER(Config)],
        "VCI_StartCAN": [C.c_uint32] * 3,
        "VCI_Receive": [C.c_uint32] * 3 + [C.POINTER(Frame), C.c_uint32, C.c_int32],
    }.items():
        fn = getattr(dll, name)
        fn.argtypes, fn.restype = params, C.c_uint32
    dev = (4, 0)  # manufacturer's USBCAN2 / CANalyst-II type
    channel = args.channel - 1
    if dll.VCI_OpenDevice(*dev, 0) != 1:
        raise RuntimeError("Cannot open CANalyst-II; check USB or another application's ownership")
    count, counts = 0, {}
    try:
        info = BoardInfo()
        if dll.VCI_ReadBoardInfo(*dev, C.byref(info)) != 1 or info.channels <= channel:
            raise RuntimeError("Cannot read board information or selected channel unavailable")
        # SJA1000 timing, accept all frames, listen-only at either vendor rate.
        cfg = Config(0, 0xFFFFFFFF, 0, 1, 0x00 if args.bitrate == 500000 else 0x01, 0x1C, 1)
        if dll.VCI_InitCAN(*dev, channel, C.byref(cfg)) != 1:
            raise RuntimeError("Listen-only CAN initialization failed")
        if dll.VCI_StartCAN(*dev, channel) != 1:
            raise RuntimeError("CAN start failed")
        frames = (Frame * 512)()
        started = time.monotonic()
        meta = {"event": "started", "utc": datetime.now(timezone.utc).isoformat(),
                "channel": args.channel, "bitrate": args.bitrate, "mode": "listen_only",
                "serial": info.serial.decode(errors="replace"), "output": str(args.output)}
        print(json.dumps(meta), flush=True)
        with args.output.open("x", encoding="utf-8") as out:
            out.write(json.dumps(meta) + "\n")
            out.flush()
            while time.monotonic() - started < args.seconds:
                n = dll.VCI_Receive(*dev, channel, frames, len(frames), 20)
                if n == 0xFFFFFFFF or n > len(frames):
                    raise RuntimeError(f"CAN receive failed: {n:#x}")
                for f in frames[:n]:
                    if f.DataLen > 8:
                        raise RuntimeError("Invalid classic CAN DLC from DLL")
                    data = bytes(f.Data[:f.DataLen])
                    record = {"event": "frame", "utc": datetime.now(timezone.utc).isoformat(),
                              "seconds": round(time.monotonic() - started, 6),
                              "id": f"0x{f.ID:08X}", "extended": bool(f.ExternFlag),
                              "remote": bool(f.RemoteFlag), "dlc": f.DataLen,
                              "data": data.hex(" ").upper(), "adapter_timestamp": f.TimeStamp,
                              "adapter_timestamp_valid": bool(f.TimeFlag)}
                    count += 1
                    command = data[:2].decode("ascii", errors="replace")
                    key = f"{record['id']}:{command}"
                    counts[key] = counts.get(key, 0) + 1
                    out.write(json.dumps(record) + "\n")
                    print(json.dumps(record), flush=True)
                if n:
                    out.flush()
                else:
                    time.sleep(0.02)
            summary = {"event": "completed", "frames": count, "counts": counts,
                       "duration_seconds": round(time.monotonic() - started, 3)}
            out.write(json.dumps(summary) + "\n")
            print(json.dumps(summary), flush=True)
    finally:
        dll.VCI_CloseDevice(*dev)


if __name__ == "__main__":
    main()
