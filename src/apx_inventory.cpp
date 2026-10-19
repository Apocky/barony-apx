// apx: our inventory, step 1 (virtual grid + mass limit) and the step 1-2 self-test. See apx_inventory.hpp.
#include "apx_inventory.hpp"
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
	++g_ticks;
	if ( g_ticks < 120 ) { return; }
	if ( g_phase == 0 ) { runGates(); g_phase = 1; g_mark = g_ticks; return; }
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
}

// Step 2 phases: the list on screen full size, gamepad and keyboard navigation, then the
// split-screen quarter layout (player 0's view set to a quadrant the way setupSplitscreen does).
static void runUiPhases()
{
	const int dt = g_ticks - g_mark;
	auto& pl = *players[0];
	static ListState A, B, C, D;
	static int itemsAtStart = 0;
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
	else if ( dt >= 104 && dt <= 124 && (dt - 104) % 4 == 0 ) { keystatus[SDLK_DOWN] = ((dt - 104) / 4) % 2 == 0; }
	else if ( dt == 140 ) { keystatus[SDLK_DOWN] = false; C = listState(0); }
	else if ( dt == 150 )
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
	else if ( dt == 220 ) { D = listState(0); g_shot = "apx-u1-quarter.png"; }
	else if ( dt == 260 )
	{
		FILE* f = fopen(diagPath().c_str(), "a");
		if ( f )
		{
			auto st = [&](const char* k, const ListState& s) {
				fprintf(f, "\"%s\":{\"shown\":%d,\"rows\":%d,\"visible\":%d,\"drawn\":%d,\"selected\":%d,\"scroll\":%d,\"moves\":%d},",
					k, s.rowsDrawn > 0, s.rowCount, s.visibleRows, s.rowsDrawn, s.selected, s.scroll, s.moves); };
			fprintf(f, "{\"test\":\"list_ui\",\"mode\":\"%s\",", vanillaGrid() ? "vanilla" : "apx");
			st("full", A); st("after_gamepad5", B); st("after_keyboard3", C); st("quarter", D);
			int itemsNow = (int)list_Size(&stats[0]->inventory);
			fprintf(f, "\"items_at_start\":%d,\"items_after_260_ticks\":%d,\"items_kept\":%s,", itemsAtStart, itemsNow, itemsNow == itemsAtStart ? "true" : "false");
			fprintf(f, "\"gamepad_moved\":%d,\"keyboard_moved\":%d,\"quarter_fewer_rows\":%s,\"res\":[%d,%d]}\n",
				B.selected - A.selected, C.selected - B.selected, D.visibleRows > 0 && D.visibleRows < A.visibleRows ? "true" : "false", xres, yres);
			fclose(f);
		}
		printlog("[apx] inventory self-test UI phases done; gamepad %d, keyboard %d, quarter rows %d of %d", B.selected - A.selected, C.selected - B.selected, D.visibleRows, A.visibleRows);
		g_test.clear();
		mainloop = 0;
	}
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
