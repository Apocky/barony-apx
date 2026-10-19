-- sam_mock.lua -- a stubbed S.A.M 3.4.0 world for OFFLINE checks of pack_tactics/main.lua.
--
-- Not a simulation of Barony and never a source of measured numbers. It exists to run every
-- code path of main.lua (level analysis, census, population, packs, tokens, verbs, telemetry,
-- the whole self-test) under a real Lua 5.4 in seconds, so a runtime error costs a second
-- instead of a three-minute game launch. S.A.M only loads <modroot>/main.*, never tools/.
--
-- The world: a 60x40 map of three rooms and corridors. Monsters walk straight at their target,
-- re-acquire the player whenever they are within REACQUIRE_R tiles (as the engine's WAIT and
-- HUNT scans do, which is the worst case for the token gate), and strike every HIT_TICKS.
-- As in actmonster.cpp, acquiring a target only changes the state (WAIT/HUNT -> ATTACK): the
-- strike happens in the ATTACK branch on a LATER tick, so a monster cannot acquire and strike in
-- the same tick.

MOCK = { tick = 0, logs = {}, ents = {}, nextUid = 2, hud = {}, settings = {}, decl = {},
  done = nil, modified = nil, seed = 1514995713, mapseed = 777, floor = 0, rng = 12345,
  weapon = "bronze_axe", shield = nil, deaths = {} }

local W, H = 60, 40
-- HITRATE 45 and STRIKERANGE 24 px = 1.5 tiles as in actmonster.cpp; PACIFY pins the timer at 22
local REACQUIRE_R, SPEED, REACH, HITRATE, HIT_DMG = 6, 0.08, 1.5, 45, 3
local WINDUP = 9                                   -- ANIMATE_DURATION_WINDUP (monster.hpp)
local SIZES = { rat = 2, troll = 7, player = 4 }
local open = {}

local function carve(x1, y1, x2, y2)
  for y = y1, y2 do for x = x1, x2 do open[y * 4096 + x] = true end end
end
function MOCK.buildMap(variant)
  open = {}
  carve(5, 5, 15, 15)                   -- start room
  carve(30, 4, 46, 18)                  -- room B
  carve(8, 25, 26, 35)                  -- room C
  carve(16, 10, 29, 10)                 -- corridor A-B
  carve(12, 16, 12, 24)                 -- corridor A-C
  carve(40, 19, 40, 30); carve(27, 30, 40, 30)
  if variant == 2 then carve(48, 22, 56, 36); carve(41, 26, 47, 26) end
end
local function isOpen(x, y) return open[y * 4096 + x] == true end

local function ent(uid) local e = MOCK.ents[uid]; if e and not e.gone then return e end return nil end
local function d2(ax, ay, bx, by) local dx, dy = ax - bx, ay - by; return math.sqrt(dx * dx + dy * dy) end

function MOCK.addPlayer(x, y, hp)
  MOCK.ents[1] = { uid = 1, player = 0, kind = "player", x = x, y = y, hp = hp, maxhp = hp, target = 0 }
end
function MOCK.addMonster(kind, x, y, hp)
  local uid = MOCK.nextUid
  MOCK.nextUid = uid + 1
  MOCK.ents[uid] = { uid = uid, kind = kind, x = x, y = y, hp = hp or 10, maxhp = hp or 10, target = 0,
    dest = nil, hitTime = 0, kbUntil = 0, vx = 0, vy = 0, pacifyUntil = 0 }
  return uid
end

-- the API -------------------------------------------------------------------
function sam_log(s) MOCK.logs[#MOCK.logs + 1] = string.format("[t%05d] %s", MOCK.tick, tostring(s)) end
function sam_message(p, s) sam_log("MSG p" .. tostring(p) .. ": " .. s) return true end
function sam_register_setting(id, spec)
  MOCK.decl[#MOCK.decl + 1] = id
  if MOCK.settings[id] == nil then MOCK.settings[id] = spec.default end
  return true
end
function sam_get_setting(id) return MOCK.settings[id] end
function sam_list_settings()
  local out = {}
  for _, id in ipairs(MOCK.decl) do out[#out + 1] = { id = id, value = MOCK.settings[id] } end
  return out
end
function sam_register_action() return true end
function sam_is_in_game() return true end
function sam_is_paused() return false end
function sam_is_loading() return false end
function sam_is_host() return true end
function sam_get_floor() return MOCK.floor end
function sam_get_map_seed() return MOCK.mapseed end
function sam_get_seed() return MOCK.seed end
function sam_get_level_info() return { floor = MOCK.floor, name = "Mock Mines", width = W, height = H } end
function sam_get_playable_bounds() return 1, 1, W - 1, H - 1 end
function sam_get_tile(x, y)
  if x < 0 or y < 0 or x >= W or y >= H then return nil end
  local o = isOpen(x, y)
  return { wall = o and 0 or 1, floor = 1, ceiling = 1, solid = not o, water = false, lava = false, walkable = o }
end
function sam_player_count() return 1 end
function sam_get_player_uid(p) if p == 0 and ent(1) then return 1 end return nil end
function sam_is_ghost() return false end
function sam_get_class() return "barbarian" end
function sam_get_race() return "human" end
function sam_get_stat(p, s)
  if s == "INT" then return -2 end
  if s == "HP" then return ent(1) and ent(1).hp end
  return 0
end
function sam_get_time_played() return MOCK.tick end
function sam_get_hp(uid) local e = ent(uid); return e and e.hp or nil end
function sam_get_max_hp(uid) local e = ent(uid); return e and e.maxhp or nil end
function sam_get_position_precise(uid) local e = ent(uid); if not e then return nil end return e.x * 16, e.y * 16, 0 end
function sam_get_position(uid) local e = ent(uid); if not e then return nil end return math.floor(e.x), math.floor(e.y) end
function sam_get_facing(p) return 0.3 end
function sam_find_entities(x, y, r, kind)
  local out = {}
  for uid, e in pairs(MOCK.ents) do
    if not e.gone and d2(x + 0.5, y + 0.5, e.x, e.y) <= r then
      if kind == "monster" and not e.player then out[#out + 1] = uid end
      if kind == "player" and e.player then out[#out + 1] = uid end
    end
  end
  table.sort(out)
  return out
end
function sam_is_enemy(a, b)
  local ea, eb = ent(a), ent(b)
  if not ea or not eb then return false end
  return (ea.player ~= nil) ~= (eb.player ~= nil)
end
function sam_is_friend(a, b) return not sam_is_enemy(a, b) end
function sam_get_monster_type(uid) local e = ent(uid); return (e and not e.player) and e.kind or nil end
function sam_get_monster_target_uid(uid) local e = ent(uid); return e and e.target or 0 end
function sam_set_monster_target_uid(uid, t)
  local e = ent(uid); if not e then return false end
  if e.target ~= t then e.acquiredAt = MOCK.tick end
  e.target = t
  return true
end
function sam_clear_monster_target(uid) local e = ent(uid); if not e then return false end e.target = 0; return true end
function sam_monster_path_to(uid, x, y)
  local e = ent(uid)
  if not e or not isOpen(x, y) then return false end
  e.dest = { x + 0.5, y + 0.5 }
  return true
end
function sam_can_stand(uid, x, y) return isOpen(x, y) end
function sam_set_position(uid, x, y)
  local e = ent(uid)
  if not e or (e.player and not isOpen(x, y)) then return false end
  e.x, e.y = x + 0.5, y + 0.5
  return true
end
function sam_is_spawnable(x, y) return isOpen(x, y) end
function sam_tiles_connected(x1, y1, x2, y2) return isOpen(x1, y1) and isOpen(x2, y2) end
function sam_line_of_sight() return true, -1, -1 end
function sam_spawn_monster(x, y, kind) if not isOpen(x, y) then return nil end return MOCK.addMonster(kind, x + 0.5, y + 0.5, 10) end
function sam_remove_entity(uid) local e = ent(uid); if not e or e.player then return false end e.removeAt = MOCK.tick + 1; return true end
function sam_monster_equip() return true end
MOCK.applyLogs = 0
function sam_apply_monster_effect(uid, eff, ticks)
  local e = ent(uid); if not e or e.player then return false end
  if eff == "PACIFY" then e.pacifyUntil = MOCK.tick + ticks end
  MOCK.applyLogs = MOCK.applyLogs + 1          -- S.A.M writes one INFO line per call
  return true
end
function sam_remove_monster_effect(uid, eff)
  local e = ent(uid); if not e then return false end
  if eff == "PACIFY" then e.pacifyUntil = 0 end
  return true
end
function sam_monster_has_effect(uid, eff)
  local e = ent(uid); if not e then return false end
  if eff == "PACIFY" then return MOCK.tick < (e.pacifyUntil or 0) end
  return false
end
function sam_get_entity_size(uid)
  local e = ent(uid); if not e then return nil end
  local s = SIZES[e.kind] or 4
  return s, s
end
function sam_set_monster_stat(uid, s, v)
  local e = ent(uid); if not e then return false end
  if s == "MAXHP" then e.maxhp = v elseif s == "HP" then e.hp = math.min(v, e.maxhp) end
  return true
end
function sam_deal_damage_typed(uid, amt, ty)
  local e = ent(uid); if not e then return nil end
  e.hp = e.hp - amt
  return amt
end
function sam_damage_number() return true end
function sam_heal(uid, n) local e = ent(uid); if not e then return nil end local b = e.hp; e.hp = math.min(e.maxhp, e.hp + n); return e.hp - b end
function sam_apply_force(uid, force, ang, ticks)
  local e = ent(uid); if not e or e.player then return false end
  e.vx, e.vy = math.cos(ang) * force * 0.06, math.sin(ang) * force * 0.06
  e.kbUntil = MOCK.tick + (ticks or 30)
  return true
end
function sam_get_equipped_item(p, slot)
  if slot == "WEAPON" then return MOCK.weapon end
  if slot == "SHIELD" then return MOCK.shield end
  return nil
end
function sam_hud_bar(id) MOCK.hud[id] = true return true end
function sam_hud_text(id) MOCK.hud[id] = true return true end
function sam_hud_clear(id) local was = MOCK.hud[id]; MOCK.hud[id] = nil; return was == true end
function sam_modify_damage(p, v) MOCK.modified = v end
function sam_random(stream, a, b)
  MOCK.rng = (MOCK.rng * 1103515245 + 12345) % 2147483648
  return a + MOCK.rng % (b - a + 1)
end
function sam_test_done(p, f) MOCK.done = { p, f } return true end

-- the world tick (monsters act before the scripts' on_tick, as in the engine) -------------
local function hurtPlayer(src, dmg)
  MOCK.modified = nil
  on_event({ name = "on_before_damage", player = 0, damage = dmg })
  local d = (MOCK.modified ~= nil) and MOCK.modified or dmg
  local p = ent(1)
  if d > 0 and p then
    p.hp = p.hp - d
    on_event({ name = "player.on_damage_taken", player = 0, damage = d, hp = p.hp, maxhp = p.maxhp,
      lethal = 0, source_uid = src, source_type = 0 })
  end
end

function MOCK.step()
  local t = MOCK.tick
  local p = ent(1)
  for uid, e in pairs(MOCK.ents) do
    if not e.gone and not e.player then
      if e.removeAt and t >= e.removeAt then
        e.gone = true
      elseif e.hp <= 0 then
        if not MOCK.deaths[uid] then
          MOCK.deaths[uid] = true
          e.gone = true
          on_event({ name = "on_monster_died", monster_uid = uid, monster_type = 1, hp = e.hp, max_hp = e.maxhp,
            killer_uid = 0, floor = MOCK.floor })
        end
      elseif t < e.kbUntil then
        local nx, ny = e.x + e.vx, e.y + e.vy
        if isOpen(math.floor(nx), math.floor(ny)) then e.x, e.y = nx, ny end
      else
        if e.target == 0 and p and d2(e.x, e.y, p.x, p.y) <= REACQUIRE_R then e.target = 1; e.acquiredAt = t end
        local gx, gy = nil, nil
        local pacified = t < (e.pacifyUntil or 0)
        if pacified then e.hitTime = HITRATE // 2 end
        if e.target ~= 0 and ent(e.target) then
          local tg = ent(e.target)
          local dd = d2(e.x, e.y, tg.x, tg.y)
          if pacified then
            if dd < 2.0 then gx, gy = e.x - (tg.x - e.x), e.y - (tg.y - e.y) end   -- shouldRetreat: back away
          elseif dd > 1.0 then
            gx, gy = tg.x, tg.y
          end
          -- a committed blow lands WINDUP ticks after the decision, held or not (entity.cpp attack():
          -- the WINDUP pose returns, the animation calls attack() again)
          if e.windupAt and t >= e.windupAt then
            e.windupAt = nil
            if tg.player then
              on_event({ name = "on_damage_multiplier", target_uid = e.target, attacker_uid = uid, damage_type = 0,
                multiplier_x1000 = 1000, projectile_uid = 0, spell_id = 0 })
              hurtPlayer(uid, HIT_DMG)
            end
          end
          if not pacified and not e.windupAt and dd <= REACH and t > (e.acquiredAt or -1) then
            e.hitTime = (e.hitTime or 0) + 1
            if e.hitTime >= HITRATE then
              e.hitTime = 0
              -- the engine's thin swing ray at the decision: a body on the line takes it, so no swing
              local blocked = false
              for ouid, o in pairs(MOCK.ents) do
                if ouid ~= uid and not o.player and not o.gone then
                  local vx, vy = tg.x - e.x, tg.y - e.y
                  local s = ((o.x - e.x) * vx + (o.y - e.y) * vy) / (dd * dd)
                  if s > 0 and s < 1 and math.abs((o.x - e.x) * vy - (o.y - e.y) * vx) / dd < (SIZES[o.kind] or 4) / 16 then
                    blocked = true
                  end
                end
              end
              if not blocked then e.windupAt = t + WINDUP end
            end
          end
        elseif e.dest then
          gx, gy = e.dest[1], e.dest[2]
          if d2(e.x, e.y, gx, gy) < 0.3 then e.dest = nil; gx = nil end
        end
        if gx then
          local dd = d2(e.x, e.y, gx, gy)
          local nx, ny = e.x + (gx - e.x) / dd * SPEED, e.y + (gy - e.y) / dd * SPEED
          if isOpen(math.floor(nx), math.floor(ny)) then e.x, e.y = nx, ny end
        end
      end
    end
  end
end

function MOCK.newLevel(floor, mapseed, variant)
  for uid, e in pairs(MOCK.ents) do if not e.player then e.gone = true end end
  MOCK.floor, MOCK.mapseed = floor, mapseed
  MOCK.buildMap(variant)
end

-- one run: game start, then ticks until the test is done or maxTicks; hooks(t) may fire events
function MOCK.run(maxTicks, hooks)
  on_event({ name = "game.on_game_start", player = 0, class_id = 0, class_name = "barbarian", race = 0, race_name = "human" })
  for t = 1, maxTicks do
    MOCK.tick = t
    MOCK.step()
    if hooks then hooks(t) end
    on_tick({ tick_count = t, delta_ticks = 1 })
    if MOCK.done then return t end
  end
  return maxTicks
end
