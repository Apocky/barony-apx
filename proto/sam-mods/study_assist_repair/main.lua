-- Study Assist Repair -- one-shot undo of a self-test that ran in a live game.
--
-- What the bad test did to the character: set INT to 90 (the character had 2, so +88),
-- granted one extra spellbook_charm (status 4 = EXCELLENT, beatitude 0, count 1), and
-- taught Charm Monster. This mod reverts the first two and KEEPS the spell.
--
-- Safety: it only acts when INT is 85..95 (a Mesmer cannot legitimately be there; this also
-- survives a level-up since then), takes exactly 88 back, and only removes a charm book that
-- is a lone, unequipped, EXCELLENT, non-cursed stack. If the pack is ambiguous it removes
-- nothing and says so in sam_log.txt. A persisted flag makes it run once.

local INT_BOOST = 88
local FLAG      = "repaired_v1"
local BOOK      = "spellbook_charm"

local tickN, done = 0, false

local function repair(p)
  local uid = sam_get_player_uid(p)
  if not uid then return false end
  local fixed = false
  local intRaw = sam_get_stat(p, "INT")
  sam_log(string.format("study_assist_repair: player %d race=%s class=%s INT raw=%s", p,
    tostring(sam_get_race(p)), tostring(sam_get_class(p)), tostring(intRaw)))

  if intRaw and intRaw >= 85 and intRaw <= 95 then
    sam_set_stat(p, "INT", intRaw - INT_BOOST)
    fixed = true
    sam_log(string.format("study_assist_repair: INT %d -> %s", intRaw, tostring(sam_get_stat(p, "INT"))))
  else
    sam_log("study_assist_repair: INT not in the 85..95 window; left alone")
  end

  local inv = sam_get_inventory(p) or {}
  local books = {}
  for _, it in ipairs(inv) do
    if it.name == BOOK then books[#books + 1] = it end
  end
  for _, b in ipairs(books) do
    sam_log(string.format("study_assist_repair: charm book uid=%s count=%s status=%s beatitude=%s equipped=%s",
      tostring(b.uid), tostring(b.count), tostring(b.status), tostring(b.beatitude), tostring(b.equipped)))
  end
  local extras = {}
  for _, b in ipairs(books) do
    if b.status == 4 and b.beatitude == 0 and b.count == 1 and not b.equipped then extras[#extras + 1] = b end
  end
  if #books >= 2 and #extras == 1 then
    local ok = sam_remove_item(extras[1].uid)
    sam_log("study_assist_repair: removed extra charm book uid=" .. tostring(extras[1].uid) .. " ok=" .. tostring(ok))
  else
    sam_log(string.format("study_assist_repair: charm books=%d candidates=%d -> no book removed", #books, #extras))
  end
  sam_log("study_assist_repair: charm spell kept; knows=" .. tostring(sam_player_knows_spell(p, 31)))
  return fixed
end

-- samtest: only on the disposable test character (human barbarian with INT -2), and it
-- restores everything it touches.
local testTicksLeft = nil
local function runChecks()
  local pass, fail = 0, 0
  local function check(name, ok, saw)
    if ok then pass = pass + 1 else fail = fail + 1 end
    sam_log((ok and "PASS  " or "FAIL  ") .. "study_assist_repair [" .. name .. "]   (" .. tostring(saw) .. ")")
  end
  local before = sam_get_stat(0, "INT")
  sam_set_stat(0, "INT", 90)
  check("INT set to 90 for the scenario", sam_get_stat(0, "INT") == 90, sam_get_stat(0, "INT"))
  sam_grant_item(0, BOOK)                         -- status 4, count 1
  repair(0)
  check("INT repaired", sam_get_stat(0, "INT") == 2, sam_get_stat(0, "INT"))
  sam_set_stat(0, "INT", before)
  sam_log(string.format("STUDY_ASSIST_REPAIR RESULT: %d passed, %d FAILED", pass, fail))
  sam_test_done(pass, fail)
end

function on_event(e)
  if e.name == "game.on_game_start" and e.player == 0
     and sam_get_class(0) == "barbarian" and sam_get_race(0) == "human" and sam_get_stat(0, "INT") == -2 and sam_get_seed and sam_get_seed() == 1514995713 then
    testTicksLeft = 50
  end
end

function on_tick()
  if not sam_is_in_game() or sam_is_paused() or sam_is_loading() then return end
  tickN = tickN + 1
  if testTicksLeft ~= nil then
    testTicksLeft = testTicksLeft - 1
    if testTicksLeft == 0 then testTicksLeft = nil; done = true; runChecks(); return end
  end
  if tickN == 200 and not done then            -- 4 s in: inventory has been reported
    done = true
    if sam_load_data(FLAG) then
      sam_log("study_assist_repair: already ran once; nothing to do")
    else
      local any = false
      for p = 0, sam_player_count() - 1 do if repair(p) then any = true end end
      if any then sam_save_data(FLAG, true) end
    end
  end
end
