"""top_flying_wing bench keyboard control: W up, S down, SPACE stop, ESC quit.

Windows, 64-bit Python, manufacturer's ControlCAN.dll. No third-party Python
packages. Disconnect the A-BOX CAN master before using this standalone tool.
Position is the driver's RD Hall-derived position (possibly a multi-rod mean).
"""
import argparse
import ctypes as C
from dataclasses import dataclass
from datetime import datetime, timezone
import json
import math
from pathlib import Path
import sys
import time

from can_listen import BoardInfo, Config, Frame

DEFAULT_DLL = Path("E:/canalyst-II-Driver/CAN/二次开发库文件/x64(64bit)/ControlCAN.dll")


@dataclass(frozen=True)
class Packet:
    ident: int
    data: bytes
    extended: bool = True
    remote: bool = False


def command(op):
    return op.encode("ascii") + bytes(6)


class ControlCan:
    """Normal bus mode and normal TX (SendType=0), no self reception.

    The vendor requires normal TX for communication between multiple nodes.
    Its adapter retries a failed transmission for up to one second. A submit
    result never replaces the controller's application ACK/status feedback.
    """
    def __init__(self, dll_path, channel, index, log):
        self.log, self.dev, self.channel = log, (4, index), channel - 1
        self.opened = False
        self.tx_count = self.rx_count = 0
        self.dll = C.WinDLL(str(Path(dll_path).resolve()))
        signatures = {
            "OpenDevice": [C.c_uint32] * 3,
            "CloseDevice": [C.c_uint32] * 2,
            "ReadBoardInfo": [C.c_uint32, C.c_uint32, C.POINTER(BoardInfo)],
            "InitCAN": [C.c_uint32] * 3 + [C.POINTER(Config)],
            "StartCAN": [C.c_uint32] * 3,
            "ClearBuffer": [C.c_uint32] * 3,
            "Transmit": [C.c_uint32] * 3 + [C.POINTER(Frame), C.c_uint32],
            "Receive": [C.c_uint32] * 3 + [C.POINTER(Frame), C.c_uint32, C.c_int32],
        }
        for name, args in signatures.items():
            fn = getattr(self.dll, "VCI_" + name)
            fn.argtypes, fn.restype = args, C.c_uint32
        if self.dll.VCI_OpenDevice(*self.dev, 0) != 1:
            raise RuntimeError("Cannot open analyzer: close other CAN programs and check USB")
        self.opened = True
        try:
            info = BoardInfo()
            if self.dll.VCI_ReadBoardInfo(*self.dev, C.byref(info)) != 1 or info.channels <= self.channel:
                raise RuntimeError("Cannot read analyzer or selected channel unavailable")
            cfg = Config(0, 0xFFFFFFFF, 0, 1, 0, 0x1C, 0)  # 500 kbit/s, normal mode
            if self.dll.VCI_InitCAN(*self.dev, self.channel, C.byref(cfg)) != 1:
                raise RuntimeError("CAN initialization failed")
            if self.dll.VCI_ClearBuffer(*self.dev, self.channel) != 1:
                raise RuntimeError("CAN RX buffer clear failed")
            if self.dll.VCI_StartCAN(*self.dev, self.channel) != 1:
                raise RuntimeError("CAN start failed")
            self.log("analyzer", serial=info.serial.decode(errors="replace"), channel=channel,
                     bitrate=500000, mode="normal", tx_mode="normal", send_type=0)
        except BaseException:
            self.close()
            raise

    def send(self, data):
        f = Frame()
        f.ID, f.ExternFlag, f.DataLen, f.SendType = 1, 1, 8, 0
        f.Data[:] = data
        if self.dll.VCI_Transmit(*self.dev, self.channel, C.byref(f), 1) != 1:
            raise RuntimeError("CAN TX submission failed")
        self.tx_count += 1
        self.log("tx_submitted", id=1, data=data.hex(" ").upper())

    def receive(self):
        frames = (Frame * 256)()
        n = self.dll.VCI_Receive(*self.dev, self.channel, frames, len(frames), 0)
        if n == 0xFFFFFFFF or n > len(frames):
            raise RuntimeError("CAN receive failed / USB disconnected")
        self.rx_count += n
        result = []
        for f in frames[:n]:
            if f.DataLen > 8:
                raise RuntimeError("Invalid classic CAN DLC")
            p = Packet(f.ID, bytes(f.Data[:f.DataLen]), bool(f.ExternFlag), bool(f.RemoteFlag))
            self.log("rx", id=p.ident, data=p.data.hex(" ").upper(), extended=p.extended, remote=p.remote)
            result.append(p)
        return result

    def statistics(self):
        return {"channel": self.channel + 1, "tx_submissions": self.tx_count,
                "rx_frames": self.rx_count}

    def close(self):
        if self.opened:
            self.dll.VCI_CloseDevice(*self.dev)
            self.opened = False


class Controller:
    """Nonblocking state machine. All times use time.monotonic(), in seconds."""
    def __init__(self, bus, log, rx_id=1, margin=2, max_run=120):
        if rx_id not in (1, 2) or margin < 0 or max_run <= 0 or not math.isfinite(max_run):
            raise ValueError("Invalid reply ID, margin or runtime")
        self.bus, self.log, self.rx_id = bus, log, rx_id
        self.margin, self.max_run = margin, max_run
        self.phase = "init"
        self.pending = None
        self.next_tx = 0.0
        self.last_status = -math.inf
        self.pos = self.bottom = self.top = None
        self.fault = 0
        self.moving = False
        self.direction = None
        self.intent = None
        self.after_stop = None
        self.stop_fault = None
        self.stop_deadline = math.inf
        self.started = math.inf
        self.best = None
        self.target = None
        self.guard_until = math.inf
        self.quit_requested = False
        self.stop_confirmed = False

    def start(self, now, probe=False):
        # Match the verified firmware: no CAN traffic at startup/while idle.
        self.phase = "idle"
        self.log("idle", message="空闲静默；W/S 开始 RD→250ms→RD→RL→UP/DN 预检。", rx_id=self.rx_id)
        if probe:
            self._query("RD", "check1", now)

    def _query(self, op, purpose, now):
        self.pending = {"op": op, "purpose": purpose, "sent": None}
        self.phase = purpose

    def _transmit(self, op, now):
        self.bus.send(command(op))
        self.next_tx = now + 0.05

    def _failed(self, why):
        self.phase = "fault"
        self.pending = None
        self.intent = self.after_stop = None
        self.direction = None
        self.log("fault", message=why)
        if hasattr(self.bus, "statistics"):
            stats = self.bus.statistics()
            self.log("can_statistics", **stats)
            if stats["rx_frames"] == 0:
                self.log("no_rx", message="未收到任何 CAN 帧；请检查直连接线、参考地、终端和驱动器实际通信参数。未取得限位，升降保持禁止。")

    def stop(self, now, reason="operator", then=None, fault=None):
        self.intent = None
        self.after_stop = then
        self.stop_fault = fault
        if self.phase in ("stop_ack", "stop_status"):
            return  # Space/ESC cancels pending reversal; never extends stop deadline.
        self.pending = None
        self.direction = None
        self.phase = "stop_ack"
        self.stop_deadline = now + 1.0
        self.stop_confirmed = False
        self.log("stopping", message=reason)
        try:
            self._transmit("ST", now)  # emergency stop bypasses ordinary command spacing
            self.pending = {"op": "ST", "purpose": "stop_ack", "sent": now}
        except Exception as e:
            self._failed("STOP could not be submitted: " + str(e))

    def key(self, key, now):
        if key in (" ", "ESC"):
            if key == "ESC":
                self.quit_requested = True
            self.stop(now)
            return
        if key not in ("w", "s") or self.quit_requested:
            return
        direction = "UP" if key == "w" else "DN"
        if self.phase == "fault":
            self.log("blocked", message="Fault latched: SPACE to stop and recheck feedback/limits")
            return
        if self.phase == "idle":
            self.intent = direction
            self._query("RD", "check1", now)
        elif self.direction and direction != self.direction:
            self.stop(now, "direction change", then=direction)
        elif self.intent and direction != self.intent:
            self.stop(now, "direction change during preflight", then=direction)
        # Busy preflight/stop ignores other motion keys. Fresh key press is required.

    def _trip(self, why, now):
        if self.phase not in ("stop_ack", "stop_status", "fault"):
            self.stop(now, why, fault=why)

    def _status(self, p):
        d = p.data
        if d[4] > 16 or d[5] > 1 or d[7] != 0:
            raise ValueError("RD invalid/rejected")
        self.pos, self.fault, self.moving = int.from_bytes(d[2:4], "big"), d[4], bool(d[5])

    def _limits(self, p):
        top, bottom = int.from_bytes(p.data[2:4], "big"), int.from_bytes(p.data[4:6], "big")
        if top <= bottom or top - bottom <= self.margin * 2:
            raise ValueError("RL invalid or travel smaller than limit margins")
        self.top, self.bottom = top, bottom

    def _begin_motion(self, now):
        if self.pos is None or now - self.last_status > 0.5 or self.fault or self.moving:
            self._trip("Preflight feedback stale/fault/moving", now)
            return
        if not self.bottom <= self.pos <= self.top:
            self._trip("Hall position outside configured travel", now)
            return
        if self.intent is None:
            self.phase = "idle"
            self.log("probe_ready", message="RD→250ms→RD→RL 已完成；未发送运动命令。")
            return
        if (self.intent == "UP" and self.pos >= self.top - self.margin or
                self.intent == "DN" and self.pos <= self.bottom + self.margin):
            self.intent = None
            self.phase = "idle"
            self.log("limit", message="Already at requested limit; motion blocked")
            return
        self.direction, self.intent = self.intent, None
        if self.direction not in ("UP", "DN"):
            self._failed("Missing motion intention")
            return
        self.started, self.best = now, self.pos
        self.stop_confirmed = False
        self.target = self.top - self.margin if self.direction == "UP" else self.bottom + self.margin
        self.log("motion_start", direction=self.direction, stop_boundary_mm=self.target)
        self.pending = {"op": self.direction, "purpose": "start_ack", "sent": None}
        self.phase = "start_ack"

    def receive(self, packets, now):
        for p in packets:
            if not p.extended or p.remote or len(p.data) != 8 or p.ident not in (1, 2):
                continue
            if p.ident != self.rx_id:
                continue
            pending = self.pending
            if not pending or pending["sent"] is None or p.data[:2] != pending["op"].encode():
                continue
            purpose, op = pending["purpose"], pending["op"]
            self.pending = None
            try:
                if op == "RL":
                    self._limits(p)
                    self.log("limits", top=self.top, bottom=self.bottom)
                    if purpose == "preflight_limits":
                        self._begin_motion(now)
                elif op == "RD":
                    self._status(p)
                    self.last_status = now
                    self.log("position", mm=self.pos, moving=self.moving, fault=self.fault, phase=purpose)
                    if purpose == "stop_status":
                        if self.fault:
                            self._failed("STOP feedback fault=" + str(self.fault))
                        elif not self.moving:
                            self.stop_confirmed = True
                            self.log("stopped_confirmed", mm=self.pos)
                            if self.stop_fault:
                                self._failed(self.stop_fault)
                            else:
                                self.phase = "idle"
                                if self.after_stop and not self.quit_requested:
                                    self.intent, self.after_stop = self.after_stop, None
                                    self._query("RD", "check1", now)
                    elif self.fault:
                        self._trip("Driver fault=" + str(self.fault), now)
                    elif purpose in ("check1", "check2"):
                        if self.moving:
                            self._trip("Driver already moving / another controller", now)
                        elif purpose == "check1":
                            self.phase, self.guard_until = "guard", now + 0.25
                        else:
                            self._query("RL", "preflight_limits", now)
                    elif purpose in ("run_status", "starting"):
                        if self._at_limit():
                            self.stop(now, "Hall position reached limit")
                        elif (self.direction == "UP" and self.pos + 3 < self.best or
                              self.direction == "DN" and self.pos > self.best + 3):
                            self._trip("Hall feedback moving in opposite direction", now)
                        elif not self.moving:
                            self.stop(now, "Driver stopped")
                        else:
                            self.best = max(self.best, self.pos) if self.direction == "UP" else min(self.best, self.pos)
                            self.phase = "moving" if self.moving else "starting"
                else:  # ST / UP / DN acceptance ACK, never physical completion
                    if p.data[7] not in (0, 1) or p.data[7] != 0:
                        raise ValueError(op + " command rejected/invalid ACK")
                    if purpose == "stop_ack":
                        self._query("RD", "stop_status", now)
                    else:
                        self._query("RD", "starting", now)
            except ValueError as e:
                if purpose in ("stop_ack", "stop_status"):
                    self._failed(str(e))
                else:
                    self._trip(str(e), now)

    def _at_limit(self):
        return (self.pos < self.bottom or self.pos > self.top or
                self.direction == "UP" and self.pos >= self.top - self.margin or
                self.direction == "DN" and self.pos <= self.bottom + self.margin)

    def tick(self, now):
        if self.phase == "fault":
            return
        if self.phase in ("stop_ack", "stop_status") and now >= self.stop_deadline:
            self._failed("STOP not confirmed: physical stop is NOT guaranteed")
            return
        if self.direction and self.phase not in ("stop_ack", "stop_status"):
            if now - self.last_status > 0.5 or now - self.started >= self.max_run:
                self._trip("Feedback timeout / maximum run time", now)
                return
        if self.pending:
            p = self.pending
            if p["sent"] is None and now >= self.next_tx:
                try:
                    self._transmit(p["op"], now)
                    p["sent"] = now
                except Exception as e:
                    self._trip(str(e), now)
            elif p["sent"] is not None and now - p["sent"] >= (
                    1.0 if p["op"] in ("UP", "DN", "ST") else 0.3):
                if self.phase in ("stop_ack", "stop_status"):
                    self._failed("STOP response timeout")
                else:
                    self._trip(p["op"] + " response timeout", now)
            return
        if self.phase == "guard" and now >= self.guard_until:
            self._query("RD", "check2", now)
        elif self.phase == "stop_status" and now - self.last_status >= 0.1:
            self._query("RD", "stop_status", now)
        elif self.phase in ("moving", "starting"):
            if now - self.last_status >= 0.1:
                self._query("RD", "starting" if self.phase == "starting" else "run_status", now)


def key_edges(events, held):
    keys = []
    for kind, key, down in events:
        if kind == "focus" and not down:
            keys.append(" ")
        elif kind == "key" and key:
            if not down:
                held.discard(key)
            elif key not in held:
                held.add(key)
                keys.append(key)
    return ["ESC"] if "ESC" in keys else [" "] if " " in keys else keys[:1]


class Console:
    """Read key edges, suppress Windows autorepeat; disable QuickEdit freezing."""
    def __init__(self):
        self.kernel = C.WinDLL("kernel32", use_last_error=True)
        self.kernel.GetStdHandle.argtypes = [C.c_uint32]
        self.kernel.GetStdHandle.restype = C.c_void_p
        self.handle = self.kernel.GetStdHandle(-10 & 0xFFFFFFFF)
        self.kernel.GetConsoleMode.argtypes = [C.c_void_p, C.POINTER(C.c_uint32)]
        self.kernel.SetConsoleMode.argtypes = [C.c_void_p, C.c_uint32]
        self.original = C.c_uint32()
        if not self.kernel.GetConsoleMode(self.handle, C.byref(self.original)):
            raise RuntimeError("Run from an interactive Windows console, not redirected stdin")
        # Processed input keeps Ctrl+C; window input enables focus events.
        if not self.kernel.SetConsoleMode(self.handle, (self.original.value | 0x88) & ~0x246):
            raise RuntimeError("Cannot configure console input")
        class Key(C.Structure):
            _fields_ = [("down", C.c_int32), ("repeat", C.c_uint16), ("vk", C.c_uint16),
                        ("scan", C.c_uint16), ("char", C.c_uint16), ("modifiers", C.c_uint32)]
        class Event(C.Union):
            _fields_ = [("key", Key), ("focus", C.c_int32), ("storage", C.c_byte * 16)]
        class Record(C.Structure):
            _fields_ = [("kind", C.c_uint16), ("event", Event)]
        self.Record = Record
        self.kernel.GetNumberOfConsoleInputEvents.argtypes = [C.c_void_p, C.POINTER(C.c_uint32)]
        self.kernel.ReadConsoleInputW.argtypes = [C.c_void_p, C.POINTER(Record), C.c_uint32, C.POINTER(C.c_uint32)]
        self.held = set()

    def read(self):
        n = C.c_uint32()
        if not self.kernel.GetNumberOfConsoleInputEvents(self.handle, C.byref(n)):
            raise RuntimeError("Console input disconnected")
        if not n.value:
            return []
        buf = (self.Record * min(n.value, 256))()
        count = C.c_uint32()
        if not self.kernel.ReadConsoleInputW(self.handle, buf, len(buf), C.byref(count)):
            raise RuntimeError("Console input read failed")
        events = []
        for r in buf[:count.value]:
            if r.kind == 0x10:
                events.append(("focus", None, bool(r.event.focus)))
            elif r.kind == 1:
                k = {0x57: "w", 0x53: "s", 0x20: " ", 0x1B: "ESC"}.get(r.event.key.vk)
                events.append(("key", k, bool(r.event.key.down)))
        # STOP/quit wins the whole batch; queued W/S must not restart motion.
        return key_edges(events, self.held)

    def close(self):
        self.kernel.SetConsoleMode(self.handle, self.original.value)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--dll", type=Path, default=DEFAULT_DLL)
    parser.add_argument("--channel", type=int, choices=(1, 2), default=1)
    parser.add_argument("--index", type=int, default=0)
    parser.add_argument("--rx-id", choices=("1", "2"), default="1")
    parser.add_argument("--margin-mm", type=int, default=2)
    parser.add_argument("--max-run-seconds", type=float, default=120)
    parser.add_argument("--probe-only", action="store_true", help="只查询和停止，不读取键盘、不发送运动命令")
    parser.add_argument("--probe-seconds", type=float, default=5)
    args = parser.parse_args()
    if sys.platform != "win32" or C.sizeof(C.c_void_p) != 8:
        parser.error("64-bit Windows Python required")
    if args.index < 0 or args.margin_mm < 0 or not math.isfinite(args.max_run_seconds) or args.max_run_seconds <= 0:
        parser.error("Invalid index, margin or maximum runtime")
    if not math.isfinite(args.probe_seconds) or not 2 <= args.probe_seconds <= 60:
        parser.error("--probe-seconds must be between 2 and 60")
    root = Path(__file__).resolve().parents[1]
    out = root / "build/app" / ("can_keyboard_" + datetime.now().strftime("%Y%m%d_%H%M%S_%f") + ".jsonl")
    out.parent.mkdir(parents=True, exist_ok=True)
    bus = console = ctl = None
    with out.open("x", encoding="utf-8") as f:
        def log(event, **fields):
            f.write(json.dumps({"utc": datetime.now(timezone.utc).isoformat(), "event": event, **fields}, ensure_ascii=False) + "\n")
            f.flush()
            if event == "position":
                print("\rPosition: {mm} mm | moving={moving} | fault={fault}          ".format(**fields), end="", flush=True)
            else:
                print("\n" + event + ": " + json.dumps(fields, ensure_ascii=False), flush=True)
        try:
            if not args.probe_only:
                console = Console()
            bus = ControlCan(args.dll, args.channel, args.index, log)
            ctl = Controller(bus, log, int(args.rx_id),
                             args.margin_mm, args.max_run_seconds)
            if args.probe_only:
                print("通信检查模式：仅限位/状态查询和停止；不会发送升降命令。")
            else:
                print("W=UP  S=DOWN  SPACE=STOP  ESC=STOP+QUIT; press once to run until stop/limit.")
            print("Standalone bench tool: disconnect A-BOX CAN master; A-BOX anti-pinch is not integrated.")
            print("Verified-project sequence: RD -> wait 250ms -> RD -> RL -> UP/DN; TX/RX ID=1 by default.")
            print("Log: " + str(out))
            began = time.monotonic()
            ctl.start(began, probe=args.probe_only)
            while True:
                now = time.monotonic()
                for key in console.read() if console else []:
                    ctl.key(key, now)
                if args.probe_only and now - began >= args.probe_seconds and not ctl.quit_requested:
                    ctl.key("ESC", now)
                ctl.receive(bus.receive(), now)
                ctl.tick(now)
                if ctl.quit_requested and ctl.phase in ("idle", "fault"):
                    break
                time.sleep(0.005)
            if not ctl.stop_confirmed:
                print("STOP NOT CONFIRMED. Use the driver's physical stop.", flush=True)
            log("session_summary", **bus.statistics(), probe_only=args.probe_only,
                stop_confirmed=ctl.stop_confirmed)
        except (Exception, KeyboardInterrupt) as e:
            log("exception", message=str(e) or "Ctrl+C")
            if ctl and bus:
                ctl.stop(time.monotonic(), "exit / exception")
                until = time.monotonic() + 1.1
                while time.monotonic() < until and ctl.phase not in ("idle", "fault"):
                    try:
                        now = time.monotonic()
                        ctl.receive(bus.receive(), now)
                        ctl.tick(now)
                        time.sleep(0.005)
                    except (Exception, KeyboardInterrupt):
                        break
                if ctl.phase != "idle":
                    print("STOP NOT CONFIRMED. Use the driver's physical stop.", flush=True)
            return 1
        finally:
            if bus:
                bus.close()
            if console:
                console.close()
    return 1 if ctl and ctl.phase == "fault" else 0


if __name__ == "__main__":
    raise SystemExit(main())
