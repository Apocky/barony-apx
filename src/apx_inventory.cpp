// apx: our inventory, step 1 (virtual grid + mass limit) and the step 1-2 self-test. See apx_inventory.hpp.
#include "apx_inventory.hpp"
#include "apx/selftest_all.hpp"
#include "apx/headless.hpp"
#include "apx/parts_engine.hpp"
#include "game.hpp"
#include "stat.hpp"
#include "items.hpp"
#include "player.hpp"
#include "scores.hpp"
#include "net.hpp"
#include "entity.hpp"
#include "interface/interface.hpp"
#include "prng.hpp"
#include "input.hpp"
#include "menu.hpp"
#include "monster.hpp"
#include "apx_bench_api.hpp"
#include "apx/headless.hpp"
#include <cmath>
#include "interface/interface.hpp"
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <set>
#include <sstream>

namespace apx_inv
{

bool vanillaGrid()
{
	static const bool v = getenv("APX_GRID") && getenv("APX_GRID")[0] == '1';
	return v;
}

int virtualRows(int sizeX)
{
	if ( sizeX < 1 ) { sizeX = 1; }
	return (VIRTUAL_SLOTS + sizeX - 1) / sizeX;
}

static const char* KIND_NAMES[KIND_MAX] = { "Weapons", "Armour", "Tools", "Consumables", "Materials", "Books", "Gold" };
const char* kindName(int kind) { return kind >= 0 && kind < KIND_MAX ? KIND_NAMES[kind] : "?"; }

int kindOf(const Item& item)
{
	switch ( itemCategory(&item) )
	{
		case WEAPON: case THROWN: case MAGICSTAFF: return WEAPONS;
		case ARMOR: case AMULET: case RING: return ARMOUR;
		case TOOL: return TOOLS;
		case POTION: case SCROLL: case FOOD: return CONSUMABLES;
		case GEM: return MATERIALS;
		case BOOK: case SPELLBOOK: case TOME_SPELL: return BOOKS;
		default: return TOOLS;
	}
}

std::vector<Row> rows(int player)
{
	std::vector<Row> out;
	if ( player < 0 || player >= MAXPLAYERS || !stats[player] ) { return out; }
	for ( node_t* node = stats[player]->inventory.first; node; node = node->next )
	{
		Item* item = static_cast<Item*>(node->element);
		if ( !item || item->type < 0 || item->type >= NUMITEMS || itemCategory(item) == SPELL_CAT ) { continue; }
		Row r;
		r.uid = item->uid;
		r.type = item->type;
		r.kind = kindOf(*item);
		r.count = item->count;
		r.mass = item->getWeight();
		r.status = item->status;
		r.beatitude = item->beatitude;
		r.equipped = players[player]->paperDoll.enabled && players[player]->paperDoll.isItemOnDoll(*item);
		r.x = item->x;
		r.y = item->y;
		out.push_back(r);
	}
	if ( !vanillaGrid() && apx_bench::pouchUnits(player) > 0 )
	{
		Row p;
		p.kind = MATERIALS;
		p.type = -2;           // the pouch row (D85)
		p.mass = (Sint32)apx_bench::pouchUnits(player);
		out.push_back(p);
	}
	if ( stats[player]->GOLD > 0 )
	{
		Row g;
		g.kind = GOLD;
		g.count = stats[player]->GOLD;
		g.mass = stats[player]->getGoldWeight();
		out.push_back(g);
	}
	return out;
}

Sint32 carriedMass(int player)
{
	if ( player < 0 || player >= MAXPLAYERS || !stats[player] ) { return 0; }
	// includes the D85 material pouch: getCharacterWeight adds it (actplayer.cpp), so movement sees it too
	return players[player]->movement.getCharacterWeight();
}

Sint32 massLimit(int player)
{
	if ( player < 0 || player >= MAXPLAYERS || !stats[player] ) { return 0; }
	Sint32 str = statGetSTR(stats[player], players[player]->entity);
	return 1000 + 100 * std::max<Sint32>(0, str);
}

bool canCarry(int player, const Item& item)
{
	if ( vanillaGrid() ) { return true; }
	if ( player < 0 || player >= MAXPLAYERS || !players[player]->isLocalPlayer() ) { return true; } // the server does not hold a client's inventory
	return carriedMass(player) + item.getWeight() <= massLimit(player);
}

std::string checkRows(const std::vector<Row>& r, Sint32 carried)
{
	long long sum = 0;
	for ( auto& row : r ) { sum += row.mass; }
	if ( sum != carried )
	{
		std::ostringstream s;
		s << "rows sum " << sum << " != carried " << carried;
		return s.str();
	}
	return "";
}

std::string massText(Sint32 units)
{
	char buf[32];
	snprintf(buf, sizeof(buf), "%d.%d kg", units / 10, std::abs(units % 10));
	return buf;
}

// ---------------------------------------------------------------- self-test
static std::string g_test;
static int g_ticks = 0, g_phase = 0, g_mark = 0;
static std::string g_shot;
static int g_loadedItems = 0;
// -apxselftest=all (D77): the gates' verdict and numbers for the "inventory" stage
static bool g_gatesPass = false;
static std::string g_gatesFields;
static void allModeDone(const std::string& extra)
{
	std::string fields = g_gatesFields + extra;
	apx::selftestall::done(apx::selftestall::INVENTORY, g_gatesPass, fields);
}
static void runGates();
static void runUiPhases();

void selfTestArm(const char* which) { g_test = which ? which : ""; }

static std::string diagPath()
{
	const char* d = getenv("APX_DIAG_DIR");
	std::string dir = d && d[0] ? d : "C:/Apocrypha/diagnostics/barony-u1";
	std::error_code ec;
	std::filesystem::create_directories(dir, ec);
	return dir + "/selftest-inventory.jsonl";
}

static void emptyInventory(int p)
{
	Stat* s = stats[p];
	s->helmet = s->breastplate = s->gloves = s->shoes = s->shield = s->weapon = nullptr;
	s->cloak = s->amulet = s->ring = s->mask = nullptr;
	for ( auto& h : players[p]->hotbar.slots() ) { h.item = 0; h.resetLastItem(); }
	list_FreeAll(&s->inventory);
	s->GOLD = 0;
}

static void randomInventory(int p, BaronyRNG& rng, int n)
{
	emptyInventory(p);
	for ( int k = 0; k < n; ++k )
	{
		ItemType t;
		do { t = static_cast<ItemType>(rng.getU32() % NUMITEMS); } while ( items[t].category == SPELL_CAT );
		Sint16 count = (Sint16)(1 + rng.getU32() % 12);
		Status st = static_cast<Status>(1 + rng.getU32() % 4);
		Sint16 beat = (Sint16)((int)(rng.getU32() % 5) - 2);
		newItem(t, st, beat, count, rng.getU32(), (rng.getU32() & 1) != 0, &stats[p]->inventory);
	}
	stats[p]->GOLD = (Sint32)(rng.getU32() % 2000);
}

static std::string snapshot(int p)
{
	std::ostringstream s;
	std::vector<Uint32> uids;
	for ( node_t* node = stats[p]->inventory.first; node; node = node->next )
	{
		Item* i = static_cast<Item*>(node->element);
		uids.push_back(i->uid);
		s << i->type << ',' << (int)i->status << ',' << i->beatitude << ',' << i->count << ',' << i->appearance << ',' << (int)i->identified << ',' << i->x << ',' << i->y << ';';
	}
	s << "|hotbar:";
	for ( auto& h : players[p]->hotbar.slots() )
	{
		int idx = -1;
		for ( size_t k = 0; k < uids.size(); ++k ) { if ( uids[k] == h.item && h.item ) { idx = (int)k; } }
		s << idx << ',';
	}
	s << "|gold:" << stats[p]->GOLD;
	return s.str();
}

void selfTestTick()
{
	if ( g_test.empty() || intro || !players[0] || !players[0]->entity || !stats[0] ) { return; }
	if ( apx::selftestall::armed() && !apx::selftestall::active(apx::selftestall::INVENTORY) ) { return; } // D77: wait for the stage
	// headless runs 50 logic ticks per UI pass: the UI phases advance one step per UI frame there,
	// so every injected tap and read sees the list update in between (windowed: one tick = one frame)
	static unsigned seenUiFrame = 0;
	if ( apx::headlessOn && g_phase >= 1 )
	{
		if ( uiFrames() == seenUiFrame ) { return; }
		seenUiFrame = uiFrames();
	}
	++g_ticks;
	if ( g_ticks < 120 ) { return; }
	if ( g_phase == 0 )
	{
		runGates(); g_phase = 1; g_mark = g_ticks;
		if ( apx::selftestall::armed() && apx::headlessOn ) // the UI phases need a visible window (keys, screenshots)
		{
			g_test.clear();
			allModeDone(",\"ui\":\"skipped_headless\"");
		}
		return;
	}
	runUiPhases();
}

static void runGates()
{
	const std::string which = g_test;
	FILE* f = fopen(diagPath().c_str(), "a");
	const char* mode = vanillaGrid() ? "vanilla" : "apx";
	auto& inv = players[0]->inventoryUI;
	auto t0 = std::chrono::steady_clock::now();
	BaronyRNG rng;
	rng.seedBytes("apxu1", 5);

	// Gate 1: carried mass equals the sum of the item rows on 100 random inventories; slots unique
	int massOk = 0, controlCaught = 0, slotClash = 0, outOfGrid = 0, maxItems = 0, maxY = 0;
	int carryChecks = 0, carryRight = 0;
	for ( int t = 0; t < 100; ++t )
	{
		int n = 1 + (int)(rng.getU32() % 120);
		randomInventory(0, rng, n);
		auto r = rows(0);
		Sint32 carried = carriedMass(0);
		std::string why = checkRows(r, carried);
		massOk += why.empty();
		if ( !why.empty() && f ) { fprintf(f, "{\"test\":\"mass\",\"mode\":\"%s\",\"trial\":%d,\"fail\":\"%s\"}\n", mode, t, why.c_str()); }
		// control: one row with a wrong mass must be caught
		if ( !r.empty() )
		{
			auto bad = r;
			bad[rng.getU32() % bad.size()].mass += 1;
			controlCaught += !checkRows(bad, carried).empty();
		}
		std::set<std::pair<int, int>> seen;
		for ( auto& row : r )
		{
			if ( row.type < 0 || row.equipped ) { continue; }
			if ( row.x < 0 || row.y < 0 || row.x >= inv.getSizeX() || row.y >= inv.getSizeY() ) { ++outOfGrid; continue; }
			slotClash += !seen.insert({ row.x, row.y }).second;
			maxY = std::max(maxY, (int)row.y);
		}
		maxItems = std::max(maxItems, (int)r.size());
		// the carry rule against an independent sum
		Item probe{};
		probe.type = static_cast<ItemType>(rng.getU32() % NUMITEMS);
		probe.count = (Sint16)(1 + rng.getU32() % 50);
		bool expect = vanillaGrid() || carried + probe.getWeight() <= massLimit(0);
		++carryChecks;
		carryRight += canCarry(0, probe) == expect;
	}

	// Gate 2: save and load keep every item and hotbar binding (byte compare of the item rows)
	randomInventory(0, rng, 80);
	{
		int k = 0;
		for ( node_t* node = stats[0]->inventory.first; node && k < NUM_HOTBAR_SLOTS; node = node->next, ++k )
		{
			Item* i = static_cast<Item*>(node->element);
			if ( k % 2 == 0 ) { players[0]->hotbar.slots()[k].item = i->uid; }
		}
	}
	std::string before = snapshot(0);
	int saved = saveGame(0);
	SaveGameInfo info = getSaveGameInfo(true, 0);
	emptyInventory(0);
	int loaded = loadGame(0, info);
	std::string after = snapshot(0);
	g_loadedItems = (int)list_Size(&stats[0]->inventory);
	bool same = before == after;
	// control: the compare must see a single changed row
	std::string tampered = before;
	tampered[tampered.find(',') - 1] ^= 1;
	bool controlSave = tampered != after;
	double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();

	if ( f )
	{
		fprintf(f, "{\"test\":\"inventory\",\"which\":\"%s\",\"mode\":\"%s\",\"size\":[%d,%d],\"mass_ok\":%d,\"mass_trials\":100,\"wrong_row_caught\":%d,"
			"\"slot_clash\":%d,\"out_of_grid\":%d,\"max_rows\":%d,\"max_y\":%d,\"carry_rule_right\":%d,\"carry_checks\":%d,\"limit\":%d,"
			"\"save_rc\":%d,\"load_rc\":%d,\"save_load_same\":%s,\"save_control_caught\":%s,\"items_saved\":%d,\"ms\":%.1f}\n",
			which.c_str(), mode, inv.getSizeX(), inv.getSizeY(), massOk, controlCaught, slotClash, outOfGrid, maxItems, maxY, carryRight, carryChecks,
			massLimit(0), saved, loaded, same ? "true" : "false", controlSave ? "true" : "false", (int)list_Size(&stats[0]->inventory), ms);
		if ( !same ) { fprintf(f, "{\"test\":\"save_diff\",\"before\":\"%.400s\",\"after\":\"%.400s\"}\n", before.c_str(), after.c_str()); }
		fclose(f);
	}
	printlog("[apx] inventory self-test gates (%s): mass %d/100, slot clashes %d, save/load %s", mode, massOk, slotClash, same ? "same" : "DIFFERENT");
	{
		char buf[320];
		snprintf(buf, sizeof buf, "\"mode\":\"%s\",\"mass_ok\":%d,\"wrong_row_caught\":%d,\"slot_clash\":%d,\"out_of_grid\":%d,"
			"\"carry_rule_right\":%d,\"carry_checks\":%d,\"save_load_same\":%s,\"save_control_caught\":%s",
			mode, massOk, controlCaught, slotClash, outOfGrid, carryRight, carryChecks, same ? "true" : "false", controlSave ? "true" : "false");
		g_gatesFields = buf;
		g_gatesPass = !vanillaGrid() && massOk == 100 && controlCaught >= 95 && slotClash == 0 && outOfGrid == 0
			&& carryRight == carryChecks && same && controlSave;
	}
}

// Step 2 phases: the list on screen full size, gamepad and keyboard navigation, then the
// split-screen quarter layout (player 0's view set to a quadrant the way setupSplitscreen does).
// Steps 3-5, deterministic checks run once (D76), full screen, between the nav checks and the quarter.
static Item* firstLoose(int p, bool single)
{
	for ( node_t* n = stats[p]->inventory.first; n; n = n->next )
	{
		Item* it = static_cast<Item*>(n->element);
		if ( it && itemCategory(it) != SPELL_CAT && !(players[p]->paperDoll.enabled && players[p]->paperDoll.isItemOnDoll(*it))
			&& (!single || it->count == 1) ) { return it; }
	}
	return nullptr;
}
static Entity* g_testChest = nullptr;
static Entity* g_testFollower = nullptr;
static Entity* g_testDeco = nullptr;
static Entity* g_testStation = nullptr;
// a station or furniture prop placed exactly as map generation places it (maps.cpp cases 300-302):
// 4x4, on the floor at z 7.5, upright, facing one of four ways
static Entity* spawnProp(int sprite, void (*behavior)(Entity*), real_t x, real_t y)
{
	Entity* e = newEntity(sprite, 1, map.entities, nullptr);
	e->sizex = 4; e->sizey = 4;
	e->x = x; e->y = y; e->z = 7.5;
	e->yaw = 0; e->pitch = 0; e->roll = 0;
	e->flags[PASSABLE] = false;
	e->behavior = behavior;
	return e;
}
static bool propPlacedRight(const Entity* e) { return e && std::abs(e->z - 7.5) < 0.01 && e->pitch == 0 && e->roll == 0 && e->sizex == 4; }
static struct S345 {
	int menuOpen = 0, menuItemRight = 0, inspectShown = 0, breakGone = 0;
	int chestFound = 0, chestView = 0, chestRows = 0, takeOk = 0, storeOk = 0, heavyRefused = 0;
	int followerFound = 0, followerView = 0, giveOk = 0, followerTakeOk = 0;
	int benchControl = -1, benchShown = 0, benchStation = 0, benchOps = 0;
	int propOk = 0;
	int benchTab0Rows = 0, benchTab1Rows = 0, benchTab2Rows = 0, benchTab1 = -1, benchTab2 = -1;
	int benchShot = 0, followerTicks = 0, followerOpen = 0, benchOpen = 0, followerWindow = 0, benchTicks = 0, benchWindow = 0, giveAt = -1, takeAt = -1;
} R;


static void probe(const char* tag)
{
	FILE* f = fopen(diagPath().c_str(), "a");
	if ( !f ) { return; }
	auto& pl = *players[0];
	auto* ui = inputs.getUIInteraction(0);
	const auto& vs = viewState(0);
	const auto& st = listState(0);
	double fd = -1;
	if ( g_testFollower && pl.entity ) { fd = sqrt((g_testFollower->x - pl.entity->x) * (g_testFollower->x - pl.entity->x) + (g_testFollower->y - pl.entity->y) * (g_testFollower->y - pl.entity->y)); }
	fprintf(f, "{\"test\":\"probe\",\"tag\":\"%s\",\"active\":%d,\"gui_mode\":%d,\"shootmode\":%d,\"inv_mode\":%d,\"menu_open\":%d,\"menu_item\":%u,"
		"\"sel\":%d,\"sel_uid\":%u,\"inspect\":%d,\"other\":%d,\"focus\":%d,\"station\":%d,\"bench\":%d,\"bench_rows\":%d,\"bench_tab\":%d,"
		"\"followers\":%u,\"follower_dist\":%.1f,\"inv_x\":%d,\"inv_w\":%d,\"inv_disabled\":%d,\"inv_opacity\":%.2f,\"doll_hide\":%.2f,\"generic_gui\":%d,\"follower_menu\":%d,\"chest_gui\":%d}\n",
		tag, (int)listActive(0), pl.gui_mode, (int)pl.shootmode, pl.inventory_mode, (int)ui->itemMenuOpen, ui->itemMenuItem,
		st.selected, st.selectedUid, (int)vs.inspectShown, vs.other, vs.focus, vs.station, (int)vs.benchShown, vs.benchRows, vs.benchTab,
		(unsigned)list_Size(&stats[0]->FOLLOWERS), fd,
		pl.inventoryUI.frame ? pl.inventoryUI.frame->getSize().x : -1, pl.inventoryUI.frame ? pl.inventoryUI.frame->getSize().w : -1,
		pl.inventoryUI.frame ? (int)pl.inventoryUI.frame->isDisabled() : -1, pl.inventoryUI.frame ? (double)pl.inventoryUI.frame->getOpacity() : -1.0,
		(double)pl.inventoryUI.animPaperDollHide, (int)GenericGUI[0].isGUIOpen(), (int)FollowerMenu[0].followerMenuIsOpen(), (int)pl.inventoryUI.chestGUI.bOpen);
	fclose(f);
}

// The bench screens driving F6's provider (apx-main 533d195b has both): Salvage keeps a chosen part,
// Break teaches the Form, the Formbook view lists it, Craft says "crafting: next slice". Run once.
static void runF6Bench(int dt)
{
	auto& pl = *players[0];
	static Entity* bench = nullptr;
	static Uint32 salvUid = 0, breakUid = 0;
	static uint32_t breakForm = 0;
	static int parts = 0, salvRows = 0, keepRow = -1, itemsBefore = 0, salvOk = 0, succBefore = -1, succAfter = -1, fbRows = 0, fbHasForm = 0;
	static std::string craftMsg, salvMsg, asmDump;
	static int partsAfter = -1, itemsAfter = 0, keepChosen = -2, carriedL0 = 0, carriedL1 = 0, weightFollows = 0;
	static long long ledger0 = -1, ledger1 = -1;
	if ( pl.shootmode || pl.gui_mode != GUI_MODE_INVENTORY ) { pl.shootmode = false; pl.openStatusScreen(GUI_MODE_INVENTORY, INVENTORY_MODE_ITEM); }
	auto selectOwn = [](ItemType t) {
		auto e = ownRowsForTest(0);
		for ( size_t i = 0; i < e.size(); ++i ) { if ( !e[i].header && e[i].row.type == t ) { setFocus(0, 0, (int)i); return uidToItem(e[i].row.uid); } }
		return (Item*)nullptr;
	};
	if ( dt == 340 )
	{
		emptyInventory(0);
		Item* a = newItem(IRON_SWORD, SERVICABLE, 0, 1, 0, true, &stats[0]->inventory);
		// a second blade: the same Form as the step-3 break (the quickstart character's input cap Wit/2 is 1 Form)
		Item* b = newItem(STEEL_SWORD, SERVICABLE, 0, 1, 0, true, &stats[0]->inventory);
		apxPartsBind(a, 1, 3); // looted (monster), depth 3: the selftest's forced roll
		apxPartsBind(b, 1, 3);
		salvUid = a->uid; breakUid = b->uid; breakForm = b->apx.form;
		parts = (int)apx_bench::provider().parts(a).size();
		bench = spawnProp(1617, &actWorkbench, pl.entity->x + 16, pl.entity->y);
	}
	else if ( dt == 344 ) { selectOwn(IRON_SWORD); setBenchTab(0, 4); } // Salvage
	else if ( dt == 348 )
	{
		auto e = benchRowsForTest(0);
		salvRows = (int)e.size();
		for ( size_t i = 0; i < e.size(); ++i ) { if ( !e[i].header && e[i].row.kind == 3 ) { keepRow = (int)i; break; } } // part index 0
		itemsBefore = (int)list_Size(&stats[0]->inventory);
		ledger0 = apx_bench::provider().pouchGrams(0);
		carriedL0 = carriedMass(0);
		if ( keepRow >= 0 ) { setFocus(0, 2, keepRow); activateSelected(0); } // choose the keep
	}
	else if ( dt == 350 )
	{
		keepChosen = benchKeep(0);
		auto e = benchRowsForTest(0);
		for ( size_t i = 0; i < e.size(); ++i ) { if ( !e[i].header && e[i].row.kind == 4 ) { setFocus(0, 2, (int)i); activateSelected(0); break; } } // Break, keeping it
	}
	else if ( dt == 352 )
	{
		// the item is consumed; the kept part and the returned materials are new items
		// F6 keeps the chosen part (as the item itself or a new one) and returns the rest as materials
		Item* left = uidToItem(salvUid);
		partsAfter = left ? (int)apx_bench::provider().parts(left).size() : 0;
		itemsAfter = (int)list_Size(&stats[0]->inventory);
		salvMsg = viewState(0).lastBenchMsg;
		salvOk = salvMsg.rfind("Salvaged", 0) == 0 && (left == nullptr || partsAfter <= 1) && itemsAfter > itemsBefore - 1;
		// live D85: the salvage grew F6's ledger and carry weight followed it by the same whole units
		ledger1 = apx_bench::provider().pouchGrams(0);
		carriedL1 = carriedMass(0);
		int pouchRowMass = -1;
		for ( auto& r : rows(0) ) { if ( r.type == -2 ) { pouchRowMass = r.mass; } }
		// the ledger grew; the list rows (with the pouch row when it holds a whole unit) sum to carried mass
		const int expectRow = ledger1 >= 100 ? (int)(ledger1 / 100) : -1;
		weightFollows = ledger1 > ledger0 && checkRows(rows(0), carriedMass(0)).empty() && pouchRowMass == expectRow ? 1 : 0;
		succBefore = apxPartsFormSuccesses(0, breakForm);
		pl.inventoryUI.activateItemContextMenuOption(uidToItem(breakUid), PROMPT_APX_BREAK);
	}
	else if ( dt == 356 ) { succAfter = apxPartsFormSuccesses(0, breakForm); setBenchTab(0, 5); } // Formbook
	else if ( dt == 360 )
	{
		fbRows = viewState(0).benchRows;
		for ( auto& f : apx_bench::provider().formbook(0) ) { if ( f.form == breakForm && f.successes >= 1 ) { fbHasForm = 1; } }
		setBenchTab(0, 0); // Craft
	}
	else if ( dt == 364 )
	{
		auto e = benchRowsForTest(0);
		for ( size_t i = 0; i < e.size(); ++i )
		{
			if ( !e[i].header && e[i].row.kind == 1 ) { setFocus(0, 2, (int)i); activateSelected(0); break; } // pick the Form
		}
	}
	else if ( dt == 366 )
	{
		// the Assemble view shows the reason it cannot assemble yet (no preview / crafting slice from F6)
		for ( auto& x : benchRowsForTest(0) ) { if ( !x.header && x.row.kind == 0 && x.row.name.find("   ") == 0 ) { craftMsg = x.row.name.substr(3); } }
		for ( auto& x : benchRowsForTest(0) ) { asmDump += (x.header ? "#" : "-") + x.row.name + "|"; }
		asmDump += " tab=" + std::to_string(viewState(0).benchTab) + " bench=" + std::to_string((int)viewState(0).benchShown);
	}
	else if ( dt == 368 )
	{
		FILE* f = fopen(diagPath().c_str(), "a");
		if ( f )
		{
			fprintf(f, "{\"test\":\"f6bench\",\"mode\":\"%s\",\"headless\":%d,\"provider\":\"%s\",\"parts\":%d,\"salvage_rows\":%d,\"keep_row\":%d,\"salvage_ok\":%d,\"salvage_msg\":\"%s\",\"keep_chosen\":%d,\"ledger_g\":[%lld,%lld],\"carried\":[%d,%d],\"weight_follows_ledger\":%d,\"items_before_after\":[%d,%d],\"parts_left_on_item\":%d,"
				"\"form\":%u,\"form_successes\":[%d,%d],\"break_teaches\":%d,\"formbook_rows\":%d,\"formbook_has_form\":%d,\"craft_msg\":\"%s\",\"assemble_reason_shown\":%d,\"asm_dump\":\"%s\"}\n",
				vanillaGrid() ? "vanilla" : "apx", (int)apx::headlessOn, apx_bench::provider().name, parts, salvRows, keepRow, salvOk, salvMsg.c_str(), keepChosen, ledger0, ledger1, carriedL0, carriedL1,
				weightFollows,
				itemsBefore, itemsAfter, partsAfter,
				breakForm, succBefore, succAfter, succAfter == succBefore + 1 ? 1 : 0, fbRows, fbHasForm, craftMsg.c_str(),
				!craftMsg.empty() ? 1 : 0, asmDump.c_str());
			fclose(f);
		}
		if ( bench ) { list_RemoveNode(bench->mynode); bench = nullptr; }
	}
}

// D85 pouch: a fixed test pouch (iron 2500 g, oak 800 g) and a recording Draw-out, installed over the
// live provider for these ticks only, so the UI mechanics are checked whatever F6 fills; the live
// provider's own pouch total is recorded beside it. Run once.
static int g_drawCalls = 0, g_drawMaterial = -1;
static long long g_drawGrams = 0;
static std::vector<apx_bench::PouchRow> testPouch(int) { return { { 3, "iron", 2500 }, { 11, "oak", 800 } }; }
static long long testPouchGrams(int) { return 3300; }
static bool testDraw(int, int material, long long grams, std::string&) { ++g_drawCalls; g_drawMaterial = material; g_drawGrams = grams; return true; }

static void runPouch(int dt)
{
	auto& pl = *players[0];
	static apx_bench::Provider live;
	static Entity* bench = nullptr;
	static long long liveGrams = -1;
	static int carried0 = 0, carried1 = 0, rowsOk = 0, pouchRow = 0, matRows = 0, bundleRow = -1, tab = -1;
	static std::string drawMsg;
	if ( pl.shootmode || pl.gui_mode != GUI_MODE_INVENTORY ) { pl.shootmode = false; pl.openStatusScreen(GUI_MODE_INVENTORY, INVENTORY_MODE_ITEM); }
	if ( dt == 370 )
	{
		live = apx_bench::provider();
		liveGrams = live.pouchGrams(0);
		carried0 = carriedMass(0) - (int)apx_bench::pouchUnits(0);
		apx_bench::Provider t = live;
		t.name = "self-test pouch";
		t.pouch = testPouch; t.pouchGrams = testPouchGrams; t.drawBundle = testDraw;
		apx_bench::setProvider(t);
		bench = spawnProp(1617, &actWorkbench, pl.entity->x + 16, pl.entity->y);
	}
	else if ( dt == 372 )
	{
		carried1 = carriedMass(0);
		rowsOk = checkRows(rows(0), carriedMass(0)).empty();
		for ( auto& e : ownRowsForTest(0) ) { if ( !e.header && e.row.name == "Material pouch" && e.row.mass == 33 ) { pouchRow = 1; } }
		setBenchTab(0, 6); tab = 6; // Materials
	}
	else if ( dt == 375 )
	{
		auto e = benchRowsForTest(0);
		matRows = (int)e.size();
		for ( size_t i = 0; i < e.size(); ++i ) { if ( !e[i].header && e[i].row.kind == 5 ) { bundleRow = (int)i; break; } }
		if ( bundleRow >= 0 ) { setFocus(0, 2, bundleRow); activateSelected(0); drawMsg = viewState(0).lastBenchMsg; }
	}
	else if ( dt == 378 )
	{
		FILE* f = fopen(diagPath().c_str(), "a");
		if ( f )
		{
			fprintf(f, "{\"test\":\"pouch\",\"mode\":\"%s\",\"headless\":%d,\"carried_without_with\":[%d,%d],\"pouch_units\":33,\"carry_counts_pouch\":%d,"
				"\"rows_sum_ok\":%d,\"pouch_row\":%d,\"materials_tab\":%d,\"materials_rows\":%d,\"bundle_row\":%d,\"draw_calls\":%d,\"draw\":[%d,%lld],\"draw_msg\":\"%s\","
				"\"live_provider\":\"%s\",\"live_pouch_grams\":%lld,\"live_draw_bundle\":%d}\n",
				vanillaGrid() ? "vanilla" : "apx", (int)apx::headlessOn, carried0, carried1, carried1 - carried0 == 33 ? 1 : 0, rowsOk, pouchRow,
				viewState(0).benchTab == 6 ? 1 : 0, matRows, bundleRow, g_drawCalls, g_drawMaterial, g_drawGrams, drawMsg.c_str(),
				live.name, liveGrams, live.drawBundle ? 1 : 0);
			fclose(f);
		}
		apx_bench::setProvider(live);
		if ( bench ) { list_RemoveNode(bench->mynode); bench = nullptr; }
	}
}

// D86 components: a test provider (component items marked by appearance 7786; a sword with two parts
// of rarity 1 and 3 and affixes; one swap slot; one make option) over the live provider, recording
// calls, so the list colours, tooltip, Swap and Make views are checked whatever F6 fills. Run once.
static int g_swapCalls = 0, g_swapPart = -1, g_makeCalls = 0, g_makeId = -1;
static Uint32 g_swapUid = 0, g_compUid = 0;
static int tComponentOf(const Item* it, apx_bench::PartRow& out)
{
	if ( !it || it->appearance != 7786 ) { return 0; }
	out.name = "keen steel blade"; out.material = "steel"; out.rarity = 2; out.affixes = { "keen (+2 cut)" };
	return 1;
}
static std::vector<apx_bench::PartRow> tParts(const Item* it)
{
	if ( !it || it->type != IRON_SWORD ) { return {}; }
	apx_bench::PartRow a; a.slotName = "Blade"; a.name = "iron blade"; a.material = "iron"; a.quality = 3; a.rarity = 1; a.affixes = { "sturdy" };
	apx_bench::PartRow b; b.slotName = "Hilt"; b.name = "oak hilt of speed"; b.material = "oak"; b.quality = 2; b.rarity = 3; b.affixes = { "of speed (+5%)" }; b.affixesKnown = false; b.integrity = 0.4f;
	return { a, b };
}
static std::vector<apx_bench::SwapSlot> tSwapSlots(int, const Item* it, int)
{
	if ( !it || it->type != IRON_SWORD ) { return {}; }
	apx_bench::SwapSlot s; s.partIndex = 0; s.name = "Blade"; s.candidateUids = { g_compUid }; s.quick = true;
	return { s };
}
static bool tSwap(int, Item*, int partIndex, uint32_t uid, std::string&) { ++g_swapCalls; g_swapPart = partIndex; g_swapUid = uid; return true; }
static std::vector<apx_bench::MakeOption> tMakeOptions(int, int)
{
	apx_bench::MakeOption o; o.id = 4; o.name = "steel blade"; o.cost = { { 3, "iron", 500 } }; o.affordable = true;
	return { o };
}
static bool tMake(int, int id, std::string&) { ++g_makeCalls; g_makeId = id; return true; }
// C3/C4 additions
static int g_craftCalls = 0, g_craftPicks = 0, g_repairCalls = 0, g_repairPart = -1;
static Uint32 g_craftPickUid = 0;
static std::string tDisplayName(const Item* it)
{
	if ( it && it->appearance == 7786 ) { return "Keen Steel Blade of Embers"; }  // Common..Rare: [prefix] [material] [slot] [suffix]
	if ( it && it->type == IRON_SWORD ) { return "Vey's Ember-Tongue"; }          // Epic and up: a generated proper name
	return "";
}
static std::vector<apx_bench::FormRow> tFormbook(int) { apx_bench::FormRow f; f.form = 1; f.name = "Blades"; f.successes = 2; return { f }; }
static std::vector<apx_bench::SlotNeed> tCraftSlots(int, uint32_t, int)
{
	apx_bench::SlotNeed a; a.slot = 0; a.name = "Blade"; a.required = true; a.candidateUids = { g_compUid };
	return { a };
}
static bool tAssemblePreview(int, uint32_t, int, const std::vector<std::pair<int, uint32_t>>& picks, int& power, int& cap, std::string& why)
{
	power = 40 + 10 * (int)picks.size(); cap = 120; why = picks.empty() ? "nothing chosen" : ""; return !picks.empty();
}
static bool tCraft(int, uint32_t, int, const std::vector<std::pair<int, uint32_t>>& picks, std::string&)
{
	++g_craftCalls; g_craftPicks = (int)picks.size(); g_craftPickUid = picks.empty() ? 0 : picks[0].second; return true;
}
static std::vector<apx_bench::RepairOption> tRepairOptions(int, const Item* it, int)
{
	if ( !it || it->type != IRON_SWORD ) { return {}; }
	apx_bench::RepairOption o; o.partIndex = 1; o.name = "Hilt"; o.integrity = 0.4f; o.cost = { { 11, "oak", 120 } }; o.affordable = true;
	return { o };
}
static bool tRepair(int, Item*, int part, std::string&) { ++g_repairCalls; g_repairPart = part; return true; }

static void runComponents(int dt)
{
	auto& pl = *players[0];
	static apx_bench::Provider live;
	static Entity* bench = nullptr;
	static Uint32 swordUid = 0;
	static int compRarity = -1, tipParts = 0, tipAffix = 0, swapRows = 0, makeRows = 0, swordRarity = -1, compNameOk = 0, tipUnknown = 0, quickLabel = 0, asmRows = 0, asmPowerRow = 0, repRows = 0;
	static std::string tipName;
	static std::string swapMsg, makeMsg;
	if ( pl.shootmode || pl.gui_mode != GUI_MODE_INVENTORY ) { pl.shootmode = false; pl.openStatusScreen(GUI_MODE_INVENTORY, INVENTORY_MODE_ITEM); }
	auto selectUid = [](Uint32 uid) {
		auto e = ownRowsForTest(0);
		for ( size_t i = 0; i < e.size(); ++i ) { if ( !e[i].header && e[i].row.uid == uid ) { setFocus(0, 0, (int)i); return; } }
	};
	if ( dt == 380 )
	{
		live = apx_bench::provider();
		apx_bench::Provider t = live;
		t.name = "self-test components";
		t.componentOf = tComponentOf; t.parts = tParts; t.swapSlots = tSwapSlots; t.swapPart = tSwap; t.makeOptions = tMakeOptions; t.make = tMake;
		t.displayName = tDisplayName; t.formbook = tFormbook; t.craftSlots = tCraftSlots; t.assemblePreview = tAssemblePreview; t.craft = tCraft;
		t.repairOptions = tRepairOptions; t.repairPart = tRepair;
		apx_bench::setProvider(t);
		emptyInventory(0);
		Item* sword = newItem(IRON_SWORD, SERVICABLE, 0, 1, 0, true, &stats[0]->inventory);
		Item* comp = newItem(TOOL_METAL_SCRAP, EXCELLENT, 0, 1, 7786, true, &stats[0]->inventory);
		swordUid = sword->uid; g_compUid = comp->uid;
		bench = spawnProp(1617, &actWorkbench, pl.entity->x + 16, pl.entity->y);
	}
	else if ( dt == 382 )
	{
		for ( auto& e : ownRowsForTest(0) )
		{
			if ( !e.header && e.row.uid == g_compUid ) { compRarity = e.row.rarity; compNameOk = e.row.name == "Keen Steel Blade of Embers"; }
			if ( !e.header && e.row.uid == swordUid ) { swordRarity = e.row.rarity; }
		}
		selectUid(swordUid);
	}
	else if ( dt == 384 )
	{
		tipParts = viewState(0).tipParts; tipAffix = viewState(0).tipAffixLines; tipUnknown = viewState(0).tipUnknownLines; tipName = viewState(0).tipName;
		shotWhen("tip", "apx-u1g-components.png"); setBenchTab(0, 1); // Swap
	}
	else if ( dt == 387 )
	{
		auto e = benchRowsForTest(0);
		swapRows = (int)e.size();
		for ( auto& x : e ) { if ( x.header && x.row.name.find("(quick slot)") != std::string::npos ) { quickLabel = 1; } }
		for ( size_t i = 0; i < e.size(); ++i ) { if ( !e[i].header && e[i].row.kind == 7 ) { setFocus(0, 2, (int)i); activateSelected(0); break; } }
		swapMsg = viewState(0).lastBenchMsg;
		setBenchTab(0, 2); // Make
	}
	else if ( dt == 390 )
	{
		auto e = benchRowsForTest(0);
		makeRows = (int)e.size();
		for ( size_t i = 0; i < e.size(); ++i ) { if ( !e[i].header && e[i].row.kind == 8 ) { setFocus(0, 2, (int)i); activateSelected(0); break; } }
		makeMsg = viewState(0).lastBenchMsg;
		setBenchTab(0, apx_logic::TAB_REPAIR); // Repair
	}
	else if ( dt == 393 )
	{
		auto e = benchRowsForTest(0);
		repRows = (int)e.size();
		for ( size_t i = 0; i < e.size(); ++i ) { if ( !e[i].header && e[i].row.kind == 12 ) { setFocus(0, 2, (int)i); activateSelected(0); break; } }
		setBenchTab(0, 0); // Assemble
	}
	else if ( dt == 396 )
	{
		auto e = benchRowsForTest(0);
		for ( size_t i = 0; i < e.size(); ++i ) { if ( !e[i].header && e[i].row.kind == 1 ) { setFocus(0, 2, (int)i); activateSelected(0); break; } } // the Form
	}
	else if ( dt == 399 )
	{
		auto e = benchRowsForTest(0);
		for ( size_t i = 0; i < e.size(); ++i ) { if ( !e[i].header && e[i].row.kind == 10 ) { setFocus(0, 2, (int)i); activateSelected(0); break; } } // a component for the Blade
	}
	else if ( dt == 402 )
	{
		auto e = benchRowsForTest(0);
		asmRows = (int)e.size();
		for ( auto& x : e ) { if ( x.header && x.row.name == "Item power 50 / cap 120" ) { asmPowerRow = 1; } }
		for ( size_t i = 0; i < e.size(); ++i ) { if ( !e[i].header && e[i].row.kind == 11 ) { setFocus(0, 2, (int)i); activateSelected(0); break; } } // Assemble
	}
	else if ( dt == 405 )
	{
		FILE* f = fopen(diagPath().c_str(), "a");
		if ( f )
		{
			fprintf(f, "{\"test\":\"components\",\"mode\":\"%s\",\"headless\":%d,\"component_row_rarity\":%d,\"tip_parts\":%d,\"tip_affix_lines\":%d,"
				"\"comp_name_ok\":%d,\"assembly_rarity_best\":%d,\"tip_name\":\"%s\",\"tip_unknown_lines\":%d,\"quick_label\":%d,\"repair\":[%d,%d,%d],"
				"\"assemble\":{\"rows\":%d,\"power_row\":%d,\"calls\":%d,\"picks\":%d,\"pick_right\":%d},"
				"\"swap_rows\":%d,\"swap_calls\":%d,\"swap\":[%d,%d],\"swap_uid_right\":%d,\"swap_msg\":\"%s\",\"make_rows\":%d,\"make_calls\":%d,\"make_id\":%d,\"make_msg\":\"%s\",\"live_provider\":\"%s\"}\n",
				vanillaGrid() ? "vanilla" : "apx", (int)apx::headlessOn, compRarity, tipParts, tipAffix,
				compNameOk, swordRarity, tipName.c_str(), tipUnknown, quickLabel, repRows, g_repairCalls, g_repairPart,
				asmRows, asmPowerRow, g_craftCalls, g_craftPicks, g_craftPickUid == g_compUid ? 1 : 0, swapRows, g_swapCalls, g_swapPart, (int)g_swapUid,
				g_swapUid == g_compUid ? 1 : 0, swapMsg.c_str(), makeRows, g_makeCalls, g_makeId, makeMsg.c_str(), live.name);
			fclose(f);
		}
		apx_bench::setProvider(live);
		if ( bench ) { list_RemoveNode(bench->mynode); bench = nullptr; }
		setBenchTab(0, 0);
	}
}

// Live D85/D86 chain on the real F6 engine (no test provider): salvage at the bench (choose the keep,
// break) -> a component item appears and the pouch grows -> draw a bundle out of the pouch (F6's
// engine call; its provider has no drawBundle yet) -> pick the bundle up -> the pouch is back.
// Carry mass = the list rows' sum at every step. Make and Swap: not in F6's engine yet (reported).
static int countComponents()
{
	int n = 0;
	for ( node_t* nd = stats[0]->inventory.first; nd; nd = nd->next ) { if ( apxPartsIsComponent((Item*)nd->element) ) { ++n; } }
	return n;
}
static void runLive86(int dt)
{
	auto& pl = *players[0];
	static Entity* bench = nullptr;
	static Uint32 swordUid = 0, compUid = 0, bundleUid = 0;
	static long long g0 = -1, g1 = -1, g2 = -1, g3 = -1;
	static int comps0 = -1, comps1 = -1, compRowRarity = -9, compParts = -1, rows0 = 0, rows1 = 0, rows2 = 0, rows3 = 0, keep = -2, bundleMat = -1, salvaged = 0, picked = 0;
	static std::string compName, why, repairRow, repairMsg;
	static Uint32 helmUid = 0;
	static int helmBefore = -1, helmAfter = -1, station = -1, ironMat = -1, ironBefore = -1, ironAfter = -1;
	if ( pl.shootmode || pl.gui_mode != GUI_MODE_INVENTORY ) { pl.shootmode = false; pl.openStatusScreen(GUI_MODE_INVENTORY, INVENTORY_MODE_ITEM); }
	auto rowsOk = []() { return checkRows(rows(0), carriedMass(0)).empty() ? 1 : 0; };
	if ( dt == 410 )
	{
		emptyInventory(0);
		Item* sword = newItem(IRON_SWORD, SERVICABLE, 0, 1, 0, true, &stats[0]->inventory);
		apxPartsBind(sword, 1, 3); // looted, depth 3
		swordUid = sword->uid;
		g0 = apx_bench::provider().pouchGrams(0);
		comps0 = countComponents();
		rows0 = rowsOk();
		bench = spawnProp(1617, &actWorkbench, pl.entity->x + 16, pl.entity->y);
		auto e = ownRowsForTest(0);
		setBenchTab(0, 4); // Salvage
	}
	else if ( dt == 412 )
	{
		auto e = ownRowsForTest(0);
		for ( size_t i = 0; i < e.size(); ++i ) { if ( !e[i].header && e[i].row.uid == swordUid ) { setFocus(0, 0, (int)i); } }
	}
	else if ( dt == 414 )
	{
		auto e = benchRowsForTest(0);
		for ( size_t i = 0; i < e.size(); ++i ) { if ( !e[i].header && e[i].row.kind == 3 ) { setFocus(0, 2, (int)i); activateSelected(0); break; } } // choose the keep
	}
	else if ( dt == 416 )
	{
		keep = benchKeep(0);
		auto e = benchRowsForTest(0);
		for ( size_t i = 0; i < e.size(); ++i ) { if ( !e[i].header && e[i].row.kind == 4 ) { setFocus(0, 2, (int)i); activateSelected(0); break; } } // Break, keeping it
		salvaged = viewState(0).lastBenchMsg.rfind("Salvaged", 0) == 0;
	}
	else if ( dt == 419 )
	{
		comps1 = countComponents();
		g1 = apx_bench::provider().pouchGrams(0);
		rows1 = rowsOk();
		for ( node_t* nd = stats[0]->inventory.first; nd; nd = nd->next )
		{
			Item* it = (Item*)nd->element;
			if ( apxPartsIsComponent(it) ) { compUid = it->uid; compName = apxPartsComponentName(it); compParts = (int)apx_bench::provider().parts(it).size(); }
		}
		for ( auto& e : ownRowsForTest(0) ) { if ( !e.header && e.row.uid == compUid ) { compRowRarity = e.row.rarity; } }
	}
	else if ( dt == 421 )
	{
		// draw out 10 g of the first material the pouch holds (a small salvage leaves under 100 g per material)
		for ( int m = 0; m < 64 && !bundleUid; ++m )
		{
			std::string w;
			if ( Item* b = apxPartsDrawBundle(0, m, 10, w) ) { bundleUid = b->uid; bundleMat = m; } else if ( why.empty() ) { why = w; }
		}
	}
	else if ( dt == 424 )
	{
		g2 = apx_bench::provider().pouchGrams(0);
		rows2 = rowsOk();
		// pick the bundle up again the way a floor pickup does: the item arrives through itemPickup
		if ( Item* b = uidToItem(bundleUid) )
		{
			Item* copy = newItem(b->type, b->status, b->beatitude, b->count, b->appearance, b->identified, nullptr);
			copy->apx = b->apx;
			list_RemoveNode(b->node);
			Item* got = itemPickup(0, copy);
			if ( got && got != copy ) { free(copy); }
			picked = got != nullptr;
		}
	}
	else if ( dt == 428 )
	{
		g3 = apx_bench::provider().pouchGrams(0);
		rows3 = rowsOk();
	}
	else if ( dt == 430 )
	{
		// props' whole-item repair through the Repair tab at an anvil: a worn iron helm, one condition step
		if ( bench ) { list_RemoveNode(bench->mynode); bench = nullptr; }
		bench = spawnProp(1621, nullptr, pl.entity->x + 16, pl.entity->y); // divine_anvil model = ANVIL
		Item* helm = newItem(IRON_HELM, WORN, 0, 1, 0, true, &stats[0]->inventory);
		apxPartsBind(helm, 1, 3);
		helmUid = helm->uid; helmBefore = (int)helm->status;
		// seed the pouch with iron the way F6's self-tests do (logged by F6), so one repair step is affordable
		for ( int m = 0; m < 64; ++m ) { const char* n = apxPartsMaterialName(m); if ( n && std::string(n) == "iron" ) { ironMat = m; break; } }
		if ( ironMat >= 0 ) { apxPartsLedgerCredit(0, ironMat, 2000); ironBefore = apxPartsLedgerMaterialGrams(0, ironMat); }
		setBenchTab(0, apx_logic::TAB_REPAIR); // Repair
	}
	else if ( dt == 432 )
	{
		auto e = ownRowsForTest(0);
		for ( size_t i = 0; i < e.size(); ++i ) { if ( !e[i].header && e[i].row.uid == helmUid ) { setFocus(0, 0, (int)i); } }
	}
	else if ( dt == 435 )
	{
		auto e = benchRowsForTest(0);
		for ( auto& x : e ) { if ( !x.header && x.row.name.find("epair") != std::string::npos ) { repairRow = x.row.name; } }
		for ( size_t i = 0; i < e.size(); ++i ) { if ( !e[i].header && e[i].row.kind == 13 ) { setFocus(0, 2, (int)i); activateSelected(0); break; } }
		repairMsg = viewState(0).lastBenchMsg;
	}
	else if ( dt == 438 )
	{
		Item* helm = uidToItem(helmUid);
		helmAfter = helm ? (int)helm->status : -1;
		ironAfter = ironMat >= 0 ? apxPartsLedgerMaterialGrams(0, ironMat) : -1;
		station = viewState(0).station;
		FILE* f = fopen(diagPath().c_str(), "a");
		if ( f )
		{
			fprintf(f, "{\"test\":\"live86\",\"mode\":\"%s\",\"headless\":%d,\"provider\":\"%s\",\"keep\":%d,\"salvaged\":%d,\"components\":[%d,%d],\"component\":{\"name\":\"%s\",\"parts\":%d,\"row_rarity\":%d},"
				"\"pouch_g\":[%lld,%lld,%lld,%lld],\"bundle\":{\"material\":%d,\"drawn\":%d,\"picked\":%d,\"why\":\"%s\"},\"rows_sum_ok\":[%d,%d,%d,%d],"
				"\"pouch_grew\":%d,\"bundle_debited\":%d,\"pickup_restored\":%d,\"make\":\"not in F6 engine yet\",\"swap\":\"not in F6 engine yet\","
				"\"repair\":{\"station\":%d,\"row\":\"%s\",\"msg\":\"%s\",\"status\":[%d,%d],\"one_step\":%d,\"iron_g\":[%d,%d],\"paid_from_pouch\":%d}}\n",
				vanillaGrid() ? "vanilla" : "apx", (int)apx::headlessOn, apx_bench::provider().name, keep, salvaged, comps0, comps1, compName.c_str(), compParts, compRowRarity,
				g0, g1, g2, g3, bundleMat, bundleUid ? 1 : 0, picked, why.c_str(), rows0, rows1, rows2, rows3,
				g1 > g0 ? 1 : 0, bundleUid && g2 == g1 - 10 ? 1 : 0, bundleUid && picked && g3 == g1 ? 1 : 0,
				station, repairRow.c_str(), repairMsg.c_str(), helmBefore, helmAfter, helmAfter == helmBefore + 1 ? 1 : 0,
				ironBefore, ironAfter, ironMat >= 0 && ironAfter < ironBefore ? 1 : 0);
			fclose(f);
		}
		if ( bench ) { list_RemoveNode(bench->mynode); bench = nullptr; }
	}
}

// Live 05b chain through the bench screens on F6's provider (apx/f6 6a9eb662): Break teaches Blades
// -> Salvage keeping part 0 -> a component item -> Make a component from the pouch -> Assemble a
// Blades item from components -> Swap a part on it -> Draw out a bundle -> pick it up. Run once.
static int countWhere(bool comps)
{
	int n = 0;
	for ( node_t* nd = stats[0]->inventory.first; nd; nd = nd->next ) { Item* it = (Item*)nd->element; if ( apxPartsIsComponent(it) == comps && itemCategory(it) != SPELL_CAT ) { ++n; } }
	return n;
}
static int firstRow(int kind)
{
	auto e = benchRowsForTest(0);
	for ( size_t i = 0; i < e.size(); ++i ) { if ( !e[i].header && e[i].row.kind == kind ) { return (int)i; } }
	return -1;
}
static std::string pressRow(int kind)
{
	int r = firstRow(kind);
	if ( r < 0 ) { return "(no row)"; }
	setFocus(0, 2, r);
	activateSelected(0);
	return viewState(0).lastBenchMsg;
}
static void runLiveF6(int dt)
{
	auto& pl = *players[0];
	static Entity* bench = nullptr;
	static Uint32 salvUid = 0, newItemUid = 0;
	static int comps0 = -1, compsSalv = -1, compsMake = -1, compsAsm = -1, items0 = -1, itemsAsm = -1, compRarity = -9, swapRows = -1, ironMat = -1;
	static long long pouch0 = -1, pouchMake = -1, pouchDraw = -1, pouchPick = -1;
	static std::string mBreak, mSalv, mMake, mAsm, mSwap, mDraw, compName, asmName, makeName;
	static int makeOptions = -1;
	static std::string mSpare;
	if ( pl.shootmode || pl.gui_mode != GUI_MODE_INVENTORY ) { pl.shootmode = false; pl.openStatusScreen(GUI_MODE_INVENTORY, INVENTORY_MODE_ITEM); }
	auto selectUid = [](Uint32 uid) { auto e = ownRowsForTest(0); for ( size_t i = 0; i < e.size(); ++i ) { if ( !e[i].header && e[i].row.uid == uid ) { setFocus(0, 0, (int)i); } } };
	if ( dt == 440 )
	{
		emptyInventory(0);
		Item* a = newItem(IRON_SWORD, SERVICABLE, 0, 1, 0, true, &stats[0]->inventory); apxPartsBind(a, 1, 3);
		Item* b = newItem(STEEL_SWORD, SERVICABLE, 0, 1, 0, true, &stats[0]->inventory); apxPartsBind(b, 1, 3);
		salvUid = a->uid;
		for ( int m = 0; m < 64; ++m ) { const char* n = apxPartsMaterialName(m); if ( n && std::string(n) == "iron" ) { ironMat = m; break; } }
		bench = spawnProp(1617, &actWorkbench, pl.entity->x + 16, pl.entity->y);
		std::string w; Item* bb = b;
		mBreak = apx_bench::provider().breakItem(0, bb, w) ? "broke: Blades learned" : "break refused: " + w;
	}
	else if ( dt == 442 ) { comps0 = countWhere(true); selectUid(salvUid); setBenchTab(0, apx_logic::TAB_SALVAGE); }
	else if ( dt == 444 ) { pressRow(apx_logic::ROW_SALVAGE_PART); }                    // choose the keep (part 0)
	else if ( dt == 446 ) { mSalv = pressRow(apx_logic::ROW_BREAK); }                    // Break, keeping it
	else if ( dt == 448 )
	{
		compsSalv = countWhere(true);
		for ( node_t* nd = stats[0]->inventory.first; nd; nd = nd->next )
		{
			Item* it = (Item*)nd->element; apx_bench::PartRow pr;
			if ( apx_bench::provider().componentOf(it, pr) ) { compRarity = pr.rarity; compName = pr.name.empty() ? std::string(it->getName()) : pr.name; }
		}
		// seed the pouch AFTER the salvage (F6's salvage pays whole scrap units out of the ledger: it drained
		// an earlier seed into 16 metal scrap; reported to F6 as a D85 question)
		if ( ironMat >= 0 ) { apxPartsLedgerCredit(0, ironMat, 5000); }
		pouch0 = apx_bench::provider().pouchGrams(0);
		setBenchTab(0, apx_logic::TAB_MAKE);
	}
	else if ( dt == 450 )
	{
		auto e = benchRowsForTest(0);
		int pick = -1;
		for ( size_t i = 0; i < e.size(); ++i )
		{
			if ( e[i].header || e[i].row.kind != apx_logic::ROW_MAKE || e[i].row.name.rfind(">> ", 0) != 0 ) { continue; }
			std::string n = e[i].row.name;
			for ( auto& c : n ) { c = (char)tolower(c); }
			if ( pick < 0 || n.find("grip") != std::string::npos ) { pick = (int)i; if ( n.find("grip") != std::string::npos ) { break; } }
		}
		makeOptions = 0;
		for ( auto& x : e ) { if ( !x.header && x.row.kind == apx_logic::ROW_MAKE ) { ++makeOptions; } }
		if ( pick >= 0 ) { makeName = e[pick].row.name; setFocus(0, 2, pick); activateSelected(0); mMake = viewState(0).lastBenchMsg; } else { mMake = "(no affordable option)"; }
	}
	else if ( dt == 452 ) { compsMake = countWhere(true); pouchMake = apx_bench::provider().pouchGrams(0); items0 = countWhere(false); setBenchTab(0, apx_logic::TAB_ASSEMBLE); }
	else if ( dt == 454 ) { pressRow(apx_logic::ROW_FORM); }                              // the Blades Form
	else if ( dt == 456 )
	{
		// one candidate per slot (the view lists them under each slot header)
		auto e = benchRowsForTest(0);
		std::set<int> done;
		for ( size_t i = 0; i < e.size(); ++i )
		{
			if ( !e[i].header && e[i].row.kind == apx_logic::ROW_ASM_PICK && !done.count(e[i].row.slot) ) { done.insert(e[i].row.slot); setFocus(0, 2, (int)i); activateSelected(0); }
		}
	}
	else if ( dt == 458 ) { mAsm = pressRow(apx_logic::ROW_ASSEMBLE); }
	else if ( dt == 460 )
	{
		compsAsm = countWhere(true); itemsAsm = countWhere(false);
		for ( node_t* nd = stats[0]->inventory.first; nd; nd = nd->next )
		{
			Item* it = (Item*)nd->element;
			if ( !apxPartsIsComponent(it) && it->apx.nParts > 0 ) { newItemUid = it->uid; std::string n = apx_bench::provider().displayName(it); asmName = n.empty() ? std::string(it->getName()) : n; }
		}
		setBenchTab(0, apx_logic::TAB_MAKE); // a spare Blade, so the assembled item has something to swap in
	}
	else if ( dt == 461 )
	{
		auto e = benchRowsForTest(0);
		for ( size_t i = 0; i < e.size(); ++i )
		{
			std::string n = e[i].row.name;
			for ( auto& c : n ) { c = (char)tolower(c); }
			if ( !e[i].header && e[i].row.kind == apx_logic::ROW_MAKE && n.rfind(">> ", 0) == 0 && n.find("blade") != std::string::npos ) { setFocus(0, 2, (int)i); activateSelected(0); mSpare = viewState(0).lastBenchMsg; break; }
		}
		selectUid(newItemUid);
		setBenchTab(0, apx_logic::TAB_SWAP);
	}
	else if ( dt == 463 )
	{
		auto e = benchRowsForTest(0); swapRows = 0;
		for ( auto& x : e ) { if ( !x.header && x.row.kind == apx_logic::ROW_SWAP ) { ++swapRows; } }
		mSwap = swapRows ? pressRow(apx_logic::ROW_SWAP) : "(no swappable slot with a fitting component)";
		setBenchTab(0, apx_logic::TAB_MATERIALS);
	}
	else if ( dt == 465 ) { mDraw = pressRow(apx_logic::ROW_DRAW_BUNDLE); pouchDraw = apx_bench::provider().pouchGrams(0); }
	else if ( dt == 467 )
	{
		for ( node_t* nd = stats[0]->inventory.first; nd; nd = nd->next )
		{
			Item* bnd = (Item*)nd->element;
			if ( !apxPartsIsBundle(bnd) ) { continue; }
			Item* copy = newItem(bnd->type, bnd->status, bnd->beatitude, bnd->count, bnd->appearance, bnd->identified, nullptr);
			copy->apx = bnd->apx;
			list_RemoveNode(bnd->node);
			Item* got = itemPickup(0, copy);
			if ( got && got != copy ) { free(copy); }
			break;
		}
	}
	else if ( dt == 469 )
	{
		pouchPick = apx_bench::provider().pouchGrams(0);
		FILE* f = fopen(diagPath().c_str(), "a");
		if ( f )
		{
			fprintf(f, "{\"test\":\"liveF6\",\"mode\":\"%s\",\"headless\":%d,\"provider\":\"%s\",\"break\":\"%s\",\"salvage\":{\"msg\":\"%s\",\"components\":[%d,%d],\"name\":\"%s\",\"rarity\":%d},"
				"\"make\":{\"options\":%d,\"chosen\":\"%s\",\"msg\":\"%s\",\"components\":%d,\"pouch_g\":[%lld,%lld]},\"assemble\":{\"msg\":\"%s\",\"components\":%d,\"items\":[%d,%d],\"name\":\"%s\"},"
				"\"swap\":{\"spare\":\"%s\",\"rows\":%d,\"msg\":\"%s\"},\"bundle\":{\"msg\":\"%s\",\"pouch_g\":[%lld,%lld,%lld]},\"rows_sum_ok\":%d}\n",
				vanillaGrid() ? "vanilla" : "apx", (int)apx::headlessOn, apx_bench::provider().name, mBreak.c_str(), mSalv.c_str(), comps0, compsSalv, compName.c_str(), compRarity,
				makeOptions, makeName.c_str(), mMake.c_str(), compsMake, pouch0, pouchMake, mAsm.c_str(), compsAsm, items0, itemsAsm, asmName.c_str(), mSpare.c_str(), swapRows, mSwap.c_str(), mDraw.c_str(),
				pouchMake, pouchDraw, pouchPick, checkRows(rows(0), carriedMass(0)).empty() ? 1 : 0);
			fclose(f);
		}
		if ( bench ) { list_RemoveNode(bench->mynode); bench = nullptr; }
	}
}

// D81: columns sort, category tabs, icons, the rich tooltip with compare-to-equipped. Run once.
// D88: the grid sits in Barony's inventory panel (no overlap with the paperdoll, stats, hotbar or
// message log) and the tooltip shows every modifier Barony's own item code gives (boots of speed: DEX)
static void runD88(int dt)
{
	auto& pl = *players[0];
	static Uint32 bootsUid = 0;
	if ( pl.shootmode || pl.gui_mode != GUI_MODE_INVENTORY ) { pl.shootmode = false; pl.openStatusScreen(GUI_MODE_INVENTORY, INVENTORY_MODE_ITEM); }
	if ( dt == 470 )
	{
		emptyInventory(0);
		setTab(0, 0);
		const ItemType t[] = { BRONZE_SWORD, IRON_SWORD, LEATHER_HELM, IRON_HELM, WOODEN_SHIELD, FOOD_APPLE, GEM_RUBY, IRON_AXE, LEATHER_BREASTPIECE };
		for ( ItemType it : t ) { newItem(it, SERVICABLE, 0, 1, 0, true, &stats[0]->inventory); }
		Item* b = newItem(LEATHER_BOOTS_SPEED, SERVICABLE, 1, 1, 0, true, &stats[0]->inventory);
		bootsUid = b ? b->uid : 0;
		inspectOpen(0, nullptr);
	}
	else if ( dt == 472 )
	{
		auto e = ownRowsForTest(0);
		for ( size_t i = 0; i < e.size(); ++i ) { if ( e[i].row.uid == bootsUid ) { setFocus(0, 0, (int)i); } }

	}
	else if ( dt == 475 ) { g_shot = "apx-d88-grid-tip.png"; } // in this phase, before the quarter view
	else if ( dt == 477 )
	{
		const auto& vs = viewState(0);
		std::string low = vs.tipText;
		for ( auto& c : low ) { c = (char)tolower(c); }
		const int dex = low.find("dex") != std::string::npos ? 1 : 0;
		const int speed = low.find("speed") != std::string::npos ? 1 : 0;
		Frame* inv = pl.inventoryUI.frame;
		Frame* g = inv ? inv->findFrame("apx list") : nullptr;
		Frame* base = inv ? inv->findFrame("inventory base") : nullptr;
		Frame* tip = inv ? inv->findFrame("apx tip") : nullptr;
		SDL_Rect gr = g ? g->getSize() : SDL_Rect{ -1, -1, -1, -1 }, br = base ? base->getSize() : SDL_Rect{ -1, -1, -1, -1 };
		SDL_Rect tr = tip ? tip->getSize() : SDL_Rect{ -1, -1, -1, -1 };
		const int fh = inv ? inv->getSize().h : -1;
		const int inside = g && base && gr.x >= br.x && gr.x + gr.w <= br.x + br.w && gr.y >= br.y && gr.y + gr.h <= br.y + br.h ? 1 : 0;
		const int tipAboveLog = tip && tr.y + tr.h <= fh * 68 / 100 + 1 ? 1 : 0;
		int hpY = 100000;
		for ( Frame* bar : { pl.hud.hpFrame, pl.hud.mpFrame } ) { if ( bar && !bar->isDisabled() && bar->getSize().y > gr.y ) { hpY = std::min(hpY, bar->getSize().y); } }
		const int aboveBars = g && gr.y + gr.h <= hpY ? 1 : 0;
		const int truncated = vs.tipText.find("  ...") != std::string::npos ? 1 : 0;
		const int opaque = g ? 1 : 0; // makeGrid sets alpha 255
		std::string esc;
		for ( char c : vs.tipText ) { if ( c == '\n' ) { esc += "|"; } else if ( c == '"' || c == '\\' ) { esc += '\''; } else if ( (unsigned char)c >= 32 ) { esc += c; } }
		FILE* f = fopen(diagPath().c_str(), "a");
		if ( f )
		{
			fprintf(f, "{\"test\":\"d88\",\"mode\":\"%s\",\"headless\":%d,\"tip_shown\":%d,\"tip_vanilla_lines\":%d,\"tip_has_dex\":%d,\"tip_has_speed\":%d,"
				"\"grid_inside_panel\":%d,\"grid_opaque\":%d,\"tip_above_log\":%d,\"grid_above_hp_bars\":%d,\"hp_bar_y\":%d,\"tip_truncated\":%d,\"grid\":[%d,%d,%d,%d],\"panel\":[%d,%d,%d,%d],\"tip\":[%d,%d,%d,%d],\"frame_h\":%d,"
				"\"cells_icons\":%d,\"tip_text\":\"%s\"}\n",
				vanillaGrid() ? "vanilla" : "apx", (int)apx::headlessOn, (int)vs.tipShown, vs.tipVanillaLines, dex, speed, inside, opaque, tipAboveLog, aboveBars, hpY, truncated,
				gr.x, gr.y, gr.w, gr.h, br.x, br.y, br.w, br.h, tr.x, tr.y, tr.w, tr.h, fh, vs.iconsDrawn, esc.c_str());
			fclose(f);
		}
	}
}

static void runD81(int dt)
{
	auto& pl = *players[0];
	static int sortOk = -1, tabOk = -1, rowsAll = 0, rowsTab = 0;
	if ( pl.shootmode || pl.gui_mode != GUI_MODE_INVENTORY ) { pl.shootmode = false; pl.openStatusScreen(GUI_MODE_INVENTORY, INVENTORY_MODE_ITEM); }
	if ( dt == 331 )
	{
		emptyInventory(0);
		const ItemType t[] = { BRONZE_SWORD, IRON_SWORD, STEEL_SWORD, IRON_AXE, LEATHER_HELM, IRON_HELM, WOODEN_SHIELD, FOOD_APPLE, GEM_RUBY };
		for ( ItemType it : t ) { newItem(it, SERVICABLE, 0, 1, 0, true, &stats[0]->inventory); }
		// wear the bronze sword, so the other blades compare against it
		for ( node_t* n = stats[0]->inventory.first; n; n = n->next ) { Item* i = (Item*)n->element; if ( i->type == BRONZE_SWORD ) { stats[0]->weapon = i; } }
		inspectOpen(0, nullptr);
		shotWhen("tip", "apx-u1c-list.png");
	}
	else if ( dt == 333 ) { setSort(0, apx_logic::COL_WEIGHT, true); }
	else if ( dt == 335 )
	{
		// within each kind group the weights must not rise (descending)
		auto e = ownRowsForTest(0);
		rowsAll = 0; sortOk = 1;
		for ( size_t i = 1; i < e.size(); ++i )
		{
			if ( !e[i].header ) { ++rowsAll; }
			if ( !e[i].header && !e[i - 1].header && e[i].row.kind == e[i - 1].row.kind && e[i].row.mass > e[i - 1].row.mass ) { sortOk = 0; }
		}
		setTab(0, 1 + apx_logic::WEAPONS);
	}
	else if ( dt == 337 )
	{
		auto e = ownRowsForTest(0);
		rowsTab = 0; tabOk = 1;
		for ( auto& x : e ) { if ( !x.header ) { ++rowsTab; if ( x.row.kind != apx_logic::WEAPONS ) { tabOk = 0; } } }
		// select the first unworn blade: its tooltip compares against the bronze sword
		for ( size_t i = 0; i < e.size(); ++i ) { if ( !e[i].header && !e[i].row.equipped ) { setFocus(0, 0, (int)i); break; } }
	}
	else if ( dt == 339 )
	{
		const auto& vs = viewState(0);
		FILE* f = fopen(diagPath().c_str(), "a");
		if ( f )
		{
			fprintf(f, "{\"test\":\"d81\",\"mode\":\"%s\",\"headless\":%d,\"sort_weight_desc_ok\":%d,\"rows_all\":%d,\"tab_weapons_only\":%d,\"rows_weapons\":%d,"
				"\"icons_drawn\":%d,\"tip_shown\":%d,\"tip_lines\":%d,\"tip_delta\":\"%s\",\"tip_vs_worn\":%d,\"sort_col\":%d,\"tab\":%d}\n",
				vanillaGrid() ? "vanilla" : "apx", (int)apx::headlessOn, sortOk, rowsAll, tabOk, rowsTab, vs.iconsDrawn, (int)vs.tipShown, vs.tipLines,
				vs.tipDelta.c_str(), (int)vs.tipVsWorn, vs.sortCol, vs.tab);
			fclose(f);
		}
		setTab(0, 0);
		setSort(0, apx_logic::COL_NAME, false);
	}
}

static void runBench(int dt)
{
	// step 5 first, before the chest and follower phases: a decoration beside the player that is not
	// a station (control: no bench), then the same decoration as a workbench; the three views
	auto& pl = *players[0];
	if ( pl.shootmode || pl.gui_mode != GUI_MODE_INVENTORY ) { pl.shootmode = false; pl.openStatusScreen(GUI_MODE_INVENTORY, INVENTORY_MODE_ITEM); }
	if ( dt == 142 )
	{
		inspectOpen(0, nullptr);
		Entity* me = pl.entity;
		// control: a yellow mailbox, a real prop that is not a station
		g_testDeco = spawnProp(1619, &actMailbox, me->x + 16, me->y);
		R.propOk += propPlacedRight(g_testDeco);
	}
	else if ( dt == 154 )
	{
		R.benchControl = viewState(0).benchShown;
		g_testDeco->flags[PASSABLE] = true; g_testDeco->x += 4000; // the mailbox goes away
		g_testStation = spawnProp(1617, &actWorkbench, pl.entity->x + 16, pl.entity->y); // a workbench as map generation makes one
		R.propOk += propPlacedRight(g_testStation);
	}
	else if ( dt == 166 ) { R.propOk += propPlacedRight(g_testStation); } // still on the floor and upright after its own ticks
	if ( dt >= 158 && dt < 172 ) { ++R.benchWindow; R.benchTicks += viewState(0).benchShown; R.benchOpen += listActive(0); if ( !R.benchShot ) { R.benchShot = 1; shotWhen("bench", "apx-u1b-bench.png"); } }
	if ( dt == 166 ) { probe("bench"); R.benchShown = R.benchTicks > 0; R.benchStation = viewState(0).station; R.benchOps = viewState(0).stationOps; }
	else if ( dt == 172 ) { R.benchTab0Rows = viewState(0).benchRows; benchTab(0, 1); }
	else if ( dt == 176 ) { R.benchTab1Rows = viewState(0).benchRows; R.benchTab1 = viewState(0).benchTab; benchTab(0, 1); }
	else if ( dt == 180 ) { R.benchTab2Rows = viewState(0).benchRows; R.benchTab2 = viewState(0).benchTab; probe("bench-tabs"); benchTab(0, 1);
		// both props removed again (Apocky saw a test prop left floating: the old spawn had z 0)
		for ( Entity* e : { g_testDeco, g_testStation } ) { if ( e ) { list_RemoveNode(e->mynode); } }
		g_testDeco = g_testStation = nullptr; }
}

static void runSteps345(int dtIn)
{
	if ( dtIn <= 180 ) { runBench(dtIn); return; }
	const int dt = dtIn - 40;  // the step 3-4 timeline below starts at 141
	static int transfersBefore = 0;
	auto& pl = *players[0];
	if ( g_testFollower && dt >= 205 && dt < 240 ) { g_testFollower->x = pl.entity->x + 16; g_testFollower->y = pl.entity->y; g_testFollower->vel_x = g_testFollower->vel_y = 0; }
	if ( dt >= 210 && dt < 240 ) { ++R.followerWindow; R.followerTicks += viewState(0).other == OTHER_FOLLOWER; R.followerOpen += listActive(0); }
	if ( g_testFollower && dt >= 212 && dt < 236 && viewState(0).other == OTHER_FOLLOWER )
	{
		if ( R.giveAt < 0 ) { R.giveAt = dt; transfersBefore = viewState(0).transfers; shotWhen("other", "apx-u1b-follower.png"); setFocus(0, 0, 1); activateSelected(0); }
		else if ( R.takeAt < 0 && dt >= R.giveAt + 4 ) { R.takeAt = dt; R.giveOk = viewState(0).transfers == transfersBefore + 1; setFocus(0, 1, 1); activateSelected(0); }
		else if ( R.takeAt >= 0 && dt == R.takeAt + 4 ) { R.followerTakeOk = viewState(0).transfers == transfersBefore + 2; }
	}
	if ( dt >= 200 && (pl.shootmode || pl.gui_mode != GUI_MODE_INVENTORY) ) { pl.shootmode = false; pl.openStatusScreen(GUI_MODE_INVENTORY, INVENTORY_MODE_ITEM); } // closeChest and the new follower close the GUI; a player reopens it
	auto* ui = inputs.getUIInteraction(0);
	static Uint32 menuUid = 0, breakUid = 0;
	if ( dt == 141 )
	{
		emptyInventory(0);
		for ( int k = 0; k < 4; ++k ) { newItem(static_cast<ItemType>(GEM_RUBY + k), SERVICABLE, 0, 1, 0, true, &stats[0]->inventory); }
		stats[0]->GOLD = 10;
	}
	else if ( dt == 146 )
	{
		// step 3a: Enter / confirm on our row opens Barony's context menu for that item
		setFocus(0, 0, 1);
		activateSelected(0);
		menuUid = listState(0).selectedUid;
	}
	else if ( dt == 147 ) { probe("menu+1"); }
	else if ( dt == 150 ) { probe("menu+4"); R.menuOpen = ui->itemMenuOpen; R.menuItemRight = ui->itemMenuItem == menuUid && menuUid != 0; g_shot = "apx-u1b-menu.png"; }
	else if ( dt == 154 )
	{
		ui->itemMenuOpen = false;
		// step 3b: Inspect, through the activation the menu entry calls
		pl.inventoryUI.activateItemContextMenuOption(uidToItem(menuUid), PROMPT_APX_INSPECT);
	}
	else if ( dt == 155 ) { probe("inspect+1"); }
	else if ( dt == 160 ) { probe("inspect+6"); R.inspectShown = viewState(0).inspectShown; g_shot = "apx-u1b-inspect.png"; }
	else if ( dt == 164 )
	{
		inspectOpen(0, nullptr);
		// step 3c: Break through the menu activation
		// a weapon with rolled parts: F6's break needs parts (it declines a plain gem); the stub breaks either
		Item* it = newItem(IRON_SWORD, SERVICABLE, 0, 1, 0, true, &stats[0]->inventory);
		apxPartsBind(it, 1, 3);
		breakUid = it ? it->uid : 0;
		pl.inventoryUI.activateItemContextMenuOption(it, PROMPT_APX_BREAK);
	}
	else if ( dt == 168 ) { R.breakGone = breakUid != 0 && uidToItem(breakUid) == nullptr; }
	else if ( dt == 170 )
	{
		// step 4a: a chest on this floor, opened the way actChest opens it, holding 3 test items
		for ( node_t* n = map.entities ? map.entities->first : nullptr; n && !g_testChest; n = n->next )
		{
			Entity* e = static_cast<Entity*>(n->element);
			if ( e && e->behavior == &actChest && e->getChestInventoryList() ) { g_testChest = e; }
		}
		R.chestFound = g_testChest != nullptr;
		if ( g_testChest )
		{
			list_t* cl = g_testChest->getChestInventoryList();
			list_FreeAll(cl);
			for ( int k = 0; k < 3; ++k ) { newItem(static_cast<ItemType>(GEM_EMERALD + k), SERVICABLE, 0, 1, 0, true, cl); }
			openedChest[0] = g_testChest;
			g_testChest->chestStatus = 1; // as actChest does, so closeChest() really closes it
			g_testChest->chestOpener = 0;
			g_testChest->x = pl.entity->x + 16; g_testChest->y = pl.entity->y; // in reach, or actChest closes it
			pl.openStatusScreen(GUI_MODE_INVENTORY, INVENTORY_MODE_ITEM);
			pl.inventoryUI.chestGUI.openChest(false);
		}
	}
	else if ( dt == 172 || dt == 176 ) { probe(dt == 172 ? "chest+2" : "chest+6"); }
	else if ( dt == 180 && g_testChest )
	{
		probe("chest+10");
		R.chestView = viewState(0).other == OTHER_CHEST;
		R.chestRows = viewState(0).otherRows;
		transfersBefore = viewState(0).transfers;
		setFocus(0, 1, 1);           // the first item row on the chest side
		activateSelected(0);         // take
		g_shot = "apx-u1b-chest.png";
	}
	else if ( dt == 186 && g_testChest )
	{
		R.takeOk = viewState(0).transfers == transfersBefore + 1;
		setFocus(0, 0, 1);           // our first item row
		activateSelected(0);         // store
	}
	else if ( dt == 192 && g_testChest )
	{
		R.storeOk = viewState(0).transfers == transfersBefore + 2;
		// over the limit: a heavy load, then taking must be refused
		newItem(STEEL_BREASTPIECE, SERVICABLE, 0, 99, 0, true, &stats[0]->inventory);
		int refused = viewState(0).refusedHeavy;
		setFocus(0, 1, 1);
		activateSelected(0);
		R.heavyRefused = viewState(0).refusedHeavy == refused + 1;
	}
	else if ( dt == 200 )
	{
		if ( g_testChest ) { g_testChest->closeChest(); openedChest[0] = nullptr; }
		emptyInventory(0);
		for ( int k = 0; k < 3; ++k ) { newItem(static_cast<ItemType>(GEM_RUBY + k), SERVICABLE, 0, 1, 0, true, &stats[0]->inventory); }
		// step 4b: a follower beside us
		Entity* me = pl.entity;
		g_testFollower = summonMonster(HUMAN, (long)me->x + 16, (long)me->y, true);
		R.followerFound = g_testFollower != nullptr;
		if ( g_testFollower ) { forceFollower(*me, *g_testFollower); }
	}
	else if ( dt == 212 ) { probe("follower"); }
	else if ( dt == 240 && g_testFollower )
	{
		R.followerView = R.followerTicks > 0;
		// remove the test follower: its uid from our followers, then the entity
		Uint32 uid = g_testFollower->getUID();
		for ( node_t* n = stats[0]->FOLLOWERS.first; n; n = n->next ) { if ( n->element && *(Uint32*)n->element == uid ) { list_RemoveNode(n); break; } }
		list_RemoveNode(g_testFollower->mynode);
		g_testFollower = nullptr;
	}
	else if ( dt == 290 )
	{
		FILE* f = fopen(diagPath().c_str(), "a");
		if ( f )
		{
			fprintf(f, "{\"test\":\"steps345\",\"mode\":\"%s\",\"menu_open\":%d,\"menu_item_right\":%d,\"inspect_shown\":%d,\"break_gone\":%d,"
				"\"chest_found\":%d,\"chest_view\":%d,\"chest_rows\":%d,\"take_ok\":%d,\"store_ok\":%d,\"heavy_refused\":%d,"
				"\"follower_found\":%d,\"follower_view\":%d,\"give_ok\":%d,\"follower_take_ok\":%d,\"bench_control_shown\":%d,\"bench_shown\":%d,\"bench_station\":\"%s\",\"bench_ops\":%d,"
				"\"bench_rows_craft_salvage_formbook\":[%d,%d,%d],\"follower_view_ticks\":[%d,%d],\"bench_ticks\":[%d,%d],\"list_open_ticks_follower_bench\":[%d,%d],\"bench_tabs_reached\":[%d,%d],\"provider\":\"%s\",\"props_placed_right\":[%d,3],\"headless\":%d}\n",
				vanillaGrid() ? "vanilla" : "apx", R.menuOpen, R.menuItemRight, R.inspectShown, R.breakGone, R.chestFound, R.chestView, R.chestRows,
				R.takeOk, R.storeOk, R.heavyRefused, R.followerFound, R.followerView, R.giveOk, R.followerTakeOk, R.benchControl, R.benchShown,
				apx_logic::stationName(R.benchStation), R.benchOps, R.benchTab0Rows, R.benchTab1Rows, R.benchTab2Rows, R.followerTicks, R.followerWindow, R.benchTicks, R.benchWindow, R.followerOpen, R.benchOpen, R.benchTab1, R.benchTab2, apx_bench::provider().name, R.propOk, (int)apx::headlessOn);
			fclose(f);
		}
	}
}

static void runUiPhases()
{
	const int dt = g_ticks - g_mark;
	auto& pl = *players[0];
	static ListState A, B, C, D;
	static int itemsAtStart = 0, itemsAfter = 0;
	if ( dt == 1 )
	{
		itemsAtStart = g_loadedItems; // counted in the same tick as the load
		listTestPad(true);
		pl.shootmode = false;
		pl.openStatusScreen(GUI_MODE_INVENTORY, INVENTORY_MODE_ITEM);
	}
	else if ( dt == 60 ) { A = listState(0); g_shot = "apx-u1-full.png"; }
	else if ( dt >= 70 && dt <= 86 && (dt - 70) % 4 == 0 ) { listTestPadPress(); }
	else if ( dt == 100 ) { B = listState(0); }
	else if ( dt >= 104 && dt <= 134 && (dt - 104) % 6 == 0 ) { keystatus[SDLK_RIGHT] = ((dt - 104) / 6) % 2 == 0; } // 3 presses (one cell each) held 6 ticks (~120 ms, a human tap)
	else if ( dt == 140 ) { keystatus[SDLK_RIGHT] = false; C = listState(0); itemsAfter = (int)list_Size(&stats[0]->inventory); }
	else if ( dt >= 141 && dt < 331 ) { runSteps345(dt); }
	else if ( dt >= 331 && dt < 340 ) { runD81(dt); }
	else if ( dt >= 340 && dt < 370 ) { runF6Bench(dt); }
	else if ( dt >= 370 && dt < 380 ) { runPouch(dt); }
	else if ( dt >= 380 && dt < 410 ) { runComponents(dt); }
	else if ( dt >= 410 && dt < 440 ) { runLive86(dt); }
	else if ( dt >= 440 && dt < 470 ) { runLiveF6(dt); }
	else if ( dt >= 470 && dt < 480 ) { runD88(dt); }
	else if ( dt == 480 )
	{
		splitscreen = true;
		pl.bSplitscreen = true;
		pl.camera().winx = 0;
		pl.camera().winy = 0;
		pl.camera().winw = xres / 2;
		pl.camera().winh = yres / 2;
		pl.hud.compactLayoutMode = Player::HUD_t::COMPACT_LAYOUT_INVENTORY;
		pl.shootmode = false;
		pl.openStatusScreen(GUI_MODE_INVENTORY, INVENTORY_MODE_ITEM);
	}
	else if ( dt == 550 ) { D = listState(0); g_shot = "apx-u1-quarter.png"; }
	else if ( dt == 590 )
	{
		FILE* f = fopen(diagPath().c_str(), "a");
		if ( f )
		{
			auto st = [&](const char* k, const ListState& s) {
				fprintf(f, "\"%s\":{\"shown\":%d,\"rows\":%d,\"visible\":%d,\"drawn\":%d,\"selected\":%d,\"scroll\":%d,\"moves\":%d},",
					k, s.rowsDrawn > 0, s.rowCount, s.visibleRows, s.rowsDrawn, s.selected, s.scroll, s.moves); };
			fprintf(f, "{\"test\":\"list_ui\",\"mode\":\"%s\",\"headless\":%d,", vanillaGrid() ? "vanilla" : "apx", (int)apx::headlessOn);
			st("full", A); st("after_gamepad5", B); st("after_keyboard3", C); st("quarter", D);
			int itemsNow = itemsAfter;
			fprintf(f, "\"items_at_start\":%d,\"items_after_140_ticks\":%d,\"items_kept\":%s,", itemsAtStart, itemsNow, itemsNow == itemsAtStart ? "true" : "false");
			fprintf(f, "\"gamepad_moved\":%d,\"keyboard_moved\":%d,\"quarter_fewer_rows\":%s,\"res\":[%d,%d]}\n",
				B.selected - A.selected, C.selected - B.selected, D.visibleRows > 0 && D.visibleRows < A.visibleRows ? "true" : "false", xres, yres);
			fclose(f);
		}
		printlog("[apx] inventory self-test UI phases done; gamepad %d, keyboard %d, quarter rows %d of %d", B.selected - A.selected, C.selected - B.selected, D.visibleRows, A.visibleRows);
		g_test.clear();
		if ( apx::selftestall::armed() )
		{
			// back to one full-screen view for the stages that follow
			splitscreen = false;
			pl.bSplitscreen = false;
			pl.camera().winx = 0;
			pl.camera().winy = 0;
			pl.camera().winw = xres;
			pl.camera().winh = yres;
			pl.closeAllGUIs(CLOSEGUI_ENABLE_SHOOTMODE, CLOSEGUI_CLOSE_ALL);
			char buf[200];
			const bool quarterFewer = D.visibleRows > 0 && D.visibleRows < A.visibleRows;
			snprintf(buf, sizeof buf, ",\"ui\":\"ran\",\"gamepad_moved\":%d,\"keyboard_moved\":%d,\"quarter_fewer_rows\":%s,\"items_kept\":%s",
				B.selected - A.selected, C.selected - B.selected, quarterFewer ? "true" : "false",
				(int)list_Size(&stats[0]->inventory) == itemsAtStart ? "true" : "false");
			g_gatesPass = g_gatesPass && quarterFewer && (int)list_Size(&stats[0]->inventory) == itemsAtStart;
			allModeDone(buf);
		}
		else { mainloop = 0; }
	}
}

static std::string g_shotWhenView, g_shotWhenName;
void shotWhen(const char* view, const char* name) { g_shotWhenView = view; g_shotWhenName = name; }
static Uint32 g_shotRunTick = 0;
static int g_shotRun = 0;
void shotIfView(const char* view)
{
	// only after the view has been drawn on 5 consecutive game ticks (the test's forced follower
	// makes the game close the inventory every few ticks; a lone frame may never reach the screen)
	if ( g_shotWhenView.empty() || g_shotWhenView != view ) { return; }
	g_shotRun = (ticks == g_shotRunTick + 1) ? g_shotRun + 1 : (ticks == g_shotRunTick ? g_shotRun : 1);
	g_shotRunTick = ticks;
	if ( g_shotRun >= 5 ) { g_shot = g_shotWhenName; g_shotWhenView.clear(); }
}

const char* takeShotRequest()
{
	static std::string last;
	if ( g_shot.empty() ) { return nullptr; }
	last = g_shot;
	g_shot.clear();
	return last.c_str();
}

} // namespace apx_inv
