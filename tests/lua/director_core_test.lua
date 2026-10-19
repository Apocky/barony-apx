-- Offline gates for director_core.lua (no Barony). Run from the repo root: apx_lua_run.exe tests/lua/director_core_test.lua
apx = { shared = {} }
local C = dofile("scripts/apx/director_core.lua")
local fails, passes = 0, 0
local function check(name, cond, extra)
  if cond then passes = passes + 1; print("ok   " .. name .. (extra and (" [" .. extra .. "]") or ""))
  else fails = fails + 1; print("FAIL " .. name .. (extra and (" [" .. extra .. "]") or "")) end
end

-- 1. party size factor and the fixed-budget control
do
  local b1 = C.budget_total(3, 1.0, 1, "story", 0, false)
  local want = { 1.00, 1.68, 2.28, 2.83 }
  local ok = true; local seen = {}
  for n = 1, 4 do
    local r = C.budget_total(3, 1.0, n, "story", 0, false) / b1
    seen[#seen + 1] = string.format("%.2f", r)
    if math.abs(r - want[n]) > 0.01 then ok = false end
  end
  check("budget ratios for party 1..4 are 1.00/1.68/2.28/2.83", ok, table.concat(seen, " "))
  local f4 = C.budget_total(3, 1.0, 4, "story", 0, true) / C.budget_total(3, 1.0, 1, "story", 0, true)
  check("CONTROL: fixed budget gives party 4 the same budget as party 1 (the size-ratio test would FAIL against it)", math.abs(f4 - 2.83) > 0.5, string.format("%.2f", f4))
  check("power ratio is clamped to 0.6..1.8", C.power_factor(0.1) == C.power_factor(0.6) and C.power_factor(9) == C.power_factor(1.8))
  check("temperament scales budget calm < story < storm", C.budget_total(3,1,1,"calm",0,false) < C.budget_total(3,1,1,"story",0,false) and C.budget_total(3,1,1,"story",0,false) < C.budget_total(3,1,1,"storm",0,false))
  check("high tension lowers the budget", C.budget_total(3,1,1,"story",1.0,false) < C.budget_total(3,1,1,"story",0.0,false))
end

-- 2. pack building stays inside budget and fills it
do
  local over, under, n = 0, 0, 1000
  for seed = 1, n do
    local depth = 1 + seed % 6
    local budget = C.budget_total(depth, 1.0, 1 + seed % 4, "story", 0, false)
    local pack, spent = C.build_pack(budget, C.rng_new(seed), depth)
    if spent > budget + 1e-9 then over = over + 1 end
    if #pack < 16 and spent < budget - 1 then under = under + 1 end   -- cheapest creature costs 1
  end
  check("1000 packs never exceed the budget", over == 0, "over=" .. over)
  check("1000 packs leave under 1 cost unit unspent (unless capped at 16 creatures)", under == 0, "under=" .. under)
  local bad = 0
  for seed = 1, 200 do for _, k in ipairs((C.build_pack(40, C.rng_new(seed), 1))) do if k == "troll" or k == "bugbear" then bad = bad + 1 end end end
  check("depth gating: no troll or bugbear on floor 1", bad == 0)
  local a, b = C.build_pack(30, C.rng_new(77), 3), C.build_pack(30, C.rng_new(77), 3)
  check("same seed gives the same pack", table.concat(a, ",") == table.concat(b, ","))
  local ones = {}
  for _, c in ipairs(C.CREATURES) do ones[#ones + 1] = { kind = c.kind, cost = 1, from = c.from } end
  local _, sp = C.build_pack(30, C.rng_new(1), 3, ones)
  check("CONTROL: all-cost-1 table fails the 85 percent spend band", sp < 0.85 * 30, "spent " .. sp)
end

-- 3. event choice invariants over a long simulated run
local function simulate(temper, seed, flags, steps)
  local rng, noise = C.rng_new(seed), C.rng_new(seed + 1)
  local total = C.budget_total(3, 1.0, 2, temper, 0.1, false)
  local st = { now_s = 0, ewma = 0.1, banked = total, total = total, temper = temper, last = {}, last_event = nil, last_hostile_s = -1e9,
    available = { ambush = true, den = true, patrol = true, collapse = false, flood = false, rival = false, peddler = false },
    no_gate = flags.no_gate, no_cooldown = flags.no_cooldown }
  local count, viol, e = {}, { gate = 0, cooldown = 0, repeated = 0, silence = 0 }, 0.1
  for step = 1, steps do
    st.now_s = step
    e = math.max(0, math.min(1, e * 0.9 + (noise:float() < 0.08 and 0.6 or 0.03)))   -- slow walk with bursts
    st.ewma = e
    st.banked = C.accrue(st.banked, total, 1)
    local ev, _, cost = C.choose_event(st, rng)
    count[ev] = (count[ev] or 0) + 1
    if ev ~= "silence" then
      if st.ewma > 0.6 then viol.gate = viol.gate + 1 end
      if st.now_s - (st.last[ev] or -1e9) < C.COOLDOWN_S[ev] then viol.cooldown = viol.cooldown + 1 end
      if st.last_event == ev then viol.repeated = viol.repeated + 1 end
      if st.now_s - st.last_hostile_s < C.MIN_SILENCE_AFTER_HOSTILE_S then viol.silence = viol.silence + 1 end
      st.last[ev] = st.now_s; st.last_event = ev; st.last_hostile_s = st.now_s; st.banked = st.banked - cost
    end
  end
  return count, viol
end
do
  for _, t in ipairs({ "calm", "story", "storm" }) do
    local c, v = simulate(t, 11, {}, 10000)
    local ev = (c.ambush or 0) + (c.den or 0) + (c.patrol or 0)
    check(t .. ": 10,000 decisions, zero violations of gate / cooldown / repeat / silence window",
      v.gate + v.cooldown + v.repeated + v.silence == 0 and ev > 20, string.format("events=%d silence=%d", ev, c.silence or 0))
  end
  local cc, ss = simulate("calm", 5, {}, 10000), simulate("storm", 5, {}, 10000)
  check("silence fraction: calm higher than storm", (cc.silence or 0) > (ss.silence or 0), string.format("calm %.3f storm %.3f", (cc.silence or 0) / 10000, (ss.silence or 0) / 10000))
  local _, v1 = simulate("storm", 11, { no_gate = true }, 10000)
  check("CONTROL: gate removed -> the gate assertion trips", v1.gate > 0, "violations " .. v1.gate)
  local _, v2 = simulate("storm", 11, { no_cooldown = true }, 10000)
  check("CONTROL: cooldowns removed -> the cooldown/repeat assertion trips", v2.cooldown + v2.repeated > 0, "violations " .. (v2.cooldown + v2.repeated))
  local c1, c2 = simulate("story", 123, {}, 2000), simulate("story", 123, {}, 2000)
  check("equal seed -> equal decisions", c1.ambush == c2.ambush and c1.den == c2.den and c1.patrol == c2.patrol and c1.silence == c2.silence)
end

-- 4. weighted mix matches the analytic probabilities (no cooldowns, calm tension, ample budget)
do
  for _, t in ipairs({ "calm", "storm" }) do
    local tw = C.TEMPER_W[t]
    local w = { ambush = C.BASE_W.ambush * tw.ambush, den = C.BASE_W.den * tw.den, patrol = C.BASE_W.patrol * tw.patrol }
    local sw = tw.silence
    local sum = w.ambush + w.den + w.patrol + sw
    local rng = C.rng_new(2024)
    local st = { now_s = 0, ewma = 0.1, banked = 1e9, total = 10, temper = t, last = {}, available = { ambush = true, den = true, patrol = true }, no_cooldown = true }
    local N, cnt = 6000, { ambush = 0, den = 0, patrol = 0, silence = 0 }
    for _ = 1, N do local e = C.choose_event(st, rng); cnt[e] = cnt[e] + 1 end
    local ok = true
    for _, e in ipairs({ "ambush", "den", "patrol", "silence" }) do
      local p = ((e == "silence") and sw or w[e]) / sum
      if math.abs(cnt[e] / N - p) > 3 * math.sqrt(p * (1 - p) / N) then ok = false end
    end
    check(t .. ": 6,000 draws match the analytic mix within 3 sigma", ok)
  end
end

-- 5. every decline is counted (a stub is never silently dropped)
do
  local st = { now_s = 1000, ewma = 0.1, banked = 100, total = 20, temper = "story", last = {},
    available = { ambush = true, den = true, patrol = true } }
  local _, _, _, c = C.choose_event(st, C.rng_new(3))
  check("4 stubs counted as event_unavailable", c.event_unavailable == 4, "got " .. c.event_unavailable)
  st.ewma = 0.8
  local ev, _, _, c2 = C.choose_event(st, C.rng_new(3))
  check("ewma 0.8: silence, and all 3 implemented events counted as gate-blocked", ev == "silence" and c2.tension_gate == 3, "gate=" .. c2.tension_gate)
  st.ewma = 0.1; st.banked = 0
  local _, _, _, c3 = C.choose_event(st, C.rng_new(3))
  check("zero banked: budget_zero counted for all three", c3.budget_zero == 3, "budget_zero=" .. c3.budget_zero)
end

print(string.format("RESULT %s %d passed, %d failed", fails == 0 and "PASS" or "FAIL", passes, fails))
os.exit(fails == 0 and 0 or 1)
