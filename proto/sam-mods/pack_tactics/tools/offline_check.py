"""Offline check of pack_tactics/main.lua: real Lua 5.4 (lupa), stubbed S.A.M world (sam_mock.lua).

NOT a measurement. The mock world is not Barony; its numbers are never reported. It exists so a
runtime error in any code path costs seconds instead of a three-minute game launch.

Exit 0 when:
  1. main.lua compiles under Lua 5.4 with os, io, require and package removed (S.A.M's sandbox);
  2. the self-test path (test seed, human barbarian, INT -2) reaches sam_test_done with 0 failed
     checks and 0 script errors;
  3. a live session (another seed, population 3x, a player who swings and shoves, one level
     change) runs with 0 script errors, spawns packs and writes ENCOUNTER lines;
  4. CONTROL: a copy of main.lua with a planted runtime error is caught (errors > 0).

Usage: python tools/offline_check.py [--log]
"""
import os
import re
import sys

from lupa.lua54 import LuaRuntime

HERE = os.path.dirname(os.path.abspath(__file__))
MAIN = os.path.join(os.path.dirname(HERE), "main.lua")
MOCK = os.path.join(HERE, "sam_mock.lua")


MOCK_SRC_OVERRIDE = None     # a control may plant a hole in the mock world


def boot(main_src):
    lua = LuaRuntime(unpack_returned_tuples=True)
    lua.execute(MOCK_SRC_OVERRIDE or open(MOCK, encoding="utf8").read())
    lua.execute("os, io, require, package, dofile, loadfile = nil, nil, nil, nil, nil, nil")
    loader = lua.eval("function(src) local f, err = load(src, '@main.lua'); if not f then return err end; f(); return nil end")
    err = loader(main_src)
    if err:
        raise SyntaxError(err)
    return lua


def logs(lua):
    m = lua.globals().MOCK
    return [m.logs[i] for i in range(1, len(m.logs) + 1)]


def scenario_selftest(main_src):
    lua = boot(main_src)
    lua.execute("""
      MOCK.floor = 1                              -- as after -samtestfloor=1
      MOCK.buildMap(1)
      MOCK.addPlayer(10.5, 10.5, 40)
      MOCK.addMonster('goblin', 35.5, 10.5); MOCK.addMonster('goblin', 38.5, 12.5); MOCK.addMonster('goblin', 41.5, 8.5)
      MOCK.addMonster('rat', 15.5, 30.5); MOCK.addMonster('rat', 20.5, 28.5)
      MOCK.addMonster('skeleton', 22.5, 10.5)
    """)
    ticks = lua.eval("MOCK.run(20000)")
    done = lua.eval("MOCK.done")
    out = logs(lua)
    out.append("[mock] PACIFY applications (one sam_log line each in S.A.M): %d" % lua.eval("MOCK.applyLogs"))
    return ticks, (done[1], done[2]) if done else None, out


def scenario_live(main_src, seed=4242):
    lua = boot(main_src)
    lua.execute("MOCK.seed = %d" % seed)
    lua.execute("""
      MOCK.buildMap(1)
      MOCK.addPlayer(10.5, 10.5, 100000)
      MOCK.addMonster('goblin', 35.5, 10.5); MOCK.addMonster('goblin', 38.5, 12.5); MOCK.addMonster('goblin', 41.5, 8.5)
      MOCK.addMonster('rat', 15.5, 30.5); MOCK.addMonster('rat', 20.5, 28.5)
      MOCK.addMonster('skeleton', 22.5, 10.5)
      ROUTE = { {22.5, 10.5}, {34.5, 10.5}, {40.5, 12.5}, {40.5, 24.5}, {30.5, 30.5}, {16.5, 30.5}, {12.5, 20.5}, {10.5, 10.5} }
      LEG = 1
      HOOKS = function(t)
        local p = MOCK.ents[1]
        if t > 200 and LEG <= #ROUTE then          -- the player walks the level, 0.04 tiles a tick
          local w = ROUTE[LEG]
          local dx, dy = w[1] - p.x, w[2] - p.y
          local d = math.sqrt(dx * dx + dy * dy)
          if d < 0.1 then LEG = LEG + 1 else p.x, p.y = p.x + dx / d * math.min(0.04, d), p.y + dy / d * math.min(0.04, d) end
        end
        if t % 25 == 0 then                       -- the player swings at the nearest monster in reach
          local best, bd = nil, nil
          for uid, e in pairs(MOCK.ents) do
            if not e.gone and not e.player and e.hp > 0 then
              local d = math.sqrt((e.x - p.x)^2 + (e.y - p.y)^2)
              if d <= 1.5 and (not bd or d < bd) then best, bd = uid, d end
            end
          end
          if best then
            on_event({ name = 'player.on_before_hit', player = 0, attacker_uid = 1, target_uid = best, target_type = 1,
              damage = 6, backstab = 0, flanking = 0, weapon_type = 0, charge = 0, max_charge = 30, fully_charged = 0 })
            sam_deal_damage_typed(best, 6, 'axe')
          end
        end
        if t % 300 == 0 then on_event({ name = 'on_action_pressed', player = 0, action = 'pack_tactics:shove', binding = 'G' }) end
        if t == 4000 then
          LEG = 1
          p.x, p.y = 10.5, 10.5
          MOCK.newLevel(1, 778, 2)
          on_event({ name = 'game.on_level_entered', player = 0, floor = 1, level_name = 'Mock Mines 2' })
          MOCK.addMonster('goblin', 50.5, 30.5); MOCK.addMonster('goblin', 52.5, 28.5); MOCK.addMonster('spider', 35.5, 8.5)
          MOCK.addMonster('rat', 18.5, 32.5)
        end
      end
    """)
    ticks = lua.eval("MOCK.run(8000, HOOKS)")
    out = logs(lua)
    out.append("[mock] PACIFY applications (one sam_log line each in S.A.M): %d" % lua.eval("MOCK.applyLogs"))
    held = lua.eval("(function() local n = 0; for _, e in pairs(MOCK.ents) do "
                    "if not e.gone and not e.player and MOCK.tick < (e.pacifyUntil or 0) then n = n + 1 end end; return n end)()")
    out.append("[mock] creatures still pacified at the end: %d" % held)
    return ticks, out


def main():
    show = "--log" in sys.argv
    src = open(MAIN, encoding="utf8").read()
    ok = True
    try:
        boot(src)
        print("compile: OK (Lua 5.4, sandboxed)")
    except Exception as e:  # noqa: BLE001 - report any load failure
        print("compile: FAIL", e)
        return 1

    ticks, verdict, out = scenario_selftest(src)
    selftest_out = out
    errs = [l for l in out if "pack_tactics: ERROR" in l or "raised an error" in l]
    fails = [l for l in out if "FAIL  pack_tactics" in l]
    passes = [l for l in out if "PASS  pack_tactics" in l]
    print("selftest: ticks=%d verdict=%s pass_lines=%d fail_lines=%d errors=%d" % (
        ticks, verdict, len(passes), len(fails), len(errs)))
    for l in fails + errs:
        print("   ", l)
    for l in out:
        if ("TRIAL" in l and "pack_tactics:" in l) or l.startswith("[mock]"):
            print("    mock:", (l.split("pack_tactics: ", 1)[1] if "pack_tactics: " in l else l)[:300])
    if not verdict or verdict[1] != 0 or errs:
        ok = False
    if show:
        print("\n".join(out[-120:]))

    ticks, out = scenario_live(src)
    errs = [l for l in out if "pack_tactics: ERROR" in l]
    enc = [l for l in out if "pack_tactics: ENCOUNTER" in l]
    spawned = [l for l in out if "pack_tactics: spawned pack" in l]
    stats = [l for l in out if "pack_tactics: stats" in l]
    levels = [l for l in out if re.search(r"pack_tactics: level \d", l)]
    print("live: ticks=%d levels=%d spawned_packs=%d encounters=%d stats_lines=%d errors=%d" % (
        ticks, len(levels), len(spawned), len(enc), len(stats), len(errs)))
    for l in errs[:8]:
        print("   ", l)
    for l in out:
        if l.startswith("[mock]"):
            print("   ", l)
    if stats:
        print("    last", stats[-1].split("pack_tactics: ", 1)[1][:600])
    for l in enc[:3]:
        print("    enc", l.split("pack_tactics: ", 1)[1][:300])
    if errs or not enc or not spawned or len(levels) < 2:
        ok = False
    # D62: in live play the natural limits act and the old cap stays off
    last = dict(re.findall(r"(\w+)=(\d+)", stats[-1])) if stats else {}
    limits_ok = int(last.get("releases", 0)) > 0 and int(last.get("cap_grants", 1)) == 0
    print("live D62: releases=%s lim_holds=%s cap_grants=%s -> %s" % (
        last.get("releases"), last.get("lim_holds"), last.get("cap_grants"), "ok" if limits_ok else "FAIL"))
    if not limits_ok:
        ok = False

    # CONTROL 1: a hold that does not pin the attack timer must be caught by the leak check
    global MOCK_SRC_OVERRIDE
    # (held creatures also back away, a second safeguard, so the hole removes that too)
    MOCK_SRC_OVERRIDE = open(MOCK, encoding="utf8").read().replace(
        "if pacified then e.hitTime = HITRATE // 2 end", "-- (planted hole: PACIFY does not pin)", 1).replace(
        "if not pacified and not e.windupAt and dd <= REACH", "if not e.windupAt and dd <= REACH", 1).replace(
        "if dd < 2.0 then gx, gy = e.x - (tg.x - e.x), e.y - (tg.y - e.y) end",
        "if dd > 1.0 then gx, gy = tg.x, tg.y end", 1)
    assert "planted hole" in MOCK_SRC_OVERRIDE and "if not e.windupAt and dd <= REACH" in MOCK_SRC_OVERRIDE \
        and "if dd > 1.0 then gx, gy = tg.x, tg.y end" in MOCK_SRC_OVERRIDE
    _, _, out_hole = scenario_selftest(src)
    MOCK_SRC_OVERRIDE = None
    caught_hole = any("FAIL" in l and "no blow decided while held" in l for l in out_hole)
    print("control (hold that does not pin): leak check %s" % ("caught" if caught_hole else "MISSED"))
    if not caught_hole:
        ok = False
    # CONTROL 2: the timings before the wind-up fix (miss timeout 30, line re-hold any time) hold
    # creatures mid-swing more often, so more blows land late, than the shipped timings
    def late(out):
        return sum(int(m) for l in out for m in re.findall(r"TRIAL v=2 .*? late_blows=(\d+)", l))
    old = src.replace("local SWING_WAIT  = 48", "local SWING_WAIT  = 30", 1).replace("local COMMIT_AT   = 18", "local COMMIT_AT   = 999", 1)
    _, _, out_old = scenario_selftest(old)
    late_new, late_old = late(selftest_out), late(out_old)
    print("control (pre-fix swing timings): late blows %d vs shipped %d -> %s" % (
        late_old, late_new, "caught" if late_old > late_new else "MISSED"))
    if not late_old > late_new:
        ok = False

    # CONTROL: a planted runtime error must be caught and counted
    bad = src.replace("local function packThink(pk, ps)\n", "local function packThink(pk, ps)\n  local _ = nil + 1\n", 1)
    _, out = scenario_live(bad)
    caught = [l for l in out if "pack_tactics: ERROR" in l]
    print("control (planted error in packThink): errors=%d -> %s" % (len(caught), "caught" if caught else "MISSED"))
    if not caught:
        ok = False

    print("OFFLINE CHECK:", "PASS" if ok else "FAIL")
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
