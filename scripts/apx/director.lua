-- The director (BF5): named, seeded temperament; reads tension once a second; spends a threat budget set by
-- party size and power; chooses at most one event per decision (or explicit silence). Pure choices live in
-- director_core.lua (offline-tested); this file reads the world and executes. Card: BF5-director-card.md,
-- plan: Apockalypsis handovers/plans/f5-acting.md. Implemented events: ambush, den, patrol. Stubs that decline
-- visibly: collapse, flood, rival, peddler (D73). Every decline is a counter in the decision row.
local NAMES = {
  "Harrow", "Orsolo", "Wend", "Keld", "Brannoch", "Maud Thorn", "Ysolt", "Corvane",
  "Sunnet", "Aurel", "Pip Lantern", "Tamsin", "Oriel", "Fenn", "Lark", "Mirabel",
  "Vael", "Ixen", "Hollis Grey", "Morrow", "Skerrit", "Nadir", "Thessaly Gaunt", "Quill",
}
local TEMPERS = { "calm", "story", "storm" }

local function mix(z)  -- splitmix64 on Lua 64-bit integers
  z = z + 0x9E3779B97F4A7C15
  z = (z ~ (z >> 30)) * 0xBF58476D1CE4E5B9
  z = (z ~ (z >> 27)) * 0x94D049BB133111EB
  return z ~ (z >> 31)
end
local function identity(seed)
  local h = mix(seed ~ 0x44495245)           -- "DIRE"
  return { name = NAMES[(h % #NAMES) + 1], temper = TEMPERS[((h >> 20) % 3) + 1] }
end

local D = { identity = identity, decisions = 0, ticks = {}, us = {}, blind = false, fixed = false,
            tension = { value = 0, ewma = 0 }, hits = {}, banked = nil, last = {}, last_event = nil,
            last_hostile_s = -1e9, force = nil, events_off = apx.is_test(), info = {}, totals = { ambush = 0, den = 0, patrol = 0, silence = 0 },
            declines = { event_unavailable = 0, budget_zero = 0, cooldown = 0, tension_gate = 0, repeat_blocked = 0,
                         silence_window = 0, event_blocked_no_tile = 0, spawn_refused = 0, no_eligible = 0, ambush_visible = 0 } }
apx.shared.director = D

local function clamp01(x) if x < 0 then return 0 elseif x > 1 then return 1 end return x end

local function read_tension()
  local now = apx.tick()
  local lost, maxsum, hpfrac_sum, n = 0, 0, 0, 0
  local keep = {}
  for _, h in ipairs(D.hits) do if now - h[1] <= 335 then keep[#keep + 1] = h; lost = lost + h[2] end end
  D.hits = keep
  local pos, dark, darkn, danger = {}, 0, 0, 0
  for i = 0, 3 do
    local u = apx.player_uid(i)
    if u then
      local hp, mhp = apx.get_stat(u, "hp"), apx.get_stat(u, "maxhp")
      if hp and mhp and mhp > 0 then n = n + 1; maxsum = maxsum + mhp; hpfrac_sum = hpfrac_sum + math.max(0, hp) / mhp end
      local x, y = apx.entity_pos(u)
      if x then
        pos[#pos + 1] = { x, y, u }
        local l = apx.light_at(math.floor(x), math.floor(y))
        if l then darkn = darkn + 1; if l < 32 then dark = dark + 1 end end
        danger = danger + #apx.monsters_near(x, y, 14)
      end
    end
  end
  if n == 0 then return nil end
  local damage6s = D.blind and 0 or clamp01(lost / maxsum)
  -- resources: only HP is readable until the inventory API exists; potions and food count as neutral (0.5)
  local resources = 1 - ((hpfrac_sum / n) * 0.5 + 0.5 * 0.3 + 0.5 * 0.2)
  local light = darkn > 0 and dark / darkn or 0
  local spread = 0
  for a = 1, #pos do for b = a + 1, #pos do
    local d = math.sqrt((pos[a][1] - pos[b][1]) ^ 2 + (pos[a][2] - pos[b][2]) ^ 2)
    spread = math.max(spread, clamp01((d - 4) / 20))
  end end
  local dng = clamp01(danger / 3)
  local value = 0.45 * damage6s + 0.20 * resources + 0.10 * light + 0.10 * spread + 0.15 * dng
  return { damage6s = damage6s, resources = resources, light = light, spread = spread, danger = dng,
           value = value, players = n, hp_frac = hpfrac_sum / n, resources_partial = true }, pos
end

apx.on("damage", function(d)
  if d.player ~= nil and not D.blind then D.hits[#D.hits + 1] = { apx.tick(), d.amount } end
end)

-- ---- event execution (engine glue) ------------------------------------------------------------
local function dist(ax, ay, bx, by) return math.sqrt((ax - bx) ^ 2 + (ay - by) ^ 2) end

local function nearest_player(pos, x, y)
  local best, bd = pos[1], 1e9
  for _, p in ipairs(pos) do local d = dist(p[1], p[2], x, y); if d < bd then best, bd = p, d end end
  return best
end

-- AMBUSH: a pack within `cost` budget spawns 14..22 tiles from the party (behind or beside), on open ground,
-- with an audible cue; after 2 s (100 ticks) it hunts the nearest player. Not in sight is only measured:
-- spawns that start in line of sight are counted (ambush_visible), because a spawned body cannot be undone.
local function do_ambush(core, rng, cost, depth, pos)
  local pack = core.build_pack(cost, rng, depth)
  local centre = pos[1 + rng:int(#pos)]
  local spawned, dists, visible = {}, {}, 0
  local ax, ay
  for try = 1, 6 do
    local ang = rng:float() * 2 * math.pi
    local r = 14 + rng:float() * 8
    ax, ay = centre[1] + math.cos(ang) * r, centre[2] + math.sin(ang) * r
    local u = pack[1] and apx.spawn(pack[1], ax, ay)
    if u then
      spawned[1] = u
      break
    else D.declines.event_blocked_no_tile = D.declines.event_blocked_no_tile + (try == 6 and 1 or 0) end
  end
  if #spawned == 0 then return { kind = "ambush", spawned = 0, pack = pack } end
  for i = 2, #pack do
    local u = apx.spawn(pack[i], ax + (rng:float() - 0.5) * 3, ay + (rng:float() - 0.5) * 3)
    if u then spawned[#spawned + 1] = u else D.declines.spawn_refused = D.declines.spawn_refused + 1 end
  end
  for _, u in ipairs(spawned) do
    local x, y = apx.entity_pos(u)
    if x then
      dists[#dists + 1] = dist(x, y, centre[1], centre[2])
      for _, p in ipairs(pos) do if apx.line_of_sight(p[3], u) then visible = visible + 1; break end end
    end
  end
  D.declines.ambush_visible = D.declines.ambush_visible + visible
  apx.message("You hear something close behind.")
  apx.after(100, function()
    for _, u in ipairs(spawned) do
      local x, y = apx.entity_pos(u)
      if x then local p = nearest_player(pos, x, y); apx.set_target(u, p[3]) end
    end
  end)
  return { kind = "ambush", spawned = #spawned, pack = pack, uids = spawned, dists = dists, visible = visible }
end

-- Sleeping/idle monsters far from every player: monster_state 0 (WAIT) and at least `far` tiles away.
local function idle_far(pos, far)
  local out = {}
  local p = pos[1]
  for _, u in ipairs(apx.monsters_near(p[1], p[2], 128)) do
    if apx.monster_state(u) == 0 then
      local x, y = apx.entity_pos(u)
      local ok = x ~= nil
      if ok then for _, q in ipairs(pos) do if dist(x, y, q[1], q[2]) < far then ok = false end end end
      if ok then out[#out + 1] = { u, x, y } end
    end
  end
  return out
end

-- DEN: wake sleepers until their creature cost reaches the event's budget; they path to the party.
local function do_den(core, rng, cost, depth, pos)
  local cands = idle_far(pos, 20)
  if #cands == 0 then D.declines.no_eligible = D.declines.no_eligible + 1; return { kind = "den", woken = 0, eligible = 0 } end
  local woken, spent = {}, 0
  for _, c in ipairs(cands) do
    if spent >= cost then break end
    local p = nearest_player(pos, c[2], c[3])
    if apx.set_target(c[1], p[3]) then woken[#woken + 1] = c[1]; spent = spent + 3 end
  end
  return { kind = "den", woken = #woken, eligible = #cands, uids = woken }
end

-- PATROL: an idle monster walks to a tile near the party's current position (a patrol crossing the route).
local function do_patrol(core, rng, cost, depth, pos)
  local cands = idle_far(pos, 15)
  if #cands == 0 then D.declines.no_eligible = D.declines.no_eligible + 1; return { kind = "patrol", moved = 0, eligible = 0 } end
  local c = cands[1 + rng:int(#cands)]
  local p = pos[1 + rng:int(#pos)]
  local ok = apx.path_to(c[1], math.floor(p[1] + (rng:float() - 0.5) * 8), math.floor(p[2] + (rng:float() - 0.5) * 8))
  if not ok then D.declines.path_failed = (D.declines.path_failed or 0) + 1 end
  return { kind = "patrol", moved = ok and 1 or 0, eligible = #cands, uid = c[1] }
end

local EXEC = { ambush = do_ambush, den = do_den, patrol = do_patrol }

-- ---- the decision ---------------------------------------------------------------------------
local me, rng
apx.every(50, function()
  if not apx.player_uid(0) then return end                 -- menu map: no player, no director
  local t0 = apx.clock_us()
  local core = apx.shared.director_core
  if not me then
    me = identity(apx.run_seed())
    D.me = me
    rng = core.rng_new(apx.run_seed() ~ 0x4442)
    D.rng = rng
    apx.log(string.format("director %s (%s), run seed %d", me.name, me.temper, apx.run_seed()))
  end
  local t, pos = read_tension()
  if not t then return end
  t.ewma = D.tension.ewma * 0.7 + t.value * 0.3
  D.tension = t
  D.decisions = D.decisions + 1
  local now_s = apx.tick() / 50
  D.ticks[#D.ticks + 1] = apx.tick()

  local depth = math.max(1, apx.floor())
  local ratio = 1.0                                         -- power ratio: ASSUMED 1.0 until F6/F1 define party power
  local total = core.budget_total(depth, ratio, t.players, me.temper, t.ewma, D.fixed)
  D.banked = D.banked == nil and total or core.accrue(D.banked, total, 1)

  local st = { now_s = now_s, ewma = t.ewma, banked = D.banked, total = total, temper = me.temper, last = D.last,
               last_event = D.last_event, last_hostile_s = D.last_hostile_s,
               available = { ambush = true, den = true, patrol = true }, no_gate = D.no_gate, no_cooldown = D.no_cooldown }
  local ev, reason, cost, counters = core.choose_event(st, rng)
  if D.force then ev, reason, cost = D.force, "forced (test)", total * (core.COST_SHARE[D.force] or 0.25); D.force = nil end
  if D.events_off then ev, cost = "silence", 0 end
  for k, v in pairs(counters) do D.declines[k] = (D.declines[k] or 0) + v end

  local result
  if ev ~= "silence" and EXEC[ev] then
    result = EXEC[ev](core, rng, cost, depth, pos)
    D.banked = D.banked - cost
    D.last[ev] = now_s; D.last_event = ev; D.last_hostile_s = now_s
    D.info[ev] = result
  end
  D.totals[ev] = (D.totals[ev] or 0) + 1

  local row = { id = D.decisions, tick = apx.tick(), floor = apx.floor(), director = me.name, temper = me.temper,
    event = ev, reason = reason, cost = cost, budget = { total = total, banked = D.banked, party = t.players, ratio = ratio, ratio_assumed = true, fixed = D.fixed },
    tension = t, declines = counters }
  if result then row.result = { kind = result.kind, spawned = result.spawned, woken = result.woken, moved = result.moved, eligible = result.eligible, visible = result.visible } end
  apx.row("dir_decisions", row)
  D.us[#D.us + 1] = apx.clock_us() - t0
end)
