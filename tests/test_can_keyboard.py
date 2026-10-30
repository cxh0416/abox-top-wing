"""No real DLL, CAN adapter, motor commands or console window are used here."""
import ctypes as C
import io
from pathlib import Path
import sys
import unittest
from unittest.mock import patch

sys.dont_write_bytecode = True
sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "tools"))
from can_keyboard import Controller, ControlCan, Packet, command, key_edges
import can_keyboard


class Bus:
    def __init__(self):
        self.sent = []
        self.fail = False

    def send(self, data):
        if self.fail:
            raise RuntimeError("simulated USB loss")
        self.sent.append(data)


class Harness:
    def __init__(self, ident=1, **kwargs):
        self.t, self.ident = 0.0, ident
        self.bus, self.events = Bus(), []
        self.c = Controller(self.bus, lambda event, **kw: self.events.append((event, kw)), **kwargs)
        self.c.start(self.t)

    def step(self, dt):
        self.t += dt
        self.c.tick(self.t)

    def answer(self, data, ident=None, **kwargs):
        if self.c.pending and self.c.pending["sent"] is None:
            self.step(.061)
        self.t += .001
        self.c.receive([Packet(self.ident if ident is None else ident, data, **kwargs)], self.t)

    def limits(self, top=100, bottom=10, **kw):
        self.answer(b"RL" + top.to_bytes(2, "big") + bottom.to_bytes(2, "big") + bytes(2), **kw)

    def status(self, pos=50, moving=False, fault=0, **kw):
        self.answer(b"RD" + pos.to_bytes(2, "big") + bytes([fault, moving, 0, 0]), **kw)

    def ack(self, op="ST", rejected=0):
        payload = command(op)
        self.answer(payload[:7] + bytes([rejected]))

    def ready(self, pos=50, top=100, bottom=10):
        self.c.start(self.t, probe=True)
        self.status(pos)
        self.step(.251)
        self.status(pos)
        self.limits(top, bottom)
        assert self.c.phase == "idle"

    def preflight(self, key="w", pos=50, top=100, bottom=10):
        self.c.key(key, self.t)
        self.status(pos)
        self.step(.251)
        self.status(pos)
        self.limits(top, bottom)

    def run(self, key="w"):
        self.ready()
        self.preflight(key)
        self.ack("UP" if key == "w" else "DN")
        self.status(52 if key == "w" else 48, True)
        assert self.c.phase == "moving"


class Tests(unittest.TestCase):
    def test_exact_protocol_vectors(self):
        self.assertEqual(command("ST").hex(), "5354000000000000")
        self.assertEqual(command("RD").hex(), "5244000000000000")
        self.assertEqual(command("RL").hex(), "524c000000000000")
        self.assertEqual(command("UP").hex(), "5550000000000000")
        self.assertEqual(command("DN").hex(), "444e000000000000")

    def test_startup_is_silent_and_preflight_obeys_quiet_guard(self):
        h = Harness()
        h.step(300)
        self.assertEqual(h.bus.sent, [])
        h.c.key("w", h.t)
        h.status()
        self.assertEqual(h.bus.sent, [command("RD")])
        h.step(.249)
        self.assertEqual(h.c.phase, "guard")
        h.step(.002)
        h.status()
        h.limits()
        h.step(.061)
        self.assertEqual(h.bus.sent, [command("RD"), command("RD"), command("RL"), command("UP")])

    def test_default_id_one_and_explicit_id_two(self):
        for ident in (1, 2):
            with self.subTest(ident=ident):
                h = Harness(ident, rx_id=ident)
                h.ready()
                self.assertEqual(h.c.rx_id, ident)

    def test_wrong_reply_id_does_not_change_default_or_advance(self):
        h = Harness()
        h.c.key("w", h.t)
        h.status(ident=2)
        self.assertEqual(h.c.phase, "check1")
        self.assertEqual(h.c.rx_id, 1)

    def test_explicit_id_and_frame_metadata_are_enforced(self):
        h = Harness(rx_id=2)
        h.c.key("w", h.t)
        for extra in ({"ident": 1}, {"extended": False}, {"remote": True}):
            h.status(**extra)
            self.assertEqual(h.c.phase, "check1")
        h.answer(b"RD" + bytes(5))
        self.assertEqual(h.c.phase, "check1")

    def test_invalid_limits_never_enable_motion(self):
        h = Harness()
        h.preflight(top=10, bottom=10)
        self.assertEqual(h.c.phase, "stop_ack")
        self.assertFalse(any(x[:2] in (b"UP", b"DN", b"MO") for x in h.bus.sent))

    def test_up_dn_commands_and_software_stop_boundaries(self):
        for key, target in (("w", 98), ("s", 12)):
            h = Harness()
            h.ready()
            h.preflight(key)
            h.step(.061)
            self.assertEqual(h.bus.sent[-1], command("UP" if key == "w" else "DN"))
            self.assertEqual(h.c.target, target)

    def test_complete_wire_sequence_matches_verified_project_both_directions(self):
        for key, motion in (("w", "55 50 00 00 00 00 00 00"),
                            ("s", "44 4E 00 00 00 00 00 00")):
            with self.subTest(key=key):
                h = Harness()
                h.preflight(key)
                h.ack("UP" if key == "w" else "DN")
                h.status(50, True)
                # ACK + first RD must confirm motion, not just acceptance.
                self.assertEqual(h.c.phase, "moving")
                h.step(.099)
                self.assertIsNone(h.c.pending)
                h.step(.002)
                h.status(50, True)
                h.c.key(" ", h.t)
                h.ack()
                h.status(50, False)
                self.assertTrue(h.c.stop_confirmed)
                self.assertEqual(h.c.phase, "idle")
                expected = [
                    "52 44 00 00 00 00 00 00",
                    "52 44 00 00 00 00 00 00",
                    "52 4C 00 00 00 00 00 00",
                    motion,
                    "52 44 00 00 00 00 00 00",
                    "52 44 00 00 00 00 00 00",
                    "53 54 00 00 00 00 00 00",
                    "52 44 00 00 00 00 00 00",
                ]
                self.assertEqual(h.bus.sent, [bytes.fromhex(x) for x in expected])
                before = list(h.bus.sent)
                h.step(5)
                self.assertEqual(h.bus.sent, before)

    def test_probe_preflight_has_same_queries_without_motion(self):
        h = Harness()
        h.ready()
        self.assertEqual(h.bus.sent, [command("RD"), command("RD"), command("RL")])
        self.assertEqual(h.c.phase, "idle")

    def test_start_ack_does_not_replace_motion_feedback(self):
        h = Harness()
        h.preflight()
        h.ack("UP")
        h.status(50, False)
        self.assertEqual(h.c.phase, "stop_ack")
        self.assertEqual(h.bus.sent[-1], command("ST"))

    def test_no_motion_at_requested_endpoint(self):
        for key, pos in (("w", 98), ("s", 12)):
            h = Harness()
            h.ready(pos)
            h.preflight(key, pos)
            self.assertEqual(h.c.phase, "idle")
            self.assertFalse(any(x[:2] in (b"UP", b"DN", b"MO") for x in h.bus.sent))

    def test_stale_preflight_position_blocks_motion(self):
        h = Harness()
        h.ready()
        h.c.intent = "UP"
        h.t += 1
        h.c._begin_motion(h.t)
        self.assertEqual(h.c.phase, "stop_ack")

    def test_position_outside_travel_blocks_motion(self):
        h = Harness()
        h.ready()
        h.preflight(pos=105)
        self.assertEqual(h.c.phase, "stop_ack")

    def test_foreign_motion_during_preflight_stops(self):
        h = Harness()
        h.ready()
        h.c.key("w", h.t)
        h.status(moving=True)
        self.assertEqual(h.c.phase, "stop_ack")

    def test_direction_change_waits_for_stop_then_new_preflight(self):
        h = Harness()
        h.run()
        h.step(.11)  # poll pending while S is pressed
        h.c.key("s", h.t)
        self.assertEqual(h.bus.sent[-1], command("ST"))
        moves = sum(x[:2] in (b"UP", b"DN") for x in h.bus.sent)
        h.status(moving=False)  # ST ACK is required first
        self.assertEqual(sum(x[:2] in (b"UP", b"DN") for x in h.bus.sent), moves)
        h.ack()
        h.status(pos=55)
        self.assertEqual(h.c.phase, "check1")
        h.status(pos=55)
        h.step(.251)
        h.status(pos=55)
        h.limits()
        h.step(.061)
        self.assertEqual(h.bus.sent[-1], command("DN"))

    def test_space_cancels_pending_reversal(self):
        h = Harness()
        h.run()
        h.c.key("s", h.t)
        h.c.key(" ", h.t)
        h.ack()
        h.status()
        h.limits()
        self.assertEqual(h.c.phase, "idle")
        self.assertIsNone(h.c.after_stop)

    def test_direction_change_during_preflight_cancels_original_motion(self):
        h = Harness()
        h.ready()
        h.c.key("w", h.t)
        h.c.key("s", h.t)
        self.assertEqual(h.c.phase, "stop_ack")
        self.assertEqual(h.bus.sent[-1], command("ST"))
        self.assertEqual(h.c.after_stop, "DN")
        self.assertFalse(any(x[:2] in (b"UP", b"DN", b"MO") for x in h.bus.sent))

    def test_running_only_polls_rd_and_ignores_unsolicited_limits(self):
        h = Harness()
        h.run()
        baseline = len(h.bus.sent)
        for _ in range(12):
            h.step(.11)
            h.status(55, True)
        self.assertTrue(all(x == command("RD") for x in h.bus.sent[baseline:]))
        h.answer(b"RL" + bytes.fromhex("00 C8 00 00 00 00"))
        self.assertEqual((h.c.top, h.c.bottom), (100, 10))

    def test_feedback_loss_stops_and_latches_fault(self):
        h = Harness()
        h.run()
        h.step(.501)
        self.assertEqual(h.bus.sent[-1], command("ST"))
        h.ack()
        h.status()
        self.assertEqual(h.c.phase, "fault")
        h.c.key("w", h.t)
        self.assertEqual(h.c.phase, "fault")
        h.c.key(" ", h.t)
        h.ack()
        h.status()
        h.limits()
        self.assertEqual(h.c.phase, "idle")

    def test_endpoints_stop_both_directions(self):
        for key, pos in (("w", 98), ("s", 12)):
            h = Harness()
            h.run(key)
            h.step(.11)
            h.status(pos, True)
            self.assertEqual(h.c.phase, "stop_ack")

    def test_opposite_hall_travel_stops(self):
        h = Harness()
        h.run()
        h.step(.11)
        h.status(45, True)
        self.assertEqual(h.c.phase, "stop_ack")

    def test_runtime_cap_stops(self):
        h = Harness(max_run=.3)
        h.run()
        h.step(.31)
        self.assertEqual(h.c.phase, "stop_ack")

    def test_fault_feedback_stops(self):
        h = Harness()
        h.run()
        h.step(.11)
        h.status(53, True, 11)
        self.assertEqual(h.c.phase, "stop_ack")

    def test_missing_start_ack_stops_even_if_driver_may_have_moved(self):
        h = Harness()
        h.ready()
        h.preflight()
        h.step(.061)
        h.step(.501)
        self.assertEqual(h.bus.sent[-1], command("ST"))

    def test_wrong_direction_ack_and_rejection(self):
        h = Harness()
        h.ready()
        h.preflight()
        h.ack("DN")
        self.assertEqual(h.c.phase, "start_ack")
        h.ack("UP", 1)
        self.assertEqual(h.c.phase, "stop_ack")

    def test_stop_timeout_and_rejected_stop_do_not_claim_success(self):
        for reject in (False, True):
            h = Harness()
            h.ready()
            h.c.key(" ", h.t)
            if reject:
                h.ack(rejected=1)
            else:
                h.step(1.01)
            self.assertEqual(h.c.phase, "fault")
            self.assertFalse(h.c.stop_confirmed)

    def test_space_does_not_extend_stop_deadline(self):
        h = Harness()
        h.ready()
        h.c.key(" ", h.t)
        deadline = h.c.stop_deadline
        h.step(.8)
        h.c.key(" ", h.t)
        self.assertEqual(deadline, h.c.stop_deadline)
        h.step(.21)
        self.assertEqual(h.c.phase, "fault")

    def test_transmit_failure_cannot_enable_motion(self):
        h = Harness()
        h.c.key("w", h.t)
        h.bus.fail = True
        h.step(.1)
        self.assertEqual(h.c.phase, "fault")

    def test_missing_first_rd_stops_without_rl_or_motion(self):
        h = Harness()
        h.c.key("w", h.t)
        h.step(.01)
        h.step(.31)
        self.assertEqual(h.bus.sent, [command("RD"), command("ST")])
        h.step(1.01)
        self.assertEqual(h.c.phase, "fault")

    def test_idle_after_probe_remains_silent(self):
        h = Harness()
        h.ready()
        before = list(h.bus.sent)
        h.step(300)
        self.assertEqual(h.bus.sent, before)

    def test_fault_recovery_requires_st_ack_then_rd_stopped(self):
        h = Harness()
        h.c._failed("initial communication unavailable")
        h.c.key(" ", h.t)
        self.assertEqual(h.bus.sent[-1], command("ST"))
        self.assertEqual(h.c.phase, "stop_ack")
        h.status()  # no ST ACK yet
        self.assertEqual(h.c.phase, "stop_ack")
        h.ack()
        h.status()
        self.assertEqual(h.c.phase, "idle")
        self.assertFalse(any(x[:2] in (b"UP", b"DN", b"MO") for x in h.bus.sent))

    def test_escape_does_not_rediscover_or_enable_motion(self):
        h = Harness()
        h.c._failed("initial communication unavailable")
        h.c.key("ESC", h.t)
        self.assertTrue(h.c.quit_requested)
        self.assertEqual(h.c.phase, "stop_ack")

    def test_probe_main_never_reads_keyboard_or_sends_movement(self):
        class ProbeBus(Bus):
            def receive(self): return []
            def statistics(self): return {"channel": 1, "tx_submissions": len(self.sent), "rx_frames": 0}
            def close(self): self.closed = True
        bus = ProbeBus()
        clock = [0.0]
        def advance(_): clock[0] += .01
        with patch.object(can_keyboard, "ControlCan", return_value=bus), \
             patch.object(can_keyboard, "Console") as console, \
             patch.object(sys, "argv", ["can_keyboard.py", "--probe-only", "--probe-seconds", "5"]), \
             patch.object(sys, "platform", "win32"), \
             patch.object(can_keyboard.time, "monotonic", side_effect=lambda: clock[0]), \
             patch.object(can_keyboard.time, "sleep", side_effect=advance), \
             patch.object(Path, "open", return_value=io.StringIO()), \
             patch("builtins.print"):
            self.assertEqual(can_keyboard.main(), 1)  # no feedback cannot report success
        console.assert_not_called()
        self.assertTrue(bus.closed)
        self.assertEqual(bus.sent.count(command("RD")), 1)
        self.assertEqual(bus.sent.count(command("RL")), 0)
        self.assertTrue(all(x[:2] in (b"RL", b"ST", b"RD") for x in bus.sent))

    def test_keyboard_edges_and_stop_batch_priority(self):
        held = set()
        self.assertEqual(key_edges([("key", "w", True)] * 5, held), ["w"])
        self.assertEqual(key_edges([("key", "w", True)], held), [])
        self.assertEqual(key_edges([("key", "w", False), ("key", "w", True)], held), ["w"])
        self.assertEqual(key_edges([("key", "s", True), ("key", " ", True)], held), [" "])
        self.assertEqual(key_edges([("focus", None, False)], held), [" "])
        self.assertEqual(key_edges([("key", "w", True)], held), [])

    def test_escape_requests_confirmed_stop_and_exit(self):
        h = Harness()
        h.run()
        h.c.key("ESC", h.t)
        self.assertTrue(h.c.quit_requested)
        h.ack()
        h.status()
        h.limits()
        self.assertEqual(h.c.phase, "idle")

    def test_margin_and_runtime_validation(self):
        for kw in ({"margin": -1}, {"max_run": 0}, {"max_run": float("inf")}):
            with self.assertRaises(ValueError):
                Harness(**kw)

    def test_adapter_uses_vendor_multinode_normal_transmission(self):
        calls = {}
        class Fn:
            def __init__(self, name): self.name = name
            def __call__(self, *args):
                if self.name == "ReadBoardInfo": args[-1]._obj.channels = 2
                if self.name == "InitCAN":
                    x = args[-1]._obj
                    calls['config'] = (x.Mode, x.Timing0, x.Timing1, x.AccCode, x.AccMask)
                if self.name == "Transmit":
                    x = args[-2]._obj
                    calls['tx'] = (x.ID, x.ExternFlag, x.RemoteFlag, x.DataLen, x.SendType, bytes(x.Data))
                if self.name == "Receive": return 0
                return 1
        class DLL:
            def __getattr__(self, name): return Fn(name.removeprefix("VCI_"))
        with patch.object(C, "WinDLL", return_value=DLL(), create=True):
            bus = ControlCan("unused.dll", 1, 0, lambda *a, **kw: None)
            bus.send(command("UP"))
            self.assertEqual(bus.receive(), [])
            bus.close()
        self.assertEqual(calls['config'], (0, 0, 0x1C, 0, 0xFFFFFFFF))
        self.assertEqual(calls['tx'], (1, 1, 0, 8, 0, command("UP")))

    def test_receive_counts_all_ids_before_protocol_filtering(self):
        class Fn:
            def __init__(self, name): self.name = name
            def __call__(self, *args):
                if self.name == "ReadBoardInfo": args[-1]._obj.channels = 2
                if self.name == "Receive":
                    frames = args[3]
                    for i, ident in enumerate((1, 2, 0x123)):
                        frames[i].ID = ident
                        frames[i].ExternFlag = 1
                        frames[i].DataLen = 8
                        frames[i].Data[:] = command("RL")
                    return 3
                return 1
        class DLL:
            def __getattr__(self, name): return Fn(name.removeprefix("VCI_"))
        with patch.object(C, "WinDLL", return_value=DLL(), create=True):
            bus = ControlCan("unused.dll", 1, 0, lambda *a, **kw: None)
            try:
                packets = bus.receive()
                self.assertEqual([p.ident for p in packets], [1, 2, 0x123])
                self.assertEqual(bus.statistics()["rx_frames"], 3)
            finally:
                bus.close()


if __name__ == "__main__":
    unittest.main()
