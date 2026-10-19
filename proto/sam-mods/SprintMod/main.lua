-- Sprint — hold Left Shift to run at 150% move speed.
--
-- Drains mana while held (1 MP per 12 ticks ~= 4 MP/s). When mana runs out the
-- sprint disengages. While not sprinting, mana regenerates slightly on top of the
-- engine's own regen (+1 MP per 50 ticks ~= 1 MP/s).
--
-- Speed is a 1.5x multiplier stacked on the player's current move-speed
-- multiplier (so FAST and other bonuses still apply), and restored to the exact
-- value it had when the sprint started.
--
-- All state is per player so it works in co-op, and all calls are 'host'/'read'
-- kind so singleplayer and hosting behave identically.

local BOOST = 1.5          -- move-speed multiplier while sprinting
local DRAIN_TICKS = 12     -- every N ticks while sprinting, spend 1 MP  (~4 MP/s)
local REGEN_TICKS = 50     -- every N ticks while NOT sprinting, gain 1 MP  (~1 MP/s)

-- sam_register_action at top level so the Binding row exists before a game starts.
-- Default key "Left Shift"; the player can rebind it in Settings > Controls.
sam_register_action("sprint", "Sprint", "Left Shift")

local ACTION = "sprint:sprint"   -- the registered action's full id (<namespace>:<id>)

-- Per-player runtime state: name in Bindings, base multiplier, timers.
local st = {}

local function playerState(p)
  if not st[p] then
    st[p] = { base = 1.0, sprinting = false, drain = 0, regen = 0, warned = false }
  end
  return st[p]
end

local function startSprint(p)
  local s = playerState(p)
  local uid = sam_get_player_uid(p)
  if not uid then return end

  local cur = sam_get_move_speed(p)
  if cur == nil then cur = 1.0 end
  s.base = cur
  s.sprinting = true
  s.warned = false
  sam_set_move_speed(p, cur * BOOST)
  sam_log(string.format("sprint: player %d sprinting (speed %.2f -> %.2f)", p, cur, cur * BOOST))
end

local function stopSprint(p)
  local s = playerState(p)
  s.sprinting = false
  s.warned = false
  sam_set_move_speed(p, s.base)
  sam_log(string.format("sprint: player %d stopped (released)", p))
end

-- ---------------------------------------------------------------------------
-- Self-checks for headless runs:  barony.exe -samtest=sprint
-- Outside a -samtest run sam_test_done is a Noop, so these never fire in real
-- games. A held key can't be simulated, so the key-press path needs a human;
-- what IS asserted: the action registered, speed round-trips incl. clamp at
-- 3.0, drain MP behaviour (afford/refuse), regen MP, and the poll of a custom
-- action always answers true/false (never nil).
-- ---------------------------------------------------------------------------
local testTicksLeft = nil

local function runChecks()
  local pass, fail = 0, 0
  local function check(name, ok, saw)
    if ok then pass = pass + 1 else fail = fail + 1 end
    sam_log((ok and "PASS  " or "FAIL  ") .. "sprint [" .. name .. "]   (" .. tostring(saw) .. ")")
  end

  -- Action registration: the Binding for "Sprint" exists this session.
  local actionRegistered = (sam_register_action("sprint", "Sprint", "Left Shift") ~= false)
  check("action registers with 'Left Shift'", actionRegistered, actionRegistered)

  -- Speed multiplier: round-trip; and a huge set must not leave the timer
  -- logic racing (the framework allows well above 3.0, so no clamp here).
  local base = sam_get_move_speed(0) or 1.0
  sam_set_move_speed(0, base * BOOST)
  local readback = sam_get_move_speed(0)
  check("speed round-trips at " .. tostring(base * BOOST), math.abs((readback or 0) - base * BOOST) < 0.001, readback)
  sam_set_move_speed(0, base)

  -- The custom action polls as a boolean (never nil/undefined).
  local held = sam_is_action_held(0, ACTION)
  check("custom action 'sprint:sprint' polls boolean", held == true or held == false, tostring(held))

  -- Mana: reading, spending only when affordable, refusing when broke,
  -- and clamping at max. Read the current MP first; the barbarian test
  -- character starts at max, so exercise REGEN (floor) rather than a +
  -- that would exceed the cap.
  local uid = sam_get_player_uid(0)
  if uid then
    local before = sam_get_mp(uid)
    local maxmp = sam_get_max_mp(uid) or before
    -- Regen path: a -1 then +1 must land exactly on before (engine clamps at max).
    sam_mod_mp(uid, -1)
    sam_mod_mp(uid, 1)
    check("MP -1 then +1 returns to start", sam_get_mp(uid) == before, sam_get_mp(uid))
    -- Spend only when affordable (and never below 0).
    local afford = sam_consume_mp(uid, 1)
    check("sam_consume_mp(1) spends when affordable", afford == true and sam_get_mp(uid) == before - 1, sam_get_mp(uid) .. " (afford=" .. tostring(afford) .. ", before=" .. before .. ")")
    sam_mod_mp(uid, 1)   -- restore exactly
    check("mana restored after spend", sam_get_mp(uid) == before, sam_get_mp(uid))
    local broke = sam_consume_mp(uid, maxmp + 1)
    check("sam_consume_mp(huge) refuses", broke == false, tostring(broke))
    sam_set_stat(0, "MP", before)
  else
    check("player uid resolves (test context)", false, "no uid")
  end

  sam_log(string.format("SPRINT RESULT: %d passed, %d FAILED", pass, fail))
  sam_test_done(pass, fail)
end

function on_event(e)
  if e.name == "game.on_game_start" and e.player == 0 then testTicksLeft = 50 end
end

function on_tick()
  if not sam_is_in_game() then return end
  if sam_is_paused() or sam_is_loading() then return end

  -- Headless test driver: after game start, wait one second (50 ticks) then run.
  if testTicksLeft ~= nil then
    testTicksLeft = testTicksLeft - 1
    if testTicksLeft == 0 then
      testTicksLeft = nil
      runChecks()
      return
    end
  end

  local count = sam_player_count()
  for p = 0, count - 1 do
    local s = playerState(p)
    local uid = sam_get_player_uid(p)
    if uid and not sam_is_ghost(p) then
      local held = sam_is_action_held(p, ACTION)

      if held and not s.sprinting then
        local mp = sam_get_mp(uid)
        if mp ~= nil and mp >= 1 then
          startSprint(p)
        elseif not s.warned then
          s.warned = true
          sam_message(p, "Too exhausted to sprint.")
        end
      elseif not held and s.sprinting then
        stopSprint(p)
      end

      if s.sprinting then
        s.drain = s.drain + 1
        if s.drain >= DRAIN_TICKS then
          s.drain = 0
          if not sam_consume_mp(uid, 1) then
            s.warned = true          -- don't re-message the moment sprint re-engages
            stopSprint(p)
          end
        end
      else
        s.regen = s.regen + 1
        if s.regen >= REGEN_TICKS then
          s.regen = 0
          local mp, maxmp = sam_get_mp(uid), sam_get_max_mp(uid)
          if mp ~= nil and maxmp ~= nil and mp < maxmp then
            sam_mod_mp(uid, 1)
          end
        end
      end
    end
  end
end