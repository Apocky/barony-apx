-- Director core: PURE logic (no engine calls), unit-tested offline by tests/lua/director_core_test.lua.
-- director.lua gathers readings, calls these functions, and executes the chosen event.
-- Constants marked ASSUMED are tuned in data (BF5-7), not argued here. Card: BF5-director-card.md.
local C = {}

-- ---- rng: splitmix64 on Lua integers, seeded, deterministic ------------------------------------
local function mix(z)
  z = (z ~ (z >> 30)) * 0xBF58476D1CE4E5B9
  z = (z ~ (z >> 27)) * 0x94D049BB133111EB
  return z ~ (z >> 31)
end
local Rng = {}; Rng.__index = Rng
function C.rng_new(seed) return setmetatable({ s = seed ~ 0x5DEECE66D }, Rng) end
function Rng:raw() self.s = self.s + 0x9E3779B97F4A7C15; return mix(self.s) end
function Rng:float() return (self:raw() >> 11) * (1.0 / 9007199254740992.0) end   -- [0,1)
function Rng:int(n) return (self:raw() >> 1) % n end                              -- [0,n)

-- ---- budget -----------------------------------------------------------------------------------
local function clamp(x, lo, hi) if x < lo then return lo elseif x > hi then return hi end return x end
C.clamp = clamp
function C.party_factor(n) return n ^ 0.75 end                       -- 1.00 1.68 2.28 2.83 (ASSUMED exponent)
function C.power_factor(ratio) return clamp(ratio, 0.6, 1.8) ^ 0.8 end
C.TEMPER_BUDGET = { calm = 0.85, story = 1.0, storm = 1.2 }
function C.base_budget(depth) return 6 + 4 * depth end                -- cost units per decision epoch (ASSUMED)
-- fixed = true is the A5 CONTROL: budget ignores party size and power
function C.budget_total(depth, ratio, n, temper, ewma, fixed)
  local t = C.base_budget(depth) * (C.TEMPER_BUDGET[temper] or 1.0) * (1 - 0.5 * clamp(ewma or 0, 0, 1))
  if fixed then return t end
  return t * C.power_factor(ratio) * C.party_factor(n)
end

-- ---- creatures: cost by tier (fodder 1, regular 3, elite 10), from depth (ASSUMED until F1 baselines) ----
C.CREATURES = {
  { kind = "rat", cost = 1, from = 1 }, { kind = "bat", cost = 1, from = 1 }, { kind = "kobold", cost = 1, from = 1 },
  { kind = "goblin", cost = 3, from = 1 }, { kind = "slime", cost = 3, from = 1 }, { kind = "gnome", cost = 3, from = 2 },
  { kind = "spider", cost = 3, from = 2 }, { kind = "skeleton", cost = 3, from = 2 }, { kind = "scorpion", cost = 3, from = 3 },
  { kind = "ghoul", cost = 3, from = 3 }, { kind = "imp", cost = 3, from = 3 }, { kind = "troll", cost = 10, from = 4 },
  { kind = "bugbear", cost = 10, from = 5 },
}
function C.cost_of(kind) for _, c in ipairs(C.CREATURES) do if c.kind == kind then return c.cost end end end

-- Greedy seeded pack: picks eligible kinds that still fit, stops when none fits or 16 creatures. Returns list, total cost.
function C.build_pack(budget, rng, depth, costs)
  costs = costs or C.CREATURES
  local pack, spent = {}, 0
  while #pack < 16 do
    local fit = {}
    for _, c in ipairs(costs) do if c.from <= depth and spent + c.cost <= budget then fit[#fit + 1] = c end end
    if #fit == 0 then break end
    local c = fit[rng:int(#fit) + 1]
    pack[#pack + 1] = c.kind; spent = spent + c.cost
  end
  return pack, spent
end

-- ---- event choice ---------------------------------------------------------------------------
C.EVENTS = { "ambush", "den", "patrol", "peddler" }                  -- implemented (peddler: D73 relief event after a hard fight)
C.STUBS = { "collapse", "flood", "rival" }                            -- decline visibly
C.BASE_W = { ambush = 1.0, den = 0.8, patrol = 1.0, collapse = 0.4, flood = 0.3, rival = 0.3, peddler = 0.6 }
C.COOLDOWN_S = { ambush = 90, den = 120, patrol = 60, collapse = 180, flood = 240, rival = 300, peddler = 400 }
C.COST_SHARE = { ambush = 0.5, den = 0.35, patrol = 0.25, collapse = 0, flood = 0, rival = 0, peddler = 0 }
C.MIN_SILENCE_AFTER_HOSTILE_S = 20
C.TEMPER_W = {
  calm = { ambush = 0.6, den = 0.5, patrol = 0.8, silence = 1.6 },
  story = { ambush = 1.0, den = 1.0, patrol = 1.0, silence = 1.0 },
  storm = { ambush = 1.5, den = 1.4, patrol = 1.2, silence = 0.5 },
}
local function gate(ewma) if ewma < 0.35 then return 1.0 elseif ewma <= 0.6 then return 0.3 else return 0.0 end end
C.gate = gate

-- st: { now_s, ewma, banked, total, temper, last = {event -> time_s}, last_event, last_hostile_s,
--       available = {event -> bool}, marks = {event -> mult} , no_gate, no_cooldown (CONTROL flags) }
-- returns event name ("silence" when nothing), reason string, cost to spend, counters table (declines)
function C.choose_event(st, rng)
  local counters = { event_unavailable = 0, budget_zero = 0, cooldown = 0, tension_gate = 0, repeat_blocked = 0, silence_window = 0 }
  local tw = C.TEMPER_W[st.temper] or C.TEMPER_W.story
  local g = st.no_gate and 1.0 or gate(st.ewma)
  local cands, wsum = {}, 0
  local silence_w = tw.silence * (1 + 6 * math.max(0, st.ewma - 0.5))
  local in_silence_window = (st.now_s - (st.last_hostile_s or -1e9)) < C.MIN_SILENCE_AFTER_HOSTILE_S
  local all = {}
  for _, e in ipairs(C.EVENTS) do all[#all + 1] = e end
  for _, e in ipairs(C.STUBS) do all[#all + 1] = e end
  for _, e in ipairs(all) do
    local ok = true
    if not (st.available and st.available[e]) then counters.event_unavailable = counters.event_unavailable + 1; ok = false end
    if ok and not st.no_cooldown and (st.now_s - ((st.last and st.last[e]) or -1e9)) < C.COOLDOWN_S[e] then counters.cooldown = counters.cooldown + 1; ok = false end
    if ok and not st.no_cooldown and st.last_event == e then counters.repeat_blocked = counters.repeat_blocked + 1; ok = false end
    if ok and g == 0 then counters.tension_gate = counters.tension_gate + 1; ok = false end
    if ok and in_silence_window and not st.no_cooldown then counters.silence_window = counters.silence_window + 1; ok = false end
    local cost = (st.total or 0) * (C.COST_SHARE[e] or 0)
    if ok and cost > 0 and cost > (st.banked or 0) then counters.budget_zero = counters.budget_zero + 1; ok = false end
    if ok then
      local w = (C.BASE_W[e] or 1) * (tw[e] or 1) * ((st.marks and st.marks[e]) or 1) * g
      cands[#cands + 1] = { e = e, w = w, cost = cost }; wsum = wsum + w
    end
  end
  local total_w = wsum + silence_w
  local r = rng:float() * total_w
  if r >= wsum or #cands == 0 then return "silence", "weighted draw chose silence", 0, counters end
  local acc = 0
  for _, c in ipairs(cands) do
    acc = acc + c.w
    if r < acc then return c.e, "weighted draw", c.cost, counters end
  end
  return "silence", "fallthrough", 0, counters
end

-- Banked budget accrues total/120 per second, capped at 1.0 x total.
function C.accrue(banked, total, dt_s) return math.min(total, banked + total * dt_s / 120) end

if apx and apx.shared then apx.shared.director_core = C end
return C
