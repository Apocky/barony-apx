#!/usr/bin/env python3
"""tools/apx/test_farm.py -- offline tests of the farm runner (no Barony; about 15 s).

  python tools/apx/test_farm.py            all tests
  python tools/apx/test_farm.py -k sched   the pure scheduler tests only (under a second)

The scheduler is a pure function, tested case by case. The runner is tested end to end with stand-in
jobs (python child processes) in temporary farm and lock folders: exit codes, the until regex, the
timeout kill, an exclusive job waiting behind another lane's slot. Each rule has a control: the same
check run against a scheduler with that rule switched off must fail.
"""

import json
import os
import sys
import tempfile
import threading
import time
import types
import unittest
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
import farm  # noqa: E402
import apx_locks  # noqa: E402

PY = sys.executable
CLEAR = {"apocky_playing": False, "low_memory": False, "mem_budget_gb": None, "foreign_wants": 0,
         "foreign_excl": 0, "foreign_slots": 0, "foreign_barony": 0}


def jobs(*modes):
    return [{"id": f"{i:03d}", "mode": m} for i, m in enumerate(modes)]


def ids(js):
    return [j["id"] for j in js]


class SchedulerTests(unittest.TestCase):
    def test_sched_capacity_headless(self):
        start, want = farm.schedule(jobs(*["headless"] * 10), [], CLEAR)
        self.assertEqual(len(start), 6)
        self.assertIsNone(want)

    def test_sched_capacity_counts_running(self):
        running = jobs(*["headless"] * 4)
        start, _ = farm.schedule(jobs(*["headless"] * 10), running, CLEAR)
        self.assertEqual(len(start), 2)

    def test_sched_capacity_control_fails(self):
        # control: with the capacity rule switched off the capacity check must fail
        start, _ = farm.schedule(jobs(*["headless"] * 10), [], CLEAR, caps={"headless": 99, "windowed": 3,
                                                                               "exclusive": 1})
        self.assertNotEqual(len(start), 6)

    def test_sched_windowed_needs_free_d70_slot(self):
        start, _ = farm.schedule(jobs("windowed", "windowed", "windowed"), [], dict(CLEAR, foreign_slots=2))
        self.assertEqual(ids(start), ["000"])

    def test_sched_per_mode_fifo(self):
        running = jobs("windowed", "windowed", "windowed")
        pend = [{"id": "w1", "mode": "windowed"}, {"id": "w2", "mode": "windowed"}, {"id": "h1", "mode": "headless"}]
        start, _ = farm.schedule(pend, running, CLEAR)
        self.assertEqual(ids(start), ["h1"])     # windowed is full; the headless job behind them goes

    def test_sched_exclusive_is_a_barrier(self):
        pend = [{"id": "h1", "mode": "headless"}, {"id": "e1", "mode": "exclusive"}, {"id": "h2", "mode": "headless"}]
        start, want = farm.schedule(pend, [], CLEAR)
        self.assertEqual(ids(start), ["h1"])     # h2 waits behind e1
        self.assertIsNone(want)

    def test_sched_exclusive_waits_for_machine(self):
        pend = [{"id": "e1", "mode": "exclusive"}, {"id": "h2", "mode": "headless"}]
        start, want = farm.schedule(pend, jobs("headless"), CLEAR)
        self.assertEqual((ids(start), want), ([], "e1"))
        start, want = farm.schedule(pend, [], dict(CLEAR, foreign_slots=1))
        self.assertEqual((ids(start), want), ([], "e1"))
        start, want = farm.schedule(pend, [], dict(CLEAR, foreign_barony=1))
        self.assertEqual((ids(start), want), ([], "e1"))
        start, want = farm.schedule(pend, [], CLEAR)
        self.assertEqual((ids(start), want), (["e1"], "e1"))

    def test_sched_foreign_want_stops_new_launches(self):
        start, _ = farm.schedule(jobs("headless", "windowed"), [], dict(CLEAR, foreign_wants=1))
        self.assertEqual(start, [])
        start, _ = farm.schedule(jobs("headless"), [], dict(CLEAR, foreign_excl=1))
        self.assertEqual(start, [])

    def test_sched_apocky_playing_and_memory(self):
        self.assertEqual(farm.schedule(jobs("headless"), [], dict(CLEAR, apocky_playing=True))[0], [])
        self.assertEqual(farm.schedule(jobs("headless"), [], dict(CLEAR, low_memory=True))[0], [])
        start, _ = farm.schedule(jobs(*["headless"] * 6), [], dict(CLEAR, mem_budget_gb=5.0))
        self.assertEqual(len(start), 2)              # 2 x 2.3 GB fit in 5 GB, a third does not
        start, _ = farm.schedule(jobs("windowed", "headless"), [], dict(CLEAR, mem_budget_gb=3.0))
        self.assertEqual(ids(start), ["001"])        # a windowed game (3.2 GB) does not fit; the headless one does

    def test_sched_validate(self):
        self.assertEqual(farm.validate({"exe": "x", "mode": "headless", "timeout_s": 5}), [])
        self.assertTrue(farm.validate({"exe": "x", "mode": "fast", "timeout_s": 5}))
        self.assertTrue(farm.validate({"exe": "x", "mode": "headless", "timeout_s": 0}))
        self.assertTrue(farm.validate({"exe": "x", "mode": "headless", "timeout_s": 5, "until": "("}))


class RunnerTests(unittest.TestCase):
    """The real runner loop on stand-in jobs, in temporary farm and lock folders."""

    def setUp(self):
        self.tmp = Path(tempfile.mkdtemp(prefix="farmtest-"))
        self.saved = {k: getattr(apx_locks, k) for k in ("ROOT", "EXCL", "SLOTS", "WANT", "apocky_playing",
                                                         "barony_processes")}
        apx_locks.ROOT = self.tmp / "locks"
        apx_locks.EXCL = apx_locks.ROOT / "barony"
        apx_locks.SLOTS = apx_locks.ROOT / "barony-slots"
        apx_locks.WANT = apx_locks.ROOT / "barony-want"
        apx_locks.apocky_playing = lambda: []
        apx_locks.barony_processes = lambda: []
        self.saved_dirs = (farm.FARM, dict(farm.DIRS), farm.RUNNER_PID, farm.RUNNER_LOG)
        farm.FARM = self.tmp / "jobs"
        farm.DIRS = {k: farm.FARM / k for k in ("queue", "running", "done", "logs", "runs")}
        farm.RUNNER_PID = farm.FARM / "runner.pid"
        farm.RUNNER_LOG = farm.FARM / "runner.log"
        farm.ensure_dirs()

    def tearDown(self):
        for k, v in self.saved.items():
            setattr(apx_locks, k, v)
        farm.FARM, farm.DIRS, farm.RUNNER_PID, farm.RUNNER_LOG = self.saved_dirs

    def run_runner(self, limit_s=40):
        args = types.SimpleNamespace(poll_s=0.1, idle_exit_s=0.6, min_free_gb=-1000.0)
        th = threading.Thread(target=farm.Runner(args).loop, daemon=True)
        th.start()
        th.join(limit_s)
        self.assertFalse(th.is_alive(), "runner did not go idle")

    def done(self, jid):
        return json.loads((farm.DIRS["done"] / f"{jid}.json").read_text())

    def job(self, code, **kw):
        return dict({"exe": PY, "args": ["-c", code], "mode": "headless", "timeout_s": 20, "lane": "test",
                     "datadir": "", "close_grace_s": 1}, **kw)

    def test_exit_code_and_timeout_and_until(self):
        a = farm.submit(self.job("import sys; sys.exit(3)"))
        b = farm.submit(self.job("import time; time.sleep(60)", timeout_s=1.5))
        c = farm.submit(self.job("import time; open('log.txt','w').write('successfully generated a dungeon "
                                 "with 9 rooms'); time.sleep(60)", until=r"successfully generated a dungeon with \d+ rooms",
                                 until_grace_s=0.2))
        self.run_runner()
        ra, rb, rc = self.done(a), self.done(b), self.done(c)
        self.assertEqual((ra["ended_by"], ra["exit_code"]), ("exit", 3))
        self.assertEqual((rb["ended_by"], rb["killed"]), ("timeout", True))
        self.assertLess(rb["wall_s"], 8)
        self.assertEqual(rc["ended_by"], "until")
        self.assertIn("9 rooms", rc["until_line"])
        self.assertTrue(Path(rc["cwd"]).is_dir())
        self.assertEqual(list(farm.DIRS["queue"].glob("*.json")) + list(farm.DIRS["running"].glob("*.json")), [])

    def test_exclusive_waits_for_foreign_slot(self):
        apx_locks.SLOTS.mkdir(parents=True)
        foreign = apx_locks.SLOTS / "slot1.LOCK"
        foreign.write_text("other lane")
        seen = {}
        e = farm.submit(self.job("import time; time.sleep(0.5)", mode="exclusive"))

        def watch():
            t0 = time.time()
            while time.time() - t0 < 10:
                wants = list(apx_locks.WANT.glob("farm-*.WANT")) if apx_locks.WANT.exists() else []
                if wants and "want" not in seen:
                    seen["want"] = True
                    seen["excl_while_want"] = list(apx_locks.EXCL.glob("*.LOCK")) if apx_locks.EXCL.exists() else []
                    time.sleep(0.5)
                    foreign.unlink()                    # the other lane finishes
                if apx_locks.EXCL.exists() and list(apx_locks.EXCL.glob("farm-*.LOCK")):
                    seen["excl"] = True
                time.sleep(0.05)
        th = threading.Thread(target=watch, daemon=True)
        th.start()
        self.run_runner()
        th.join(12)
        self.assertTrue(seen.get("want"), "the exclusive job never wrote its .WANT")
        self.assertEqual(seen.get("excl_while_want"), [])
        self.assertTrue(seen.get("excl"), "the exclusive job never held barony/farm-<id>.LOCK")
        self.assertEqual(self.done(e)["ended_by"], "exit")
        self.assertEqual(list(apx_locks.WANT.glob("*.WANT")), [])
        self.assertEqual(list(apx_locks.EXCL.glob("*.LOCK")), [])

    def test_windowed_holds_and_frees_a_slot(self):
        w = farm.submit(self.job("import time; time.sleep(0.8)", mode="windowed"))
        held = []

        def watch():
            t0 = time.time()
            while time.time() - t0 < 5:
                if apx_locks.SLOTS.exists():
                    held.extend(p.name for p in apx_locks.SLOTS.glob("*.LOCK"))
                time.sleep(0.05)
        th = threading.Thread(target=watch, daemon=True)
        th.start()
        self.run_runner()
        th.join(6)
        self.assertIn("slot1.LOCK", held)
        self.assertEqual(list(apx_locks.SLOTS.glob("*.LOCK")), [])
        self.assertEqual(self.done(w)["ended_by"], "exit")


if __name__ == "__main__":
    if "-k" in sys.argv:
        pat = sys.argv[sys.argv.index("-k") + 1]
        sys.argv = [sys.argv[0], "-k", pat]
    unittest.main(verbosity=1)
