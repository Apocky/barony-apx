-- Smart Followers -- allies that follow in formation, engage proactively and fight as a group.
--
-- Barony has one shared monster state machine, so this mod does not replace a brain; it
-- steers that machine from outside, a few times a second, with the engine's own pathfinder
-- and target setter. A follower is any monster the engine calls a friend of a player
-- (sam_is_friend), so recruited allies and charmed creatures both count.
--
-- What each follower does (priority order, re-evaluated every THINK_TICKS):
--   1. RETREAT   badly wounded with enemies near -> drop the fight and fall back to the
--                leader for a while, then fight on (cooldown, so it never abandons you).
--   2. REJOIN    far from the leader, or stuck while trying to reach them -> teleport
--                beside them (cooldown).
--   3. LEASH     chasing something that drags it away from the party -> give up, return.
--   4. PROTECT   an enemy is hunting the leader (or another follower) -> take it as target
--                even without line of sight; when the leader is hurt, this wins over a
--                lower-priority current target.
--   5. ENGAGE    no target and an enemy in sight near the party -> attack it. Nearest to
--                the leader first, so the group focuses fire instead of scattering.
--   6. FORMATION idle and too far away -> path to a slot behind/beside the leader. Slots
--                are spread per follower so they do not stack on one tile.
--
-- Counters are written to sam_log.txt every STATS_TICKS so a silent no-op is visible.

local THINK_TICKS   = 10     -- per-follower brain interval (0.2 s at 50 ticks/s)
local SEARCH_R      = 30     -- tiles around a player in which followers are looked for
local ENGAGE_R      = 10     -- tiles around a follower in which it scans for enemies
local PARTY_R       = 13     -- an enemy must be this close to the leader to be engaged
local LEASH_R       = 15     -- chasing a target farther than this from the leader = drop it
local FOLLOW_FAR    = 6.0    -- idle follower farther than this from leader walks back
local FOLLOW_NEAR   = 3.0    -- ...and stops issuing walk orders inside this
local SLOT_RADIUS   = 2.5    -- formation slot distance from the leader
local REJOIN_DIST   = 30     -- farther than this for REJOIN_AFTER ticks -> teleport
local REJOIN_AFTER  = 250    -- 5 s
local STUCK_AFTER   = 300    -- 6 s without progress toward a far leader -> teleport
local REJOIN_CD     = 500    -- 10 s between teleports per follower
local RETREAT_HP    = 0.30   -- retreat below this fraction of max HP
local RETREAT_END   = 0.55   -- ...and resume fighting above this
local RETREAT_TICKS = 200    -- max retreat length (4 s)
local RETREAT_CD    = 600    -- after a retreat, no new one for 12 s
local PROTECT_HP    = 0.5    -- leader below this fraction: protecting beats everything but retreat
local RETARGET_CD   = 40     -- min ticks between target changes per follower
local STATS_TICKS   = 1500   -- 30 s

-- friendly species that are not followers
local NOT_FOLLOWERS = { shopkeeper = true }

local tickN = 0
local st = {}                -- per-follower state, keyed by uid
local stats = { think = 0, engage = 0, protect = 0, leash = 0, retreat = 0, rejoin = 0,
                walk = 0, errors = 0, followers = 0 }

local function dist(ax, ay, bx, by) local dx, dy = ax - bx, ay - by; return math.sqrt(dx * dx + dy * dy) end

local function alive(uid)
  local hp = sam_get_hp(uid)
  return hp ~= nil and hp > 0
end

local function frac(uid)
  local hp, mx = sam_get_hp(uid), sam_get_max_hp(uid)
  if not hp or not mx or mx <= 0 then return 1.0 end
  return hp / mx
end

local function state(uid)
  local s = st[uid]
  if not s then
    s = { retreatUntil = 0, retreatCd = 0, retargetAt = 0, walkAt = 0, farSince = nil,
          stuckSince = nil, lastX = nil, lastY = nil, rejoinCd = 0, wasEngaged = false }
    st[uid] = s
  end
  return s
end

-- A walkable tile at or near (x, y) for this entity; nil if none found.
local function standable(uid, x, y)
  x, y = math.floor(x + 0.5), math.floor(y + 0.5)
  if sam_can_stand(uid, x, y) then return x, y end
  for r = 1, 2 do
    for dx = -r, r do
      for dy = -r, r do
        if sam_can_stand(uid, x + dx, y + dy) then return x + dx, y + dy end
      end
    end
  end
  return nil
end

local function enemiesNear(f, x, y, r)
  local out = {}
  for _, e in ipairs(sam_find_entities(x, y, r, "monster") or {}) do
    if e ~= f and alive(e) and sam_is_enemy(f, e) then out[#out + 1] = e end
  end
  return out
end

local function setTarget(f, s, target)
  if tickN < s.retargetAt then return false end
  if sam_set_monster_target_uid(f, target, false) then
    s.retargetAt = tickN + RETARGET_CD
    return true
  end
  return false
end

local function walkTo(f, s, x, y)
  if tickN < s.walkAt then return end
  s.walkAt = tickN + 25
  if sam_monster_path_to(f, x, y) then stats.walk = stats.walk + 1 end
end

local function rejoin(f, s, lx, ly)
  if tickN < s.rejoinCd then return false end
  local x, y = standable(f, lx, ly)
  if not x then return false end
  if sam_set_position(f, x, y) then
    s.rejoinCd = tickN + REJOIN_CD
    s.farSince, s.stuckSince = nil, nil
    stats.rejoin = stats.rejoin + 1
    sam_log(string.format("smart_followers: follower %d rejoined leader at (%d,%d)", f, x, y))
    return true
  end
  return false
end

-- One brain update for follower f whose leader is player p (leader uid lu at lx, ly).
local function think(f, p, lu, lx, ly, slot, slots)
  local s = state(f)
  local fx, fy = sam_get_position(f)
  if not fx then return end
  stats.think = stats.think + 1

  local d = dist(fx, fy, lx, ly)
  local target = sam_get_monster_target_uid(f) or 0
  local hasTarget = target ~= 0 and alive(target) and sam_is_enemy(f, target)
  local leaderHurt = frac(lu) < PROTECT_HP

  -- progress tracking toward a far leader
  if d > FOLLOW_FAR * 1.5 and not hasTarget then
    if s.lastX and dist(fx, fy, s.lastX, s.lastY) < 0.5 then
      s.stuckSince = s.stuckSince or tickN
    else
      s.stuckSince = nil
    end
  else
    s.stuckSince = nil
  end
  s.lastX, s.lastY = fx, fy

  -- 1. RETREAT
  local myFrac = frac(f)
  if tickN < s.retreatUntil then
    if myFrac >= RETREAT_END or d < FOLLOW_NEAR then
      s.retreatUntil = 0
    else
      if hasTarget then sam_clear_monster_target(f, true) end
      walkTo(f, s, lx, ly)
      return
    end
  elseif myFrac < RETREAT_HP and tickN >= s.retreatCd then
    local threats = enemiesNear(f, fx, fy, 5)
    if #threats > 0 then
      s.retreatUntil = tickN + RETREAT_TICKS
      s.retreatCd = tickN + RETREAT_TICKS + RETREAT_CD
      sam_clear_monster_target(f, true)
      walkTo(f, s, lx, ly)
      stats.retreat = stats.retreat + 1
      return
    end
  end

  -- 2. REJOIN
  if d > REJOIN_DIST then
    s.farSince = s.farSince or tickN
    if tickN - s.farSince >= REJOIN_AFTER and rejoin(f, s, lx, ly) then return end
  else
    s.farSince = nil
  end
  if s.stuckSince and tickN - s.stuckSince >= STUCK_AFTER and rejoin(f, s, lx, ly) then return end

  -- 3. LEASH
  if hasTarget then
    local tx, ty = sam_get_position(target)
    if tx and dist(tx, ty, lx, ly) > LEASH_R and d > LEASH_R * 0.6 then
      sam_clear_monster_target(f, true)
      walkTo(f, s, lx, ly)
      stats.leash = stats.leash + 1
      return
    end
  end

  -- 4. PROTECT: anything hunting the leader or a friend of the leader, near the leader
  local best, bestScore = nil, nil
  for _, e in ipairs(enemiesNear(f, lx, ly, PARTY_R)) do
    local et = sam_get_monster_target_uid(e) or 0
    local hunting = et ~= 0 and (et == lu or sam_is_friend(f, et))
    if hunting then
      local ex, ey = sam_get_position(e)
      if ex then
        local score = dist(ex, ey, lx, ly) + (et == lu and -4 or 0)   -- leader's attackers first
        if not bestScore or score < bestScore then best, bestScore = e, score end
      end
    end
  end
  if best and (not hasTarget or leaderHurt) and best ~= target then
    if setTarget(f, s, best) then
      stats.protect = stats.protect + 1
      return
    end
  end
  if hasTarget then return end   -- already fighting something sensible

  -- 5. ENGAGE: nearest-to-leader enemy this follower can see
  local pick, pickD = nil, nil
  for _, e in ipairs(enemiesNear(f, fx, fy, ENGAGE_R)) do
    local ex, ey = sam_get_position(e)
    if ex then
      local dl = dist(ex, ey, lx, ly)
      if dl <= PARTY_R and (not pickD or dl < pickD) then
        local seen = sam_line_of_sight(math.floor(fx), math.floor(fy), math.floor(ex), math.floor(ey))
        if seen then pick, pickD = e, dl end
      end
    end
  end
  if pick and setTarget(f, s, pick) then
    stats.engage = stats.engage + 1
    return
  end

  -- 6. FORMATION
  if d > FOLLOW_FAR or (s.wasFar and d > FOLLOW_NEAR) then
    s.wasFar = true
    local face = sam_get_facing(p) or 0
    local ang = face + math.pi + (slot - (slots + 1) / 2) * 0.9
    local sx, sy = standable(f, lx + math.cos(ang) * SLOT_RADIUS, ly + math.sin(ang) * SLOT_RADIUS)
    if not sx then sx, sy = math.floor(lx + 0.5), math.floor(ly + 0.5) end
    walkTo(f, s, sx, sy)
  else
    s.wasFar = false
  end
end

local function brainPass()
  local count = sam_player_count()
  -- gather followers, each assigned to its nearest living player
  local groups = {}
  local seen = {}
  local total = 0
  for p = 0, count - 1 do
    local pu = sam_get_player_uid(p)
    if pu and not sam_is_ghost(p) then
      local px, py = sam_get_position(pu)
      if px then
        groups[p] = groups[p] or { uid = pu, x = px, y = py, list = {} }
        for _, m in ipairs(sam_find_entities(math.floor(px), math.floor(py), SEARCH_R, "monster") or {}) do
          if not seen[m] and sam_is_friend(m, pu) and alive(m) then
            local ty = sam_get_monster_type(m)
            if ty and not NOT_FOLLOWERS[ty] then seen[m] = true end
          end
        end
      end
    end
  end
  for m in pairs(seen) do
    local mx, my = sam_get_position(m)
    local bp, bd = nil, nil
    if mx then
      for p, g in pairs(groups) do
        local dd = dist(mx, my, g.x, g.y)
        if not bd or dd < bd then bp, bd = p, dd end
      end
    end
    if bp then table.insert(groups[bp].list, m) end
  end
  for p, g in pairs(groups) do
    table.sort(g.list)
    total = total + #g.list
    for i, f in ipairs(g.list) do
      -- stagger: brainPass runs every 2nd tick, so phase on tickN/2 (not tickN, whose
      -- parity would starve odd uids)
      if (math.floor(tickN / 2) + f) % math.floor(THINK_TICKS / 2) == 0 then
        local ok, err = pcall(think, f, p, g.uid, g.x, g.y, i, #g.list)
        if not ok then
          stats.errors = stats.errors + 1
          if stats.errors <= 5 then sam_log("smart_followers: ERROR " .. tostring(err)) end
        end
      end
    end
  end
  stats.followers = total
  -- forget followers that are gone
  for uid in pairs(st) do if not seen[uid] then st[uid] = nil end end
end

-- ---------------------------------------------------------------------------
-- Headless self-check:  barony.exe -samtest=smart_followers
-- Checks that every API this mod depends on exists, and that a brain pass with no
-- followers present runs clean. Real follower behaviour needs a recruited ally in a
-- live game; see README.md for the in-game checklist.
-- ---------------------------------------------------------------------------
local testTicksLeft = nil

local function runChecks()
  local pass, fail = 0, 0
  local function check(name, ok, saw)
    if ok then pass = pass + 1 else fail = fail + 1 end
    sam_log((ok and "PASS  " or "FAIL  ") .. "smart_followers [" .. name .. "]   (" .. tostring(saw) .. ")")
  end
  local needed = { "sam_is_friend", "sam_is_enemy", "sam_find_entities", "sam_monster_path_to",
    "sam_set_monster_target_uid", "sam_get_monster_target_uid", "sam_clear_monster_target",
    "sam_can_stand", "sam_set_position", "sam_get_position", "sam_get_hp", "sam_get_max_hp",
    "sam_line_of_sight", "sam_get_facing", "sam_get_monster_type", "sam_player_count" }
  for _, fn in ipairs(needed) do check("api " .. fn, _G[fn] ~= nil, type(_G[fn])) end
  local ok, err = pcall(brainPass)
  check("brain pass runs with no followers", ok, err)
  check("no followers detected in empty level", stats.followers == 0, stats.followers)
  local names = {}
  for _, m in ipairs(sam_list_monsters() or {}) do names[#names + 1] = tostring(m.name) end
  sam_log("smart_followers: species names = " .. table.concat(names, ","))
  check("shopkeeper species name exists", table.concat(names, ","):find("shopkeeper", 1, true) ~= nil, "see list above")
  sam_log(string.format("SMART_FOLLOWERS RESULT: %d passed, %d FAILED", pass, fail))
  sam_test_done(pass, fail)
end

function on_event(e)
  if e.name == "game.on_game_start" and e.player == 0 and sam_get_class(0) == "barbarian" and sam_get_race(0) == "human" and sam_get_seed and sam_get_seed() == 1514995713 then testTicksLeft = 50 end
  if e.name == "game.on_level_entered" then st = {} end
end

function on_tick()
  if not sam_is_in_game() then return end
  if sam_is_paused() or sam_is_loading() then return end
  tickN = tickN + 1

  if testTicksLeft ~= nil then
    testTicksLeft = testTicksLeft - 1
    if testTicksLeft == 0 then testTicksLeft = nil; runChecks(); return end
  end

  -- one scan per tick-phase 0 keeps the entity search cost to ~5 passes a second
  if tickN % 2 == 0 then
    local ok, err = pcall(brainPass)
    if not ok then
      stats.errors = stats.errors + 1
      if stats.errors <= 5 then sam_log("smart_followers: ERROR " .. tostring(err)) end
    end
  end

  if tickN % STATS_TICKS == 0 then
    sam_log(string.format(
      "smart_followers: followers=%d think=%d engage=%d protect=%d leash=%d retreat=%d rejoin=%d walk=%d errors=%d",
      stats.followers, stats.think, stats.engage, stats.protect, stats.leash, stats.retreat,
      stats.rejoin, stats.walk, stats.errors))
  end
end
