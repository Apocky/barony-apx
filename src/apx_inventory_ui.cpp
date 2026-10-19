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
#include <algorithm>

namespace apx_inv
{
static ListState g_list[MAXPLAYERS];
static bool g_shown[MAXPLAYERS] = {};
static bool g_keyHeld[MAXPLAYERS][2] = {};
constexpr int ROW_H = 20, HEAD_H = 34, MAX_ROWS = 40;

const ListState& listState(int player) { return g_list[std::max(0, std::min(MAXPLAYERS - 1, player))]; }
bool listActive(int player) { return !vanillaGrid() && player >= 0 && player < MAXPLAYERS && g_shown[player]; }

static const char* statusWord(int s)
{
	switch ( s )
	{
		case BROKEN: return "broken";
		case DECREPIT: return "decrepit";
		case WORN: return "worn";
		case SERVICABLE: return "serviceable";
		case EXCELLENT: return "excellent";
		default: return "";
	}
}

// rows for display: by kind, then name; equipped items and gold included
static std::vector<std::pair<Row, std::string>> displayRows(int player)
{
	std::vector<std::pair<Row, std::string>> out;
	for ( auto& r : rows(player) )
	{
		std::string name;
		if ( r.type < 0 ) { name = std::to_string(r.count) + " gold"; }
		else if ( Item* it = uidToItem(r.uid) )
		{
			name = it->getName();
			if ( r.count > 1 ) { name += " x" + std::to_string(r.count); }
		}
		out.push_back({ r, name });
	}
	std::stable_sort(out.begin(), out.end(), [](const std::pair<Row, std::string>& a, const std::pair<Row, std::string>& b) {
		return a.first.kind != b.first.kind ? a.first.kind < b.first.kind : a.second < b.second; });
	return out;
}

static void move(int player, int d)
{
	auto& st = g_list[player];
	if ( d ) { ++st.moves; }
	if ( st.rowCount <= 0 ) { st.selected = 0; return; }
	st.selected = std::max(0, std::min(st.rowCount - 1, st.selected + d));
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

	// geometry: the slot area of this layout plus the backpack strip below it when not compact
	SDL_Rect r = invSlots->getSize();
	if ( r.w <= 0 || r.h <= 0 ) { r = SDL_Rect{ 0, 202, 210, 242 }; }
	if ( !pl.inventoryUI.bCompactView && backpackSlots )
	{
		const SDL_Rect& b = backpackSlots->getSize();
		r.h = std::max(r.h, b.y + b.h - r.y);
	}
	r.h = std::max(HEAD_H + ROW_H, std::min(r.h, inventoryFrame->getSize().h - r.y));
	list->setSize(r);
	auto& st = g_list[player];
	st.visibleRows = std::max(1, std::min(MAX_ROWS, (r.h - HEAD_H) / ROW_H));

	auto dr = displayRows(player);
	st.rowCount = (int)dr.size();
	for ( int i = 0; i < st.rowCount; ++i ) // keep the selection on the same item when rows move
	{
		if ( dr[i].first.uid == st.selectedUid && (st.selectedUid || dr[i].first.kind == st.selectedKind) ) { st.selected = i; break; }
	}
	move(player, 0);
	st.scroll = std::max(0, std::min(st.scroll, std::max(0, st.rowCount - st.visibleRows)));

	// header and mass bar: carried and limit as numbers
	Sint32 carried = carriedMass(player), limit = std::max<Sint32>(1, massLimit(player));
	char head[128];
	snprintf(head, sizeof(head), "Carried %s / %s   %d rows", massText(carried).c_str(), massText(limit).c_str(), st.rowCount);
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
			st.selected = idx; // hover selects
			if ( in.binaryToggle("MenuLeftClick") ) { in.consumeBinaryToggle("MenuLeftClick"); }
		}
		const auto& d = dr[idx];
		char t[192];
		snprintf(t, sizeof(t), "%-.4s %s%s  %s  %s", kindName(d.first.kind), d.second.c_str(), d.first.equipped ? " [E]" : "",
			massText(d.first.mass).c_str(), d.first.type >= 0 ? statusWord(d.first.status) : "");
		f->setSize(SDL_Rect{ 4, 0, r.w - 16, ROW_H });
		f->setText(t);
		bool sel = idx == st.selected;
		row->setColor(sel ? makeColor(120, 90, 30, 255) : (i % 2 ? makeColor(36, 28, 20, 255) : makeColor(28, 22, 16, 255)));
		f->setTextColor(d.first.equipped ? makeColor(140, 220, 255, 255) : makeColor(255, 255, 255, 255));
		++st.rowsDrawn;
	}
	if ( st.selected >= 0 && st.selected < st.rowCount )
	{
		st.selectedUid = dr[st.selected].first.uid;
		st.selectedKind = dr[st.selected].first.kind;
	}
}
} // namespace apx_inv
