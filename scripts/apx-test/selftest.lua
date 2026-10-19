-- Self-test (only loaded with -apx-luatest=<seed>; never in a live game).
local pass, fail = 0, 0
local function check(name, cond)
  if cond then pass = pass + 1; apx.log("TEST ok   " .. name)
  else fail = fail + 1; apx.log("TEST FAIL " .. name) end
end
local seen = { level = false, spawned = false, damage = false, death = false, tick = 0 }
local rat, base

apx.on("level_entered", function(ev)
  seen.level = true
  check("level_entered carries floor/seed/size", ev.floor ~= nil and ev.seed ~= nil and ev.width > 0)
end)
apx.after(50 * 120, function() if not started then apx.log("APX-LUA-TEST FAIL no player after 120 s"); apx.test_quit(1) end end)
apx.on("monster_spawned", function(m) if m.source == "summon" and m.kind == "rat" then seen.spawned = m.uid end end)
apx.on("damage", function(d) if d.uid == rat then seen.damage = d.amount end end)
apx.on("death", function(d) if d.uid == rat then seen.death = true end end)

local started = false
local poll
poll = apx.every(10, function()
  if started or not apx.player_uid(0) then return end   -- wait for a real game (the menu map has no player)
  started = true; apx.cancel(poll)
  base = apx.monster_count()
  local px, py = 20, 20
  local pu = apx.player_uid(0)
  if pu then local x, y = apx.entity_pos(pu); if x then px, py = x + 2, y end end
  rat = apx.spawn("rat", px, py)
  check("spawn returns a uid", rat ~= nil)
  check("monster_count rose by 1", apx.monster_count() == (base or 0) + 1)
  check("monster_spawned event fired for the summon", seen.spawned == rat)
  check("unknown monster is refused", apx.spawn("not_a_monster", px, py) == nil)
  check("spawn outside the map is refused", apx.spawn("rat", -5, 3) == nil)
  local hp = apx.get_stat(rat, "hp")
  check("get_stat hp > 0", hp ~= nil and hp > 0)
  check("set_stat hp", apx.set_stat(rat, "hp", 3) and apx.get_stat(rat, "hp") == 3)
  local t = apx.get_tile(1, 1, 0)
  check("tile round trip", t ~= nil and apx.set_tile(1, 1, 0, t) and apx.get_tile(1, 1, 0) == t)
  check("sandbox: io is absent", io == nil and os == nil and dofile == nil and require == nil and load == nil)
  check("sandbox: apx is read-only", not pcall(function() apx.log = nil end))
  apx.message("Lua layer self-test running")
  apx.hurt(rat, 1)
  check("damage event fired", seen.damage == 1)
  apx.hurt(rat, 50)
  check("death event fired", seen.death)
  apx.after(30, function()
    local st = apx.stats()
    check("control: errors were caught (>= 3) and the game kept running", st.errors >= 3)
    check("control: the looping handler did not hang the game", true)
    check("hello script loaded", st.scripts_loaded >= 3)
    check("level_entered seen", seen.level)
    if fail == 0 then apx.log(string.format("APX-LUA-TEST PASS %d/%d", pass, pass + fail)) else apx.log(string.format("APX-LUA-TEST FAIL %d failed of %d", fail, pass + fail)) end
    apx.test_quit(fail == 0 and 0 or 1)
  end)
end)
