-- Spawn budget gate: 200 spawn calls inside one second -> at most 40 accepted, the rest refused with reason "budget"
-- and counted. With APX_LUA_NOBUDGET=1 (the control) the same script must FAIL its first check. Test mode only.
local started, poll = false, nil
poll = apx.every(10, function()
  if started or not apx.player_uid(0) then return end
  started = true; apx.cancel(poll)
  apx.after(100, function()
    local px, py = apx.entity_pos(apx.player_uid(0))
    local ok, budget, nospace = 0, 0, 0
    for i = 1, 200 do
      local u, why = apx.spawn("rat", px + 3 + (i % 5), py + 3 + (i % 7))
      if u then ok = ok + 1 elseif why == "budget" then budget = budget + 1 else nospace = nospace + 1 end
    end
    local st = apx.stats()
    local pass = ok <= 40 and budget >= 100 and st.budget_refused_spawn == budget
    apx.log(string.format("BTEST accepted=%d refused_budget=%d no_room=%d counter=%d", ok, budget, nospace, st.budget_refused_spawn))
    apx.log(pass and "BTEST PASS: at most 40 accepted, refusals counted" or "BTEST FAIL: budget not enforced")
    apx.test_quit(pass and 0 or 1)
  end)
end)
apx.after(50 * 60, function() apx.log("BTEST FAIL timeout"); apx.test_quit(1) end)
