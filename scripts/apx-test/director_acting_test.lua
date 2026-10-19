-- Director acting gate: forced events execute within budget and their effects are real. Test mode only.
-- One process, phases in order; stop at the first decisive failure (D76). Needs a floor with sleeping monsters (floor 2).
local pass, fail = 0, 0
local function check(name, cond, extra)
  if cond then pass = pass + 1; apx.log("ATEST ok   " .. name .. (extra and (" [" .. extra .. "]") or ""))
  else fail = fail + 1; apx.log("ATEST FAIL " .. name .. (extra and (" [" .. extra .. "]") or "")) end
  return cond
end
local D = apx.shared.director
local core = apx.shared.director_core
local function finish()
  apx.log(string.format("ATEST %s", fail == 0 and ("PASS " .. pass .. "/" .. (pass + fail)) or ("FAIL " .. fail .. " of " .. (pass + fail))))
  apx.test_quit(fail == 0 and 0 or 1)
end
local function after_decisions(n, fn)
  local target = D.decisions + n
  local id; id = apx.every(10, function() if D.decisions >= target then apx.cancel(id); fn() end end)
end
local function decisive(ok) if not ok then finish() end return ok end

local started, poll = false, nil
poll = apx.every(10, function()
  if started or not apx.player_uid(0) then return end
  started = true; apx.cancel(poll)
  D.events_off = true
  after_decisions(2, function()
    local pu = apx.player_uid(0)
    -- PHASE A: ambush
    local before = apx.monster_count()
    D.events_off = false; D.force = "ambush"
    after_decisions(1, function()
      D.events_off = true
      local r = D.info.ambush
      if not decisive(check("ambush executed", r ~= nil and r.spawned >= 1, r and ("spawned " .. r.spawned) or "no result")) then return end
      local alive = 0
      for _, u in ipairs(r.uids) do if apx.get_stat(u, "hp") then alive = alive + 1 end end
      apx.log(string.format("ATEST info: monster_count before %d after %d; spawned bodies with stats %d of %d", before, apx.monster_count(), alive, r.spawned))
      check("every spawned body exists with stats (floor counts fluctuate naturally, so the raw count is not asserted)", alive == r.spawned, alive .. " of " .. r.spawned)
      local spent = 0
      for _, k in ipairs(r.pack) do spent = spent + core.cost_of(k) end
      local row_total = D.banked and D.last and true
      local budget = core.budget_total(math.max(1, apx.floor()), 1.0, 1, D.me.temper, D.tension.ewma, false)
      check("pack cost within the ambush share of the budget", spent <= budget * core.COST_SHARE.ambush + 1e-9, string.format("spent %d of %.1f", spent, budget * core.COST_SHARE.ambush))
      local ok = #r.dists > 0
      local lo, hi = 1e9, 0
      for _, d in ipairs(r.dists) do lo = math.min(lo, d); hi = math.max(hi, d); if d < 9 or d > 28 then ok = false end end
      check("pack spawned 9..28 tiles from the party (target 14..22, nearest free tile)", ok, string.format("%.1f..%.1f", lo, hi))
      check("default ambush (14..22 tiles): none of the pack spawned in line of sight", r.visible == 0, string.format("%d of %d visible", r.visible, r.spawned))
      apx.after(110, function()   -- the director sends the pack after 100 ticks; sample 10 ticks later, before they can lose the target
        local hunting = 0
        for _, u in ipairs(r.uids) do local st = apx.monster_state(u); if st and st ~= 0 then hunting = hunting + 1 end end
        check("after the 2 s cue the pack hunts (state != WAIT)", hunting >= 1, hunting .. " of " .. #r.uids)
        -- keep the player alive for the next phases
        apx.set_stat(pu, "hp", apx.get_stat(pu, "maxhp"))
        -- PHASE C: den
        D.events_off = false; D.force = "den"
        after_decisions(1, function()
          D.events_off = true
          local d = D.info.den
          if not decisive(check("den found sleepers 20+ tiles away on this floor", d ~= nil and d.eligible >= 1, d and ("eligible " .. d.eligible) or "no result")) then return end
          check("den woke at least one", d.woken >= 1, d.woken .. " of " .. d.eligible)
          apx.after(50, function()
            local awake = 0
            for _, u in ipairs(d.uids or {}) do local st = apx.monster_state(u); if st and st ~= 0 then awake = awake + 1 end end
            check("woken monsters left WAIT", awake >= 1, awake .. " awake")
            -- PHASE D: patrol
            D.events_off = false; D.force = "patrol"
            after_decisions(1, function()
              D.events_off = true
              local p = D.info.patrol
              check("patrol found an idle monster", p ~= nil and p.eligible >= 1, p and ("eligible " .. p.eligible) or "no result")
              check("patrol path was generated", p ~= nil and p.moved == 1)
              check("stubs are counted as declines, never silent", D.declines.event_unavailable > 0, tostring(D.declines.event_unavailable))
              local rows_ok = apx.stats().errors == 0
              check("no script errors in the whole acting run", rows_ok, "errors=" .. apx.stats().errors)
              -- PHASE A2: line of sight. At 3..5 tiles almost no point is hidden: the check must decline or hide, never spawn in sight.
              apx.set_stat(pu, "hp", apx.get_stat(pu, "maxhp"))
              D.ambush_radius = { 3, 5 }; D.events_off = false; D.force = "ambush"
              after_decisions(1, function()
                D.events_off = true
                local n = D.info.ambush
                check("LOS check on, ambush at 3..5 tiles: nothing spawns in sight", n ~= nil and n.visible == 0,
                  string.format("spawned %d, visible %d, candidates rejected %d", n and n.spawned or -1, n and n.visible or -1, D.declines.ambush_candidate_visible or 0))
                check("rejected candidates are counted (never silent)", (D.declines.ambush_candidate_visible or 0) > 0, tostring(D.declines.ambush_candidate_visible))
                apx.set_stat(pu, "hp", apx.get_stat(pu, "maxhp"))
                D.no_los_check = true; D.events_off = false; D.force = "ambush"
                after_decisions(1, function()
                  D.events_off = true
                  local c = D.info.ambush
                  check("CONTROL: LOS check off at 3..5 tiles spawns in sight (the gate has teeth)", c ~= nil and c.visible > 0,
                    string.format("spawned %d, visible %d", c and c.spawned or -1, c and c.visible or -1))
                  D.no_los_check = false; D.ambush_radius = nil
                  finish()
                end)
              end)
            end)
          end)
        end)
      end)
    end)
  end)
end)
apx.after(50 * 150, function() apx.log("ATEST FAIL timeout"); apx.test_quit(1) end)
