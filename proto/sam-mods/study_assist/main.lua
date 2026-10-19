-- Study Assist -- charm-book learning that cannot be refused by a stale skill read.
--
-- Barony learns a spell from a book when  mysticism + INT >= spell difficulty  (a mysticism
-- of 100 counts as passing outright). Charm Monster is difficulty 80 in this install's
-- items.json. A refused read ("too difficult") therefore means the game's own numbers were
-- below that line, whatever the skill sheet looked like. This mod:
--   1. LOGS those numbers (raw and effective mysticism, raw and effective INT, race, class)
--      at game start, whenever a spellbook is carried, and when a cast fails -- so the cause
--      of a refusal is on record instead of guessed;
--   2. LEARNS the spell from a carried, unequipped charm book as soon as the effective
--      numbers qualify, or mysticism is raw-maxed, without needing the Read action.
-- The book is left in the pack. Counters print every 30 s.

local BOOK        = "spellbook_charm"
local SPELL       = 31   -- spell id from items.json; overridden by sam_get_tome_spell(book)
local SKILL       = "mysticism"
local DIFFICULTY  = 80
local CHECK_TICKS = 25

local tickN = 0
local lastLog = {}           -- player -> last logged summary string (log only on change)
local stats = { checks = 0, granted = 0, refused_logged = 0 }

local function snapshot(p, uid)
  local raw   = sam_get_skill(uid, SKILL, false)
  local eff   = sam_get_skill(uid, SKILL, true)
  local intRaw = sam_get_stat(p, "INT")
  local intEff = sam_get_effective_stat(uid, "INT")
  return raw, eff, intRaw, intEff
end

local function summary(p, uid)
  local raw, eff, ir, ie = snapshot(p, uid)
  local knows = sam_player_knows_spell(p, SPELL)
  return string.format(
    "player %d race=%s class=%s mysticism raw=%s eff=%s INT raw=%s eff=%s need=%d knows_charm=%s",
    p, tostring(sam_get_race(p)), tostring(sam_get_class(p)), tostring(raw), tostring(eff),
    tostring(ir), tostring(ie), DIFFICULTY, tostring(knows)), raw, eff, ie
end

local function qualifies(raw, eff, intEff)
  if raw and raw >= 100 then return true end            -- the engine treats maxed as 100
  return (eff or 0) + (intEff or 0) >= DIFFICULTY
end

local function checkPlayer(p)
  local uid = sam_get_player_uid(p)
  if not uid or sam_is_ghost(p) then return end
  local inv = sam_get_inventory(p)
  if not inv then return end
  local book
  for _, it in ipairs(inv) do
    if it.name == BOOK and not it.equipped then book = it; break end
  end
  if not book then return end
  stats.checks = stats.checks + 1

  local spell = sam_get_tome_spell(book.uid)
  if type(spell) ~= "number" then spell = SPELL end
  local knows = sam_player_knows_spell(p, spell)
  if knows == nil or knows then return end

  local text, raw, eff, ie = summary(p, uid)
  if qualifies(raw, eff, ie) then
    if sam_grant_spell(p, spell) then
      stats.granted = stats.granted + 1
      sam_message(p, "You study the Charm Monster spellbook and grasp it.")
      sam_log("study_assist: GRANTED charm -- " .. text)
    end
  elseif lastLog[p] ~= text then
    lastLog[p] = text
    stats.refused_logged = stats.refused_logged + 1
    sam_log("study_assist: charm book carried but NOT qualified -- " .. text)
  end
end

-- ---------------------------------------------------------------------------
-- samtest: barbarian test character. Verifies the snapshot reads, that an unqualified
-- player is NOT granted (control), and that a qualified one IS (INT raised to 90).
-- ---------------------------------------------------------------------------
local testTicksLeft = nil

local function runChecks()
  local pass, fail = 0, 0
  local function check(name, ok, saw)
    if ok then pass = pass + 1 else fail = fail + 1 end
    sam_log((ok and "PASS  " or "FAIL  ") .. "study_assist [" .. name .. "]   (" .. tostring(saw) .. ")")
  end
  local uid = sam_get_player_uid(0)
  check("player uid", uid ~= nil, uid)
  if uid then
    local text, raw, eff, ie = summary(0, uid)
    sam_log("study_assist: test snapshot -- " .. text)
    check("mysticism readable", raw ~= nil and eff ~= nil, tostring(raw) .. "/" .. tostring(eff))
    check("INT readable", ie ~= nil, ie)
    check("raw-maxed rule", qualifies(100, 100, 0) == true, "qualifies(100,100,0)")
    check("sum rule", qualifies(0, 60, 20) == true and qualifies(0, 60, 19) == false, "60+20 / 60+19")
    -- end-to-end grant path, leaving the character exactly as found
    local intBefore = sam_get_stat(0, "INT")
    local hadSpell  = sam_player_knows_spell(0, SPELL)
    local gave = sam_grant_item(0, BOOK)
    check("charm book granted to pack", gave ~= nil and gave ~= false, tostring(gave))
    checkPlayer(0)
    local unq = qualifies(raw, eff, ie)
    check("control: unqualified player not granted", unq or sam_player_knows_spell(0, SPELL) == false, tostring(sam_player_knows_spell(0, SPELL)))
    sam_set_stat(0, "INT", 90)
    checkPlayer(0)
    check("qualified player is granted", sam_player_knows_spell(0, SPELL) == true, tostring(sam_player_knows_spell(0, SPELL)))
    -- restore
    sam_set_stat(0, "INT", intBefore)
    if not hadSpell then sam_remove_spell(0, SPELL) end
    for _, it in ipairs(sam_get_inventory(0) or {}) do
      if it.name == BOOK then sam_remove_item(it.uid) end
    end
    check("INT restored", sam_get_stat(0, "INT") == intBefore, tostring(sam_get_stat(0, "INT")) .. " vs " .. tostring(intBefore))
  end
  sam_log(string.format("STUDY_ASSIST RESULT: %d passed, %d FAILED", pass, fail))
  sam_test_done(pass, fail)
end

function on_event(e)
  if e.name == "game.on_game_start" and e.player == 0 and sam_get_race(0) == "human" and sam_get_class(0) == "barbarian" and sam_get_time_played and (sam_get_time_played() or 0) < 500 and sam_get_seed and sam_get_seed() == 1514995713 then testTicksLeft = 50 end
  if e.name == "player.on_spell_failed" then
    sam_log(string.format("study_assist: spell failed player=%s spell=%s reason=%s",
      tostring(e.player), tostring(e.spell_name), tostring(e.reason)))
  end
end

function on_tick()
  if not sam_is_in_game() then return end
  if sam_is_paused() or sam_is_loading() then return end
  tickN = tickN + 1

  if testTicksLeft ~= nil then
    testTicksLeft = testTicksLeft - 1
    if testTicksLeft == 0 then testTicksLeft = nil; runChecks(); return end
  end

  if tickN == 100 then   -- two seconds in: record every player's numbers once
    for p = 0, sam_player_count() - 1 do
      local uid = sam_get_player_uid(p)
      if uid then sam_log("study_assist: start -- " .. (summary(p, uid))) end
    end
  end

  if tickN % CHECK_TICKS == 0 then
    for p = 0, sam_player_count() - 1 do
      local ok, err = pcall(checkPlayer, p)
      if not ok then sam_log("study_assist: ERROR " .. tostring(err)) end
    end
  end

  if tickN % 1500 == 0 then
    sam_log(string.format("study_assist: checks=%d granted=%d refused_logged=%d",
      stats.checks, stats.granted, stats.refused_logged))
  end
end
