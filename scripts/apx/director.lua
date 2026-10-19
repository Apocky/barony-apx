-- BF5-1/BF5-2 (reading half): the named director with a seeded temperament and a tension reading.
-- It decides once per second (50 ticks) and writes one row per decision. Events, budget and the
-- chronicle come in later steps; until then every decision is "silence" with the reason logged.
-- Card: Apockalypsis handovers/tasks/BF5-director-card.md (written for the C++ route; the Lua route
-- keeps its rules: one seeded rng, every decline counted, rows are the only output).
local NAMES = {
  -- weathered
  "Harrow", "Orsolo", "Wend", "Keld", "Brannoch", "Maud Thorn", "Ysolt", "Corvane",
  -- bright
  "Sunnet", "Aurel", "Pip Lantern", "Tamsin", "Oriel", "Fenn", "Lark", "Mirabel",
  -- gaunt
  "Vael", "Ixen", "Hollis Grey", "Morrow", "Skerrit", "Nadir", "Thessaly Gaunt", "Quill",
}
local TEMPERS = { "calm", "story", "storm" }
local WEIGHTS = { calm = { ambush = 0.6, den = 0.5, silence = 1.6, budget = 0.85 },
                  story = { ambush = 1.0, den = 1.0, silence = 1.0, budget = 1.0 },
                  storm = { ambush = 1.5, den = 1.4, silence = 0.5, budget = 1.2 } }

local function mix(z)  -- splitmix64 on Lua 64-bit integers
  z = z + 0x9E3779B97F4A7C15
  z = (z ~ (z >> 30)) * 0xBF58476D1CE4E5B9
  z = (z ~ (z >> 27)) * 0x94D049BB133111EB
  return z ~ (z >> 31)
end

local function identity(seed)
  local h = mix(seed ~ 0x44495245)           -- "DIRE"
  local name = NAMES[(h % #NAMES) + 1]
  local temper = TEMPERS[((h >> 20) % 3) + 1]
  return { name = name, temper = temper, weights = WEIGHTS[temper] }
end

local D = { identity = identity, decisions = 0, ticks = {}, us = {}, blind = false,
            tension = { value = 0, ewma = 0 }, hits = {} }
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
        pos[#pos + 1] = { x, y }
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
           value = value, players = n, hp_frac = hpfrac_sum / n, resources_partial = true }
end

apx.on("damage", function(d)
  if d.player ~= nil and not D.blind then D.hits[#D.hits + 1] = { apx.tick(), d.amount } end
end)

local me
apx.every(50, function()
  if not apx.player_uid(0) then return end                 -- menu map: no player, no director
  local t0 = apx.clock_us()
  if not me then
    me = identity(apx.run_seed())
    D.me = me
    apx.log(string.format("director %s (%s), run seed %d", me.name, me.temper, apx.run_seed()))
  end
  local t = read_tension()
  if not t then return end
  t.ewma = D.tension.ewma * 0.7 + t.value * 0.3
  D.tension = t
  D.decisions = D.decisions + 1
  D.ticks[#D.ticks + 1] = apx.tick()
  local row = { id = D.decisions, tick = apx.tick(), floor = apx.floor(), director = me.name, temper = me.temper,
    event = "silence", reason = "reading only (BF5-1)", tension = t }
  apx.row("dir_decisions", row)
  D.us[#D.us + 1] = apx.clock_us() - t0
end)
