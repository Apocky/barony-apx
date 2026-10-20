// apx: our inventory UI (slice U1, D57; card BU1 steps 2-5).
// Step 2: the list frame -- rows grouped by kind with name, mass and condition; a carried-mass bar
//   with the limit as a number. Mouse hover selects, wheel scrolls; keyboard arrows; gamepad: the
//   inventory move bindings, taken over from the grid. Sized from the vanilla slot area of the
//   current layout, so compact and split-screen quarter layouts get fewer rows.
// Step 3: row actions -- right click, Enter or gamepad confirm opens Barony's own context menu for
//   the row's item (plus our Inspect and Break entries, playerinventory.cpp); the inspect panel.
// Step 4: a second list beside ours for an open chest, an open shop, or a follower standing next to
//   the player; activating a row moves it to the other side (take, buy obey the mass limit; store,
//   sell, give); right click on our row still opens the context menu. Left/Right switch lists.
// Step 5: the bench shell -- at a recognised station (apx_inv_logic.hpp table) or with the alchemy
//   (alembic) window open, a frame lists the station's operations and an empty Form board (F6).
// Pure parts (entries, navigation, transfer rule, stations, inspect text) live in apx_inv_logic.hpp.
#include "apx_inventory.hpp"
#include "apx/props.hpp"
#include "game.hpp"
#include "stat.hpp"
#include "items.hpp"
#include "player.hpp"
#include "entity.hpp"
#include "draw.hpp"
#include "input.hpp"
#include "menu.hpp"
#include "shops.hpp"
#include "net.hpp"
#include "monster.hpp"
#include "interface/interface.hpp"
#include "ui/Frame.hpp"
#include "ui/Field.hpp"
#include "ui/Font.hpp"
#include "apx_bench_api.hpp"
#include "apx/combat/impact.hpp"
#include <algorithm>

namespace apx_inv
{
using apx_logic::Entry;
static ListState g_list[MAXPLAYERS];
static ViewState g_view[MAXPLAYERS];
static bool g_shown[MAXPLAYERS] = {};
static bool g_keyHeld[MAXPLAYERS][11] = {};
static std::vector<Entry> g_own[MAXPLAYERS], g_other[MAXPLAYERS];
static std::vector<Item*> g_otherItems[MAXPLAYERS];   // parallel to g_other (nullptr for headers)
static SDL_Rect g_ownRect[MAXPLAYERS], g_otherRect[MAXPLAYERS];
static int g_otherScroll[MAXPLAYERS] = {};
static Entity* g_follower[MAXPLAYERS] = {};
static std::vector<Entry> g_bench[MAXPLAYERS];
static int g_benchSel[MAXPLAYERS] = {}, g_benchScroll[MAXPLAYERS] = {}, g_benchTab[MAXPLAYERS] = {};
static uint32_t g_benchForm[MAXPLAYERS] = {};
std::vector<Entry> benchEntries(int player, int tab, int station, Item* selectedOwn, uint32_t selectedForm, std::string& head);
bool benchActivate(int player, int tab, int station, Item* selectedOwn, const Entry& row, uint32_t& selectedForm, std::string& msg);
constexpr int ROW_H = 20, HEAD_H = 34, MAX_ROWS = 40, PANEL_W = 220, TIP_W = 260;
static const char* FONT = "fonts/pixel_maz_multiline.ttf#16#2";

const ListState& listState(int player) { return g_list[std::max(0, std::min(MAXPLAYERS - 1, player))]; }
const ViewState& viewState(int player) { return g_view[std::max(0, std::min(MAXPLAYERS - 1, player))]; }
bool listActive(int player) { return !vanillaGrid() && player >= 0 && player < MAXPLAYERS && g_shown[player]; }

static bool g_testPad = false;
static int g_padPending = 0;
void listTestPad(bool on) { g_testPad = on; }
void listTestPadPress() { ++g_padPending; }

// ---------------------------------------------------------------- rows
static int g_tab[MAXPLAYERS] = {}, g_sortCol[MAXPLAYERS] = {}, g_iconsDrawn[MAXPLAYERS] = {};
static bool g_sortDesc[MAXPLAYERS] = {};

// damage kinds of a weapon (D66 names) from F4b's reference move for its Barony class
static std::string kindsOf(const Item& it)
{
	int cls = -1;
	switch ( checkEquipType(&it) )
	{
		case TYPE_SWORD: cls = apx::combat::CLS_SWORD; break;
		case TYPE_MACE: cls = apx::combat::CLS_MACE; break;
		case TYPE_AXE: cls = apx::combat::CLS_AXE; break;
		case TYPE_SPEAR: cls = apx::combat::CLS_POLEARM; break;
		case TYPE_BOW: case TYPE_PROJECTILE: cls = apx::combat::CLS_RANGED; break;
		default: break;
	}
	if ( itemCategory(&it) == MAGICSTAFF ) { return "magic"; }
	if ( itemCategory(&it) == THROWN && cls < 0 ) { cls = apx::combat::CLS_RANGED; }
	apx::combat::Move m;
	if ( cls < 0 || !apx::combat::classMove(cls, m) ) { return ""; }
	static const char* n[] = { "crushing", "slashing", "piercing" };
	return m.kind >= 0 && m.kind < 3 ? n[m.kind] : "";
}

static apx_logic::Row logicRow(const Item& it, int player, int priceMode)
{
	apx_logic::Row r;
	r.uid = it.uid; r.type = it.type; r.kind = kindOf(it); r.count = it.count; r.mass = it.getWeight();
	r.status = it.status; r.beatitude = it.beatitude; r.identified = it.identified;
	r.equipped = players[player]->paperDoll.enabled && players[player]->paperDoll.isItemOnDoll(it);
	r.name = it.getName();
	if ( it.count > 1 ) { r.name += " x" + std::to_string(it.count); }
	if ( priceMode == 1 ) { r.price = it.buyValue(player); }
	r.value = priceMode == 1 ? r.price : (it.type >= 0 && it.type < NUMITEMS ? items[it.type].gold_value : -1);
	Category c = itemCategory(&it);
	if ( c == WEAPON || c == THROWN || c == MAGICSTAFF ) { r.power = it.weaponGetAttack(stats[player]); r.powerKind = 'D'; }
	else if ( c == ARMOR ) { r.power = it.armorGetAC(stats[player]); r.powerKind = 'A'; }
	r.slot = checkEquipType(&it);
	r.kinds = kindsOf(it);
	r.icon = getItemSpritePath(player, const_cast<Item&>(it));
	apx_bench::PartRow comp;
	if ( apx_bench::provider().componentOf(&it, comp) ) { r.rarity = std::max(0, std::min(4, comp.rarity)); }
	return r;
}

// the item worn in the same place as an item of this equipment type (compare-to-equipped)
static Item* equippedFor(int player, const Item& it, int slot)
{
	Stat* s = stats[player];
	if ( !s ) { return nullptr; }
	Category c = itemCategory(&it);
	if ( c == WEAPON || c == MAGICSTAFF || c == THROWN ) { return s->weapon; }
	switch ( slot )
	{
		case TYPE_HELM: case TYPE_HAT: return s->helmet;
		case TYPE_BREASTPIECE: return s->breastplate;
		case TYPE_BOOTS: return s->shoes;
		case TYPE_SHIELD: case TYPE_OFFHAND: return s->shield;
		case TYPE_GLOVES: return s->gloves;
		case TYPE_CLOAK: return s->cloak;
		case TYPE_RING: return s->ring;
		case TYPE_AMULET: return s->amulet;
		case TYPE_MASK: return s->mask;
		case TYPE_SWORD: case TYPE_AXE: case TYPE_SPEAR: case TYPE_MACE: case TYPE_BOW: case TYPE_PROJECTILE: return s->weapon;
		default: return nullptr;
	}
}

static std::vector<Entry> ownEntries(int player)
{
	std::vector<apx_logic::Row> lr;
	for ( auto& r : rows(player) )
	{
		apx_logic::Row x;
		if ( r.type == -2 ) { x.kind = MATERIALS; x.mass = r.mass; x.name = "Material pouch"; x.status = 4; }
		else if ( r.type < 0 ) { x.kind = GOLD; x.count = r.count; x.mass = r.mass; x.value = r.count; x.name = std::to_string(r.count) + " gold"; }
		else if ( Item* it = uidToItem(r.uid) ) { x = logicRow(*it, player, 0); }
		else { continue; }
		lr.push_back(x);
	}
	return apx_logic::entriesSorted(lr, g_tab[player], g_sortCol[player], g_sortDesc[player]);
}

std::vector<Entry> ownRowsForTest(int player) { return g_own[player]; }
std::vector<Entry> benchRowsForTest(int player) { return g_bench[player]; }
void setTab(int player, int tab) { g_tab[player] = (tab % apx_logic::tabCount() + apx_logic::tabCount()) % apx_logic::tabCount(); g_list[player].selected = 0; g_list[player].scroll = 0; }
void setSort(int player, int col, bool desc) { g_sortCol[player] = std::max(0, std::min(apx_logic::COL_MAX - 1, col)); g_sortDesc[player] = desc; }

static list_t* otherList(int player, int view)
{
	switch ( view )
	{
		case OTHER_CHEST: return openedChest[player] ? openedChest[player]->getChestInventoryList() : nullptr;
		case OTHER_SHOP: return shopInv[player];
		case OTHER_FOLLOWER: { Stat* s = g_follower[player] ? g_follower[player]->getStats() : nullptr; return s ? &s->inventory : nullptr; }
		default: return nullptr;
	}
}

static void buildOther(int player, int view)
{
	g_other[player].clear();
	g_otherItems[player].clear();
	list_t* l = otherList(player, view);
	if ( !l ) { return; }
	std::vector<apx_logic::Row> lr;
	std::vector<Item*> its;
	for ( node_t* n = l->first; n; n = n->next )
	{
		Item* it = static_cast<Item*>(n->element);
		if ( !it || it->type < 0 || it->type >= NUMITEMS || itemCategory(it) == SPELL_CAT ) { continue; }
		if ( view == OTHER_SHOP && it->itemHiddenFromShop ) { continue; }
		lr.push_back(logicRow(*it, player, view == OTHER_SHOP ? 1 : 0));
		its.push_back(it);
	}
	auto e = apx_logic::entriesSorted(lr, 0, g_sortCol[player], g_sortDesc[player]);
	// map entries back to items by uid (entries() sorts)
	for ( auto& en : e )
	{
		Item* found = nullptr;
		if ( !en.header ) { for ( Item* it : its ) { if ( it->uid == en.row.uid ) { found = it; break; } } }
		g_otherItems[player].push_back(found);
	}
	g_other[player] = std::move(e);
}

// a follower of this player standing within 1.5 tiles
static Entity* nearFollower(int player)
{
	Entity* me = players[player]->entity;
	if ( !me || !stats[player] || multiplayer == CLIENT ) { return nullptr; }
	for ( node_t* n = stats[player]->FOLLOWERS.first; n; n = n->next )
	{
		Entity* f = n->element ? uidToEntity(*static_cast<Uint32*>(n->element)) : nullptr;
		if ( f && f->getStats() && (f->x - me->x) * (f->x - me->x) + (f->y - me->y) * (f->y - me->y) <= 24.0 * 24.0 ) { return f; }
	}
	return nullptr;
}

// the station decoration within 1.5 tiles, from the logic table
static int nearStation(int player)
{
	Entity* me = players[player]->entity;
	if ( !me || !map.entities ) { return apx_logic::NONE; }
	for ( node_t* n = map.entities->first; n; n = n->next )
	{
		Entity* e = static_cast<Entity*>(n->element);
		if ( !e ) { continue; }
		int s = apx_logic::stationOfModel(e->sprite);
		if ( s != apx_logic::NONE && (e->x - me->x) * (e->x - me->x) + (e->y - me->y) * (e->y - me->y) <= 24.0 * 24.0 ) { return s; }
	}
	if ( apx::props::nearFieldBench(player) ) { return apx_logic::FIELD; } // apx BP1: a table used as a field bench
	return apx_logic::NONE;
}

// ---------------------------------------------------------------- actions
static void rememberSel(int player)
{
	auto& st = g_list[player];
	const auto& own = g_own[player];
	if ( st.selected >= 0 && st.selected < (int)own.size() && !own[st.selected].header )
	{
		st.selectedUid = own[st.selected].row.uid;
		st.selectedKind = own[st.selected].row.kind;
	}
}

static void moveSel(int player, int d)
{
	auto& st = g_list[player];
	auto& vs = g_view[player];
	if ( d ) { ++st.moves; }
	if ( vs.focus == 1 && !g_other[player].empty() )
	{
		vs.otherSelected = apx_logic::move(g_other[player], vs.otherSelected, d);
		return;
	}
	if ( vs.focus == 2 )
	{
		g_benchSel[player] = apx_logic::move(g_bench[player], g_benchSel[player], d);
		return;
	}
	st.selected = apx_logic::move(g_own[player], st.selected, d);
	if ( st.selected < st.scroll ) { st.scroll = st.selected; }
	if ( st.selected > 0 && st.selected - 1 < st.scroll && g_own[player][st.selected - 1].header ) { st.scroll = st.selected - 1; }
	if ( st.visibleRows > 0 && st.selected >= st.scroll + st.visibleRows ) { st.scroll = st.selected - st.visibleRows + 1; }
	rememberSel(player);
}

void setFocus(int player, int focus, int row)
{
	auto& vs = g_view[player];
	if ( focus == 1 && vs.other == OTHER_NONE && vs.benchShown ) { focus = 2; }
	vs.focus = focus == 1 && vs.other != OTHER_NONE ? 1 : focus == 2 && vs.benchShown ? 2 : 0;
	if ( row >= 0 )
	{
		if ( vs.focus == 1 ) { vs.otherSelected = apx_logic::move(g_other[player], row, 0); }
		else if ( vs.focus == 2 ) { g_benchSel[player] = apx_logic::move(g_bench[player], row, 0); }
		else { g_list[player].selected = apx_logic::move(g_own[player], row, 0); rememberSel(player); }
	}
}

bool openRowMenu(int player, Item* item)
{
	if ( !item ) { return false; }
	auto* ui = inputs.getUIInteraction(player);
	auto& pl = *players[player];
	const SDL_Rect& r = g_ownRect[player];
	const auto& st = g_list[player];
	ui->itemMenuOpen = true;
	ui->itemMenuFromHotbar = false;
	ui->itemMenuItem = item->uid;
	ui->itemMenuSelected = 0;
	ui->toggleclick = true; // click-and-release mode: stays open until the next click
	int x = pl.camera_virtualx1() + r.x + r.w + 8;
	int y = pl.camera_virtualy1() + r.y + HEAD_H + (st.selected - st.scroll) * ROW_H;
	ui->itemMenuX = x + (x & 1);
	ui->itemMenuY = std::max(y + (y & 1), pl.camera_virtualy1());
	pl.GUI.activateModule(Player::GUI_t::MODULE_INVENTORY);
	return true;
}

void inspectOpen(int player, Item* item)
{
	auto& vs = g_view[player];
	vs.inspectShown = item != nullptr;
	vs.inspectUid = item ? item->uid : 0;
}

static bool tooHeavy(int player, Sint32 mass)
{
	if ( vanillaGrid() || apx_logic::canTake(carriedMass(player), mass, massLimit(player)) ) { return false; }
	messagePlayer(player, MESSAGE_INVENTORY, "Too heavy to carry (%s of %s).", massText(carriedMass(player) + mass).c_str(), massText(massLimit(player)).c_str());
	++g_view[player].refusedHeavy;
	return true;
}

// move the other side's selected row to us
static bool takeOther(int player)
{
	auto& vs = g_view[player];
	if ( vs.otherSelected < 0 || vs.otherSelected >= (int)g_otherItems[player].size() ) { return false; }
	Item* it = g_otherItems[player][vs.otherSelected];
	if ( !it ) { return false; }
	switch ( vs.other )
	{
		case OTHER_CHEST:
			if ( tooHeavy(player, it->getWeight()) ) { return false; }
			if ( !takeItemFromChest(player, it, it->count, nullptr, false, true) ) { return false; }
			break;
		case OTHER_SHOP:
		{
			Sint32 one = it->type >= 0 && it->type < NUMITEMS ? items[it->type].weight : 0;
			if ( tooHeavy(player, one) ) { return false; }
			bool consumed = false;
			if ( !buyItemFromShop(player, it, consumed) ) { return false; }
			break;
		}
		case OTHER_FOLLOWER:
		{
			if ( tooHeavy(player, it->getWeight()) ) { return false; }
			Item* copy = newItem(it->type, it->status, it->beatitude, it->count, it->appearance, it->identified, &stats[player]->inventory);
			if ( !copy ) { return false; }
			Stat* fs = g_follower[player] ? g_follower[player]->getStats() : nullptr;
			if ( fs ) // the follower no longer holds it as equipment either
			{
				for ( Item** slot : { &fs->helmet, &fs->breastplate, &fs->gloves, &fs->shoes, &fs->shield, &fs->weapon, &fs->cloak, &fs->amulet, &fs->ring, &fs->mask } )
				{
					if ( *slot == it ) { *slot = nullptr; }
				}
			}
			if ( it->node ) { list_RemoveNode(it->node); }
			break;
		}
		default: return false;
	}
	++vs.transfers;
	return true;
}

// move our selected row to the other side
static bool giveOwn(int player, Item* it)
{
	auto& vs = g_view[player];
	if ( !it ) { return false; }
	if ( players[player]->paperDoll.enabled && players[player]->paperDoll.isItemOnDoll(*it) )
	{
		messagePlayer(player, MESSAGE_INVENTORY, "Unequip the %s first.", it->getName());
		return false;
	}
	switch ( vs.other )
	{
		case OTHER_CHEST:
			if ( !openedChest[player] || !openedChest[player]->addItemToChestFromInventory(player, it, it->count, false, nullptr) ) { return false; }
			break;
		case OTHER_SHOP:
			if ( !sellItemToShop(player, it) ) { return false; }
			break;
		case OTHER_FOLLOWER:
		{
			Stat* fs = g_follower[player] ? g_follower[player]->getStats() : nullptr;
			if ( !fs ) { return false; }
			Item* copy = newItem(it->type, it->status, it->beatitude, it->count, it->appearance, it->identified, &fs->inventory);
			if ( !copy ) { return false; }
			if ( it->node ) { list_RemoveNode(it->node); }
			break;
		}
		default: return false;
	}
	++vs.transfers;
	return true;
}

bool activateSelected(int player)
{
	auto& vs = g_view[player];
	if ( !listActive(player) ) { return false; }
	if ( vs.focus == 1 ) { return takeOther(player); }
	if ( vs.focus == 2 )
	{
		int s = g_benchSel[player];
		if ( s < 0 || s >= (int)g_bench[player].size() ) { return false; }
		const auto& own0 = g_own[player];
		int os = g_list[player].selected;
		Item* target = os >= 0 && os < (int)own0.size() && !own0[os].header && own0[os].row.uid ? uidToItem(own0[os].row.uid) : nullptr;
		std::string msg;
		bool ok = benchActivate(player, g_benchTab[player], vs.station, target, g_bench[player][s], g_benchForm[player], msg);
		if ( !msg.empty() ) { messagePlayer(player, MESSAGE_INVENTORY, "%s", msg.c_str()); }
		vs.lastBenchMsg = msg;
		if ( ok ) { ++vs.benchActions; }
		return ok;
	}
	const auto& own = g_own[player];
	int sel = g_list[player].selected;
	if ( sel < 0 || sel >= (int)own.size() || own[sel].header || own[sel].row.uid == 0 ) { return false; }
	Item* it = uidToItem(own[sel].row.uid);
	if ( vs.other != OTHER_NONE ) { return giveOwn(player, it); }
	return openRowMenu(player, it);
}

void setBenchTab(int player, int tab)
{
	g_benchTab[player] = ((tab % 6) + 6) % 6;
	g_benchSel[player] = 0; g_benchScroll[player] = 0;
}

void benchTab(int player, int d)
{
	g_benchTab[player] = (g_benchTab[player] + 6 + d) % 6;
	g_benchSel[player] = 0; g_benchScroll[player] = 0;
	++g_view[player].benchTabSwitches;
}

bool listHandleMove(int player)
{
	if ( !listActive(player) ) { return false; }
	auto& in = Input::inputs[player];
	int d = 0, side = 0;
	for ( const char* b : { "InventoryMoveUp", "InventoryMoveUpAnalog" } ) { if ( in.binaryToggle(b) ) { in.consumeBinaryToggle(b); d = -1; } }
	for ( const char* b : { "InventoryMoveDown", "InventoryMoveDownAnalog" } ) { if ( in.binaryToggle(b) ) { in.consumeBinaryToggle(b); d = 1; } }
	if ( g_view[player].other != OTHER_NONE )
	{
		for ( const char* b : { "InventoryMoveLeft", "InventoryMoveLeftAnalog" } ) { if ( in.binaryToggle(b) ) { in.consumeBinaryToggle(b); side = -1; } }
		for ( const char* b : { "InventoryMoveRight", "InventoryMoveRightAnalog" } ) { if ( in.binaryToggle(b) ) { in.consumeBinaryToggle(b); side = 1; } }
	}
	if ( side ) { setFocus(player, side > 0 ? 1 : 0, -1); }
	for ( const char* b : { "MenuPageLeft", "MenuPageRight" } )
	{
		if ( !in.binaryToggle(b) ) { continue; }
		in.consumeBinaryToggle(b);
		int d2 = b[8] == 'L' ? -1 : 1;
		if ( g_view[player].benchShown && g_view[player].focus == 2 ) { benchTab(player, d2); } else { setTab(player, g_tab[player] + d2); }
	}
	if ( in.binaryToggle("MenuAlt2") ) { in.consumeBinaryToggle("MenuAlt2"); setSort(player, (g_sortCol[player] + 1) % apx_logic::COL_MAX, g_sortDesc[player]); }
	if ( d ) { moveSel(player, d); }
	return d != 0 || side != 0;
}

// ---------------------------------------------------------------- frames
static Uint32 rarityColor(int r)
{
	static const Uint32 c[] = { makeColor(235, 235, 235, 255), makeColor(110, 220, 110, 255), makeColor(110, 160, 255, 255),
		makeColor(200, 120, 255, 255), makeColor(255, 160, 60, 255) };
	return r >= 0 && r <= 4 ? c[r] : c[0];
}

static Field* label(Frame* parent, const char* name, int len, int hj)
{
	auto f = parent->addField(name, len);
	f->setFont(FONT);
	f->setHJustify(hj);
	f->setVJustify(Field::justify_t::CENTER);
	f->setTextColor(makeColor(235, 235, 235, 255));
	return f;
}

static Frame* makeList(Frame* parent, const char* name, int player)
{
	Frame* list = parent->addFrame(name);
	list->setOwner(player);
	list->setColor(makeColor(14, 11, 8, 230));
	list->setBorder(0);
	auto head = list->addField("apx head", 128);
	head->setFont(FONT);
	head->setHJustify(Field::justify_t::LEFT);
	head->setVJustify(Field::justify_t::TOP);
	head->setTextColor(makeColor(255, 255, 255, 255));
	auto barBg = list->addFrame("apx bar bg");
	barBg->setColor(makeColor(60, 50, 40, 255));
	barBg->addFrame("apx bar");
	Frame* tabs = list->addFrame("apx tabs");
	for ( int t = 0; t < 8; ++t )
	{
		char n[16]; snprintf(n, sizeof(n), "tab %d", t);
		Frame* tf = tabs->addFrame(n); tf->setOwner(player);
		label(tf, "t", 16, Field::justify_t::CENTER);
	}
	Frame* cols = list->addFrame("apx cols");
	for ( int c = 0; c < apx_logic::COL_MAX; ++c )
	{
		char n[16]; snprintf(n, sizeof(n), "col %d", c);
		Frame* cf = cols->addFrame(n); cf->setOwner(player);
		label(cf, "t", 16, c == 0 ? Field::justify_t::LEFT : Field::justify_t::RIGHT);
	}
	for ( int i = 0; i < MAX_ROWS; ++i )
	{
		char n[32];
		snprintf(n, sizeof(n), "apx row %d", i);
		auto row = list->addFrame(n);
		row->setOwner(player);
		row->addImage(SDL_Rect{ 4, 1, 18, 18 }, makeColor(255, 255, 255, 255), "", "icon");
		label(row, "text", 192, Field::justify_t::LEFT);
		label(row, "mass", 32, Field::justify_t::RIGHT);
		for ( int c = 1; c < apx_logic::COL_MAX; ++c ) { char cn[8]; snprintf(cn, sizeof(cn), "c%d", c); label(row, cn, 24, Field::justify_t::RIGHT); }
	}
	return list;
}

// the tooltip: a large icon and up to 14 lines
static Frame* makeTip(Frame* parent, int player)
{
	Frame* tip = parent->addFrame("apx tip");
	tip->setOwner(player);
	tip->setColor(makeColor(20, 16, 12, 240));
	tip->addImage(SDL_Rect{ 8, 8, 64, 64 }, makeColor(255, 255, 255, 255), "", "big");
	for ( int i = 0; i < 14; ++i ) { char n[16]; snprintf(n, sizeof(n), "line %d", i); label(tip, n, 128, Field::justify_t::LEFT); }
	return tip;
}

static Frame* makePanel(Frame* parent, const char* name, int player, int lines)
{
	Frame* p = parent->addFrame(name);
	p->setOwner(player);
	p->setColor(makeColor(24, 18, 12, 236));
	for ( int i = 0; i < lines; ++i )
	{
		char n[32];
		snprintf(n, sizeof(n), "line %d", i);
		auto f = p->addField(n, 160);
		f->setFont(FONT);
		f->setHJustify(Field::justify_t::LEFT);
		f->setVJustify(Field::justify_t::CENTER);
		f->setTextColor(makeColor(255, 255, 255, 255));
	}
	return p;
}

static void setLines(Frame* p, const std::vector<std::string>& lines, int w, int maxLines)
{
	for ( int i = 0; i < maxLines; ++i )
	{
		char n[32];
		snprintf(n, sizeof(n), "line %d", i);
		if ( Field* f = p->findField(n) )
		{
			f->setSize(SDL_Rect{ 8, 6 + i * ROW_H, w - 16, ROW_H });
			f->setText(i < (int)lines.size() ? lines[i].c_str() : "");
			f->setTextColor(i == 0 ? makeColor(230, 190, 90, 255) : makeColor(235, 235, 235, 255));
		}
	}
}

static std::string fitText(Field* f, std::string s, int maxW)
{
	if ( Font* font = Font::get(f->getFont()) ) // fields do not clip: shorten to the column
	{
		int w = 0, h = 0;
		while ( s.size() > 2 && font->sizeText(s.c_str(), &w, &h) == 0 && w > maxW ) { s.erase(s.size() - 2); s += "."; }
	}
	return s;
}

// draw one list; returns rows drawn. sel/scroll are this list's; mouse hover sets focus and selection.
// Style: tabs (category filter) and columns (Wt, Val, Dmg/AC, Cond, clickable to sort) for item lists;
// without columns a single right-hand column shows mass or price (the bench).
struct ListStyle { bool tabs = false; bool cols = false; int* tab = nullptr; int* sortCol = nullptr; bool* sortDesc = nullptr; };
static int listTop(const ListStyle& ls) { return 32 + (ls.tabs ? 20 : 0) + (ls.cols ? 18 : 0); }

static int drawList(int player, Frame* list, const SDL_Rect& r, const std::vector<Entry>& e, int sel, int& scroll, int visible,
	const char* headText, long long carried, long long limit, int focusId, bool focused, const ListStyle& ls = ListStyle())
{
	list->setSize(r);
	auto& in = Input::inputs[player];
	const bool mouse = inputs.getVirtualMouse(player)->draw_cursor;
	if ( auto hf = list->findField("apx head") ) { hf->setSize(SDL_Rect{ 6, 2, r.w - 12, 18 }); hf->setText(fitText(hf, headText, r.w - 14).c_str()); }
	if ( auto barBg = list->findFrame("apx bar bg") )
	{
		barBg->setSize(SDL_Rect{ 6, 22, r.w - 12, 6 });
		barBg->setDisabled(limit <= 0);
		if ( auto bar = barBg->findFrame("apx bar") )
		{
			long long lim = std::max<long long>(1, limit);
			bar->setSize(SDL_Rect{ 0, 0, (int)((long long)(r.w - 12) * std::min(carried, lim) / lim), 6 });
			bar->setColor(carried > lim ? makeColor(220, 60, 40, 255) : makeColor(200, 170, 60, 255));
		}
	}
	// column layout: numeric columns from the right; narrow lists keep Wt and Dmg/AC only
	const bool wide = r.w >= 400;
	int colX[apx_logic::COL_MAX] = {}, colW[apx_logic::COL_MAX] = {};
	bool colOn[apx_logic::COL_MAX] = { true, true, wide, true, wide };
	if ( !ls.cols ) { for ( int c = 1; c < apx_logic::COL_MAX; ++c ) { colOn[c] = false; } }
	static const int width[apx_logic::COL_MAX] = { 0, 66, 52, 66, 80 };
	int x = r.w - 10;
	for ( int c = apx_logic::COL_MAX - 1; c >= 1; --c ) { if ( colOn[c] ) { colW[c] = width[c]; x -= width[c]; colX[c] = x; } }
	const int nameX = 28, nameW = (ls.cols ? x : r.w - 74) - nameX - 4;

	int y = 32;
	if ( Frame* tabs = list->findFrame("apx tabs") )
	{
		tabs->setDisabled(!ls.tabs);
		if ( ls.tabs )
		{
			tabs->setSize(SDL_Rect{ 4, y, r.w - 8, 18 });
			int tw = (r.w - 8) / apx_logic::tabCount();
			for ( int t = 0; t < 8; ++t )
			{
				char n[16]; snprintf(n, sizeof(n), "tab %d", t);
				Frame* tf = tabs->findFrame(n);
				if ( !tf ) { continue; }
				tf->setDisabled(t >= apx_logic::tabCount());
				if ( t >= apx_logic::tabCount() ) { continue; }
				tf->setSize(SDL_Rect{ t * tw, 0, tw - 2, 18 });
				bool on = ls.tab && *ls.tab == t;
				tf->setColor(on ? makeColor(120, 90, 30, 255) : makeColor(36, 28, 20, 255));
				if ( Field* f = tf->findField("t") )
				{
					f->setSize(SDL_Rect{ 0, 0, tw - 2, 18 });
					f->setText(apx_logic::tabShort(t));
					f->setTextColor(on ? makeColor(255, 235, 170, 255) : makeColor(190, 180, 160, 255));
				}
				if ( mouse && ls.tab && tf->capturesMouseInRealtimeCoords() && in.binaryToggle("MenuLeftClick") ) { in.consumeBinaryToggle("MenuLeftClick"); setTab(player, t); }
			}
			y += 20;
		}
	}
	if ( Frame* cols = list->findFrame("apx cols") )
	{
		cols->setDisabled(!ls.cols);
		if ( ls.cols )
		{
			cols->setSize(SDL_Rect{ 0, y, r.w, 18 });
			for ( int c = 0; c < apx_logic::COL_MAX; ++c )
			{
				char n[16]; snprintf(n, sizeof(n), "col %d", c);
				Frame* cf = cols->findFrame(n);
				if ( !cf ) { continue; }
				cf->setDisabled(!colOn[c]);
				if ( !colOn[c] ) { continue; }
				SDL_Rect cr = c == 0 ? SDL_Rect{ nameX, 0, nameW, 18 } : SDL_Rect{ colX[c], 0, colW[c], 18 };
				cf->setSize(cr);
				bool on = ls.sortCol && *ls.sortCol == c;
				std::string t = std::string(apx_logic::columnName(c)) + (on ? (ls.sortDesc && *ls.sortDesc ? " v" : " ^") : "");
				if ( Field* f = cf->findField("t") )
				{
					f->setSize(SDL_Rect{ 0, 0, cr.w, 18 });
					f->setText(t.c_str());
					f->setTextColor(on ? makeColor(255, 220, 120, 255) : makeColor(170, 160, 140, 255));
				}
				if ( mouse && ls.sortCol && cf->capturesMouseInRealtimeCoords() && in.binaryToggle("MenuLeftClick") )
				{
					in.consumeBinaryToggle("MenuLeftClick");
					if ( *ls.sortCol == c ) { *ls.sortDesc = !*ls.sortDesc; } else { *ls.sortCol = c; *ls.sortDesc = c != 0; }
				}
			}
			y += 18;
		}
	}

	const int n = (int)e.size();
	scroll = std::max(0, std::min(scroll, std::max(0, n - visible)));
	if ( sel < scroll ) { scroll = sel; }
	if ( sel >= scroll + visible ) { scroll = sel - visible + 1; }
	static const Uint32 cond[] = { makeColor(230, 70, 60, 255), makeColor(240, 140, 60, 255), makeColor(240, 210, 90, 255),
		makeColor(225, 225, 225, 255), makeColor(255, 255, 255, 255) };
	int drawn = 0;
	for ( int i = 0; i < MAX_ROWS; ++i )
	{
		char nm[32];
		snprintf(nm, sizeof(nm), "apx row %d", i);
		Frame* row = list->findFrame(nm);
		Field* f = row ? row->findField("text") : nullptr;
		Field* mf = row ? row->findField("mass") : nullptr;
		if ( !row || !f || !mf ) { continue; }
		int idx = scroll + i;
		if ( i >= visible || idx >= n ) { row->setDisabled(true); continue; }
		row->setDisabled(false);
		row->setSize(SDL_Rect{ 2, y + i * ROW_H, r.w - 4, ROW_H });
		if ( mouse && row->capturesMouseInRealtimeCoords() && !e[idx].header ) { setFocus(player, focusId, idx); } // hover selects
		const auto& d = e[idx];
		++drawn;
		auto* icon = row->findImage("icon");
		Field* cf[apx_logic::COL_MAX] = {};
		for ( int c = 1; c < apx_logic::COL_MAX; ++c ) { char cn[8]; snprintf(cn, sizeof(cn), "c%d", c); cf[c] = row->findField(cn); if ( cf[c] ) { cf[c]->setText(""); } }
		if ( d.header )
		{
			if ( icon ) { icon->disabled = true; }
			f->setSize(SDL_Rect{ 4, 0, r.w - 12, ROW_H });
			f->setText(d.row.name.c_str());
			f->setTextColor(makeColor(230, 190, 90, 255));
			mf->setText("");
			row->setColor(makeColor(0, 0, 0, 0));
			continue;
		}
		if ( icon )
		{
			icon->disabled = d.row.icon.empty();
			if ( !d.row.icon.empty() ) { icon->path = d.row.icon; icon->pos = SDL_Rect{ 6, 1, 18, 18 }; if ( focusId == 0 ) { ++g_iconsDrawn[player]; } }
		}
		const bool info = d.row.price == -2;
		f->setSize(SDL_Rect{ info ? 10 : nameX, 0, info ? r.w - 16 : nameW, ROW_H });
		f->setText(fitText(f, d.row.name + (d.row.equipped ? " [E]" : ""), info ? r.w - 20 : nameW).c_str());
		int c0 = d.row.type < 0 ? 4 : std::max(0, std::min(4, d.row.status));
		f->setTextColor(d.row.equipped ? makeColor(140, 220, 255, 255) : d.row.rarity >= 0 ? rarityColor(d.row.rarity) : cond[c0]);
		if ( ls.cols )
		{
			mf->setText("");
			auto put = [&](int c, const std::string& t, Uint32 col) { if ( colOn[c] && cf[c] ) { cf[c]->setSize(SDL_Rect{ colX[c], 0, colW[c] - 4, ROW_H }); cf[c]->setText(t.c_str()); cf[c]->setTextColor(col); } };
			put(apx_logic::COL_WEIGHT, massText(d.row.mass), makeColor(200, 200, 200, 255));
			put(apx_logic::COL_VALUE, d.row.value >= 0 ? std::to_string(d.row.value) + (d.row.price >= 0 ? " g" : "") : "", d.row.price >= 0 ? makeColor(240, 210, 90, 255) : makeColor(200, 190, 150, 255));
			put(apx_logic::COL_POWER, d.row.power >= 0 ? std::string(d.row.powerKind == 'A' ? "AC " : "") + std::to_string(d.row.power) : "", makeColor(220, 220, 220, 255));
			put(apx_logic::COL_CONDITION, d.row.type >= 0 ? apx_logic::conditionShort(d.row.status) : "", cond[c0]);
		}
		else
		{
			mf->setSize(SDL_Rect{ r.w - 72, 0, 60, ROW_H });
			mf->setText(d.row.price >= 0 ? (std::to_string(d.row.price) + " g").c_str() : info ? "" : massText(d.row.mass).c_str());
			mf->setTextColor(d.row.price >= 0 ? makeColor(240, 210, 90, 255) : makeColor(200, 200, 200, 255));
		}
		bool isSel = idx == sel;
		row->setColor(isSel ? (focused ? makeColor(120, 90, 30, 255) : makeColor(70, 55, 25, 255))
			: (i % 2 ? makeColor(34, 27, 20, 255) : makeColor(26, 21, 15, 255)));
	}
	return drawn;
}

// the tooltip for one item: large icon, name, kind, stats with compare-to-equipped, kinds, parts
static int drawTip(int player, Frame* tip, Item* it, int x, int y)
{
	if ( !it ) { tip->setDisabled(true); return 0; }
	auto r = logicRow(*it, player, openedChest[player] || !players[player]->shopGUI.bOpen ? 0 : 0);
	std::vector<std::pair<std::string, Uint32>> L;
	const Uint32 W = makeColor(235, 235, 235, 255), G = makeColor(170, 160, 140, 255);
	L.push_back({ r.name, makeColor(255, 220, 120, 255) });
	L.push_back({ std::string(kindName(r.kind)) + (r.equipped ? "  (equipped)" : ""), G });
	L.push_back({ "Weight " + massText(r.mass) + (r.value >= 0 ? "   Value " + std::to_string(r.value) + " g" : ""), W });
	std::string delta;
	if ( r.power >= 0 )
	{
		Item* eq = equippedFor(player, *it, r.slot);
		apx_logic::Row er;
		if ( eq && eq != it ) { er = logicRow(*eq, player, 0); }
		delta = eq == it ? "" : apx_logic::compareDelta(r, eq && eq != it ? &er : nullptr);
		std::string line = std::string(r.powerKind == 'A' ? "Armour " : "Damage ") + std::to_string(r.power);
		if ( !delta.empty() ) { line += "   " + delta + (eq && eq != it ? " vs worn" : " vs none"); }
		Uint32 col = delta.empty() || delta == "=" ? W : (delta[0] == '+' ? makeColor(120, 230, 120, 255) : makeColor(240, 110, 90, 255));
		L.push_back({ line, col });
	}
	L.push_back({ std::string("Condition: ") + apx_logic::conditionWord(r.status), W });
	L.push_back({ r.identified ? "Beatitude: " + std::string(r.beatitude > 0 ? "+" : "") + std::to_string(r.beatitude) : "Beatitude: unknown", W });
	if ( !r.kinds.empty() ) { L.push_back({ "Damage kinds: " + r.kinds, W }); }
	// D86: a component item shows its own rarity and affixes; an assembled item lists its components
	apx_bench::PartRow self;
	int affixLines = 0;
	if ( apx_bench::provider().componentOf(it, self) )
	{
		L.push_back({ std::string("Component, ") + apx_logic::rarityName(self.rarity) + (self.material.empty() ? "" : " " + self.material), rarityColor(self.rarity) });
		for ( auto& a : self.affixes ) { L.push_back({ "  " + a, makeColor(180, 200, 255, 255) }); ++affixLines; }
	}
	auto parts = apx_bench::provider().parts(it);
	if ( !parts.empty() )
	{
		L.push_back({ "Components", makeColor(230, 190, 90, 255) });
		for ( auto& p : parts )
		{
			std::string nm = p.name.empty() ? p.material : p.name;
			L.push_back({ "  " + p.slotName + ": " + nm + "  q" + std::to_string(p.quality), rarityColor(p.rarity) });
			if ( !p.affixes.empty() )
			{
				std::string ax;
				for ( auto& a : p.affixes ) { ax += (ax.empty() ? "" : ", ") + a; }
				L.push_back({ "    " + ax, makeColor(180, 200, 255, 255) });
				++affixLines;
			}
		}
	}
	const int w = TIP_W, top = 80;
	int h = top + (int)std::min<size_t>(L.size() - 2, 12) * ROW_H + 10;
	tip->setDisabled(false);
	tip->setSize(SDL_Rect{ x, y, w, h });
	if ( auto* big = tip->findImage("big") ) { big->path = r.icon; big->disabled = r.icon.empty(); big->pos = SDL_Rect{ 8, 8, 64, 64 }; }
	for ( int i = 0; i < 14; ++i )
	{
		char n[16]; snprintf(n, sizeof(n), "line %d", i);
		Field* f = tip->findField(n);
		if ( !f ) { continue; }
		if ( i >= (int)L.size() ) { f->setText(""); continue; }
		// the first two lines sit beside the big icon
		SDL_Rect fr = i < 2 ? SDL_Rect{ 80, 10 + i * 22, w - 88, 20 } : SDL_Rect{ 10, top + (i - 2) * ROW_H, w - 20, ROW_H };
		f->setSize(fr);
		f->setText(fitText(f, L[i].first, fr.w).c_str());
		f->setTextColor(L[i].second);
	}
	auto& vs = g_view[player];
	vs.tipLines = (int)L.size();
	vs.tipDelta = delta;
	vs.tipAffixLines = affixLines;
	vs.tipParts = (int)parts.size();
	{
		Item* eqw = r.power >= 0 ? equippedFor(player, *it, r.slot) : nullptr;
		vs.tipVsWorn = eqw && eqw != it;
	}
	return (int)L.size();
}

static unsigned g_uiFrames = 0;
unsigned uiFrames() { return g_uiFrames; }

void listUpdate(int player, Frame* inventoryFrame, Frame* invSlots, Frame* backpackSlots)
{
	if ( player == 0 ) { ++g_uiFrames; }
	if ( vanillaGrid() || !inventoryFrame || !invSlots || player < 0 || player >= MAXPLAYERS ) { return; }
	Frame* list = inventoryFrame->findFrame("apx list");
	if ( !list )
	{
		list = makeList(inventoryFrame, "apx list", player);
		makeList(inventoryFrame, "apx other", player);
		makePanel(inventoryFrame, "apx inspect", player, 8);
		makeTip(inventoryFrame, player);
		makeList(inventoryFrame, "apx bench", player);
	}
	Frame* other = inventoryFrame->findFrame("apx other");
	Frame* inspect = inventoryFrame->findFrame("apx inspect");
	Frame* bench = inventoryFrame->findFrame("apx bench");
	Frame* tip = inventoryFrame->findFrame("apx tip");
	auto& pl = *players[player];
	auto& st = g_list[player];
	auto& vs = g_view[player];
	bool open = pl.isLocalPlayer() && !pl.shootmode && !nohud
		&& (pl.gui_mode == GUI_MODE_INVENTORY || pl.gui_mode == GUI_MODE_SHOP)
		&& !inventoryFrame->isDisabled() && pl.inventory_mode == INVENTORY_MODE_ITEM
		&& !(pl.inventoryUI.bCompactView && pl.hud.compactLayoutMode != Player::HUD_t::COMPACT_LAYOUT_INVENTORY);
	g_shown[player] = open;
	list->setDisabled(!open);
	if ( !open )
	{
		other->setDisabled(true); inspect->setDisabled(true); bench->setDisabled(true); tip->setDisabled(true); vs.tipShown = false;
		vs.inspectShown = false; vs.benchShown = false; vs.other = OTHER_NONE; vs.focus = 0;
		return;
	}
	invSlots->setDisabled(true);
	if ( backpackSlots ) { backpackSlots->setDisabled(true); }
	if ( Frame* bag = inventoryFrame->findFrame("inventory backpack") ) { bag->setDisabled(true); }

	// geometry: the slot area of this layout down to the bottom of the base panel
	SDL_Rect r = invSlots->getSize();
	if ( r.w <= 0 || r.h <= 0 ) { r = SDL_Rect{ 0, 202, 210, 242 }; }
	if ( !pl.inventoryUI.bCompactView && backpackSlots )
	{
		const SDL_Rect& b = backpackSlots->getSize();
		r.h = std::max(r.h, b.y + b.h - r.y);
	}
	if ( Frame* base = inventoryFrame->findFrame("inventory base") )
	{
		const SDL_Rect& b = base->getSize();
		r.h = std::max(r.h, b.y + (pl.inventoryUI.bCompactView ? 250 : b.h) - r.y);
	}
	r.h = std::max(HEAD_H + ROW_H, std::min(r.h, inventoryFrame->getSize().h - r.y));
	// D81: the list is wider than the old slot area where the view allows (columns need room)
	r.w = std::max(r.w, std::min(470, inventoryFrame->getSize().w * 37 / 100));
	g_ownRect[player] = r;
	ListStyle ownStyle; ownStyle.tabs = true; ownStyle.cols = true; ownStyle.tab = &g_tab[player]; ownStyle.sortCol = &g_sortCol[player]; ownStyle.sortDesc = &g_sortDesc[player];
	ListStyle otherStyle; otherStyle.cols = true; otherStyle.sortCol = &g_sortCol[player]; otherStyle.sortDesc = &g_sortDesc[player];
	st.visibleRows = std::max(1, std::min(MAX_ROWS, (r.h - listTop(ownStyle)) / ROW_H));

	// which second list: an open chest, an open shop, or a follower next to us
	int view = OTHER_NONE;
	g_follower[player] = nullptr;
	if ( openedChest[player] && otherList(player, OTHER_CHEST) ) { view = OTHER_CHEST; }
	else if ( pl.shopGUI.bOpen && shopInv[player] ) { view = OTHER_SHOP; }
	else if ( (g_follower[player] = nearFollower(player)) != nullptr ) { view = OTHER_FOLLOWER; }
	if ( view != vs.other ) { vs.focus = 0; vs.otherSelected = 0; g_otherScroll[player] = 0; }
	vs.other = view;

	g_own[player] = ownEntries(player);
	st.rowCount = (int)g_own[player].size();
	auto holds = [&](int i) { const auto& en = g_own[player][i]; return !en.header && en.row.uid == st.selectedUid && (st.selectedUid || en.row.kind == st.selectedKind); };
	bool stillThere = st.selected >= 0 && st.selected < st.rowCount && holds(st.selected);
	for ( int i = 0; i < st.rowCount && !stillThere; ++i ) // keep the selection on the same item when rows move
	{
		const auto& en = g_own[player][i];
		if ( !en.header && en.row.uid == st.selectedUid && (st.selectedUid || en.row.kind == st.selectedKind) ) { st.selected = i; break; }
	}
	st.selected = apx_logic::move(g_own[player], st.selected, 0);
	buildOther(player, view);
	vs.otherRows = (int)g_other[player].size();
	vs.otherSelected = apx_logic::move(g_other[player], vs.otherSelected, 0);
	if ( vs.other == OTHER_NONE ) { vs.focus = 0; }

	// input: the gamepad path (whichever of this and handleInventoryMovement runs first), arrows,
	// Left/Right between lists, Enter activates, right click on a row activates (ours: the menu)
	if ( g_testPad && g_padPending > 0 && player == 0 )
	{
		// self-test: the state a D-pad press leaves in the binding, set just before it is read
		auto& b = const_cast<std::unordered_map<std::string, Input::binding_t>&>(Input::inputs[0].getBindings())["InventoryMoveDown"];
		b.binary = true;
		b.consumed = false;
		--g_padPending;
	}
	auto& in = Input::inputs[player];
	if ( inputs.hasController(player) || g_testPad )
	{
		listHandleMove(player);
		if ( !inputs.getUIInteraction(player)->itemMenuOpen && in.binaryToggle("MenuConfirm") ) { in.consumeBinaryToggle("MenuConfirm"); activateSelected(player); }
	}
	if ( inputs.bPlayerUsingKeyboardControl(player) && !inputs.getUIInteraction(player)->itemMenuOpen )
	{
		const SDL_Keycode keys[11] = { SDLK_UP, SDLK_DOWN, SDLK_LEFT, SDLK_RIGHT, SDLK_RETURN, SDLK_LEFTBRACKET, SDLK_RIGHTBRACKET,
			SDLK_PAGEUP, SDLK_PAGEDOWN, SDLK_HOME, SDLK_END };
		for ( int k = 0; k < 11; ++k )
		{
			bool down = keystatus[keys[k]];
			if ( down && !g_keyHeld[player][k] )
			{
				if ( k == 0 ) { moveSel(player, -1); }
				else if ( k == 1 ) { moveSel(player, 1); }
				else if ( k == 2 ) { setFocus(player, 0, -1); }
				else if ( k == 3 ) { setFocus(player, vs.focus == 0 ? 1 : 2, -1); }
				else if ( k == 4 ) { activateSelected(player); }
				else if ( k == 5 || k == 6 ) { if ( vs.benchShown ) { benchTab(player, k == 5 ? -1 : 1); } }
				else if ( k == 7 || k == 8 ) { setTab(player, g_tab[player] + (k == 7 ? -1 : 1)); }          // PageUp / PageDown: category
				else if ( k == 9 ) { setSort(player, (g_sortCol[player] + 1) % apx_logic::COL_MAX, g_sortDesc[player]); } // Home: next column
				else { setSort(player, g_sortCol[player], !g_sortDesc[player]); }                           // End: flip direction
			}
			g_keyHeld[player][k] = down;
		}
	}
	if ( list->capturesMouseInRealtimeCoords() || (other && !other->isDisabled() && other->capturesMouseInRealtimeCoords()) )
	{
		for ( int dir = -1; dir <= 1; dir += 2 )
		{
			const char* b = dir < 0 ? "MenuScrollUp" : "MenuScrollDown";
			if ( in.binaryToggle(b) )
			{
				in.consumeBinaryToggle(b);
				if ( vs.focus == 1 ) { g_otherScroll[player] += dir; } else { st.scroll += dir; }
			}
		}
		if ( in.binaryToggle("MenuRightClick") && inputs.getVirtualMouse(player)->draw_cursor )
		{
			in.consumeBinaryToggle("MenuRightClick");
			if ( vs.focus == 1 ) { takeOther(player); }
			else
			{
				const auto& own = g_own[player];
				if ( st.selected >= 0 && st.selected < (int)own.size() && !own[st.selected].header ) { openRowMenu(player, uidToItem(own[st.selected].row.uid)); }
			}
		}
	}

	// our list
	Sint32 carried = carriedMass(player), limit = std::max<Sint32>(1, massLimit(player));
	std::string head = massText(carried) + " / " + massText(limit);
	g_iconsDrawn[player] = 0;
	st.rowsDrawn = drawList(player, list, r, g_own[player], st.selected, st.scroll, st.visibleRows, head.c_str(), carried, limit, 0, vs.focus == 0, ownStyle);
	vs.iconsDrawn = g_iconsDrawn[player];
	vs.tab = g_tab[player]; vs.sortCol = g_sortCol[player]; vs.sortDesc = g_sortDesc[player];
	if ( st.selected >= 0 && st.selected < st.rowCount && !g_own[player][st.selected].header )
	{
		st.selectedUid = g_own[player][st.selected].row.uid;
		st.selectedKind = g_own[player][st.selected].row.kind;
	}

	// the right-hand column: second list, else inspect, else bench
	SDL_Rect rr{ r.x + r.w + 4, r.y, std::max(PANEL_W, std::min(330, inventoryFrame->getSize().w / 4)), r.h };
	g_otherRect[player] = rr;
	other->setDisabled(view == OTHER_NONE);
	if ( view != OTHER_NONE )
	{
		if ( pl.inventoryUI.chestFrame && view == OTHER_CHEST ) { pl.inventoryUI.chestFrame->setDisabled(true); }
		if ( pl.shopGUI.shopFrame && view == OTHER_SHOP ) { pl.shopGUI.shopFrame->setDisabled(true); }
		long long otherMass = 0;
		for ( auto& en : g_other[player] ) { otherMass += en.header ? 0 : en.row.mass; }
		std::string oh = view == OTHER_CHEST ? "Chest " + massText((Sint32)otherMass)
			: view == OTHER_SHOP ? "Shop  " + std::to_string(stats[player]->GOLD) + " gold"
			: std::string("Pack ") + (g_follower[player]->getStats() ? g_follower[player]->getStats()->name : "") + " " + massText((Sint32)otherMass);
		drawList(player, other, rr, g_other[player], vs.otherSelected, g_otherScroll[player], (r.h - listTop(otherStyle)) / ROW_H, oh.c_str(), otherMass, 0, 1, vs.focus == 1, otherStyle);
		shotIfView("other");
	}
	// inspect panel (closed by Escape / cancel, or when its item is gone)
	Item* insp = vs.inspectShown ? uidToItem(vs.inspectUid) : nullptr;
	if ( vs.inspectShown && (!insp || in.binaryToggle("MenuCancel") || keystatus[SDLK_ESCAPE]) )
	{
		if ( in.binaryToggle("MenuCancel") ) { in.consumeBinaryToggle("MenuCancel"); }
		vs.inspectShown = false;
		insp = nullptr;
	}
	inspect->setDisabled(true); // Inspect pins the tooltip (D81) instead of the old text panel
	if ( false )
	{
		SDL_Rect ir{ view == OTHER_NONE ? rr.x : rr.x + PANEL_W + 4, r.y, PANEL_W, 8 * ROW_H + 12 };
		inspect->setSize(ir);
		auto il = apx_logic::inspectLines(logicRow(*insp, player, 0));
		for ( auto& p : apx_bench::provider().parts(insp) ) { il.insert(il.end() - 1, "  " + p.slotName + ": " + p.material + " q" + std::to_string(p.quality)); }
		if ( il.size() > 8 ) { il.resize(8); }
		setLines(inspect, il, ir.w, 8);
	}
	// bench shell: a station decoration next to us, or the alchemy (alembic) window open
	if ( ticks % 10 == 0 || vs.station == apx_logic::NONE )
	{
		vs.station = nearStation(player);
		if ( vs.station == apx_logic::NONE && GenericGUI[player].alchemyGUI.bOpen ) { vs.station = apx_logic::ALEMBIC; }
	}
	vs.benchShown = vs.station != apx_logic::NONE && view == OTHER_NONE;
	bench->setDisabled(!vs.benchShown);
	if ( !vs.benchShown && vs.focus == 2 ) { vs.focus = 0; }
	if ( vs.benchShown )
	{
		vs.stationOps = (int)apx_logic::stationOps(vs.station).size();
		const auto& own0 = g_own[player];
		Item* target = st.selected >= 0 && st.selected < (int)own0.size() && !own0[st.selected].header && own0[st.selected].row.uid ? uidToItem(own0[st.selected].row.uid) : nullptr;
		std::string bh;
		g_bench[player] = benchEntries(player, g_benchTab[player], vs.station, target, g_benchForm[player], bh);
		g_benchSel[player] = apx_logic::move(g_bench[player], g_benchSel[player], 0);
		vs.benchRows = (int)g_bench[player].size();
		vs.benchTab = g_benchTab[player];
		SDL_Rect br{ rr.x, r.y, rr.w, r.h };
		drawList(player, bench, br, g_bench[player], g_benchSel[player], g_benchScroll[player], st.visibleRows, bh.c_str(), carried, limit, 2, vs.focus == 2);
		shotIfView("bench");
	}
	// the tooltip: pinned by Inspect, else the focused list's selected item; right of the last panel
	Item* tipItem = insp;
	if ( !tipItem && vs.focus == 1 && vs.otherSelected >= 0 && vs.otherSelected < (int)g_otherItems[player].size() ) { tipItem = g_otherItems[player][vs.otherSelected]; }
	if ( !tipItem && vs.focus == 0 && st.selected >= 0 && st.selected < st.rowCount && !g_own[player][st.selected].header && g_own[player][st.selected].row.uid ) { tipItem = uidToItem(g_own[player][st.selected].row.uid); }
	int tipX = (view != OTHER_NONE || vs.benchShown) ? rr.x + rr.w + 4 : r.x + r.w + 4;
	const int sheetW = 210; // the character sheet on the right edge
	if ( tipX + TIP_W > inventoryFrame->getSize().w - sheetW ) { tipX = std::max(0, inventoryFrame->getSize().w - sheetW - TIP_W - 4); }
	vs.tipShown = drawTip(player, tip, tipItem, tipX, r.y) > 0;
	if ( vs.tipShown ) { shotIfView("tip"); }
}
} // namespace apx_inv
