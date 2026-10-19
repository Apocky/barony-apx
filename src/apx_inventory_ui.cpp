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
#include <algorithm>

namespace apx_inv
{
using apx_logic::Entry;
static ListState g_list[MAXPLAYERS];
static ViewState g_view[MAXPLAYERS];
static bool g_shown[MAXPLAYERS] = {};
static bool g_keyHeld[MAXPLAYERS][7] = {};
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
constexpr int ROW_H = 20, HEAD_H = 34, MAX_ROWS = 40, PANEL_W = 220;
static const char* FONT = "fonts/pixel_maz_multiline.ttf#16#2";

const ListState& listState(int player) { return g_list[std::max(0, std::min(MAXPLAYERS - 1, player))]; }
const ViewState& viewState(int player) { return g_view[std::max(0, std::min(MAXPLAYERS - 1, player))]; }
bool listActive(int player) { return !vanillaGrid() && player >= 0 && player < MAXPLAYERS && g_shown[player]; }

static bool g_testPad = false;
static int g_padPending = 0;
void listTestPad(bool on) { g_testPad = on; }
void listTestPadPress() { ++g_padPending; }

// ---------------------------------------------------------------- rows
static apx_logic::Row logicRow(const Item& it, int player, int priceMode)
{
	apx_logic::Row r;
	r.uid = it.uid; r.type = it.type; r.kind = kindOf(it); r.count = it.count; r.mass = it.getWeight();
	r.status = it.status; r.beatitude = it.beatitude; r.identified = it.identified;
	r.equipped = players[player]->paperDoll.enabled && players[player]->paperDoll.isItemOnDoll(it);
	r.name = it.getName();
	if ( it.count > 1 ) { r.name += " x" + std::to_string(it.count); }
	if ( priceMode == 1 ) { r.price = it.buyValue(player); }
	return r;
}

static std::vector<Entry> ownEntries(int player)
{
	std::vector<apx_logic::Row> lr;
	for ( auto& r : rows(player) )
	{
		apx_logic::Row x;
		if ( r.type < 0 ) { x.kind = GOLD; x.count = r.count; x.mass = r.mass; x.name = std::to_string(r.count) + " gold"; }
		else if ( Item* it = uidToItem(r.uid) ) { x = logicRow(*it, player, 0); }
		else { continue; }
		lr.push_back(x);
	}
	return apx_logic::entries(lr);
}

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
	auto e = apx_logic::entries(lr);
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
	return apx_logic::NONE;
}

// ---------------------------------------------------------------- actions
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
		else { g_list[player].selected = apx_logic::move(g_own[player], row, 0); }
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

void benchTab(int player, int d)
{
	g_benchTab[player] = (g_benchTab[player] + 3 + d) % 3;
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
	if ( g_view[player].benchShown )
	{
		for ( const char* b : { "MenuPageLeft", "MenuPageRight" } )
		{
			if ( in.binaryToggle(b) ) { in.consumeBinaryToggle(b); benchTab(player, b[8] == 'L' ? -1 : 1); }
		}
	}
	if ( d ) { moveSel(player, d); }
	return d != 0 || side != 0;
}

// ---------------------------------------------------------------- frames
static Frame* makeList(Frame* parent, const char* name, int player)
{
	Frame* list = parent->addFrame(name);
	list->setOwner(player);
	list->setColor(makeColor(16, 12, 8, 224));
	list->setBorder(0);
	auto head = list->addField("apx head", 128);
	head->setFont(FONT);
	head->setHJustify(Field::justify_t::LEFT);
	head->setVJustify(Field::justify_t::TOP);
	head->setTextColor(makeColor(255, 255, 255, 255));
	auto barBg = list->addFrame("apx bar bg");
	barBg->setColor(makeColor(60, 50, 40, 255));
	barBg->addFrame("apx bar");
	for ( int i = 0; i < MAX_ROWS; ++i )
	{
		char n[32];
		snprintf(n, sizeof(n), "apx row %d", i);
		auto row = list->addFrame(n);
		row->setOwner(player);
		auto f = row->addField("text", 192);
		f->setFont(FONT);
		f->setHJustify(Field::justify_t::LEFT);
		f->setVJustify(Field::justify_t::CENTER);
		auto m = row->addField("mass", 32);
		m->setFont(FONT);
		m->setHJustify(Field::justify_t::RIGHT);
		m->setVJustify(Field::justify_t::CENTER);
	}
	return list;
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
static int drawList(int player, Frame* list, const SDL_Rect& r, const std::vector<Entry>& e, int sel, int& scroll, int visible,
	const char* headText, long long carried, long long limit, int focusId, bool focused)
{
	list->setSize(r);
	if ( auto hf = list->findField("apx head") ) { hf->setSize(SDL_Rect{ 6, 2, r.w - 12, 18 }); hf->setText(headText); }
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
		row->setSize(SDL_Rect{ 4, HEAD_H + i * ROW_H, r.w - 8, ROW_H });
		if ( inputs.getVirtualMouse(player)->draw_cursor && row->capturesMouseInRealtimeCoords() && !e[idx].header )
		{
			setFocus(player, focusId, idx); // hover selects (and focuses that list)
		}
		const auto& d = e[idx];
		++drawn;
		if ( d.header )
		{
			f->setSize(SDL_Rect{ 2, 0, r.w - 12, ROW_H });
			f->setText(d.row.name.c_str());
			f->setTextColor(makeColor(230, 190, 90, 255));
			mf->setText("");
			row->setColor(makeColor(0, 0, 0, 0));
			continue;
		}
		f->setSize(SDL_Rect{ 10, 0, r.w - 74, ROW_H });
		f->setText(fitText(f, d.row.name + (d.row.equipped ? " [E]" : ""), r.w - 78).c_str());
		mf->setSize(SDL_Rect{ r.w - 72, 0, 60, ROW_H });
		mf->setText(d.row.price >= 0 ? (std::to_string(d.row.price) + " g").c_str() : massText(d.row.mass).c_str());
		mf->setTextColor(d.row.price >= 0 ? makeColor(240, 210, 90, 255) : makeColor(200, 200, 200, 255));
		int c = d.row.type < 0 ? 4 : std::max(0, std::min(4, d.row.status));
		f->setTextColor(d.row.equipped ? makeColor(140, 220, 255, 255) : cond[c]);
		bool isSel = idx == sel;
		row->setColor(isSel ? (focused ? makeColor(120, 90, 30, 255) : makeColor(70, 55, 25, 255))
			: (i % 2 ? makeColor(36, 28, 20, 255) : makeColor(28, 22, 16, 255)));
	}
	return drawn;
}

void listUpdate(int player, Frame* inventoryFrame, Frame* invSlots, Frame* backpackSlots)
{
	if ( vanillaGrid() || !inventoryFrame || !invSlots || player < 0 || player >= MAXPLAYERS ) { return; }
	Frame* list = inventoryFrame->findFrame("apx list");
	if ( !list )
	{
		list = makeList(inventoryFrame, "apx list", player);
		makeList(inventoryFrame, "apx other", player);
		makePanel(inventoryFrame, "apx inspect", player, 8);
		makeList(inventoryFrame, "apx bench", player);
	}
	Frame* other = inventoryFrame->findFrame("apx other");
	Frame* inspect = inventoryFrame->findFrame("apx inspect");
	Frame* bench = inventoryFrame->findFrame("apx bench");
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
		other->setDisabled(true); inspect->setDisabled(true); bench->setDisabled(true);
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
	g_ownRect[player] = r;
	st.visibleRows = std::max(1, std::min(MAX_ROWS, (r.h - HEAD_H) / ROW_H));

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
	for ( int i = 0; i < st.rowCount; ++i ) // keep the selection on the same item when rows move
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
		const SDL_Keycode keys[7] = { SDLK_UP, SDLK_DOWN, SDLK_LEFT, SDLK_RIGHT, SDLK_RETURN, SDLK_LEFTBRACKET, SDLK_RIGHTBRACKET };
		for ( int k = 0; k < 7; ++k )
		{
			bool down = keystatus[keys[k]];
			if ( down && !g_keyHeld[player][k] )
			{
				if ( k == 0 ) { moveSel(player, -1); }
				else if ( k == 1 ) { moveSel(player, 1); }
				else if ( k == 2 ) { setFocus(player, 0, -1); }
				else if ( k == 3 ) { setFocus(player, vs.focus == 0 ? 1 : 2, -1); }
				else if ( k == 4 ) { activateSelected(player); }
				else if ( vs.benchShown ) { benchTab(player, k == 5 ? -1 : 1); }
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
	st.rowsDrawn = drawList(player, list, r, g_own[player], st.selected, st.scroll, st.visibleRows, head.c_str(), carried, limit, 0, vs.focus == 0);
	if ( st.selected >= 0 && st.selected < st.rowCount && !g_own[player][st.selected].header )
	{
		st.selectedUid = g_own[player][st.selected].row.uid;
		st.selectedKind = g_own[player][st.selected].row.kind;
	}

	// the right-hand column: second list, else inspect, else bench
	SDL_Rect rr{ r.x + r.w + 4, r.y, PANEL_W, r.h };
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
		drawList(player, other, rr, g_other[player], vs.otherSelected, g_otherScroll[player], st.visibleRows, oh.c_str(), otherMass, 0, 1, vs.focus == 1);
	}
	// inspect panel (closed by Escape / cancel, or when its item is gone)
	Item* insp = vs.inspectShown ? uidToItem(vs.inspectUid) : nullptr;
	if ( vs.inspectShown && (!insp || in.binaryToggle("MenuCancel") || keystatus[SDLK_ESCAPE]) )
	{
		if ( in.binaryToggle("MenuCancel") ) { in.consumeBinaryToggle("MenuCancel"); }
		vs.inspectShown = false;
		insp = nullptr;
	}
	inspect->setDisabled(!insp);
	if ( insp )
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
	vs.benchShown = vs.station != apx_logic::NONE && view == OTHER_NONE && !insp;
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
		SDL_Rect br{ rr.x, r.y, PANEL_W + 40, r.h };
		drawList(player, bench, br, g_bench[player], g_benchSel[player], g_benchScroll[player], st.visibleRows, bh.c_str(), carried, limit, 2, vs.focus == 2);
	}
}
} // namespace apx_inv
