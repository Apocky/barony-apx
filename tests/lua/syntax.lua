-- Syntax check for every shipped and test Lua script (T0 lint, seconds, no Barony):  apx_lua_run.exe tests/lua/syntax.lua  (cwd = repo root)
local files = {
  "scripts/apx/hello.lua", "scripts/apx/director_core.lua", "scripts/apx/director.lua",
  "scripts/apx-test/control_error.lua", "scripts/apx-test/selftest.lua", "scripts/apx-test/director_test.lua",
  "scripts/apx-test/director_acting_test.lua", "scripts/apx-test/spawn_budget_test.lua", "scripts/apx-test/trade_test.lua",
}
local bad = 0
for _, f in ipairs(files) do
  local fn, err = loadfile(f)
  if not fn then bad = bad + 1; print("SYNTAX ERROR " .. err) end
end
print(bad == 0 and ("SYNTAX OK " .. #files .. " files") or ("SYNTAX FAIL " .. bad))
os.exit(bad == 0 and 0 or 1)
