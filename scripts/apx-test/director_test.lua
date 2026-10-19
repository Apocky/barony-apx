-- Director gate (BF5-1/2 reading half). Test mode only.
local pass, fail = 0, 0
local function check(name, cond, extra)
  if cond then pass = pass + 1; apx.log("DTEST ok   " .. name .. (extra and (" [" .. extra .. "]") or ""))
  else fail = fail + 1; apx.log("DTEST FAIL " .. name .. (extra and (" [" .. extra .. "]") or "")) end
end
local D = apx.shared.director
-- 1. identity: deterministic, spread across seeds
do
  local seen, n = {}, 0
  local same = true
  for s = 1, 20 do
    local a, b = D.identity(s * 7919), D.identity(s * 7919)
    if a.name ~= b.name or a.temper ~= b.temper then same = false end
    local k = a.name .. "/" .. a.temper
    if not seen[k] then seen[k] = true; n = n + 1 end
  end
  check("identity is deterministic for the same seed", same)
  check("20 seeds give at least 8 distinct (name, temper) pairs", n >= 8, n .. " distinct")
  -- control: a constant seed must NOT spread (the distinctness check has teeth)
  local c, cn = {}, 0
  for s = 1, 20 do local a = D.identity(42); local k = a.name .. a.temper; if not c[k] then c[k] = true; cn = cn + 1 end end
  check("control: constant seed gives 1 pair", cn == 1)
end

local started, poll = false, nil
local function hurt_burst(u)
  local mhp = apx.get_stat(u, "maxhp")
  for _ = 1, 3 do apx.hurt(u, math.max(1, math.floor(mhp * 0.15))) end
end
local function after_decisions(n, fn)  -- run fn after n more decisions
  local target = D.decisions + n
  local id; id = apx.every(10, function() if D.decisions >= target then apx.cancel(id); fn() end end)
end

poll = apx.every(10, function()
  if started or not apx.player_uid(0) then return end
  started = true; apx.cancel(poll)
  local u = apx.player_uid(0)
  after_decisions(6, function()
    local base = D.tension.ewma
    check("run identity equals identity(run_seed)", D.me and D.identity(apx.run_seed()).name == D.me.name)
    check("quiet baseline ewma < 0.35", base < 0.35, string.format("%.3f", base))
    hurt_burst(u)
    after_decisions(2, function()
      local up = D.tension.ewma
      check("tension rises within 2 decisions of a damage burst", up >= base + 0.04, string.format("%.3f -> %.3f", base, up))
      apx.set_stat(u, "hp", apx.get_stat(u, "maxhp"))
      after_decisions(12, function()
        local down = D.tension.ewma
        check("tension falls back within 12 decisions", down < up * 0.6 or down < 0.2, string.format("%.3f -> %.3f", up, down))
        -- CONTROL: a blind director (damage ignored) must NOT rise; the same rise check would fail against it
        D.blind = true
        local b0 = D.tension.ewma
        hurt_burst(u)
        after_decisions(2, function()
          local b1 = D.tension.ewma
          check("control: blind director does not rise (the rise check would fail)", not (b1 >= b0 + 0.04), string.format("%.3f -> %.3f", b0, b1))
          D.blind = false; apx.set_stat(u, "hp", apx.get_stat(u, "maxhp"))
          local ok = #D.ticks >= 20
          for i = 2, #D.ticks do if D.ticks[i] - D.ticks[i - 1] ~= 50 then ok = false end end
          check("decisions are exactly 50 ticks apart (>= 20 of them)", ok, #D.ticks .. " decisions")
          local first = D.us[1]
          local steady = {}
          for i = 2, #D.us do steady[#steady + 1] = D.us[i] end
          table.sort(steady)
          local p99 = steady[math.max(1, math.ceil(#steady * 0.99))]
          local med = steady[math.ceil(#steady / 2)]
          apx.log(string.format("DTEST cost: first decision %d us (one-time: identity, log, file open), steady median %d us, p99 %d us, n=%d", first, med, p99, #steady))
          check("steady-state decision cost p99 under 200 us", p99 < 200, "p99=" .. p99 .. "us")
          check("first decision under 5000 us", first < 5000, first .. "us")
          apx.log(string.format("DTEST %s", fail == 0 and ("PASS " .. pass .. "/" .. (pass + fail)) or ("FAIL " .. fail)))
          apx.test_quit(fail == 0 and 0 or 1)
        end)
      end)
    end)
  end)
end)
