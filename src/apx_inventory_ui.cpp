// apx: our inventory, step 2 -- the list frame (slice U1, D57). Rows grouped by kind with name,
// mass and condition; a carried-mass bar with the limit as a number. Mouse: hover and click
// select, wheel scrolls. Keyboard: arrow keys. Gamepad: the inventory move bindings (D-pad and
// stick), taken over from the grid by listHandleMove. The list is sized from the vanilla slot
// area of the current layout, so the compact and split-screen quarter layouts get fewer rows.
#include "apx_inventory.hpp"
#include "game.hpp"
#include "stat.hpp"
#include "items.hpp"
#include "player.hpp"
#include "draw.hpp"
#include "input.hpp"
#include "menu.hpp"
#include "ui/Frame.hpp"
#include "ui/Field.hpp"
#include "ui/Font.hpp"
#include <algorithm>

namespace apx_inv
{
static ListState g_list[MAXPLAYERS];
static bool g_shown[MAXPLAYERS] = {};
static bool g_keyHeld[MAXPLAYERS][2] = {};
constexpr int ROW_H = 20, HEAD_H = 34, MAX_ROWS = 40;

const ListState& listState(int player) { return g_list[std::max(0, std::min(MAXPLAYERS - 1, player))]; }
bool listActive(int player) { return !vanillaGrid() && player >= 0 && player < MAXPLAYERS && g_shown[player]; }

// display entries: a header per kind, then that kind's rows by name; equipped items and gold included
struct Entry { bool header; Row row; std::string name; };
static std::vector<Entry> displayRows(int player)
{
	std::vector<Entry> items;
	for ( auto& r : rows(player) )
	{
		std::string name;
		if ( r.type < 0 ) { name = std::to_string(r.count) + " gold"; }
		else if ( Item* it = uidToItem(r.uid) )
		{
			name = it->getName();
			if ( r.count > 1 ) { name += " x" + std::to_string(r.count); }
		}
		items.push_back({ false, r, name });
	}
	std::stable_sort(items.begin(), items.end(), [](const Entry& a, const Entry& b) {
		return a.row.kind != b.row.kind ? a.row.kind < b.row.kind : a.name < b.name; });
	std::vector<Entry> out;
	int kind = -1;
	for ( auto& e : items )
	{
		if ( e.row.kind != kind )
		{
			kind = e.row.kind;
			Entry h{ true, Row{}, kindName(kind) };
			h.row.kind = kind;
			out.push_back(h);
		}
		out.push_back(e);
	}
	return out;
}
static std::vector<char> g_isHeader[MAXPLAYERS];
static bool g_testPad = false;
void listTestPad(bool on) { g_testPad = on; }
static int g_padPending = 0;
void listTestPadPress() { ++g_padPending; }

static void move(int player, int d)
{
	auto& st = g_list[player];
	if ( d ) { ++st.moves; }
	if ( st.rowCount <= 0 ) { st.selected = 0; return; }
	auto& hdr = g_isHeader[player];
	auto isHeader = [&](int i) { return i >= 0 && i < (int)hdr.size() && hdr[i]; };
	int step = d < 0 ? -1 : 1, n = std::abs(d), sel = st.selected;
	if ( n == 0 && isHeader(sel) ) { n = 1; step = 1; }
	for ( int k = 0; k < n; ++k )
	{
		int next = sel + step;
		while ( next >= 0 && next < st.rowCount && isHeader(next) ) { next += step; }
		if ( next < 0 || next >= st.rowCount ) { break; }
		sel = next;
	}
	st.selected = std::max(0, std::min(st.rowCount - 1, sel));
	if ( st.selected > 0 && isHeader(st.selected - 1) && st.selected - 1 < st.scroll ) { st.scroll = st.selected - 1; }
	if ( st.selected < st.scroll ) { st.scroll = st.selected; }
	if ( st.visibleRows > 0 && st.selected >= st.scroll + st.visibleRows ) { st.scroll = st.selected - st.visibleRows + 1; }
}

bool listHandleMove(int player)
{
	if ( !listActive(player) ) { return false; }
	auto& in = Input::inputs[player];
	int d = 0;
	for ( const char* b : { "InventoryMoveUp", "InventoryMoveUpAnalog" } )
	{
		if ( in.binaryToggle(b) ) { in.consumeBinaryToggle(b); d = -1; }
	}
	for ( const char* b : { "InventoryMoveDown", "InventoryMoveDownAnalog" } )
	{
		if ( in.binaryToggle(b) ) { in.consumeBinaryToggle(b); d = 1; }
	}
	if ( d == 0 ) { return false; }
	move(player, d);
	return true;
}

static Field* rowField(Frame* list, int i, Frame** rowOut)
{
	char n[32];
	snprintf(n, sizeof(n), "apx row %d", i);
	Frame* row = list->findFrame(n);
	if ( rowOut ) { *rowOut = row; }
	return row ? row->findField("text") : nullptr;
}

void listUpdate(int player, Frame* inventoryFrame, Frame* invSlots, Frame* backpackSlots)
{
	if ( vanillaGrid() || !inventoryFrame || !invSlots || player < 0 || player >= MAXPLAYERS ) { return; }
	Frame* list = inventoryFrame->findFrame("apx list");
	if ( !list )
	{
		list = inventoryFrame->addFrame("apx list");
		list->setOwner(player);
		list->setColor(makeColor(16, 12, 8, 224));
		list->setBorder(0);
		auto head = list->addField("apx head", 128);
		head->setFont("fonts/pixel_maz_multiline.ttf#16#2");
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
			f->setFont("fonts/pixel_maz_multiline.ttf#16#2");
			f->setHJustify(Field::justify_t::LEFT);
			f->setVJustify(Field::justify_t::CENTER);
			auto m = row->addField("mass", 32);
			m->setFont("fonts/pixel_maz_multiline.ttf#16#2");
			m->setHJustify(Field::justify_t::RIGHT);
			m->setVJustify(Field::justify_t::CENTER);
		}
	}
	// shown exactly when the vanilla inventory shows its item slots
	auto& pl = *players[player];
	bool open = pl.isLocalPlayer() && !pl.shootmode && !nohud
		&& (pl.gui_mode == GUI_MODE_INVENTORY || pl.gui_mode == GUI_MODE_SHOP)
		&& !inventoryFrame->isDisabled() && pl.inventory_mode == INVENTORY_MODE_ITEM
		&& !(pl.inventoryUI.bCompactView && pl.hud.compactLayoutMode != Player::HUD_t::COMPACT_LAYOUT_INVENTORY);
	g_shown[player] = open;
	list->setDisabled(!open);
	if ( !open ) { return; }
	invSlots->setDisabled(true);
	if ( backpackSlots ) { backpackSlots->setDisabled(true); }
	if ( Frame* bag = inventoryFrame->findFrame("inventory backpack") ) { bag->setDisabled(true); } // the bag strip under the backpack rows

	// geometry: the slot area of this layout plus the backpack strip below it when not compact
	SDL_Rect r = invSlots->getSize();
	if ( r.w <= 0 || r.h <= 0 ) { r = SDL_Rect{ 0, 202, 210, 242 }; }
	if ( !pl.inventoryUI.bCompactView && backpackSlots )
	{
		const SDL_Rect& b = backpackSlots->getSize();
		r.h = std::max(r.h, b.y + b.h - r.y);
	}
	if ( Frame* base = inventoryFrame->findFrame("inventory base") )
	{
		const SDL_Rect& b = base->getSize(); // the painted slot grid lives in the base image: cover it
		int bottom = b.y + (pl.inventoryUI.bCompactView ? 250 : b.h);
		r.h = std::max(r.h, bottom - r.y);
	}
	r.h = std::max(HEAD_H + ROW_H, std::min(r.h, inventoryFrame->getSize().h - r.y));
	list->setSize(r);
	auto& st = g_list[player];
	st.visibleRows = std::max(1, std::min(MAX_ROWS, (r.h - HEAD_H) / ROW_H));

	auto dr = displayRows(player);
	st.rowCount = (int)dr.size();
	g_isHeader[player].assign(dr.size(), 0);
	for ( size_t i = 0; i < dr.size(); ++i ) { g_isHeader[player][i] = dr[i].header; }
	for ( int i = 0; i < st.rowCount; ++i ) // keep the selection on the same item when rows move
	{
		if ( !dr[i].header && dr[i].row.uid == st.selectedUid && (st.selectedUid || dr[i].row.kind == st.selectedKind) ) { st.selected = i; break; }
	}
	if ( g_testPad && g_padPending > 0 && player == 0 )
	{
		// self-test: the state a D-pad press leaves in the binding, set just before it is read
		auto& b = const_cast<std::unordered_map<std::string, Input::binding_t>&>(Input::inputs[0].getBindings())["InventoryMoveDown"];
		b.binary = true;
		b.consumed = false;
		--g_padPending;
	}
	if ( inputs.hasController(player) || g_testPad ) { listHandleMove(player); } // the gamepad path, whichever runs first
	move(player, 0);
	st.scroll = std::max(0, std::min(st.scroll, std::max(0, st.rowCount - st.visibleRows)));

	// header and mass bar: carried and limit as numbers
	Sint32 carried = carriedMass(player), limit = std::max<Sint32>(1, massLimit(player));
	char head[128];
	snprintf(head, sizeof(head), "%s / %s", massText(carried).c_str(), massText(limit).c_str());
	if ( auto headField = list->findField("apx head") )
	{
		headField->setSize(SDL_Rect{ 6, 2, r.w - 12, 18 });
		headField->setText(head);
	}
	if ( auto barBg = list->findFrame("apx bar bg") )
	{
		barBg->setSize(SDL_Rect{ 6, 22, r.w - 12, 6 });
		if ( auto bar = barBg->findFrame("apx bar") )
		{
			bar->setSize(SDL_Rect{ 0, 0, (int)((long long)(r.w - 12) * std::min(carried, limit) / limit), 6 });
			bar->setColor(carried > limit ? makeColor(220, 60, 40, 255) : makeColor(200, 170, 60, 255));
		}
	}

	// mouse wheel scrolls the list; arrow keys move the selection (keyboard player only)
	auto& in = Input::inputs[player];
	if ( list->capturesMouseInRealtimeCoords() )
	{
		for ( int dir = -1; dir <= 1; dir += 2 )
		{
			const char* b = dir < 0 ? "MenuScrollUp" : "MenuScrollDown";
			if ( in.binaryToggle(b) )
			{
				in.consumeBinaryToggle(b);
				st.scroll = std::max(0, std::min(std::max(0, st.rowCount - st.visibleRows), st.scroll + dir));
			}
		}
	}
	if ( inputs.bPlayerUsingKeyboardControl(player) )
	{
		bool up = keystatus[SDLK_UP], down = keystatus[SDLK_DOWN];
		if ( up && !g_keyHeld[player][0] ) { move(player, -1); }
		if ( down && !g_keyHeld[player][1] ) { move(player, 1); }
		g_keyHeld[player][0] = up;
		g_keyHeld[player][1] = down;
	}

	st.rowsDrawn = 0;
	for ( int i = 0; i < MAX_ROWS; ++i )
	{
		Frame* row = nullptr;
		Field* f = rowField(list, i, &row);
		if ( !row || !f ) { continue; }
		int idx = st.scroll + i;
		if ( i >= st.visibleRows || idx >= st.rowCount ) { row->setDisabled(true); continue; }
		row->setDisabled(false);
		row->setSize(SDL_Rect{ 4, HEAD_H + i * ROW_H, r.w - 8, ROW_H });
		if ( inputs.getVirtualMouse(player)->draw_cursor && row->capturesMouseInRealtimeCoords() )
		{
			if ( !dr[idx].header ) { st.selected = idx; } // hover selects
			if ( in.binaryToggle("MenuLeftClick") ) { in.consumeBinaryToggle("MenuLeftClick"); }
		}
		const auto& d = dr[idx];
		Field* mf = row->findField("mass");
		bool sel = idx == st.selected;
		if ( d.header )
		{
			f->setSize(SDL_Rect{ 2, 0, r.w - 12, ROW_H });
			f->setText(d.name.c_str());
			f->setTextColor(makeColor(230, 190, 90, 255));
			if ( mf ) { mf->setText(""); }
			row->setColor(makeColor(0, 0, 0, 0));
			++st.rowsDrawn;
			continue;
		}
		std::string name = d.name + (d.row.equipped ? " [E]" : "");
		f->setSize(SDL_Rect{ 10, 0, r.w - 74, ROW_H });
		if ( Font* font = Font::get(f->getFont()) ) // fields do not clip: shorten the name to its column
		{
			const int maxW = r.w - 78;
			int w = 0, h = 0;
			while ( name.size() > 2 && font->sizeText(name.c_str(), &w, &h) == 0 && w > maxW ) { name.erase(name.size() - 2); name += "."; }
		}
		f->setText(name.c_str());
		if ( mf )
		{
			mf->setSize(SDL_Rect{ r.w - 72, 0, 60, ROW_H });
			mf->setText(massText(d.row.mass).c_str());
			mf->setTextColor(makeColor(200, 200, 200, 255));
		}
		// condition by colour: excellent white .. broken red; equipped blue
		static const Uint32 cond[] = { makeColor(230, 70, 60, 255), makeColor(240, 140, 60, 255), makeColor(240, 210, 90, 255),
			makeColor(225, 225, 225, 255), makeColor(255, 255, 255, 255) };
		int c = d.row.type < 0 ? 4 : std::max(0, std::min(4, d.row.status));
		f->setTextColor(d.row.equipped ? makeColor(140, 220, 255, 255) : cond[c]);
		row->setColor(sel ? makeColor(120, 90, 30, 255) : (i % 2 ? makeColor(36, 28, 20, 255) : makeColor(28, 22, 16, 255)));
		++st.rowsDrawn;
	}
	if ( st.selected >= 0 && st.selected < st.rowCount && !dr[st.selected].header )
	{
		st.selectedUid = dr[st.selected].row.uid;
		st.selectedKind = dr[st.selected].row.kind;
	}
}
} // namespace apx_inv
