-- BT1 trade gate (D73). Test mode only; run headless on a normal floor. Everything goes through the real engine hooks
-- (initShopkeeper stock, isItemSellableToShop, Item::sellValue, the purse check, checkEnemy); no UI is opened.
local pass, fail = 0, 0
local function check(name, cond, extra)
  if cond then pass = pass + 1; apx.log("TTEST ok   " .. name .. (extra and (" [" .. extra .. "]") or ""))
  else fail = fail + 1; apx.log("TTEST FAIL " .. name .. (extra and (" [" .. extra .. "]") or "")) end
  return cond
end
local function finish()
  apx.log(string.format("TTEST %s", fail == 0 and ("PASS " .. pass .. "/" .. (pass + fail)) or ("FAIL " .. fail .. " of " .. (pass + fail))))
  local st = apx.trade_state()
  apx.log(string.format("TTEST state shops=%d realized=%d pending_pouch=%d pending_service=%d class_refused=%d purse_refused=%d peddlers=%d on=%s",
    st.shops, st.rows_realized, st.rows_pending_pouch, st.rows_pending_service, st.class_refused, st.purse_refused, st.peddlers, tostring(st.on)))
  apx.test_quit(fail == 0 and 0 or 1)
end
-- classes the unit tests proved for these vanilla types (tools/apx/trade_units_test.cpp)
local PROBES = { IRON_SWORD = "metal", WOODEN_SHIELD = "wood", FOOD_BREAD = "provision", GEM_RUBY = "gem" }
local BUYS = { [0] = { metal = 1, stone = 1 }, [4] = { reagent = 1, shard = 1, gem = 1 }, [3] = { paper = 1, fibre = 1 },
               [6] = { provision = 1, wood = 1, hide = 1, fibre = 1, bone = 1 }, [2] = { gem = 1, shard = 1, other = 1 }, [7] = { provision = 1, wood = 1, hide = 1 } }
local TRADE_ITEMS = { TOOL_TORCH = 1, TOOL_LANTERN = 1, TOOL_METAL_SCRAP = 1, TOOL_MAGIC_SCRAP = 1, STEEL_SWORD = 1, IRON_SHIELD = 1,
                      BRONZE_AXE = 1, CHAIN_BOOTS = 1, CLOAK_PROTECTION = 1, HAT_PHRYGIAN = 1 }
local function allowed(t) return TRADE_ITEMS[t] or t:match("^FOOD_") ~= nil end

local function check_shop(uid, label)
  local info = apx.shop_info(uid)
  if not check(label .. ": shop exists", info ~= nil) then return nil end
  local bad = {}
  for _, it in ipairs(info.items) do if not allowed(it.type) then bad[#bad + 1] = it.type end end
  check(label .. ": stock has only trade goods (no finished goods, one rare piece max)", #bad == 0, #info.items .. " items" .. (#bad > 0 and (" bad: " .. table.concat(bad, ",")) or ""))
  local rare = 0
  for _, it in ipairs(info.items) do if TRADE_ITEMS[it.type] and not it.type:match("^TOOL_") then rare = rare + 1 end end
  check(label .. ": at most one rare finished piece", rare <= 1, tostring(rare))
  local buys = BUYS[info.store_type]
  check(label .. ": store type maps to a trade", buys ~= nil, tostring(info.store_type))
  if not buys then return info end
  for name, cls in pairs(PROBES) do
    local sellable, offer, vanilla, purse_refused = apx.shop_probe_sell(uid, name)
    local want = buys[cls] ~= nil
    check(string.format("%s: %s (%s) %s", label, name, cls, want and "bought" or "refused"), sellable == want)
    if sellable then check(label .. ": offer for " .. name .. " is about a third", offer >= 1 and offer <= vanilla // 3 + 1, offer .. " of " .. vanilla) end
  end
  return info
end

local started, poll = false, nil
poll = apx.every(10, function()
  if started or not apx.player_uid(0) then return end
  started = true; apx.cancel(poll)
  local px, py = apx.entity_pos(apx.player_uid(0))
  local on = apx.trade_state().on
  local gen = apx.spawn("shopkeeper", px + 5, py)
  local hav = apx.spawn_haven(px - 5, py)
  local ped = apx.spawn_peddler(px, py + 5)
  check("shopkeepers spawned", gen ~= nil and hav ~= nil and ped ~= nil)
  apx.after(10, function()
    if not on then   -- CONTROL (APX_TRADE=0): vanilla stock must trip the stock check
      check("CONTROL APX_TRADE=0 reports the layer off", apx.trade_state().on == false)
      check_shop(gen, "vanilla general")
      finish(); return
    end
    local st = apx.trade_state()
    check("three shops initialised by BT1", st.shops == 3, tostring(st.shops))
    check("rows not yet realizable (pouch/services) are counted, not dropped", st.rows_pending_pouch + st.rows_pending_service > 0,
      st.rows_pending_pouch .. "+" .. st.rows_pending_service)
    local gi = check_shop(gen, "general store")
    check("general store uses store type 7", gi ~= nil and gi.store_type == 7)
    local hi = check_shop(hav, "haven merchant")
    check("haven merchant is one of the five trades (not general)", hi ~= nil and hi.store_type ~= 7 and BUYS[hi.store_type] ~= nil)
    local pi = apx.shop_info(ped)
    check("peddler has a small stock and half a purse", pi ~= nil and #pi.items >= 1 and #pi.items <= 5, pi and (#pi.items .. " items, purse " .. pi.purse) or "nil")
    -- purse is finite: set it to 1 gold, an in-trade sale worth more than 1 must be refused
    local buys = BUYS[hi.store_type]
    local probe
    for name, cls in pairs(PROBES) do if buys[cls] then probe = name end end
    if probe then
      local _, offer0 = apx.shop_probe_sell(hav, probe)
      apx.shop_set_purse(hav, 0)
      local sellable, offer, _, refused = apx.shop_probe_sell(hav, probe)
      check("empty purse: the in-trade sale is refused by the purse", sellable and refused, probe .. " offer " .. offer)
      apx.shop_set_purse(hav, 100000)
      local _, _, _, refused2 = apx.shop_probe_sell(hav, probe)
      check("CONTROL: a full purse does not refuse", refused2 == false)
    end
    -- peddlers can be attacked; ordinary shopkeepers cannot
    local sk = apx.spawn("skeleton", px + 8, py + 8)
    check("a wild monster counts the peddler as an enemy", sk ~= nil and apx.check_enemy(sk, ped) == true)
    check("CONTROL: a wild monster does not attack the general store keeper", sk ~= nil and apx.check_enemy(sk, gen) == false)
    check("no script errors", apx.stats().errors == 0, tostring(apx.stats().errors))
    finish()
  end)
end)
apx.after(50 * 150, function() apx.log("TTEST FAIL timeout"); apx.test_quit(1) end)
