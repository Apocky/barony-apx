-- Pack Tactics -- PROTOTYPE, never ships.
--
-- S-track of Apockalypsis specs/barony-fork/09 section 3: the fork's tactics layer (03 sections
-- 4-5) and crowd combat (04 sections 3 and 8), tried in Lua on the installed S.A.M 3.4.0 build so
-- the C++ of slices F3 and F4 is written from measurements, not guesses.
--
-- Barony has one shared monster state machine. Like smart_followers (whose formation-slot,
-- staggered-thinking and counter code this reuses, reversed: slots ring an enemy instead of
-- trailing a leader), this mod steers that machine from outside a few times a second.
--
--   PACKS     Every hostile group is a pack: spawned packs, and vanilla monsters grouped by room.
--             A pack has a home room, morale and a target player. On contact one member flanks
--             (detours through a side waypoint and arrives behind the target). Losses lower
--             morale; below MORALE_BREAK the pack falls back to its home room, heals, regroups.
--   TOKENS    CONTROL ONLY, off by default (D62 dropped the cap as "too artificial"): at most
--             TOKENS_MELEE creatures may attack one player; one without a token that comes inside
--             HOLD_R is held on a ring slot 3-5 tiles out; a token passes on after
--             TOKEN_MAX_TICKS when others wait; bosses take all of them.
--   VERBS     A melee hit sweeps: typed damage, with falloff, to the other hostiles in a 100
--             degree arc within reach. Shove (default G) kicks, or bashes with a shield, through
--             the engine's knockback. Both spend stamina, shown as a HUD bar with its number.
--   DENSITY   When a level starts, extra packs are spawned room by room until the floor holds
--             `population` times its vanilla hostile count (1 = off; 3 is the F3 target).
--   TELEMETRY One sam_log.txt line per encounter ("pack_tactics: ENCOUNTER ..."). The parser
--             tools/pack_telemetry.py turns them into JSONL rows plus a baseline.
--
-- SUBSTITUTES (logged once per level, so nobody mistakes them for the fork's design):
--   tokens  The fork gates the motor layer's attack state. S.A.M can only clear a creature's
--           target (sam_clear_monster_target) and path it to a slot (sam_monster_path_to), so a
--           held creature is cleared EVERY tick; the engine re-acquires a visible target within a
--           tick or two (counted as reacquire), so held creatures stutter instead of circling.
--   flanks  The fork runs a second A* with the main route's tiles penalised. S.A.M exposes no
--           path costs, so the flanker detours through a side waypoint, cleared every tick.
--   retreat Same lever as tokens: cleared every tick while it walks home.
--   sweeps  sam_deal_damage_typed has no attacker: sweep damage earns no XP and draws no aggro.
--   tiers   Elites (2 tokens) and ranged tokens are not modelled; vanilla has no tiers.
--
-- Counters print to sam_log.txt every STATS_TICKS so a silent no-op is visible.

local NS = "pack_tactics"

-- brain timing (03 section 1: decide every 0.2 s, staggered so the cost is flat)
local THINK_TICKS     = 10
local PACK_TICKS      = 10
local TOKEN_TICKS     = 5
-- tokens and rings (03 sections 4-5)
local TOKENS_MELEE    = 3
local TOKEN_MAX_TICKS = 250      -- 5 s (spec: 4-6 s, ASSUMED)
local RING_MIN        = 3        -- ring slot radius band, tiles
local RING_MAX        = 5
local HOLD_R          = 2.75     -- a waiting creature closer than this is held back (tiles)
local ENGAGE_R        = 12       -- token pool radius around a player (tiles)
local SLOT_REPATH     = 10       -- ticks between slot path orders per held creature
local SLOT_REACHED    = 1.25     -- tiles
-- flanks
local FLANK_MIN_PACK  = 3
local FLANK_SIDE      = 6        -- side waypoint offset from the approach line (tiles)
local FLANK_BEHIND    = 3        -- arrival point behind the target, seen from the pack (tiles)
local FLANK_TIMEOUT   = 600      -- 12 s
-- morale and contact (03 section 4)
local MORALE_START    = 100
local MORALE_BREAK    = 45
local MORALE_REGROUP  = 70
local MORALE_DEATHS   = 90       -- spread over the pack's starting size
local MORALE_LEADER   = 15       -- extra loss when the leader dies
local MORALE_HIT      = 3        -- loss when a member takes 25 percent of its max HP in one hit
local REGROUP_RATE    = 0.03     -- morale per tick at home (1.5 per second)
local RETREAT_MAX     = 750      -- 15 s to get home, then it regroups where it stands
local RETREAT_CD      = 1500     -- 30 s between retreats of one pack
local ENGAGE_TRIGGER  = 7        -- a visible player this close starts an encounter (tiles)
local DISENGAGE_TICKS = 500      -- 10 s without contact ends an encounter
-- density (09 section 2: population 1x, 3x after F3, 5x after F4)
local SAFE_R          = 10       -- no pack spawns this close to a player (tiles)
local MAX_PACKS_ROOM  = 3
local TILES_PER_PACK  = 8        -- a room needs this many interior tiles per pack it holds
local PACK_MIN, PACK_MAX = 3, 5
-- crowd verbs (04 section 3)
local STAMINA_MAX     = 100
local REGEN_DELAY     = 40       -- 0.8 s after the last spend
local REGEN_PER_TICK  = 0.3      -- 15 per second
local SWEEP_COST      = 10
local KICK_COST, KICK_FORCE = 15, 2.0
local BASH_COST, BASH_FORCE = 25, 3.5
local STAGGER_TICKS   = 30       -- 0.6 s (04 section 3, shield bash)
local SHOVE_R         = 1.8
local ARC_HALF        = math.rad(50)          -- the 100 degree arc
local FALLOFF         = { 0.6, 0.45, 0.3, 0.2 }
-- HUD, in Barony's virtual 1280x720 screen (sam_hud.cpp positions in Frame::virtualScreen)
local HUD_X, HUD_Y, HUD_W, HUD_H = 520, 610, 240, 16   -- D68: bar sized like a vitals bar
local STATS_TICKS     = 1500     -- 30 s

local TEST_SEED       = 1514995713   -- S.A.M's fixed -samtest dungeon seed (0x5A4D0001)

-- creatures that never form packs (bosses, shop and town folk, constructs, mimics)
local NO_PACK = { nothing = true, shopkeeper = true, human = true, lich = true, lichice = true,
  lichfire = true, devil = true, minotaur = true, shadow = true, mimic = true, minimimic = true,
  sentrybot = true, spellbot = true, gyrobot = true, dummybot = true, hologram = true,
  duck_small = true, revenant_skull = true, monster_adorcised_weapon = true, automaton = true }
-- creatures the token gate ignores; bosses hold every token of their target
local NO_TOKEN = { shopkeeper = true, sentrybot = true, spellbot = true, gyrobot = true,
  dummybot = true, hologram = true, mimic = true, duck_small = true }
local BOSS = { lich = true, lichice = true, lichfire = true, devil = true, minotaur = true }
-- species for extra packs when the floor's census found none to copy (ASSUMED by depth)
local FALLBACK = {
  { 4,  { "rat", "goblin", "skeleton", "spider" } },
  { 9,  { "goblin", "spider", "ghoul", "skeleton", "scorpion" } },
  { 14, { "goblin", "kobold", "insectoid", "gnome", "scarab" } },
  { 19, { "skeleton", "goatman", "ghoul", "insectoid" } },
  { 99, { "imp", "demon", "succubus", "goatman" } },
}
-- levels that keep their vanilla population (lower-case substrings of the level name)
local SKIP_LEVELS = { "minetown", "mages guild", "temple", "sanctum", "boss", "hamlet" }

-- ---------------------------------------------------------------------------
-- settings (Settings > General > MOD SETTINGS). Read when needed, never cached.
-- ---------------------------------------------------------------------------
-- D62 (Apocky, 2026-10-18): no attack cap -- "too artificial". The old 3-attacker token cap stays
-- only as a CONTROL knob for comparisons, off by default, under a new id so no stored value of the
-- old "tokens" setting can switch it back on.
local DEFAULTS = { population = 3, tactics = true, limits = true, token_cap = false, verbs = true }
sam_register_setting("population", { type = "slider", label = "Population (x vanilla)", min = 1, max = 5,
  step = 0.5, default = DEFAULTS.population,
  tip = "Extra packs spawn room by room when a level starts, until it holds this many times its vanilla hostiles. 1 = off." })
sam_register_setting("tactics", { type = "toggle", label = "Pack tactics (rings, flanks, morale)",
  default = DEFAULTS.tactics })
sam_register_setting("limits", { type = "toggle", label = "Natural attack limits (room, rhythm, swing line)",
  default = DEFAULTS.limits,
  tip = "D62: only as many attack you as fit round you, attacks interleave, nobody swings through an ally." })
sam_register_setting("token_cap", { type = "toggle", label = "CONTROL: old 3-attacker cap (dropped by D62)",
  default = DEFAULTS.token_cap,
  tip = "Leave off in play. Only for comparisons: caps melee attackers per player at 3 by clearing targets." })
sam_register_setting("verbs", { type = "toggle", label = "Crowd verbs (sweeps, shoves, stamina)",
  default = DEFAULTS.verbs })
local SETTING_IDS = { "population", "tactics", "limits", "token_cap", "verbs" }
sam_register_action("shove", "Shove / shield bash", "G")
local SHOVE_ACTION = NS .. ":shove"

local override = {}          -- the self-test's values; never written to the settings file
local function cfg(id)
  if override[id] ~= nil then return override[id] end
  local v = sam_get_setting(id)
  if v == nil then return DEFAULTS[id] end
  return v
end

-- ---------------------------------------------------------------------------
-- state
-- ---------------------------------------------------------------------------
local tickN = 0
local L = nil                -- this level: rooms, packs, census; rebuilt on every level
local cs = {}                -- per-creature tactical state, keyed by uid (uids restart per level)
local tok = {}               -- per-player tokens: { holders = {uid = since}, waiting = {uid = slot} }
local sta = {}               -- per-player stamina
local T = nil                -- self-test state (headless test character only)
local trial = nil            -- counters of the self-test arm being measured
local stats = { think = 0, packs = 0, spawned = 0, grants = 0, holds = 0, reacquire = 0,
  ringHolds = 0, tokenTimeouts = 0, flankPlans = 0, flankArrivals = 0, flankTimeouts = 0,
  retreats = 0, regroups = 0, sweeps = 0, sweepHits = 0, sweepDamage = 0, winded = 0,
  shoves = 0, encounters = 0, errors = 0 }
local lastSweep = {}         -- uid -> damage dealt by the most recent sweep (self-test reads it)
local lastShove = {}         -- the creatures the most recent shove pushed (self-test reads it)

-- ---------------------------------------------------------------------------
-- small helpers
-- ---------------------------------------------------------------------------
local function I(x) return math.floor((x or 0) + 0.5) end
local function onoff(b) return b and "on" or "off" end
local function dist(ax, ay, bx, by) local dx, dy = ax - bx, ay - by; return math.sqrt(dx * dx + dy * dy) end
local function wrap(a) return (a + math.pi) % (2 * math.pi) - math.pi end  -- into [-pi, pi)
local function key(x, y) return y * 4096 + x end

local function alive(uid)
  local hp = sam_get_hp(uid)
  return hp ~= nil and hp > 0
end

-- position in tiles with the sub-tile fraction (tile i spans [i, i+1))
local function fpos(uid)
  local x, y = sam_get_position_precise(uid)
  if not x then return nil end
  return x / 16, y / 16
end

-- a tile this creature can stand on at or near (x, y)
local function standable(uid, x, y)
  x, y = math.floor(x), math.floor(y)
  if sam_can_stand(uid, x, y) then return x, y end
  for r = 1, 2 do
    for dx = -r, r do
      for dy = -r, r do
        if (dx == -r or dx == r or dy == -r or dy == r) and sam_can_stand(uid, x + dx, y + dy) then
          return x + dx, y + dy
        end
      end
    end
  end
  return nil
end

local function players()
  local out = {}
  for p = 0, 3 do
    local pu = sam_get_player_uid(p)
    if pu and not sam_is_ghost(p) and alive(pu) then
      local x, y = fpos(pu)
      if x then out[#out + 1] = { p = p, uid = pu, x = x, y = y } end
    end
  end
  return out
end

local function playerByIndex(ps, p)
  for _, pl in ipairs(ps) do if pl.p == p then return pl end end
  return nil
end

local function nearestPlayer(ps, x, y)
  local best, bd = nil, nil
  for _, pl in ipairs(ps) do
    local d = dist(pl.x, pl.y, x, y)
    if not bd or d < bd then best, bd = pl, d end
  end
  return best, bd
end

local function state(uid)
  local c = cs[uid]
  if not c then c = {}; cs[uid] = c end
  return c
end

local function packOf(uid)
  local id = L and L.memberOf[uid]
  return id and L.packs[id] or nil
end

local function encAdd(uid, field, n)
  local pk = packOf(uid)
  if pk and pk.enc then pk.enc[field] = pk.enc[field] + n end
end

local function trialAdd(field, n)
  if trial and trial.measuring then trial[field] = (trial[field] or 0) + n end
end

-- ---------------------------------------------------------------------------
-- crowd-verb geometry (pure, so the self-test can check it without a fight)
-- ---------------------------------------------------------------------------
-- weapon name -> S.A.M damage class, extra targets a sweep may reach, reach in tiles
local function weaponClass(name)
  if not name then return "unarmed", 1, 1.6 end
  local n = string.lower(name)
  if n:find("spear") or n:find("halberd") or n:find("trident") or n:find("lance") or n:find("staff") then
    return "polearm", 2, 2.4
  end
  if n:find("axe") then return "axe", 3, 1.9 end
  if n:find("mace") or n:find("hammer") or n:find("maul") or n:find("flail") or n:find("club") then
    return "mace", 2, 1.8
  end
  if n:find("sword") or n:find("rapier") or n:find("anelace") or n:find("blade") or n:find("dagger") then
    return "sword", 3, 1.9
  end
  return "unarmed", 1, 1.6
end

local function inArc(px, py, face, x, y, reach)
  local d = dist(px, py, x, y)
  if d > reach then return false, d, nil end
  local diff = math.abs(wrap(math.atan(y - py, x - px) - face))
  return diff <= ARC_HALF, d, diff
end

local function sweepAmounts(base, n, cap)
  local out = {}
  for k = 1, math.min(n, cap, #FALLOFF) do out[k] = I(base * FALLOFF[k]) end
  return out
end

-- ---------------------------------------------------------------------------
-- tokens (pure allocation, so the self-test can feed it synthetic crowds)
-- cands: { uid, d, boss } sorted or not; holders: uid -> tick the token was taken
-- returns newHolders (uid -> since), waiting list, granted list, timeouts
-- ---------------------------------------------------------------------------
local function allocate(cands, holders, budget, now)
  table.sort(cands, function(a, b) if a.d ~= b.d then return a.d < b.d end return a.uid < b.uid end)
  local newH, waiting, granted, timeouts = {}, {}, {}, 0
  local others = 0
  for _, c in ipairs(cands) do if not holders[c.uid] then others = others + 1 end end
  local expired = {}
  for _, c in ipairs(cands) do
    if c.boss then newH[c.uid] = holders[c.uid] or now; budget = 0 end
  end
  for _, c in ipairs(cands) do
    if not c.boss and holders[c.uid] then
      if budget > 0 and (now - holders[c.uid] < TOKEN_MAX_TICKS or others == 0) then
        newH[c.uid] = holders[c.uid]; budget = budget - 1
      else
        expired[c.uid] = true; timeouts = timeouts + 1
      end
    end
  end
  for _, c in ipairs(cands) do
    if budget > 0 and not newH[c.uid] and not expired[c.uid] and not c.boss then
      newH[c.uid] = now; budget = budget - 1; granted[#granted + 1] = c
    end
  end
  for _, c in ipairs(cands) do if not newH[c.uid] then waiting[#waiting + 1] = c end end
  return newH, waiting, granted, timeouts
end

-- ring slots 3-5 tiles round (px, py), spread evenly, in the waiting creatures' angular order
local function assignRing(px, py, list, out)
  local n = #list
  if n == 0 then return end
  local sx, sy = 0, 0
  for _, c in ipairs(list) do
    c.a = math.atan(c.y - py, c.x - px)
    sx, sy = sx + math.cos(c.a), sy + math.sin(c.a)
  end
  local base = (sx == 0 and sy == 0) and 0 or math.atan(sy, sx)
  for _, c in ipairs(list) do c.rel = wrap(c.a - base) end
  table.sort(list, function(a, b) return a.rel < b.rel end)
  local spacing = math.min(2 * math.pi / n, 0.9)
  local band = RING_MAX - RING_MIN + 1
  for i, c in ipairs(list) do
    local ang = base + (i - (n + 1) / 2) * spacing
    local r = RING_MIN + ((i - 1) % band)
    out[c.uid] = { a = ang, r = r, x = px + math.cos(ang) * r, y = py + math.sin(ang) * r }
  end
end

local function moraleLoss(size0, isLeader)
  local loss = MORALE_DEATHS / math.max(size0, 1)
  if isLeader then loss = loss + MORALE_LEADER end
  return loss
end

-- ---------------------------------------------------------------------------
-- packs and encounters
-- ---------------------------------------------------------------------------
local function newPack(kind, members, hx, hy, rid, spawned)
  local id = L.nextPack
  L.nextPack = id + 1
  local pk = { id = id, kind = kind, members = members, size0 = #members, alive = #members,
    leader = members[1], hx = hx, hy = hy, room = rid, spawned = spawned, state = "idle",
    morale = MORALE_START, target = nil, enc = nil, retreatCd = 0, retreatAt = 0, lastContact = 0,
    flanker = nil }
  L.packs[id] = pk
  for _, m in ipairs(members) do L.memberOf[m] = id end
  stats.packs = stats.packs + 1
  return pk
end

local function endEncounter(pk, outcome)
  local en = pk.enc
  if not en then return end
  pk.enc = nil
  local dur = (tickN - en.t0) / 50
  local np = 0
  for _ in pairs(en.players) do np = np + 1 end
  local ppm = en.dmg / math.max(np, 1) / (math.max(dur, 5) / 60)
  local ttk = (outcome == "killed" and en.lastDeath) and string.format("%.1f", (en.lastDeath - en.t0) / 50) or "na"
  sam_log(string.format("pack_tactics: ENCOUNTER v=1 floor=%d pack=%d kind=%s size=%d players=%d dur_s=%.1f"
    .. " dmg=%d dmg_ppm=%.1f hits=%d tokens_granted=%d tokens_peak=%d holds=%d reacquire=%d ring_holds=%d"
    .. " flank_arrivals=%d flank_timeouts=%d retreats=%d kills=%d player_deaths=%d ttk_s=%s outcome=%s"
    .. " spawned=%s tactics=%s limits=%s token_cap=%s verbs=%s population=%.1f test=%s"
    .. " releases=%d lim_holds=%d line_blocks=%d swings_hit=%d leaks=%d",
    I(L.floor), pk.id, pk.kind, en.size, np, dur, I(en.dmg), ppm, en.hits, en.grants, en.peak, en.holds,
    en.reacq, en.ring, en.flankArr, en.flankTo, en.retreats, en.kills, en.pdeaths, ttk, outcome,
    tostring(pk.spawned), onoff(cfg("tactics")), onoff(cfg("limits")), onoff(cfg("token_cap")), onoff(cfg("verbs")),
    cfg("population"), tostring(T ~= nil), en.releases or 0, en.limHolds or 0, en.lineBlocks or 0,
    en.swingsHit or 0, en.leaks or 0))
end

local function release(m, pl)
  local c = state(m)
  c.flank = nil
  if pl and (sam_get_monster_target_uid(m) or 0) ~= pl.uid then sam_set_monster_target_uid(m, pl.uid) end
end

local function planFlank(pk, pl, cx, cy)
  local f = nil
  for i = #pk.members, 2, -1 do
    if alive(pk.members[i]) then f = pk.members[i]; break end
  end
  if not f then return end
  local ax, ay = pl.x - cx, pl.y - cy                    -- approach: pack -> player
  local len = math.sqrt(ax * ax + ay * ay)
  if len < 1 then return end
  ax, ay = ax / len, ay / len
  local ex, ey = standable(f, pl.x + ax * FLANK_BEHIND, pl.y + ay * FLANK_BEHIND)
  if not ex then return end
  local mx, my = (cx + pl.x) / 2, (cy + pl.y) / 2
  local first = (pk.id % 2 == 0) and 1 or -1            -- alternate sides between packs
  for _, side in ipairs({ first, -first }) do
    local wx, wy = standable(f, mx - ay * FLANK_SIDE * side, my + ax * FLANK_SIDE * side)
    if wx and sam_tiles_connected(wx, wy, ex, ey) and sam_monster_path_to(f, wx, wy) then
      local c = state(f)
      c.flank, c.wx, c.wy, c.ex, c.ey = "out", wx, wy, ex, ey
      c.flankUntil, c.pathAt = tickN + FLANK_TIMEOUT, tickN + 25
      sam_clear_monster_target(f, true)
      pk.flanker = f
      stats.flankPlans = stats.flankPlans + 1
      if trial then trial.flankPlans = trial.flankPlans + 1 end   -- planned at arm start, before the window
      return
    end
  end
end

local function engage(pk, pl, cx, cy)
  pk.state = "engaged"
  pk.target = pl.p
  pk.lastContact = tickN
  pk.enc = { t0 = tickN, players = { [pl.p] = true }, size = pk.alive, dmg = 0, hits = 0, grants = 0,
    peak = 0, holds = 0, reacq = 0, ring = 0, flankArr = 0, flankTo = 0, retreats = 0, kills = 0,
    pdeaths = 0, lastDeath = nil }
  stats.encounters = stats.encounters + 1
  if cfg("tactics") and pk.alive >= FLANK_MIN_PACK then planFlank(pk, pl, cx, cy) end
end

local function retreat(pk)
  pk.state = "retreat"
  pk.retreatAt = tickN
  pk.retreatCd = tickN + RETREAT_CD
  stats.retreats = stats.retreats + 1
  if pk.enc then pk.enc.retreats = pk.enc.retreats + 1 end
  endEncounter(pk, "broke")
  for _, m in ipairs(pk.members) do
    if alive(m) then
      local c = state(m)
      c.flank = nil
      sam_clear_monster_target(m, true)
      sam_monster_path_to(m, pk.hx, pk.hy)
      c.pathAt = tickN + 25
    end
  end
  sam_log(string.format("pack_tactics: pack %d (%s) breaks at morale %d and falls back to (%d,%d)",
    pk.id, pk.kind, I(pk.morale), pk.hx, pk.hy))
end

local function centroid(pk, except)
  local sx, sy, n = 0, 0, 0
  for _, m in ipairs(pk.members) do
    if m ~= except and alive(m) then
      local x, y = fpos(m)
      if x then sx, sy, n = sx + x, sy + y, n + 1 end
    end
  end
  if n == 0 then return nil end
  return sx / n, sy / n, n
end

local function packThink(pk, ps)
  local cx, cy, n = centroid(pk)
  if not cx then
    endEncounter(pk, "killed")
    pk.state = "dead"
    return
  end
  local pl, d = nearestPlayer(ps, cx, cy)
  local hunting = false
  for _, m in ipairs(pk.members) do
    local t = sam_get_monster_target_uid(m) or 0
    if t ~= 0 then
      for _, q in ipairs(ps) do if q.uid == t then hunting = true end end
    end
  end
  local seen = false
  if pl and d <= ENGAGE_TRIGGER then
    seen = sam_line_of_sight(math.floor(cx), math.floor(cy), math.floor(pl.x), math.floor(pl.y)) and true or false
  end
  local contact = hunting or seen or (pl ~= nil and d <= 3)
  if contact then pk.lastContact = tickN end

  if pk.state == "idle" then
    if contact and pl then engage(pk, pl, cx, cy) end
  elseif pk.state == "engaged" then
    if pl then
      pk.target = pl.p
      if pk.enc then pk.enc.players[pl.p] = true end
    end
    if cfg("tactics") and pk.size0 >= 2 and pk.morale < MORALE_BREAK and tickN >= pk.retreatCd then
      retreat(pk)
    elseif tickN - pk.lastContact > DISENGAGE_TICKS then
      endEncounter(pk, "disengaged")
      pk.state = "idle"
    end
  elseif pk.state == "retreat" then
    if dist(cx, cy, pk.hx + 0.5, pk.hy + 0.5) <= 3 or tickN - pk.retreatAt > RETREAT_MAX then
      pk.state = "regroup"
      stats.regroups = stats.regroups + 1
    end
  elseif pk.state == "regroup" then
    pk.morale = math.min(MORALE_START, pk.morale + REGROUP_RATE * PACK_TICKS)
    if tickN % 50 < PACK_TICKS then
      for _, m in ipairs(pk.members) do if alive(m) then sam_heal(m, 1) end end
    end
    if pk.morale >= MORALE_REGROUP or (pl and d <= 4) then pk.state = "idle" end
  end
end

local function memberThink(m, pk, ps)
  stats.think = stats.think + 1
  local c = state(m)
  if pk.state == "retreat" then
    if tickN >= (c.pathAt or 0) then
      c.pathAt = tickN + 25
      sam_monster_path_to(m, pk.hx, pk.hy)
    end
    return
  end
  if c.flank == "out" or c.flank == "in" then
    local pl = playerByIndex(ps, pk.target)
    if not pl then c.flank = nil; return end
    local x, y = fpos(m)
    if not x then return end
    if tickN > c.flankUntil then
      stats.flankTimeouts = stats.flankTimeouts + 1
      if pk.enc then pk.enc.flankTo = pk.enc.flankTo + 1 end
      release(m, pl)
      return
    end
    if c.flank == "out" and dist(x, y, c.wx + 0.5, c.wy + 0.5) <= 2 then c.flank = "in"; c.pathAt = 0 end
    if c.flank == "in" and (dist(x, y, c.ex + 0.5, c.ey + 0.5) <= 2 or dist(x, y, pl.x, pl.y) <= 2.5) then
      -- arrived: behind or beside if the angle pack-player-flanker is at least 90 degrees
      local ox, oy = centroid(pk, m)
      if ox then
        local a1 = math.atan(y - pl.y, x - pl.x)
        local a2 = math.atan(oy - pl.y, ox - pl.x)
        if math.abs(wrap(a1 - a2)) >= math.pi / 2 then
          stats.flankArrivals = stats.flankArrivals + 1
          if trial then trial.flankArrivals = trial.flankArrivals + 1 end
          if pk.enc then pk.enc.flankArr = pk.enc.flankArr + 1 end
        end
      end
      release(m, pl)
      return
    end
    if tickN >= (c.pathAt or 0) then
      c.pathAt = tickN + 25
      if c.flank == "out" then sam_monster_path_to(m, c.wx, c.wy) else sam_monster_path_to(m, c.ex, c.ey) end
    end
  end
end

-- ---------------------------------------------------------------------------
-- tokens: allocation every TOKEN_TICKS, the hold enforced every tick
-- ---------------------------------------------------------------------------
local function grant(c, pl)
  stats.grants = stats.grants + 1
  encAdd(c.uid, "grants", 1)
  trialAdd("grants", 1)
  if (sam_get_monster_target_uid(c.uid) or 0) ~= pl.uid then sam_set_monster_target_uid(c.uid, pl.uid) end
  local s = cs[c.uid]
  if s then s.cleared, s.holding, s.reached = nil, nil, nil end
end

local function tokenPass(ps)
  for _, pl in ipairs(ps) do
    local t = tok[pl.p]
    if not t then t = { holders = {}, waiting = {} }; tok[pl.p] = t end
    local cands = {}
    for _, m in ipairs(sam_find_entities(math.floor(pl.x), math.floor(pl.y), ENGAGE_R, "monster") or {}) do
      if alive(m) and sam_is_enemy(m, pl.uid) then
        local kind = sam_get_monster_type(m) or "?"
        local pk = packOf(m)
        local c = cs[m]
        local skip = NO_TOKEN[kind] or (pk and pk.state == "retreat") or (c and (c.flank == "out" or c.flank == "in"))
        if not skip then
          local tg = sam_get_monster_target_uid(m) or 0
          local mine = t.holders[m] or t.waiting[m]
          local x, y = fpos(m)
          if x and (tg == pl.uid or mine) then
            cands[#cands + 1] = { uid = m, d = dist(x, y, pl.x, pl.y), x = x, y = y, boss = BOSS[kind] }
          end
        end
      end
    end
    local newH, waiting, granted, timeouts = allocate(cands, t.holders, TOKENS_MELEE, tickN)
    stats.tokenTimeouts = stats.tokenTimeouts + timeouts
    for _, c in ipairs(granted) do grant(c, pl) end
    local newW = {}
    assignRing(pl.x, pl.y, waiting, newW)
    t.holders, t.waiting = newH, newW
    -- peak simultaneous holders per encounter
    local perPack = {}
    for m in pairs(newH) do
      local id = L.memberOf[m]
      if id then perPack[id] = (perPack[id] or 0) + 1 end
    end
    for id, n in pairs(perPack) do
      local pk = L.packs[id]
      if pk and pk.enc and n > pk.enc.peak then pk.enc.peak = n end
    end
  end
end

local function enforce(ps)
  for _, pl in ipairs(ps) do
    local t = tok[pl.p]
    if t then
      for m, slot in pairs(t.waiting) do
        local x, y = fpos(m)
        if x and alive(m) then
          local c = state(m)
          if dist(x, y, pl.x, pl.y) <= HOLD_R then
            if (sam_get_monster_target_uid(m) or 0) == pl.uid then
              if c.cleared then
                stats.reacquire = stats.reacquire + 1
                encAdd(m, "reacq", 1)
                trialAdd("reacq", 1)
              end
              if sam_clear_monster_target(m, true) then
                stats.holds = stats.holds + 1
                encAdd(m, "holds", 1)
                trialAdd("holds", 1)
                c.cleared = true
              end
            end
            c.holding = true
            if tickN >= (c.pathAt or 0) then
              c.pathAt = tickN + SLOT_REPATH
              local sx, sy = standable(m, slot.x, slot.y)
              if sx then sam_monster_path_to(m, sx, sy) end
            end
          elseif c.holding and not c.reached and dist(x, y, slot.x, slot.y) <= SLOT_REACHED then
            c.reached = true
            stats.ringHolds = stats.ringHolds + 1
            encAdd(m, "ring", 1)
            trialAdd("ring", 1)
          end
        end
      end
    end
  end
end

-- pack tactics: flankers on their detour and retreating packs must not turn and charge
local function steer()
  for _, pk in pairs(L.packs) do
    if pk.state == "retreat" then
      for _, m in ipairs(pk.members) do
        if (sam_get_monster_target_uid(m) or 0) ~= 0 then sam_clear_monster_target(m, true) end
      end
    elseif pk.flanker then
      local c = cs[pk.flanker]
      if c and (c.flank == "out" or c.flank == "in") and (sam_get_monster_target_uid(pk.flanker) or 0) ~= 0 then
        sam_clear_monster_target(pk.flanker, true)
      end
    end
  end
end

-- ---------------------------------------------------------------------------
-- natural attack limits (D62, Apocky 2026-10-18: no attack cap, "too artificial"). All creatures
-- in reach may attack; what limits them is physical:
--   ROOM    only as many attack one player as fit round it: slots on a ring at contact
--           distance, each as wide as the attacker's body plus swing room, counted only where the
--           floor is open (a corridor fits fewer).
--   RHYTHM  attacks interleave: on one player, releases are at least STAGGER_GAP ticks apart; a
--           released creature swings after the engine's own wind-up (monsterHitTime climbs from
--           HITRATE/2 = 22 ticks to HITRATE = 45, about 0.46 s), then recovers, held, for RECOVERY.
--   LINE    a creature does not swing while another body stands in its swing line.
-- SUBSTITUTE: the fork gates the swing decision in handleMonsterAttack. From S.A.M the swing is
-- held with EFF_PACIFY, which pins monsterHitTime at HITRATE/2 (no swing) and makes the creature
-- back away (shouldRetreat) while it keeps its target; the engine also shows its pacify sparkle.
-- Releasing removes the effect. Applying it writes one line to sam_log.txt, so a hold is applied
-- once and lifted by removal, never refreshed every tick.
-- ---------------------------------------------------------------------------
local LIM_TICKS   = 2        -- scheduler pass per player
local REACH_R     = 1.5      -- STRIKERANGE 24 px: a monster's melee reach (actmonster.cpp)
local MANAGE_R    = 2.75     -- a creature hunting a player this close is managed (held until released)
local KEEP_R      = 5.0      -- a managed creature stays managed out to here (held ones back away)
local ELBOW       = 0.25     -- lateral room a swing needs beyond the body, tiles (ASSUMED)
local RING_GAP    = 0.1      -- space between bodies on the attack ring, tiles (ASSUMED)
local STAGGER_GAP = 12       -- ticks between two releases on one player: 0.24 s (ASSUMED)
local REACH_WAIT  = 100      -- a released creature that never comes within reach gives its turn up
-- A humanoid's swing lands in two steps (entity.cpp attack(): a WINDUP pose first, then the
-- animation calls attack() again after ANIMATE_DURATION_WINDUP = 9 ticks): from release that is
-- 23 ticks of timer in reach + 9 of wind-up. Holding it after the swing is committed does not stop
-- the blow (S.A.M session 32: 3 such leaks), so the miss timeout and the line re-hold respect it.
local SWING_WAIT  = 48       -- ticks in reach with no hit: taken as a swing that missed
local COMMIT_AT   = 18       -- after this many ticks in reach the swing may be committed: no re-hold
local LATE_TICKS  = 12       -- a blow this soon after a hold was committed before it (wind-up 9 + 3)
local RECOVERY    = 25       -- ticks held after a swing: 0.5 s (ASSUMED)
local HOLD_TICKS  = 300      -- PACIFY length per application; lifted early by removal
local SWING_HALF  = 0.15     -- the swing line's half-width beyond a blocking body, tiles (ASSUMED)
local BURST_TICKS = 5        -- two hits on one player this close count as a burst

local lim = {}               -- uid -> { p, mode = queued|released|recover, since, at, recoverEnd, ours }
local limP = {}              -- player index -> { lastRelease, lastHit, cap }
local bodyCache = {}         -- uid -> body half-size in tiles

local function bodyR(uid)
  local r = bodyCache[uid]
  if r then return r end
  local sx, sy = sam_get_entity_size(uid)
  r = math.max(sx or 4, sy or 4) / 16
  bodyCache[uid] = r
  return r
end

-- attack slots round a target at (px, py) of body radius rp, for attackers of radius r: a ring at
-- contact distance cut into body-plus-elbow arcs, keeping the slots that stand on open floor
local function ringSlots(px, py, rp, r, open)
  local R = rp + r + RING_GAP
  local n = math.max(1, math.floor(2 * math.pi * R / (2 * r + ELBOW)))
  local out = {}
  for i = 0, n - 1 do
    local a = i * 2 * math.pi / n
    local x, y = px + math.cos(a) * R, py + math.sin(a) * R
    if not open or open(math.floor(x), math.floor(y)) then out[#out + 1] = a end
  end
  return out, n
end

-- is another body in the swing line from (ax, ay) to the target at (px, py)?
local function swingBlocked(ax, ay, px, py, others, self)
  local dx, dy = px - ax, py - ay
  local len2 = dx * dx + dy * dy
  if len2 < 1e-6 then return false end
  local len = math.sqrt(len2)
  for _, o in ipairs(others) do
    if o.uid ~= self then
      local t = ((o.x - ax) * dx + (o.y - ay) * dy) / len2
      if t > 0 and t < 1 and math.abs((o.x - ax) * dy - (o.y - ay) * dx) / len < o.r + SWING_HALF then
        return true
      end
    end
  end
  return false
end

local function limStat(field, n, uid)
  stats[field] = (stats[field] or 0) + n
  trialAdd(field, n)
  local pk = uid and packOf(uid)
  if pk and pk.enc then pk.enc[field] = (pk.enc[field] or 0) + n end
end

local function limHold(uid, c, mode)
  if c.mode == "released" or c.mode == "new" then c.heldAt = tickN end   -- a blow already wound up may still land
  if not c.ours then
    if not sam_apply_monster_effect(uid, "PACIFY", HOLD_TICKS) then return false end   -- immune
    c.ours = true
    limStat("limHolds", 1, uid)
  end
  c.mode = mode
  return true
end

local function limDrop(uid, c)
  if c.ours and alive(uid) then sam_remove_monster_effect(uid, "PACIFY") end
  lim[uid] = nil
end

local function limClear()
  for uid, c in pairs(lim) do limDrop(uid, c) end
  lim, limP = {}, {}
end

local function openTile(x, y) return not L or L.phase == "scan" or L.open[key(x, y)] == true end

local function limitsPass(ps)
  -- a player who died, became a ghost or left releases everything held for them
  local present = {}
  for _, pl in ipairs(ps) do present[pl.p] = pl.uid end
  for uid, c in pairs(lim) do
    if not present[c.p] then limDrop(uid, c) end
  end
  for _, pl in ipairs(ps) do
    local P = limP[pl.p]
    if not P then P = { lastRelease = -1000, lastHit = -1000, cap = 0 }; limP[pl.p] = P end
    local rp = bodyR(pl.uid)
    local bodies, mine = {}, {}
    for _, m in ipairs(sam_find_entities(math.floor(pl.x), math.floor(pl.y), KEEP_R + 1, "monster") or {}) do
      local x, y = fpos(m)
      if x and alive(m) then
        local d = dist(x, y, pl.x, pl.y)
        bodies[#bodies + 1] = { uid = m, x = x, y = y, r = bodyR(m) }
        local c = lim[m]
        local kind = sam_get_monster_type(m) or "?"
        local pk = packOf(m)
        local skip = NO_TOKEN[kind] or BOSS[kind] or (pk and pk.state == "retreat")
          or (cs[m] and (cs[m].flank == "out" or cs[m].flank == "in"))
        if c and c.p == pl.p then
          local tg = sam_get_monster_target_uid(m) or 0
          local other = false
          for p2, u2 in pairs(present) do if p2 ~= pl.p and u2 == tg then other = true end end
          if skip or d > KEEP_R or other then      -- out of range, or now hunting another player
            limDrop(m, c)
          else
            mine[#mine + 1] = { uid = m, c = c, x = x, y = y, d = d }
          end
        elseif not c and not skip and d <= MANAGE_R and sam_is_enemy(m, pl.uid)
            and (sam_get_monster_target_uid(m) or 0) == pl.uid and not sam_monster_has_effect(m, "PACIFY") then
          c = { p = pl.p, mode = "new", since = tickN }
          lim[m] = c
          mine[#mine + 1] = { uid = m, c = c, x = x, y = y, d = d }
        end
      end
    end
    -- released creatures: did they swing (hit seen, or in reach long enough), or is an ally now
    -- in their swing line? Held ones: is the hold still on (it expires after HOLD_TICKS, and
    -- nothing else should lift it, but a silent loss would let a held creature swing)?
    local released, rAvg, rN = 0, 0, 0
    for _, e in ipairs(mine) do
      local c = e.c
      rAvg, rN = rAvg + bodyR(e.uid), rN + 1
      if c.ours and not sam_monster_has_effect(e.uid, "PACIFY") then
        c.ours = false
        limStat("reholds", 1, e.uid)
        if not limHold(e.uid, c, c.mode) then limDrop(e.uid, c) end
      end
      if not lim[e.uid] then
        -- dropped just above (immune)
      elseif c.mode == "released" then
        if e.d <= REACH_R and not c.inReach then c.inReach = tickN end
        if c.hitAt and c.hitAt >= c.at then
          limStat("swingsHit", 1, e.uid)
          if limHold(e.uid, c, "recover") then c.recoverEnd = tickN + RECOVERY else limDrop(e.uid, c) end
        elseif (c.inReach and tickN - c.inReach > SWING_WAIT) or (not c.inReach and tickN - c.at > REACH_WAIT) then
          limStat("swingsMissed", 1, e.uid)
          if limHold(e.uid, c, "recover") then c.recoverEnd = tickN + RECOVERY else limDrop(e.uid, c) end
        elseif (not c.inReach or tickN - c.inReach < COMMIT_AT) and swingBlocked(e.x, e.y, pl.x, pl.y, bodies, e.uid) then
          limStat("lineBlocks", 1, e.uid)
          if limHold(e.uid, c, "queued") then c.since = tickN else limDrop(e.uid, c) end
        else
          released = released + 1
        end
      elseif c.mode == "recover" and tickN >= c.recoverEnd then
        c.mode, c.since = "queued", tickN
      end
    end
    -- room: the ring's open slots for this player's attackers
    local slots = ringSlots(pl.x, pl.y, rp, (rN > 0) and (rAvg / rN) or 0.25, openTile)
    P.cap = #slots
    trialAdd("capSum", #slots)
    trialAdd("capSamples", 1)
    if trial and released > trial.relPeak then trial.relPeak = released end
    -- rhythm and line: release the creature that has waited longest (a held creature backs away,
    -- so distance beyond reach counts against it, 15 ticks a tile, but never shuts it out), if
    -- the ring has room, the last release on this player was long enough ago, and its swing line
    -- is clear
    local best, bestKey = nil, nil
    if released < #slots and tickN - P.lastRelease >= STAGGER_GAP then
      for _, e in ipairs(mine) do
        local c = e.c
        if c.mode == "queued" or c.mode == "new" then
          local k = c.since + 15 * math.max(0, e.d - REACH_R)
          if (not bestKey or k < bestKey) and not swingBlocked(e.x, e.y, pl.x, pl.y, bodies, e.uid) then
            best, bestKey = e, k
          end
        end
      end
    end
    for _, e in ipairs(mine) do
      local c = e.c
      if e == best then
        if c.ours then sam_remove_monster_effect(e.uid, "PACIFY"); c.ours = false end
        c.mode, c.at, c.inReach, c.hitAt = "released", tickN, nil, nil
        P.lastRelease = tickN
        limStat("releases", 1, e.uid)
      elseif c.mode == "new" then
        if not limHold(e.uid, c, "queued") then limDrop(e.uid, c) end
      end
    end
  end
end

-- on_damage_multiplier: a hit is being resolved; monster -> player hits feed the limits
local function onHit(e)
  local att, tgt = e.attacker_uid, e.target_uid
  if not att or att == 0 or not tgt then return end
  local pidx = nil
  for p = 0, 3 do if sam_get_player_uid(p) == tgt then pidx = p end end
  if not pidx then return end
  limStat("multHits", 1)
  -- bursts are counted with the limits off too (the comparison needs both sides)
  local P = limP[pidx]
  if not P then P = { lastRelease = -1000, lastHit = -1000, cap = 0 }; limP[pidx] = P end
  if tickN - P.lastHit <= BURST_TICKS then limStat("bursts", 1) end
  P.lastHit = tickN
  local c = lim[att]
  if c then
    c.hitAt = tickN
    if c.mode ~= "released" and c.heldAt and tickN - c.heldAt <= LATE_TICKS then
      limStat("lateBlows", 1, att)     -- decided while released; the wind-up carried it past the hold
    elseif c.mode ~= "released" then
      limStat("leaks", 1, att)
      -- a held creature hit: say how, so the hole in the hold can be found
      stats.leakLogs = (stats.leakLogs or 0) + 1
      if stats.leakLogs <= 12 then
        local x, y = fpos(att)
        local pu = sam_get_player_uid(pidx)
        local px, py = fpos(pu)
        sam_log(string.format("pack_tactics: LEAK uid=%d kind=%s mode=%s ours=%s pacify_now=%s mode_age=%d released_age=%s"
          .. " recover_left=%s dist=%.2f damage_type=%s projectile=%s spell=%s mult=%s",
          att, tostring(sam_get_monster_type(att)), tostring(c.mode), tostring(c.ours),
          tostring(sam_monster_has_effect(att, "PACIFY")), tickN - (c.since or tickN),
          c.at and tostring(tickN - c.at) or "never", c.recoverEnd and tostring(c.recoverEnd - tickN) or "na",
          (x and px) and dist(x, y, px, py) or -1, tostring(e.damage_type), tostring(e.projectile_uid),
          tostring(e.spell_id), tostring(e.multiplier_x1000)))
      end
    end
  end
end

-- ---------------------------------------------------------------------------
-- crowd verbs: sweeps, shoves, stamina
-- ---------------------------------------------------------------------------
local function stamina(p)
  local s = sta[p]
  if not s then s = { cur = STAMINA_MAX, last = -1000, shown = nil, warnAt = 0 }; sta[p] = s end
  return s
end

local function spend(p, cost)
  local s = stamina(p)
  if s.cur < cost then return false end
  s.cur = s.cur - cost
  s.last = tickN
  return true
end

local function winded(p, what)
  stats.winded = stats.winded + 1
  local s = stamina(p)
  if tickN >= s.warnAt then
    s.warnAt = tickN + 100
    sam_message(p, "Too winded to " .. what .. ".")
  end
end

local function drawStamina(p, s)
  local v = I(s.cur)
  if v == s.shown then return end
  s.shown = v
  -- D68: a yellow bar like health (red) and mana (blue); dimmer yellow when too winded to sweep
  local color = (v >= SWEEP_COST) and 0xF0C828FF or 0x8C7414FF
  sam_hud_bar("stamina", HUD_X, HUD_Y, HUD_W, HUD_H, s.cur / STAMINA_MAX, color, p)
  sam_hud_text("stamina_txt", HUD_X + 4, HUD_Y + 1, string.format("%d/%d", v, STAMINA_MAX), 0x1E1E1EFF, p)
end

local function verbsTick(ps)
  if not cfg("verbs") then
    for p, s in pairs(sta) do
      if s.shown then sam_hud_clear("stamina", p); sam_hud_clear("stamina_txt", p); s.shown = nil end
    end
    return
  end
  for _, pl in ipairs(ps) do
    local s = stamina(pl.p)
    if tickN - s.last >= REGEN_DELAY and s.cur < STAMINA_MAX then
      s.cur = math.min(STAMINA_MAX, s.cur + REGEN_PER_TICK)
    end
    if tickN % 10 == 0 or s.shown == nil then drawStamina(pl.p, s) end
  end
end

-- player.on_before_hit: the blow has connected and its damage is decided.
-- faceOverride is for the self-test only (a script cannot turn a player).
local function onBeforeHit(e, faceOverride)
  if not cfg("verbs") then return end
  local p, primary, base = e.player, e.target_uid, e.damage
  if p == nil or not base or base <= 0 then return end
  if not primary or sam_get_hp(primary) == nil then return end   -- a door or a chest, not a creature
  local pu = sam_get_player_uid(p)
  if not pu then return end
  local px, py = fpos(pu)
  local face = faceOverride or sam_get_facing(p)
  if not px or not face then return end
  local dtype, cap, reach = weaponClass(sam_get_equipped_item(p, "WEAPON"))
  local hits = {}
  for _, m in ipairs(sam_find_entities(math.floor(px), math.floor(py), reach + 1, "monster") or {}) do
    if m ~= primary and alive(m) and sam_is_enemy(pu, m) then
      local x, y = fpos(m)
      if x then
        local ok, d, diff = inArc(px, py, face, x, y, reach)
        if ok then hits[#hits + 1] = { uid = m, d = d, diff = diff } end
      end
    end
  end
  if #hits == 0 then return end
  if not spend(p, SWEEP_COST) then winded(p, "sweep"); return end
  table.sort(hits, function(a, b) if a.diff ~= b.diff then return a.diff < b.diff end return a.uid < b.uid end)
  lastSweep = {}
  local amounts = sweepAmounts(base, #hits, cap)
  for k, amt in ipairs(amounts) do
    if amt >= 1 then
      local dealt = sam_deal_damage_typed(hits[k].uid, amt, dtype) or 0
      lastSweep[hits[k].uid] = dealt
      if dealt > 0 then
        sam_damage_number(hits[k].uid, dealt)
        stats.sweepHits = stats.sweepHits + 1
        stats.sweepDamage = stats.sweepDamage + dealt
      end
    end
  end
  stats.sweeps = stats.sweeps + 1
end

-- returns the number of creatures the shove reached (faceOverride: self-test only)
local function shove(p, faceOverride)
  if not cfg("verbs") or sam_is_ghost(p) then return 0 end
  local pu = sam_get_player_uid(p)
  if not pu then return 0 end
  local px, py = fpos(pu)
  local face = faceOverride or sam_get_facing(p)
  if not px or not face then return 0 end
  local shield = sam_get_equipped_item(p, "SHIELD")
  local cost = shield and BASH_COST or KICK_COST
  local force = shield and BASH_FORCE or KICK_FORCE
  local targets = {}
  for _, m in ipairs(sam_find_entities(math.floor(px), math.floor(py), SHOVE_R + 1, "monster") or {}) do
    if alive(m) and sam_is_enemy(pu, m) then
      local x, y = fpos(m)
      if x then
        local ok, d, diff = inArc(px, py, face, x, y, SHOVE_R)
        if ok then targets[#targets + 1] = { uid = m, x = x, y = y, diff = diff } end
      end
    end
  end
  if #targets == 0 then return 0 end
  if not spend(p, cost) then winded(p, shield and "bash" or "kick"); return 0 end
  table.sort(targets, function(a, b) return a.diff < b.diff end)
  local n = 0
  lastShove = {}
  for i = 1, math.min(3, #targets) do
    local c = targets[i]
    local ang = math.atan(c.y - py, c.x - px)
    if sam_apply_force(c.uid, force, ang, STAGGER_TICKS) then
      n = n + 1
      lastShove[#lastShove + 1] = { uid = c.uid, d0 = dist(c.x, c.y, px, py), ang = ang, x = c.x, y = c.y }
    end
  end
  stats.shoves = stats.shoves + n
  return n
end

-- ---------------------------------------------------------------------------
-- the level: incremental room analysis, census, population
-- ---------------------------------------------------------------------------
local SCAN_ROWS, CORE_ROWS, FILL_BUDGET = 3, 6, 1200

local function levelKey()
  return tostring(sam_get_floor()) .. ":" .. tostring(sam_get_map_seed())
end

local function newLevel(k)
  local x1, y1, x2, y2 = sam_get_playable_bounds()
  local info = sam_get_level_info() or {}
  L = { key = k, floor = sam_get_floor() or 0, name = tostring(info.name or "?"), t0 = tickN,
    x1 = x1 or 1, y1 = y1 or 1, x2 = x2 or 64, y2 = y2 or 64, phase = "scan", row = y1 or 1,
    open = {}, nOpen = 0, coreList = {}, core = {}, roomOf = {}, rooms = {}, fillIdx = 1,
    queue = nil, packs = {}, memberOf = {}, nextPack = 1, vanilla = 0, kinds = {},
    spawnQueue = {}, spawnTarget = 0, spawnedCount = 0, used = {}, note = nil }
  cs, tok = {}, {}
  lim, limP, bodyCache = {}, {}, {}             -- uids restart on every level
  sam_log(string.format("pack_tactics: level %s floor=%d name=%s bounds=(%d,%d)-(%d,%d) -- PROTOTYPE",
    k, I(L.floor), L.name, L.x1, L.y1, L.x2, L.y2))
  sam_log("pack_tactics: SUBSTITUTE natural limits (D62) = a swing is held with EFF_PACIFY (pins the attack timer at"
    .. " 22 of 45 ticks; held creatures back away; engine pacify sparkle; one log line per hold), not the fork's"
    .. " gate in handleMonsterAttack; a blow wound up before a hold still lands (counted as late_blows)")
  sam_log("pack_tactics: CONTROL token cap (off unless set) = target clearing every tick plus sam_monster_path_to"
    .. " to a ring slot; lossy: the engine re-acquires a visible target")
  sam_log("pack_tactics: SUBSTITUTE flank route = side-waypoint detour (S.A.M has no penalised second A*);"
    .. " retreat = target clearing while walking home; sweep damage has no attacker (no XP, no aggro)")
end

local function finishRoom(rm)
  rm.cx, rm.cy = rm.sx / rm.n, rm.sy / rm.n
  local bk, bd = nil, nil
  for _, k in ipairs(rm.tiles) do
    local d = dist(k % 4096, k // 4096, rm.cx, rm.cy)
    if not bd or d < bd then bk, bd = k, d end
  end
  rm.ax, rm.ay = bk % 4096, bk // 4096
  rm.small = rm.n < 6
  -- spawn order: nearest the anchor first (squared distances precomputed, so the sort is cheap)
  local d2 = {}
  for _, k in ipairs(rm.tiles) do
    local dx, dy = k % 4096 - rm.ax, k // 4096 - rm.ay
    d2[k] = dx * dx + dy * dy
  end
  table.sort(rm.tiles, function(a, b) if d2[a] ~= d2[b] then return d2[a] < d2[b] end return a < b end)
end

-- the room at or near tile (x, y), searching a square ring out to r
local function roomNear(x, y, r)
  local id = L.roomOf[key(x, y)]
  if id and not L.rooms[id].small then return id end
  for rr = 1, r do
    for dx = -rr, rr do
      for dy = -rr, rr do
        local id2 = L.roomOf[key(x + dx, y + dy)]
        if id2 and not L.rooms[id2].small then return id2 end
      end
    end
  end
  return nil
end

local function skipLevel(name)
  local n = string.lower(name or "")
  for _, s in ipairs(SKIP_LEVELS) do if n:find(s, 1, true) then return true end end
  return false
end

local function pickKind()
  if #L.kinds > 0 then return L.kinds[sam_random("pack_tactics:kind", 1, #L.kinds)] end
  for _, row in ipairs(FALLBACK) do
    if L.floor <= row[1] then return row[2][sam_random("pack_tactics:kind", 1, #row[2])] end
  end
  return "skeleton"
end

local function census(ps)
  local ref = ps[1] and ps[1].uid
  local groups, order = {}, {}
  local cx, cy = (L.x1 + L.x2) // 2, (L.y1 + L.y2) // 2
  local r = math.max(L.x2 - L.x1, L.y2 - L.y1)
  for _, m in ipairs(sam_find_entities(cx, cy, r, "monster") or {}) do
    if alive(m) and ref and sam_is_enemy(m, ref) and not L.memberOf[m] then
      local kind = sam_get_monster_type(m) or "?"
      L.vanilla = L.vanilla + 1
      if not NO_PACK[kind] then
        L.kinds[#L.kinds + 1] = kind
        local x, y = sam_get_position(m)
        if x then
          local rid = roomNear(x, y, 4)
          local gk = rid and ("r" .. rid) or ("m" .. m)
          if not groups[gk] then
            groups[gk] = { rid = rid, x = x, y = y, list = {}, kind = kind }
            order[#order + 1] = gk
          end
          table.insert(groups[gk].list, m)
        end
      end
    end
  end
  table.sort(order)
  local adopted = 0
  for _, gk in ipairs(order) do
    local g = groups[gk]
    table.sort(g.list)
    local hx, hy = g.x, g.y
    if g.rid then hx, hy = L.rooms[g.rid].ax, L.rooms[g.rid].ay end
    local i = 1
    while i <= #g.list do                     -- packs of at most 6
      local part = {}
      for j = i, math.min(i + 5, #g.list) do part[#part + 1] = g.list[j] end
      newPack(sam_get_monster_type(part[1]) or g.kind, part, hx, hy, g.rid, false)
      adopted = adopted + #part
      i = i + 6
    end
  end
  local big = 0
  for _, rm in ipairs(L.rooms) do if not rm.small then big = big + 1 end end
  sam_log(string.format("pack_tactics: census floor=%d open=%d interior=%d rooms=%d vanilla_hostiles=%d adopted=%d packs=%d",
    I(L.floor), L.nOpen, #L.coreList, big, L.vanilla, adopted, L.nextPack - 1))
end

-- plan extra packs for a population multiple; returns nil or the reason nothing is planned
local function planPopulation(mult, ps)
  L.spawnQueue = {}
  local target = I((mult - 1) * L.vanilla)
  L.spawnTarget = target
  if target <= 0 then return "population x" .. tostring(mult) .. " with " .. L.vanilla .. " vanilla hostiles" end
  if skipLevel(L.name) then return "special level " .. L.name end
  local rooms = {}
  for _, rm in ipairs(L.rooms) do
    if not rm.small then
      local far = true
      for _, pl in ipairs(ps) do
        if dist(rm.ax + 0.5, rm.ay + 0.5, pl.x, pl.y) < SAFE_R then far = false end
      end
      if far then rooms[#rooms + 1] = rm end
    end
  end
  if #rooms == 0 then return "no room at least " .. SAFE_R .. " tiles from the players" end
  for i = #rooms, 2, -1 do                     -- a deterministic shuffle (same seed, same rooms)
    local j = sam_random("pack_tactics:rooms", 1, i)
    rooms[i], rooms[j] = rooms[j], rooms[i]
  end
  local planned = 0
  for round = 1, MAX_PACKS_ROOM do
    for _, rm in ipairs(rooms) do
      if planned < target and rm.n >= round * TILES_PER_PACK then
        local size = math.min(sam_random("pack_tactics:size", PACK_MIN, PACK_MAX), target - planned)
        L.spawnQueue[#L.spawnQueue + 1] = { room = rm, kind = pickKind(), size = size }
        planned = planned + size
      end
    end
  end
  return nil
end

local function spawnPlanned(plan, ps)
  local rm = plan.room
  local members = {}
  for _, k in ipairs(rm.tiles) do
    if #members >= plan.size then break end
    if not L.used[k] then
      L.used[k] = true
      local x, y = k % 4096, k // 4096
      local near = false
      for _, pl in ipairs(ps) do if dist(x + 0.5, y + 0.5, pl.x, pl.y) < SAFE_R then near = true end end
      if not near and sam_is_spawnable(x, y) then
        local uid = sam_spawn_monster(x, y, plan.kind)
        if uid then members[#members + 1] = uid end
      end
    end
  end
  if #members == 0 then return nil end
  local pk = newPack(plan.kind, members, rm.ax, rm.ay, rm.id, true)
  L.spawnedCount = L.spawnedCount + #members
  stats.spawned = stats.spawned + #members
  sam_log(string.format("pack_tactics: spawned pack %d kind=%s size=%d room=%d home=(%d,%d)",
    pk.id, plan.kind, #members, rm.id, rm.ax, rm.ay))
  return pk
end

local function analyse(ps)
  if L.phase == "scan" then
    for _ = 1, SCAN_ROWS do
      local y = L.row
      if y >= L.y2 then L.phase = "core"; L.row = L.y1; return end
      for x = L.x1, L.x2 - 1 do
        local t = sam_get_tile(x, y)
        if t and t.walkable and not t.lava and not t.water then
          L.open[key(x, y)] = true
          L.nOpen = L.nOpen + 1
        end
      end
      L.row = y + 1
    end
  elseif L.phase == "core" then
    for _ = 1, CORE_ROWS do
      local y = L.row
      if y >= L.y2 then L.phase = "rooms"; return end
      for x = L.x1, L.x2 - 1 do
        if L.open[key(x, y)] then
          local all = true
          for dy = -1, 1 do
            for dx = -1, 1 do
              if not L.open[key(x + dx, y + dy)] then all = false end
            end
          end
          if all then
            local k = key(x, y)
            L.core[k] = true
            L.coreList[#L.coreList + 1] = k
          end
        end
      end
      L.row = y + 1
    end
  elseif L.phase == "rooms" then
    local budget = FILL_BUDGET
    while budget > 0 do
      local q = L.queue
      if q then
        if q.head > #q.items then
          finishRoom(q.room)
          L.queue = nil
          return                                -- at most one room sorted per tick
        else
          local k = q.items[q.head]
          q.head = q.head + 1
          budget = budget - 1
          local x, y = k % 4096, k // 4096
          local rm = q.room
          rm.n, rm.sx, rm.sy = rm.n + 1, rm.sx + x, rm.sy + y
          rm.tiles[#rm.tiles + 1] = k
          for _, nk in ipairs({ key(x + 1, y), key(x - 1, y), key(x, y + 1), key(x, y - 1) }) do
            if L.core[nk] and not L.roomOf[nk] then
              L.roomOf[nk] = rm.id
              q.items[#q.items + 1] = nk
            end
          end
        end
      else
        local k = L.coreList[L.fillIdx]
        if not k then L.phase = "census"; return end
        L.fillIdx = L.fillIdx + 1
        budget = budget - 1
        if not L.roomOf[k] then
          local rm = { id = #L.rooms + 1, n = 0, sx = 0, sy = 0, tiles = {} }
          L.rooms[rm.id] = rm
          L.roomOf[k] = rm.id
          L.queue = { items = { k }, head = 1, room = rm }
        end
      end
    end
  elseif L.phase == "census" then
    if tickN - L.t0 < 100 or #ps == 0 then return end   -- let game start (and the test gate) settle
    census(ps)
    L.phase = "populate"
    if T then
      L.note = "self-test: the test populates by hand"
    else
      L.note = planPopulation(cfg("population"), ps)
    end
    sam_log(string.format("pack_tactics: population x%.1f target_extra=%d planned_packs=%d%s",
      cfg("population"), L.spawnTarget, #L.spawnQueue, L.note and (" -- none: " .. L.note) or ""))
  elseif L.phase == "populate" then
    local plan = table.remove(L.spawnQueue, 1)   -- one pack per tick
    if plan then
      spawnPlanned(plan, ps)
    else
      L.phase = "live"
      sam_log(string.format("pack_tactics: floor %d live: vanilla=%d spawned=%d (x%.2f) packs=%d",
        I(L.floor), L.vanilla, L.spawnedCount, (L.vanilla + L.spawnedCount) / math.max(L.vanilla, 1), L.nextPack - 1))
    end
  end
end

local function endLevel(outcome)
  if not L then return end
  for _, pk in pairs(L.packs) do endEncounter(pk, outcome) end
end

local function logStats()
  local alivePacks = 0
  if L then for _, pk in pairs(L.packs) do if pk.state ~= "dead" then alivePacks = alivePacks + 1 end end end
  local held = 0
  for _, c in pairs(lim) do if c.ours then held = held + 1 end end
  sam_log(string.format("pack_tactics: stats packs=%d alive_packs=%d spawned=%d think=%d releases=%d lim_holds=%d"
    .. " held_now=%d line_blocks=%d swings_hit=%d swings_missed=%d leaks=%d bursts=%d mult_hits=%d"
    .. " cap_grants=%d cap_holds=%d flank_plans=%d flank_arrivals=%d flank_timeouts=%d retreats=%d regroups=%d"
    .. " sweeps=%d sweep_hits=%d sweep_dmg=%d winded=%d shoves=%d encounters=%d errors=%d",
    stats.packs, alivePacks, stats.spawned, stats.think, stats.releases or 0, stats.limHolds or 0, held,
    stats.lineBlocks or 0, stats.swingsHit or 0, stats.swingsMissed or 0, stats.leaks or 0, stats.bursts or 0,
    stats.multHits or 0, stats.grants, stats.holds, stats.flankPlans, stats.flankArrivals, stats.flankTimeouts,
    stats.retreats, stats.regroups, stats.sweeps, stats.sweepHits, I(stats.sweepDamage), stats.winded,
    stats.shoves, stats.encounters, stats.errors))
end

-- ---------------------------------------------------------------------------
-- Self-test:  barony.exe -samtest=pack_tactics -samtesttimeout=480
-- Runs ONLY on S.A.M's headless test character: human barbarian, raw INT -2, under 500 ticks
-- played, AND the run seed S.A.M's test runner uses (a new human barbarian in a real game also
-- has INT -2, so the seed is what tells the two apart). Everything it spawns is removed, the
-- player's HP is checked unchanged, and the settings file is never written (overrides only).
--   1. API presence and the pure rules: arc, falloff, weapon classes, rings, token allocation
--      (closest win, hysteresis, timeout pass-on, bosses), morale.
--   2. Room analysis of the real floor; the density spawner at 3x (counts, distances), removed.
--   3. Sweep and shove on rats around the player, against the same geometry; stamina spend,
--      refusal when winded, regeneration; the HUD bar is showing.
--   4. The trial: 4 x 3 = 12 sword-armed skeletons in 3 packs around the passive test character,
--      ABAB with tokens off / on, 20 s measured per arm. Incoming damage is counted and negated
--      (on_before_damage), so the character never dies. CONTROL: tokens off must take more damage.
-- ---------------------------------------------------------------------------
local TRIAL_POP, TRIAL_KIND, TRIAL_WEAPON = 3, "skeleton", "bronze_sword"
local ARM_SETTLE, ARM_TICKS = 100, 1000
local ARMS = { false, true, false, true, false, true, false, true }   -- natural limits off/on, 4 pairs

local function check(name, ok, saw)
  if ok then T.pass = T.pass + 1 else T.fail = T.fail + 1 end
  sam_log((ok and "PASS  " or "FAIL  ") .. "pack_tactics [" .. name .. "]   (" .. tostring(saw) .. ")")
end

local function removeAll(list)
  for _, m in ipairs(list) do
    if L then
      local id = L.memberOf[m]
      if id and L.packs[id] then L.packs[id] = nil end
      L.memberOf[m] = nil
    end
    cs[m] = nil
    for _, t in pairs(tok) do t.holders[m] = nil; t.waiting[m] = nil end
    sam_remove_entity(m)
  end
end

local function allGone(list)
  for _, m in ipairs(list) do if sam_get_hp(m) ~= nil then return false end end
  return true
end

-- open, spawnable tiles in an annulus round the player, connected to the player's tile
local function ringTiles(pl, rMin, rMax)
  local px, py = math.floor(pl.x), math.floor(pl.y)
  local out = {}
  local R = math.ceil(rMax)                      -- integer steps: a fractional bound gives fractional tiles
  for dy = -R, R do
    for dx = -R, R do
      local x, y = px + dx, py + dy
      local d = dist(x + 0.5, y + 0.5, pl.x, pl.y)
      if d >= rMin and d <= rMax and L.open[key(x, y)] and sam_is_spawnable(x, y)
         and sam_tiles_connected(px, py, x, y) then
        out[#out + 1] = { x = x, y = y, a = math.atan(y + 0.5 - pl.y, x + 0.5 - pl.x) }
      end
    end
  end
  table.sort(out, function(a, b) return a.a < b.a end)
  return out
end

local TESTS = {}

TESTS[#TESTS + 1] = function(ps)        -- 1. API and pure rules
  -- the limits hold creatures with PACIFY, and held creatures back away: keep them off until the
  -- trial sets them per arm, so the sweep and shove checks see only the verbs
  override.limits = false
  local needed = { "sam_find_entities", "sam_get_position_precise", "sam_get_position", "sam_get_facing",
    "sam_get_monster_target_uid", "sam_set_monster_target_uid", "sam_clear_monster_target",
    "sam_monster_path_to", "sam_can_stand", "sam_is_spawnable", "sam_tiles_connected", "sam_get_tile",
    "sam_get_playable_bounds", "sam_get_level_info", "sam_get_map_seed", "sam_get_seed", "sam_spawn_monster",
    "sam_remove_entity", "sam_monster_equip", "sam_remove_monster_effect", "sam_deal_damage_typed",
    "sam_damage_number", "sam_apply_force", "sam_get_equipped_item", "sam_hud_bar", "sam_hud_text",
    "sam_hud_clear", "sam_register_setting", "sam_get_setting", "sam_list_settings", "sam_register_action",
    "sam_modify_damage", "sam_heal", "sam_is_enemy", "sam_get_monster_type", "sam_line_of_sight",
    "sam_random", "sam_get_hp", "sam_set_monster_stat", "sam_get_time_played", "sam_set_position",
    "sam_apply_monster_effect", "sam_monster_has_effect", "sam_get_entity_size", "sam_test_done" }
  local missing = {}
  for _, fn in ipairs(needed) do if _G[fn] == nil then missing[#missing + 1] = fn end end
  check("api: " .. #needed .. " functions present", #missing == 0, #missing == 0 and "all" or table.concat(missing, ","))
  local ids = {}
  for _, s in ipairs(sam_list_settings() or {}) do ids[#ids + 1] = tostring(s.id) .. "=" .. tostring(s.value) end
  check("settings registered", #ids == #SETTING_IDS, table.concat(ids, " "))
  check("D62: the old token cap is off by default and in force", DEFAULTS.token_cap == false
    and sam_get_setting("token_cap") == false, tostring(sam_get_setting("token_cap")))
  local pop = sam_get_setting("population")
  check("population setting is a number in 1..5", type(pop) == "number" and pop >= 1 and pop <= 5, pop)
  check("wrap(3pi/2) = -pi/2", math.abs(wrap(1.5 * math.pi) + 0.5 * math.pi) < 1e-9, wrap(1.5 * math.pi))
  local in40 = inArc(0, 0, 0, math.cos(math.rad(40)), math.sin(math.rad(40)), 1.9)
  local in60 = inArc(0, 0, 0, math.cos(math.rad(60)), math.sin(math.rad(60)), 1.9)
  local far = inArc(0, 0, 0, 2.5, 0, 1.9)
  check("arc: 40 deg in, 60 deg out, beyond reach out", in40 and not in60 and not far,
    tostring(in40) .. "/" .. tostring(in60) .. "/" .. tostring(far))
  local a = sweepAmounts(20, 5, 3)
  check("falloff 20 -> 12, 9, 6 (cap 3)", #a == 3 and a[1] == 12 and a[2] == 9 and a[3] == 6, table.concat(a, ","))
  local w = {}
  for _, n in ipairs({ "iron_spear", "bronze_sword", "steel_greataxe", "tool_pickaxe", "shillelagh_mace", "quarterstaff" }) do
    w[#w + 1] = (weaponClass(n))
  end
  w[#w + 1] = (weaponClass(nil))
  check("weapon classes", table.concat(w, ",") == "polearm,sword,axe,axe,mace,polearm,unarmed", table.concat(w, ","))
  -- rings: 6 waiting creatures in a tight clump get 6 slots 3-5 tiles out, at least 1.5 tiles apart
  local list, out = {}, {}
  for i = 1, 6 do list[i] = { uid = 100 + i, x = 10 + 1.5 + i * 0.1, y = 10 + 0.2 * i } end
  assignRing(10, 10, list, out)
  local okR, minSep, n = true, 99, 0
  local slots = {}
  for _, s in pairs(out) do
    n = n + 1
    local r = dist(s.x, s.y, 10, 10)
    if r < RING_MIN - 1e-6 or r > RING_MAX + 1e-6 then okR = false end
    slots[#slots + 1] = s
  end
  for i = 1, #slots do
    for j = i + 1, #slots do minSep = math.min(minSep, dist(slots[i].x, slots[i].y, slots[j].x, slots[j].y)) end
  end
  check("ring: 6 slots, radius 3-5, spread >= 1.5 tiles", n == 6 and okR and minSep >= 1.5, string.format("n=%d sep=%.2f", n, minSep))
  -- tokens
  local function cands(spec) local c = {}; for i, d in ipairs(spec) do c[i] = { uid = i, d = d } end; return c end
  local h1, w1, g1 = allocate(cands({ 5, 1, 7, 2, 3, 9, 4, 6 }), {}, 3, 1000)
  check("tokens: 8 creatures -> the 3 closest attack, 5 wait", h1[2] and h1[4] and h1[5] and #w1 == 5 and #g1 == 3,
    string.format("holders %s,%s,%s waiting %d", tostring(h1[2] ~= nil), tostring(h1[4] ~= nil), tostring(h1[5] ~= nil), #w1))
  local h2 = allocate(cands({ 5, 1, 7, 2 }), { [3] = 990 }, 3, 1000)
  check("tokens: a holder keeps its token though others are closer (hysteresis)", h2[3] ~= nil and h2[2] ~= nil and h2[4] ~= nil and h2[1] == nil,
    tostring(h2[3] ~= nil))
  local h3, w3, g3, to3 = allocate(cands({ 5, 1, 7, 2 }), { [3] = 1000 - TOKEN_MAX_TICKS }, 3, 1000)
  check("tokens: a timed-out holder passes its token on", h3[3] == nil and to3 == 1 and h3[1] ~= nil, "timeouts=" .. to3)
  local cb = cands({ 5, 1, 7, 2 }); cb[1].boss = true
  local h4, w4 = allocate(cb, {}, 3, 1000)
  check("tokens: a boss holds every token", h4[1] ~= nil and #w4 == 3, "waiting " .. #w4)
  local leaderAnd1 = MORALE_START - moraleLoss(4, true) - moraleLoss(4, false)
  local one = MORALE_START - moraleLoss(4, false)
  check("morale: a pack of 4 breaks on leader + 1 lost, holds on 1 lost", leaderAnd1 < MORALE_BREAK and one >= MORALE_BREAK,
    string.format("%.1f / %.1f", leaderAnd1, one))
  -- natural limits (D62): room by body size, and the swing line
  local openAll = function() return true end
  local openCorridor = function(x, y) return y == 10 end   -- a 1-tile corridor along x
  local sOpen = ringSlots(10.5, 10.5, 0.25, 0.25, openAll)
  local sBig = ringSlots(10.5, 10.5, 0.25, 0.44, openAll)
  local sCorr = ringSlots(10.5, 10.5, 0.25, 0.25, openCorridor)
  check("room: 5 skeleton-sized attackers fit round a player in the open, fewer large ones, fewer in a corridor",
    #sOpen == 5 and #sBig < #sOpen and #sCorr < #sOpen and #sCorr >= 2,
    string.format("open %d, large %d, corridor %d", #sOpen, #sBig, #sCorr))
  local ally = { { uid = 7, x = 0.7, y = 0.1, r = 0.25 } }
  local beside = { { uid = 7, x = 0.0, y = 0.5, r = 0.25 } }
  local behind = { { uid = 7, x = 2.0, y = 0.0, r = 0.25 } }
  check("swing line: an ally between attacker and target blocks; one beside or behind does not",
    swingBlocked(0, 0, 1.4, 0, ally, 1) and not swingBlocked(0, 0, 1.4, 0, beside, 1)
      and not swingBlocked(0, 0, 1.4, 0, behind, 1) and not swingBlocked(0, 0, 1.4, 0, ally, 7),
    "between / beside / behind / self")
  return 1
end

TESTS[#TESTS + 1] = function(ps)        -- 2. room analysis of the real floor
  -- a new game opens on the Start Map (floor 0, no vanilla hostiles); run with -samtestfloor=1
  -- and the runner takes the stairs after game start, so wait up to 20 s for a dungeon floor
  if L.floor < 1 and tickN - T.stepStart < 1000 then return "again" end
  if L.phase ~= "live" and L.phase ~= "populate" then return "again" end
  local big = 0
  for _, rm in ipairs(L.rooms) do if not rm.small then big = big + 1 end end
  local pl = ps[1]
  local onOpen = pl and L.open[key(math.floor(pl.x), math.floor(pl.y))] or false
  check("floor scanned: open tiles found", L.nOpen > 0, L.nOpen)
  check("rooms found", big >= 1, string.format("%d rooms, %d interior tiles, floor %d, %s", big, #L.coreList, I(L.floor), L.name))
  check("the player stands on an open tile", onOpen == true, pl and string.format("(%.1f,%.1f)", pl.x, pl.y) or "no player")
  -- every vanilla hostile that may form packs was adopted into exactly one pack
  local packable, adopted, twice = 0, 0, 0
  for _, m in ipairs(sam_find_entities(math.floor(pl.x), math.floor(pl.y), 200, "monster") or {}) do
    if alive(m) and sam_is_enemy(m, pl.uid) and not NO_PACK[sam_get_monster_type(m) or "?"] then packable = packable + 1 end
  end
  local seen = {}
  for _, pk in pairs(L.packs) do
    if not pk.spawned then
      for _, m in ipairs(pk.members) do
        if seen[m] then twice = twice + 1 end
        seen[m] = true
        if alive(m) then adopted = adopted + 1 end
      end
    end
  end
  check("census: every packable vanilla hostile is in one pack", adopted == packable and twice == 0,
    string.format("packable %d, adopted %d, in two packs %d, vanilla hostiles %d", packable, adopted, twice, L.vanilla))
  check("sam_get_facing reads the player's yaw in [0, 2pi)", type(sam_get_facing(0)) == "number"
    and sam_get_facing(0) >= 0 and sam_get_facing(0) < 2 * math.pi, sam_get_facing(0))
  return 1
end

TESTS[#TESTS + 1] = function(ps)        -- 3. density spawner at 3x
  local why = planPopulation(TRIAL_POP, ps)
  local planned = 0
  for _, plan in ipairs(L.spawnQueue) do planned = planned + plan.size end
  check("3x plan: (3 - 1) x vanilla extra creatures planned", why == nil and planned == L.spawnTarget and L.spawnTarget == 2 * L.vanilla,
    string.format("vanilla=%d target=%d planned=%d packs=%d%s", L.vanilla, L.spawnTarget, planned, #L.spawnQueue, why and (" why=" .. why) or ""))
  T.density = {}
  local rooms, nearest = {}, 1e9
  while #L.spawnQueue > 0 do
    local pk = spawnPlanned(table.remove(L.spawnQueue, 1), ps)
    if pk then
      rooms[pk.room] = true
      for _, m in ipairs(pk.members) do
        T.density[#T.density + 1] = m
        local x, y = fpos(m)
        if x then nearest = math.min(nearest, dist(x, y, ps[1].x, ps[1].y)) end
      end
    end
  end
  local nRooms = 0
  for _ in pairs(rooms) do nRooms = nRooms + 1 end
  check("3x spawn: at least 90 percent of the plan placed", #T.density >= math.floor(0.9 * planned) and #T.density > 0,
    string.format("%d of %d in %d rooms", #T.density, planned, nRooms))
  check("3x spawn: nothing within " .. SAFE_R .. " tiles of the player", nearest >= SAFE_R - 0.75, string.format("nearest %.1f", nearest))
  local hostile = 0
  for _, m in ipairs(T.density) do if alive(m) and sam_is_enemy(m, ps[1].uid) then hostile = hostile + 1 end end
  check("3x spawn: every spawned creature is alive and hostile", hostile == #T.density, hostile .. "/" .. #T.density)
  removeAll(T.density)
  return 10
end

TESTS[#TESTS + 1] = function(ps)        -- 3b. the density spawns are gone
  check("3x spawn removed again", allGone(T.density), #T.density .. " removed")
  return 1
end

TESTS[#TESTS + 1] = function(ps)        -- 4. verbs need room: go to the most open interior tile
  -- a shove can only show its push where there is floor behind the creature, and the stairs a
  -- floor opens on are often in a corridor, so the verbs are tested on the interior tile with the
  -- most open floor within 3 tiles; the player is put back before the trial
  local pl = ps[1]
  T.home = { x = math.floor(pl.x), y = math.floor(pl.y) }
  -- keep away from the floor's own monsters, so none is woken and follows the player to the trial
  local foes = {}
  local r = math.max(L.x2 - L.x1, L.y2 - L.y1)
  for _, m in ipairs(sam_find_entities((L.x1 + L.x2) // 2, (L.y1 + L.y2) // 2, r, "monster") or {}) do
    if alive(m) and sam_is_enemy(m, pl.uid) then
      local x, y = fpos(m)
      if x then foes[#foes + 1] = { x = x, y = y } end
    end
  end
  local best, bestN, quiet = nil, -1, false
  for _, rm in ipairs(L.rooms) do
    if not rm.small then
      for _, k in ipairs(rm.tiles) do
        local x, y = k % 4096, k // 4096
        local n = 0
        for dy = -3, 3 do
          for dx = -3, 3 do
            if L.open[key(x + dx, y + dy)] then n = n + 1 end
          end
        end
        local calm = true
        for _, f in ipairs(foes) do if dist(f.x, f.y, x + 0.5, y + 0.5) < 10 then calm = false; break end end
        -- a calm tile beats any tile near monsters; among equals, more open floor wins
        if (calm and not quiet) or (calm == quiet and n > bestN) then best, bestN, quiet = k, n, calm end
      end
    end
  end
  T.arena = best and { x = best % 4096, y = best // 4096, open = bestN, calm = quiet } or nil
  if T.arena and (T.arena.x ~= T.home.x or T.arena.y ~= T.home.y) then
    T.moved = sam_set_position(pl.uid, T.arena.x, T.arena.y)
  end
  return 3
end

TESTS[#TESTS + 1] = function(ps)        -- 4a. sweep and shove setup: rats round the player
  local pl = ps[1]
  if T.arena then
    local d = dist(pl.x, pl.y, T.arena.x + 0.5, T.arena.y + 0.5)
    check("verbs arena: the player stands on the most open interior tile", d <= 1,
      string.format("(%d,%d), %d of 49 tiles open within 3, %s, %.2f tiles away, home (%d,%d)",
        T.arena.x, T.arena.y, T.arena.open, T.arena.calm and "no monster within 10" or "monsters near",
        d, T.home.x, T.home.y))
  end
  T.rats = {}
  local tiles = ringTiles(pl, 0.8, 1.7)
  -- a script cannot turn a player, so the test faces the most open direction itself and hands
  -- that facing to the same sweep and shove code a real blow runs through
  -- neighbour tiles lie about 45 degrees apart, so a front window of 47 degrees (inside the 50
  -- degree half-arc) holds a tile and both of its neighbours; the hit itself re-measures the arc
  local FRONT, SIDE = math.rad(47), math.rad(80)
  local bestA, bestN = 0, -1
  for _, a in ipairs(tiles) do
    local n, s = 0, 0
    for _, b in ipairs(tiles) do
      local diff = math.abs(wrap(b.a - a.a))
      if diff <= FRONT then n = n + 1 elseif diff >= SIDE then s = s + 1 end
    end
    if s >= 1 and n > bestN then bestA, bestN = a.a, n end
  end
  T.face = bestA % (2 * math.pi)
  local front, side = {}, {}
  for _, t in ipairs(tiles) do
    local diff = math.abs(wrap(t.a - T.face))
    if diff <= FRONT then front[#front + 1] = t elseif diff >= SIDE then side[#side + 1] = t end
  end
  local pick = {}
  for i = 1, math.min(4, #front) do pick[#pick + 1] = front[i] end
  for i = 1, math.min(2, #side) do pick[#pick + 1] = side[i] end
  for _, t in ipairs(pick) do
    local uid = sam_spawn_monster(t.x, t.y, "rat")
    if uid then
      sam_set_monster_stat(uid, "MAXHP", 500)
      sam_set_monster_stat(uid, "HP", 500)
      T.rats[#T.rats + 1] = uid
    end
  end
  check("rats placed: at least 2 in front, 1 beside or behind", #front >= 2 and #side >= 1 and #T.rats >= 3,
    string.format("%d rats, %d front tiles, %d side tiles, facing %.2f", #T.rats, #front, #side, T.face))
  return 1
end

TESTS[#TESTS + 1] = function(ps)        -- 4b. one synthetic blow, then the same blow winded
  local pl = ps[1]
  local s = stamina(0)
  s.cur = STAMINA_MAX
  local face = T.face
  local dtype, cap, reach = weaponClass(sam_get_equipped_item(0, "WEAPON"))
  -- every hostile the sweep can see (rats, and any floor monster that wandered in), measured the
  -- way the handler measures, so the expectation is exact
  local all, inside, hpBefore = {}, {}, {}
  for _, m in ipairs(sam_find_entities(math.floor(pl.x), math.floor(pl.y), reach + 1, "monster") or {}) do
    if alive(m) and sam_is_enemy(pl.uid, m) then
      local x, y = fpos(m)
      if x then
        all[#all + 1] = m
        hpBefore[m] = sam_get_hp(m)
        local ok, d, diff = inArc(pl.x, pl.y, face, x, y, reach)
        if ok then inside[#inside + 1] = { uid = m, diff = diff } end
      end
    end
  end
  table.sort(inside, function(a, b) if a.diff ~= b.diff then return a.diff < b.diff end return a.uid < b.uid end)
  -- the primary is the creature closest to the facing if one is inside, else any rat
  local primary = (#inside > 0) and table.remove(inside, 1).uid or T.rats[1]
  onBeforeHit({ name = "player.on_before_hit", player = 0, attacker_uid = pl.uid, target_uid = primary,
    damage = 20, weapon_type = 0, charge = 0, max_charge = 30, fully_charged = 0 }, face)
  local expectHit = math.min(#inside, cap)
  local expected = {}
  for k = 1, expectHit do expected[inside[k].uid] = true end
  local okIn, okOut, total, nOut = true, true, 0, 0
  for _, m in ipairs(all) do
    local lost = (hpBefore[m] or 0) - (sam_get_hp(m) or 0)
    if expected[m] then
      if lost <= 0 or lost ~= (lastSweep[m] or -1) then okIn = false end
      total = total + lost
    else
      nOut = nOut + 1
      if lost ~= 0 then okOut = false end
    end
  end
  check(string.format("sweep (%s, cap %d): the %d other creatures in the arc lose what the sweep dealt", dtype, cap, expectHit),
    okIn and expectHit >= 1, string.format("in-arc besides the primary %d, damage %d", #inside, total))
  check("sweep: the primary, creatures outside the arc and beyond the cap are untouched", okOut,
    string.format("%d untouched as expected", nOut))
  if expectHit > 0 then
    check("sweep spends " .. SWEEP_COST .. " stamina", I(stamina(0).cur) == STAMINA_MAX - SWEEP_COST, I(stamina(0).cur))
  else
    check("sweep with nothing in the arc spends no stamina", I(stamina(0).cur) == STAMINA_MAX, I(stamina(0).cur))
  end
  -- CONTROL: winded, the same blow sweeps nothing
  s.cur, s.last = SWEEP_COST - 1, tickN
  local w0 = stats.winded
  for _, m in ipairs(all) do hpBefore[m] = sam_get_hp(m) end
  onBeforeHit({ name = "player.on_before_hit", player = 0, attacker_uid = pl.uid, target_uid = primary,
    damage = 20, weapon_type = 0, charge = 0, max_charge = 30, fully_charged = 0 }, face)
  local changed = 0
  for _, m in ipairs(all) do if hpBefore[m] ~= sam_get_hp(m) then changed = changed + 1 end end
  check("control: winded (stamina 9) the blow sweeps nobody", changed == 0 and (expectHit == 0 or stats.winded == w0 + 1),
    string.format("changed %d, winded +%d", changed, stats.winded - w0))
  -- shove: face the nearest rat first so it is inside the arc
  s.cur, s.last = STAMINA_MAX, tickN
  T.shoved = {}
  local nearest, nd = nil, nil
  for _, m in ipairs(T.rats) do
    local x, y = fpos(m)
    if x then
      local ok, d = inArc(pl.x, pl.y, face, x, y, SHOVE_R)
      if ok and (not nd or d < nd) then nearest, nd = m, d end
    end
  end
  T.shoveTarget, T.shoveD = nearest, nd
  local n = shove(0, face)
  T.shoveN = n
  check("shove reaches the rats in front", nearest == nil or n >= 1, string.format("reached %d, nearest in arc %s", n, tostring(nearest)))
  if nearest then
    local spent = STAMINA_MAX - I(stamina(0).cur)
    local shield = sam_get_equipped_item(0, "SHIELD")
    check("shove spends " .. (shield and BASH_COST or KICK_COST) .. " stamina", spent == (shield and BASH_COST or KICK_COST), spent)
  end
  -- watch every pushed rat for 15 ticks; a rat with a wall right behind it cannot show the push,
  -- so the push path (two tiles beyond it, away from the player) is recorded
  T.shoveWatch = {}
  for _, s2 in ipairs(lastShove) do
    local clear = true
    for k = 1, 2 do
      local tx, ty = math.floor(s2.x + math.cos(s2.ang) * k), math.floor(s2.y + math.sin(s2.ang) * k)
      if not L.open[key(tx, ty)] then clear = false end
    end
    T.shoveWatch[#T.shoveWatch + 1] = { uid = s2.uid, d0 = s2.d0, maxD = s2.d0, clear = clear }
  end
  T.shoveUntil = tickN + 15
  return 1
end

TESTS[#TESTS + 1] = function(ps)        -- 4c. the pushed rats moved away (peak over 15 ticks)
  local pl = ps[1]
  for _, w in ipairs(T.shoveWatch or {}) do
    local x, y = fpos(w.uid)
    if x then w.maxD = math.max(w.maxD, dist(x, y, pl.x, pl.y)) end
  end
  if tickN < T.shoveUntil then return "again" end
  if T.shoveTarget then
    local best, bestClear, nClear, desc = 0, 0, 0, {}
    for _, w in ipairs(T.shoveWatch or {}) do
      local gain = w.maxD - w.d0
      best = math.max(best, gain)
      if w.clear then nClear = nClear + 1; bestClear = math.max(bestClear, gain) end
      desc[#desc + 1] = string.format("%.2f->%.2f%s", w.d0, w.maxD, w.clear and "" or "(wall)")
    end
    local gain = (nClear > 0) and bestClear or best
    check("shove pushes a rat at least 0.25 tiles farther (peak in 15 ticks)", gain >= 0.25,
      string.format("%s; open push paths %d", table.concat(desc, " "), nClear))
  end
  local s = stamina(0)
  s.cur, s.last = 50, tickN - REGEN_DELAY
  T.regenFrom = s.cur
  return 50
end

TESTS[#TESTS + 1] = function(ps)        -- 4d. regeneration and the HUD
  local s = stamina(0)
  local gained = s.cur - T.regenFrom
  check("stamina regenerates about 15 per second", gained >= 12 and gained <= 16, string.format("+%.1f in 50 ticks", gained))
  s.shown = nil
  drawStamina(0, s)
  check("stamina HUD bar is showing", sam_hud_clear("stamina", 0) == true, "hud_clear returned true")
  s.shown = nil
  removeAll(T.rats)
  return 10
end

TESTS[#TESTS + 1] = function(ps)        -- 4e. rats gone; back to the tile the floor opened on
  check("rats removed again", allGone(T.rats), #T.rats .. " removed")
  if T.moved then sam_set_position(ps[1].uid, T.home.x, T.home.y) end
  T.arm = 0
  return 3
end

TESTS[#TESTS + 1] = function(ps)        -- 4f. the player is home again (the trial runs there)
  if T.moved then
    local d = dist(ps[1].x, ps[1].y, T.home.x + 0.5, T.home.y + 0.5)
    check("player put back on the tile the floor opened on", d <= 1, string.format("%.2f tiles away", d))
  end
  return 1
end

local function armStart(ps)
  T.arm = T.arm + 1
  local limitsOn = ARMS[T.arm]
  override.limits = limitsOn
  override.token_cap = false
  override.tactics = true
  tok = {}
  limClear()
  trial = { arm = T.arm, limits = limitsOn, measuring = false, dmg = 0, hits = 0, flankPlans = 0,
    flankArrivals = 0, contactSum = 0, contactPeak = 0, samples = 0, nearSum = 0, lostSum = 0,
    foreignPeak = 0, creatures = 0, releases = 0, limHolds = 0, lineBlocks = 0, swingsHit = 0,
    swingsMissed = 0, leaks = 0, lateBlows = 0, bursts = 0, multHits = 0, capSum = 0, capSamples = 0,
    relPeak = 0, inputs = 0, attacks = 0 }
  local pl = ps[1]
  local tiles = ringTiles(pl, 4, 7)
  local want = 4 * TRIAL_POP
  T.trialUids = {}
  local placed = {}
  if #tiles > 0 then
    for i = 1, want do
      local t = tiles[1 + math.floor((i - 1) * #tiles / want)]
      if t and not placed[key(t.x, t.y)] then
        placed[key(t.x, t.y)] = true
        local uid = sam_spawn_monster(t.x, t.y, TRIAL_KIND)
        if uid then
          sam_monster_equip(uid, "weapon", TRIAL_WEAPON)
          sam_remove_monster_effect(uid, "ASLEEP")
          T.trialUids[#T.trialUids + 1] = { uid = uid, x = t.x, y = t.y }
        end
      end
    end
  end
  -- three packs of four, by angle, each engaged on the player at once
  local list = {}
  local i = 1
  while i <= #T.trialUids do
    local members, hx, hy = {}, 0, 0
    for j = i, math.min(i + 3, #T.trialUids) do
      local u = T.trialUids[j]
      members[#members + 1] = u.uid
      hx, hy = hx + u.x, hy + u.y
      list[#list + 1] = u.uid
    end
    local pk = newPack(TRIAL_KIND, members, math.floor(hx / #members), math.floor(hy / #members), nil, true)
    for _, m in ipairs(members) do sam_set_monster_target_uid(m, pl.uid) end
    local cx, cy = centroid(pk)
    if cx then engage(pk, pl, cx, cy) end
    i = i + 4
  end
  T.trialList = list
  T.trialSet = {}
  for _, m in ipairs(list) do T.trialSet[m] = true end
  trial.creatures = #list
  return ARM_SETTLE
end

local function armMeasure(ps)
  trial.measuring = true
  trial.t0 = tickN
  return ARM_TICKS
end

local function armEnd(ps)
  trial.measuring = false
  local secs = (tickN - trial.t0) / 50
  trial.secs = secs
  local capMean = trial.capSum / math.max(trial.capSamples, 1)
  sam_log(string.format("pack_tactics: TRIAL v=2 arm=%d limits=%s token_cap=off creatures=%d population=%.1f"
    .. " window_s=%.1f dmg=%d dmg_pm=%.1f hits=%d mult_hits=%d bursts=%d releases=%d lim_holds=%d line_blocks=%d"
    .. " swings_hit=%d swings_missed=%d leaks=%d late_blows=%d cap_mean=%.2f rel_peak=%d contact_mean=%.2f contact_peak=%d"
    .. " near_mean=%.2f lost_mean=%.2f flank_plans=%d flank_arrivals=%d foreign_peak=%d player_input=%d"
    .. " player_attacks=%d kind=%s weapon=%s floor=%d seed=%d",
    trial.arm, onoff(trial.limits), trial.creatures, TRIAL_POP, secs, I(trial.dmg), trial.dmg / (secs / 60), trial.hits,
    trial.multHits, trial.bursts, trial.releases, trial.limHolds, trial.lineBlocks, trial.swingsHit, trial.swingsMissed,
    trial.leaks, trial.lateBlows, capMean, trial.relPeak, trial.contactSum / math.max(trial.samples, 1), trial.contactPeak,
    trial.nearSum / math.max(trial.samples, 1), trial.lostSum / math.max(trial.samples, 1),
    trial.flankPlans, trial.flankArrivals, trial.foreignPeak, trial.inputs, trial.attacks, TRIAL_KIND, TRIAL_WEAPON,
    I(L.floor), I(sam_get_map_seed())))
  T.results = T.results or {}
  T.results[#T.results + 1] = { on = trial.limits, dmg = trial.dmg, secs = secs, creatures = trial.creatures,
    hits = trial.hits, multHits = trial.multHits, bursts = trial.bursts, leaks = trial.leaks,
    relPeak = trial.relPeak, capMean = capMean, releases = trial.releases, inputs = trial.inputs,
    attacks = trial.attacks }
  for _, pk in pairs(L.packs) do if pk.spawned and pk.enc then endEncounter(pk, "trial_end") end end
  limClear()
  removeAll(T.trialList)
  T.allTrial = T.allTrial or {}
  for _, m in ipairs(T.trialList) do T.allTrial[#T.allTrial + 1] = m end
  trial = nil
  override.limits, override.token_cap = nil, nil
  tok = {}
  return 25
end

for _ = 1, #ARMS do
  TESTS[#TESTS + 1] = armStart
  TESTS[#TESTS + 1] = armMeasure
  TESTS[#TESTS + 1] = armEnd
end

TESTS[#TESTS + 1] = function(ps)        -- 5. verdict of the trial and the restore checks
  local off, on, offS, onS, n = 0, 0, 0, 0, 0
  local burstsOff, burstsOn, hitsOff, hitsOn, leaks, relPeak, capMin = 0, 0, 0, 0, 0, 0, 1e9
  for _, r in ipairs(T.results or {}) do
    n = math.max(n, r.creatures)
    if r.on then
      on, onS = on + r.dmg, onS + r.secs
      burstsOn, hitsOn = burstsOn + r.bursts, hitsOn + r.multHits
      leaks, relPeak = leaks + r.leaks, math.max(relPeak, r.relPeak)
      capMin = math.min(capMin, r.capMean)
    else
      off, offS = off + r.dmg, offS + r.secs
      burstsOff, hitsOff = burstsOff + r.bursts, hitsOff + r.multHits
    end
  end
  local pmOff, pmOn = off / math.max(offS / 60, 1e-9), on / math.max(onS / 60, 1e-9)
  local ratio = (on > 0) and (off / on) or 0
  sam_log(string.format("pack_tactics: TRIALSUM v=2 mode=limits population=%.1f creatures=%d arms=%d dmg_off=%d dmg_on=%d"
    .. " dmg_pm_off=%.1f dmg_pm_on=%.1f ratio=%.2f a2_plus50=%s control_more_damage=%s burst_share_off=%.3f"
    .. " burst_share_on=%.3f leaks=%d rel_peak=%d",
    TRIAL_POP, n, #(T.results or {}), I(off), I(on), pmOff, pmOn, ratio, tostring(off >= 1.5 * on and on > 0),
    tostring(off > on), burstsOff / math.max(hitsOff, 1), burstsOn / math.max(hitsOn, 1), leaks, relPeak))
  check("trial ran " .. #ARMS .. " arms with creatures", #(T.results or {}) == #ARMS and n > 0,
    string.format("%d arms, %d creatures", #(T.results or {}), n))
  -- a measurement is only clean if nobody played the test character (S.A.M sessions 22 and 31 had
  -- key presses and swings from someone at the keyboard while the test window was open)
  local inputs, attacks = 0, 0
  for _, r in ipairs(T.results or {}) do inputs, attacks = inputs + r.inputs, attacks + r.attacks end
  check("the test character stayed passive during the trial (nobody touched the game window)",
    inputs == 0 and attacks == 0, string.format("%d key presses, %d swings; if not 0, rerun hands-off", inputs, attacks))
  -- per pair (off, on): how often the limits took less damage
  local pairsWon, pairsN = 0, 0
  for i = 1, #(T.results or {}) - 1, 2 do
    local a, b = T.results[i], T.results[i + 1]
    if not a.on and b.on then
      pairsN = pairsN + 1
      if b.dmg < a.dmg then pairsWon = pairsWon + 1 end
    end
  end
  sam_log(string.format("pack_tactics: TRIALPAIRS limits_on_less_damage=%d of %d pairs", pairsWon, pairsN))
  check("CONTROL: with the natural limits off the character takes more damage", off > on,
    string.format("off %d (%.1f/min) vs on %d (%.1f/min)", I(off), pmOff, I(on), pmOn))
  check("hits are seen through on_damage_multiplier (the limits' swing signal)", hitsOff > 0 and hitsOn > 0,
    string.format("%d hits seen with limits off, %d on", hitsOff, hitsOn))
  check("room: never more released attackers than the ring fits", relPeak <= math.floor(capMin + 0.5),
    string.format("peak %d released, smallest mean ring %.2f slots", relPeak, capMin))
  check("no blow decided while held (leaks; wind-ups that land just after a hold are late blows)", leaks == 0, leaks)
  check("trial creatures removed", allGone(T.allTrial or {}), #(T.allTrial or {}) .. " removed")
  local hp = sam_get_stat(0, "HP")
  check("player HP unchanged by the test", hp == T.hp0, tostring(hp) .. " vs " .. tostring(T.hp0))
  check("no script errors during the test", stats.errors == T.errors0, stats.errors - T.errors0)
  return 1
end

local function testFinish()
  override = {}
  for _, id in ipairs({ "stamina", "stamina_txt" }) do sam_hud_clear(id, 0) end
  sta = {}
  sam_log(string.format("PACK_TACTICS RESULT: %d passed, %d FAILED", T.pass, T.fail))
  local p, f = T.pass, T.fail
  T = nil
  sam_test_done(p, f)
end

local function testTick(ps)
  if tickN < T.at then return end
  if #ps == 0 then return end
  local fn = TESTS[T.step]
  if not fn then testFinish(); return end
  local ok, r = pcall(fn, ps)
  if not ok then
    T.fail = T.fail + 1
    sam_log("FAIL  pack_tactics [step " .. T.step .. " raised an error]   (" .. tostring(r) .. ")")
    r = 1
  end
  if r == "again" then
    if tickN - T.stepStart > 1500 then
      check("step " .. T.step .. " finished within 30 s", false, "timed out")
      r = 1
    else
      return
    end
  end
  T.step = T.step + 1
  T.stepStart = tickN
  T.at = tickN + (tonumber(r) or 1)
end

-- ---------------------------------------------------------------------------
-- events and the tick
-- ---------------------------------------------------------------------------
local function handle(e)
  local n = e.name
  if n == "game.on_game_start" then
    if e.player ~= 0 then return end
    L, cs, tok, sta, trial, override = nil, {}, {}, {}, nil, {}
    lim, limP, bodyCache = {}, {}, {}
    local seed, mseed = sam_get_seed(), sam_get_map_seed()
    local cls, race, int, played = sam_get_class(0), sam_get_race(0), sam_get_stat(0, "INT"), sam_get_time_played()
    local gate = cls == "barbarian" and race == "human" and int == -2 and (played or 0) < 500
      and (seed == TEST_SEED or mseed == TEST_SEED)
    sam_log(string.format("pack_tactics: game start class=%s race=%s INT=%s played=%s seed=%s mapseed=%s selftest=%s",
      tostring(cls), tostring(race), tostring(int), tostring(played), tostring(seed), tostring(mseed), tostring(gate)))
    if gate then
      T = { step = 1, at = tickN + 50, stepStart = tickN, pass = 0, fail = 0, hp0 = sam_get_stat(0, "HP"),
        errors0 = stats.errors }
    end
  elseif n == "game.on_level_entered" then
    if L and L.key ~= levelKey() then endLevel("left"); L = nil end
  elseif n == "on_before_damage" then
    if T and e.player == 0 then
      if trial and trial.measuring then
        trial.dmg = trial.dmg + (e.damage or 0)
        trial.hits = trial.hits + 1
      end
      sam_modify_damage(0, 0)              -- the test character only watches; it never dies
    end
  elseif n == "player.on_damage_taken" then
    local pk = packOf(e.source_uid)
    if pk and pk.enc then
      pk.enc.dmg = pk.enc.dmg + (e.damage or 0)
      pk.enc.hits = pk.enc.hits + 1
      pk.enc.players[e.player] = true
    end
  elseif n == "player.on_before_hit" then
    onBeforeHit(e)
  elseif n == "on_action_pressed" then
    -- the self-test character must stay passive: a press means someone touched the test window
    if T then T.inputs = (T.inputs or 0) + 1 end
    if trial then trial.inputs = trial.inputs + 1 end
    if e.action == SHOVE_ACTION then shove(e.player) end
  elseif n == "player.on_attack_start" then
    if trial then trial.attacks = trial.attacks + 1 end
  elseif n == "on_monster_damaged" then
    local pk = packOf(e.monster_uid)
    if pk and e.max_hp and e.max_hp > 0 and (e.damage or 0) >= 0.25 * e.max_hp then
      pk.morale = pk.morale - MORALE_HIT
    end
  elseif n == "on_monster_died" then
    if not L then return end
    local m = e.monster_uid
    local pk = packOf(m)
    if pk then
      pk.alive = math.max(0, pk.alive - 1)
      pk.morale = pk.morale - moraleLoss(pk.size0, m == pk.leader)
      if pk.enc then pk.enc.kills = pk.enc.kills + 1; pk.enc.lastDeath = tickN end
      if pk.alive <= 0 then endEncounter(pk, "killed"); pk.state = "dead" end
    end
    L.memberOf[m] = nil
    cs[m] = nil
    lim[m], bodyCache[m] = nil, nil
    for _, t in pairs(tok) do t.holders[m] = nil; t.waiting[m] = nil end
  elseif n == "on_damage_multiplier" then
    onHit(e)                                       -- observe only: never changes the hit
  elseif n == "player.on_death" then
    local pk = packOf(e.killer_uid)
    if pk and pk.enc then pk.enc.pdeaths = pk.enc.pdeaths + 1 end
  end
end

function on_event(e)
  local ok, err = pcall(handle, e)
  if not ok then
    stats.errors = stats.errors + 1
    if stats.errors <= 8 then sam_log("pack_tactics: ERROR in " .. tostring(e.name) .. ": " .. tostring(err)) end
  end
end

local function step()
  local k = levelKey()
  if not L or L.key ~= k then
    endLevel("left")
    newLevel(k)
  end
  local ps = players()
  if L.phase ~= "live" then analyse(ps) end
  verbsTick(ps)
  if T then testTick(ps) end
  if #ps == 0 then return end
  for id, pk in pairs(L.packs) do
    if pk.state ~= "dead" and (tickN + id) % PACK_TICKS == 0 then packThink(pk, ps) end
  end
  if cfg("tactics") then
    for _, pk in pairs(L.packs) do
      if pk.state == "engaged" or pk.state == "retreat" then
        for _, m in ipairs(pk.members) do
          -- stagger: every creature thinks once per THINK_TICKS, phased by its uid
          if (tickN + m) % THINK_TICKS == 0 and alive(m) then memberThink(m, pk, ps) end
        end
      end
    end
  end
  if cfg("tactics") then steer() end              -- flankers keep their detour, retreats keep going
  if cfg("token_cap") then                         -- CONTROL only (D62 dropped the cap)
    if next(lim) ~= nil then limClear() end
    if tickN % TOKEN_TICKS == 0 then tokenPass(ps) end
    enforce(ps)
  else
    if next(tok) ~= nil then tok = {} end
    if cfg("limits") then
      if tickN % LIM_TICKS == 0 then limitsPass(ps) end
    elseif next(lim) ~= nil then
      limClear()                                   -- switched off: nobody stays held
    end
  end
  if trial and trial.measuring then
    -- crowding at the test character, any creature not spawned by the trial (contamination), and
    -- how many trial creatures stay in the fight (within 6 tiles) or have drifted out of it
    -- (beyond 8 tiles): the price of the target-clearing substitute is creatures that lose the fight
    local pl = ps[1]
    local contact, foreign, near, lost = 0, 0, 0, 0
    for _, m in ipairs(sam_find_entities(math.floor(pl.x), math.floor(pl.y), 6, "monster") or {}) do
      local x, y = fpos(m)
      if x and alive(m) then
        local d = dist(x, y, pl.x, pl.y)
        if d <= 1.6 then contact = contact + 1 end
        if not T.trialSet[m] and d <= 6 and sam_is_enemy(m, pl.uid) then foreign = foreign + 1 end
      end
    end
    for m in pairs(T.trialSet) do
      local x, y = fpos(m)
      if x and alive(m) then
        local d = dist(x, y, pl.x, pl.y)
        if d <= 6 then near = near + 1 elseif d > 8 then lost = lost + 1 end
      end
    end
    trial.contactSum = trial.contactSum + contact
    trial.nearSum = trial.nearSum + near
    trial.lostSum = trial.lostSum + lost
    trial.samples = trial.samples + 1
    if contact > trial.contactPeak then trial.contactPeak = contact end
    if foreign > trial.foreignPeak then trial.foreignPeak = foreign end
  end
  if tickN % STATS_TICKS == 0 then logStats() end
end

function on_tick()
  if not sam_is_in_game() then return end
  if sam_is_paused() or sam_is_loading() then return end
  tickN = tickN + 1
  local ok, err = pcall(step)
  if not ok then
    stats.errors = stats.errors + 1
    if stats.errors <= 8 then sam_log("pack_tactics: ERROR " .. tostring(err)) end
  end
end
