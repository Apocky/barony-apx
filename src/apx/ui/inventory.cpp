// apx/ui/inventory.cpp -- D92 P1, the inventory window. See inventory.hpp and spec 08b.
// Written from scratch for D92; nothing from the U1 list (D57/D81/D88) is used.
#include "inventory.hpp"
#include "ui.hpp"
#include "../../main.hpp"
#include "../../game.hpp"
#include "../../stat.hpp"
#include "../../items.hpp"
#include "../../entity.hpp"
#include "../../player.hpp"
#include "../../input.hpp"
#include "../../mod_tools.hpp"
#include "../../imgui/imgui_internal.h" // ClearActiveID (leave the search field)
#include "../../interface/interface.hpp"
#include "../../ui/Frame.hpp"
#include "../../ui/Field.hpp"
#include "../../ui/GameUI.hpp"
#include "../../apx_inventory.hpp"
#include "../../apx_bench_api.hpp"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <set>
#include <unordered_map>
#include <sstream>
#include <string>
#include <vector>

namespace apx
{
namespace ui
{
namespace inv
{
namespace
{
	// ------------------------------------------------------------------ categories, sort, rarity
	enum Cat : int { CAT_ALL, CAT_WEAPONS, CAT_ARMOUR, CAT_COMPONENTS, CAT_MATERIALS, CAT_CONSUMABLES, CAT_BOOKS, CAT_TOOLS, CAT_JUNK, CAT_MAX };
	const char* CAT_NAMES[CAT_MAX] = { "All", "Weapons", "Armour", "Components", "Materials", "Consumables", "Books", "Tools", "Junk" };
	const char* CAT_IDS[CAT_MAX] = { "all", "weapons", "armour", "components", "materials", "consumables", "books", "tools", "junk" };
	enum Sort : int { SORT_NAME, SORT_RARITY, SORT_WEIGHT, SORT_VALUE, SORT_TYPE, SORT_NEWEST, SORT_MAX };
	const char* SORT_NAMES[SORT_MAX] = { "Name", "Rarity", "Weight", "Value", "Type", "Newest" };
	const char* RARITY_NAMES[6] = { "Common", "Uncommon", "Rare", "Epic", "Legendary", "Unique" };
	const char* RARITY_MARK[6] = { "", "U", "R", "E", "L", "*" }; // corner mark: colour is never the only cue

	ImU32 rarityCol(int r, int a = 255)
	{
		// 05b section 4 (ASSUMED there): Common white, Uncommon green, Rare blue, Epic purple, Legendary orange, Unique gold
		static const int c[6][3] = { { 225, 220, 205 }, { 90, 200, 90 }, { 80, 140, 240 }, { 175, 95, 230 }, { 240, 140, 40 }, { 245, 205, 70 } };
		r = std::max(0, std::min(5, r));
		return IM_COL32(c[r][0], c[r][1], c[r][2], a);
	}
	const ImU32 COL_GOLD = IM_COL32(204, 158, 64, 255), COL_GOLD_HI = IM_COL32(245, 200, 90, 255), COL_TEXT = IM_COL32(237, 219, 178, 255);
	const ImU32 COL_DIM = IM_COL32(140, 128, 102, 255), COL_GOOD = IM_COL32(110, 220, 110, 255), COL_BAD = IM_COL32(235, 90, 80, 255);
	const ImU32 COL_CELL = IM_COL32(30, 22, 16, 255), COL_CELL_HOVER = IM_COL32(58, 42, 26, 255);

	// equipment slots, in the order of the Equipment column (and the gamepad focus order)
	struct SlotDef { const char* id; const char* label; ItemEquippableSlot slot; };
	const SlotDef SLOTS[] = {
		{ "head", "Head", EQUIPPABLE_IN_SLOT_HELM }, { "face", "Face", EQUIPPABLE_IN_SLOT_MASK },
		{ "neck", "Neck", EQUIPPABLE_IN_SLOT_AMULET }, { "cloak", "Cloak", EQUIPPABLE_IN_SLOT_CLOAK },
		{ "body", "Body", EQUIPPABLE_IN_SLOT_BREASTPLATE }, { "hands", "Hands", EQUIPPABLE_IN_SLOT_GLOVES },
		{ "ring", "Ring", EQUIPPABLE_IN_SLOT_RING }, { "feet", "Feet", EQUIPPABLE_IN_SLOT_BOOTS },
		{ "mainhand", "Main hand", EQUIPPABLE_IN_SLOT_WEAPON }, { "offhand", "Off hand / quiver", EQUIPPABLE_IN_SLOT_SHIELD } };
	constexpr int NUM_SLOTS = sizeof(SLOTS) / sizeof(SLOTS[0]);

	Item* worn(int p, ItemEquippableSlot s)
	{
		Stat* st = stats[p];
		if ( !st ) { return nullptr; }
		switch ( s )
		{
			case EQUIPPABLE_IN_SLOT_WEAPON: return st->weapon;
			case EQUIPPABLE_IN_SLOT_SHIELD: return st->shield;
			case EQUIPPABLE_IN_SLOT_MASK: return st->mask;
			case EQUIPPABLE_IN_SLOT_HELM: return st->helmet;
			case EQUIPPABLE_IN_SLOT_GLOVES: return st->gloves;
			case EQUIPPABLE_IN_SLOT_BOOTS: return st->shoes;
			case EQUIPPABLE_IN_SLOT_BREASTPLATE: return st->breastplate;
			case EQUIPPABLE_IN_SLOT_CLOAK: return st->cloak;
			case EQUIPPABLE_IN_SLOT_AMULET: return st->amulet;
			case EQUIPPABLE_IN_SLOT_RING: return st->ring;
			default: return nullptr;
		}
	}
	ItemEquippableSlot slotOf(const Item* it) { return it && it->type >= 0 && it->type < NUMITEMS ? items[it->type].item_slot : NO_EQUIP; }

	int rarityOf(const Item* it)
	{
		const auto& pv = apx_bench::provider();
		apx_bench::PartRow row;
		if ( pv.componentOf && pv.componentOf(it, row) == 1 ) { return std::max(0, std::min(5, row.rarity)); }
		int r = 0;
		if ( pv.parts ) { for ( auto& pr : pv.parts(it) ) { r = std::max(r, pr.rarity); } }
		return std::max(0, std::min(5, r));
	}
	bool isComponent(const Item* it) { apx_bench::PartRow row; const auto& pv = apx_bench::provider(); return pv.componentOf && pv.componentOf(it, row) == 1; }

	int catOf(const Item* it)
	{
		if ( isComponent(it) ) { return CAT_COMPONENTS; }
		switch ( itemCategory(it) )
		{
			case WEAPON: case THROWN: case MAGICSTAFF: return CAT_WEAPONS;
			case ARMOR: case AMULET: case RING: return CAT_ARMOUR;
			case GEM: return CAT_MATERIALS;
			case POTION: case FOOD: return CAT_CONSUMABLES;
			case SCROLL: case BOOK: case SPELLBOOK: case TOME_SPELL: return CAT_BOOKS;
			default: return CAT_TOOLS;
		}
	}
	const char* catNameOf(const Item* it)
	{
		switch ( itemCategory(it) )
		{
			case WEAPON: return "Weapon"; case ARMOR: return "Armour"; case AMULET: return "Amulet"; case RING: return "Ring";
			case MAGICSTAFF: return "Staff"; case THROWN: return "Thrown"; case GEM: return "Gem"; case POTION: return "Potion";
			case FOOD: return "Food"; case SCROLL: return "Scroll"; case BOOK: return "Book"; case SPELLBOOK: return "Spellbook";
			case TOME_SPELL: return "Tome"; case TOOL: return "Tool"; default: return "Item";
		}
	}

	std::string lower(std::string s) { for ( auto& c : s ) { c = (char)std::tolower((unsigned char)c); } return s; }
	std::string displayName(const Item* it)
	{
		const auto& pv = apx_bench::provider();
		if ( pv.displayName ) { std::string n = pv.displayName(it); if ( !n.empty() ) { return n; } }
		const char* d = it->description();
		return d ? d : "?";
	}

	// ------------------------------------------------------------------ Barony's own item lines
	// The details column shows exactly what Barony's tooltip logic writes for an item (stat icons,
	// attribute effects such as leather boots of speed's +1 DEX, description), produced by
	// Player::HUD_t::updateFrameTooltip into a hidden, never-drawn frame of ours, then read back.
	struct Lines { std::string header; std::vector<std::string> icons, attributes, description; std::string weight, value; bool ok = false; };

	std::vector<std::string> splitLines(const char* t)
	{
		std::vector<std::string> out;
		if ( !t ) { return out; }
		std::string cur;
		for ( const char* c = t; *c; ++c )
		{
			if ( *c == '\n' ) { if ( !cur.empty() ) { out.push_back(cur); } cur.clear(); }
			else { cur += *c; }
		}
		if ( !cur.empty() ) { out.push_back(cur); }
		for ( auto& s : out ) { while ( !s.empty() && s.back() == ' ' ) { s.pop_back(); } while ( !s.empty() && s[0] == ' ' ) { s.erase(0, 1); } }
		out.erase(std::remove_if(out.begin(), out.end(), [](const std::string& s) { return s.empty(); }), out.end());
		return out;
	}

	struct Harvester { Frame* parent = nullptr; Frame* container = nullptr; Frame* titleOnly = nullptr; Frame* tip = nullptr; Frame* interact = nullptr; Frame* prompt = nullptr; };
	Harvester g_harv[MAXPLAYERS];
	unsigned g_harvestCalls = 0;
	double g_harvestMs = 0.0;

	Lines harvest(int p, Item* it)
	{
		Lines L;
		if ( !it || !gameUIFrame[p] || !gui ) { return L; }
		Harvester& H = g_harv[p];
		if ( !H.parent )
		{
			H.parent = gameUIFrame[p]->addFrame("apx d92 item lines");
			H.parent->setSize(SDL_Rect{ 0, 0, 1280, 720 });
			H.parent->setHollow(true);
			createInventoryTooltipFrame(0, H.parent, H.container, H.titleOnly, H.tip, H.interact, H.prompt); // named "... 0" inside our parent
		}
		H.parent->setDisabled(true); // never drawn, never takes input
		const Uint64 t0 = SDL_GetPerformanceCounter();
		auto& disp = players[p]->inventoryUI.itemTooltipDisplay;
		disp.reset(); // force a full rebuild for this item
		// in shootmode Barony builds only the title-only tooltip: the full lines need the menu state
		const bool wasShoot = players[p]->shootmode; players[p]->shootmode = false;
		players[p]->hud.updateFrameTooltip(it, 0, 0, Player::PANEL_JUSTIFY_LEFT, H.parent);
		players[p]->shootmode = wasShoot;
		disp.reset(); // and Barony's own tooltip rebuilds next time it shows (chest/shop windows)
		++g_harvestCalls;
		g_harvestMs = 1000.0 * (double)(SDL_GetPerformanceCounter() - t0) / (double)SDL_GetPerformanceFrequency();
		printlog("[apx-ui] item lines for type %d in %.2f ms", (int)it->type, g_harvestMs);
		if ( !H.tip ) { return L; }
		auto txt = [&](Frame* f, const char* name) -> const char* { if ( !f ) { return nullptr; } Field* fl = f->findField(name); return fl ? fl->getText() : nullptr; };
		if ( const char* h = txt(H.tip, "inventory mouse tooltip header") ) { L.header = h; }
		Frame* attr = H.tip->findFrame("inventory mouse tooltip attributes frame");
		Frame* desc = H.tip->findFrame("inventory mouse tooltip description frame");
		Frame* vals = H.tip->findFrame("inventory mouse tooltip value frame");
		const char* iconFields[] = { "inventory mouse tooltip primary value", "inventory mouse tooltip secondary value", "inventory mouse tooltip third value" };
		for ( const char* n : iconFields )
		{
			Field* f = attr ? attr->findField(n) : nullptr;
			if ( f && !f->isDisabled() ) { for ( auto& s : splitLines(f->getText()) ) { L.icons.push_back(s); } }
		}
		if ( attr ) { if ( Field* f = attr->findField("inventory mouse tooltip attributes text") ) { if ( !f->isDisabled() ) { L.attributes = splitLines(f->getText()); } } }
		if ( desc ) { if ( Field* f = desc->findField("inventory mouse tooltip description") ) { if ( !f->isDisabled() ) { L.description = splitLines(f->getText()); } } }
		if ( const char* w = txt(vals, "inventory mouse tooltip weight value") ) { L.weight = w; }
		if ( const char* v = txt(vals, "inventory mouse tooltip gold value") ) { L.value = v; }
		L.ok = !L.header.empty() && L.header != "Nothing"; // "Nothing" = Barony's placeholder, the tooltip was not built
		return L;
	}

	// Barony renders each item's tooltip text on first sight (6-22 ms cold, 0.1 ms warm). The lines are
	// cached by item state + the stats that shape them, and warmed one item per frame while the window is
	// CLOSED, so opening it and selecting items costs only cache hits.
	std::unordered_map<unsigned long long, Lines> g_lineCache;
	unsigned g_cacheHits = 0, g_cacheMisses = 0, g_warmed = 0, g_warmFail = 0;
	unsigned long long mixH(unsigned long long h, unsigned long long v) { h ^= v + 0x9e3779b97f4a7c15ULL + (h << 6) + (h >> 2); return h; }
	unsigned long long lineKey(int p, const Item* it)
	{
		unsigned long long h = 0xcbf29ce484222325ULL;
		h = mixH(h, it->uid); h = mixH(h, (unsigned)it->type); h = mixH(h, (unsigned)it->count); h = mixH(h, (unsigned)it->status);
		h = mixH(h, (unsigned)(it->beatitude + 1000)); h = mixH(h, it->identified ? 1 : 2); h = mixH(h, it->appearance);
		if ( Stat* st = stats[p] )
		{
			h = mixH(h, (unsigned)st->STR); h = mixH(h, (unsigned)st->DEX); h = mixH(h, (unsigned)st->CON); h = mixH(h, (unsigned)st->INT);
			h = mixH(h, (unsigned)st->PER); h = mixH(h, (unsigned)st->CHR); h = mixH(h, (unsigned)st->LVL);
			for ( int i = 0; i < NUMPROFICIENCIES; ++i ) { h = mixH(h, (unsigned)st->getProficiency(i)); }
		}
		return h;
	}
	Lines harvestCached(int p, Item* it)
	{
		if ( !it ) { return Lines(); }
		const unsigned long long k = lineKey(p, it);
		auto f = g_lineCache.find(k);
		if ( f != g_lineCache.end() ) { ++g_cacheHits; return f->second; }
		++g_cacheMisses;
		Lines L = harvest(p, it);
		if ( g_lineCache.size() > 600 ) { g_lineCache.clear(); }
		if ( L.ok ) { g_lineCache[k] = L; }
		return L;
	}

	// ------------------------------------------------------------------ the per-player view model
	struct Cell
	{
		Uint32 uid = 0; int type = 0; std::string name, lname; int count = 1, cat = 0, rarity = 0, status = 0, beat = 0;
		bool equipped = false, identified = true, curseKnown = false, component = false; Sint32 weight = 0; int value = 0; void* tex = nullptr; int iconW = 0, iconH = 0;
	};
	struct Act { std::string id, label; int prompt = -1; bool enabled = true; };
	enum Zone : int { Z_BAG, Z_EQUIP, Z_ACTIONS, Z_HOTBAR, Z_QUICK };
	struct State
	{
		bool open = false;
		unsigned long long sig = ~0ULL, viewKey = ~0ULL;
		std::vector<Cell> cells; std::vector<int> shown;
		char search[64] = {}; int cat = CAT_ALL, sort = SORT_NAME; bool sortDesc = false;
		Uint32 sel = 0; int zone = Z_BAG, slotIdx = 0, actionIdx = 0, hotbarIdx = 0, quickIdx = 0, cols = 1;
		Uint32 detUid = 0; unsigned long long detSig = 0; Lines det; Lines detWorn; Uint32 detWornUid = 0;
		unsigned long long bonusSig = ~0ULL; std::vector<std::string> bonuses; int bonusNext = NUM_SLOTS;
		int lastHP = -1; Uint32 flashUntil = 0; int flashLoss = 0;
		std::set<Uint32> locked, junk;
		std::string lastAction, lastResult; int actions = 0;
		bool focusSearch = false, openQuick = false, confirmDrop = false; Uint32 quickUid = 0, confirmUid = 0;
		bool narrowEquipTab = false;
		bool scrollToSel = false;
		unsigned long long actsKey = ~0ULL; std::vector<Act> acts;        // the selected item's actions (cached)
		unsigned long long statKey = ~0ULL; int atk = 0, ac = 0, str = 0, dex = 0, con = 0, intl = 0, per = 0, chr = 0; Sint32 carried = 0, limit = 1;
	};
	State g_st[MAXPLAYERS];
	bool g_popupOpen = false, g_popupWasOpen = false; // one of our popups is open (Escape belongs to it)
	unsigned g_rebuilds = 0;

	unsigned long long mix(unsigned long long h, unsigned long long v) { h ^= v + 0x9e3779b97f4a7c15ULL + (h << 6) + (h >> 2); return h; }

	unsigned long long invSig(int p)
	{
		unsigned long long h = 1469598103934665603ULL;
		Stat* st = stats[p];
		if ( !st ) { return 0; }
		for ( node_t* n = st->inventory.first; n; n = n->next )
		{
			const Item* it = static_cast<Item*>(n->element);
			if ( !it ) { continue; }
			h = mix(h, it->uid); h = mix(h, (unsigned)it->type); h = mix(h, (unsigned)it->count); h = mix(h, (unsigned)it->status);
			h = mix(h, (unsigned)(it->beatitude + 1000)); h = mix(h, it->identified ? 1 : 2); h = mix(h, it->appearance);
		}
		for ( int s = 0; s < NUM_SLOTS; ++s ) { const Item* w = worn(p, SLOTS[s].slot); h = mix(h, w ? w->uid : 0); }
		return h;
	}

	void rebuild(int p, State& S)
	{
		const Uint64 t0 = SDL_GetPerformanceCounter();
		struct Done { Uint64 t0; ~Done() { printlog("[apx-ui] view model rebuilt in %.2f ms", 1000.0 * (double)(SDL_GetPerformanceCounter() - t0) / (double)SDL_GetPerformanceFrequency()); } } done{ t0 };
		++g_rebuilds;
		S.cells.clear();
		for ( node_t* n = stats[p]->inventory.first; n; n = n->next )
		{
			Item* it = static_cast<Item*>(n->element);
			if ( !it || it->type < 0 || it->type >= NUMITEMS || itemCategory(it) == SPELL_CAT ) { continue; }
			Cell c;
			c.uid = it->uid; c.type = it->type; c.name = displayName(it); c.lname = lower(c.name);
			c.count = it->count; c.cat = catOf(it); c.rarity = rarityOf(it); c.status = (int)it->status; c.beat = it->beatitude;
			c.equipped = itemIsEquipped(it, p); c.identified = it->identified; c.curseKnown = it->identified && it->beatitude < 0;
			c.component = c.cat == CAT_COMPONENTS; c.weight = it->getWeight(); c.value = it->sellValue(p);
			c.tex = itemIcon(p, *it, &c.iconW, &c.iconH);
			S.cells.push_back(std::move(c));
		}
	}

	void refilter(State& S)
	{
		S.shown.clear();
		const std::string q = lower(S.search);
		for ( int i = 0; i < (int)S.cells.size(); ++i )
		{
			const Cell& c = S.cells[i];
			const bool junk = S.junk.count(c.uid) > 0;
			if ( S.cat == CAT_JUNK ) { if ( !junk ) { continue; } }
			else if ( S.cat != CAT_ALL && c.cat != S.cat ) { continue; }
			if ( !q.empty() && c.lname.find(q) == std::string::npos ) { continue; }
			S.shown.push_back(i);
		}
		auto key = [&](const Cell& a, const Cell& b) -> int {
			switch ( S.sort )
			{
				case SORT_RARITY: return a.rarity != b.rarity ? (a.rarity > b.rarity ? -1 : 1) : 0;
				case SORT_WEIGHT: return a.weight != b.weight ? (a.weight > b.weight ? -1 : 1) : 0;
				case SORT_VALUE: return a.value != b.value ? (a.value > b.value ? -1 : 1) : 0;
				case SORT_TYPE: return a.cat != b.cat ? (a.cat < b.cat ? -1 : 1) : (a.type != b.type ? (a.type < b.type ? -1 : 1) : 0);
				case SORT_NEWEST: return a.uid != b.uid ? (a.uid > b.uid ? -1 : 1) : 0;
				default: return 0;
			}
		};
		std::stable_sort(S.shown.begin(), S.shown.end(), [&](int ia, int ib) {
			const Cell& a = S.cells[ia]; const Cell& b = S.cells[ib];
			int k = key(a, b);
			if ( k == 0 ) { k = a.lname < b.lname ? -1 : (a.lname > b.lname ? 1 : (a.uid < b.uid ? -1 : (a.uid > b.uid ? 1 : 0))); }
			return S.sortDesc ? k > 0 : k < 0;
		});
	}

	const Cell* cellOf(const State& S, Uint32 uid) { for ( auto& c : S.cells ) { if ( c.uid == uid ) { return &c; } } return nullptr; }
	int shownIndex(const State& S, Uint32 uid) { for ( int i = 0; i < (int)S.shown.size(); ++i ) { if ( S.cells[S.shown[i]].uid == uid ) { return i; } } return -1; }

	Item* ownItem(int p, Uint32 uid)
	{
		if ( !uid || !stats[p] ) { return nullptr; }
		for ( node_t* n = stats[p]->inventory.first; n; n = n->next ) { Item* it = static_cast<Item*>(n->element); if ( it && it->uid == uid ) { return it; } }
		return nullptr;
	}

	bool plainInventory(int p)
	{
		Player& pl = *players[p];
		if ( !pl.isLocalPlayer() || pl.shootmode || pl.gui_mode != GUI_MODE_INVENTORY || pl.inventory_mode != INVENTORY_MODE_ITEM ) { return false; }
		if ( !stats[p] || intro || gamePaused ) { return false; }
		// modes of later phases keep Barony's windows (P2 chest, P3 shop, P5 generic item GUIs); never both open
		if ( pl.inventoryUI.chestGUI.bOpen || openedChest[p] || pl.shopGUI.bOpen || pl.inventoryUI.spellPanel.bOpen ) { return false; }
		if ( GenericGUI[p].isGUIOpen() || GenericGUI[p].tinkerGUI.bOpen || GenericGUI[p].alchemyGUI.bOpen || GenericGUI[p].featherGUI.bOpen
			|| GenericGUI[p].itemfxGUI.bOpen || GenericGUI[p].mailboxGUI.bOpen || GenericGUI[p].assistShrineGUI.bOpen ) { return false; }
		if ( pl.bookGUI.bBookOpen || pl.signGUI.bSignOpen ) { return false; }
		return true;
	}

	// ------------------------------------------------------------------ actions (game functions only)
	enum OurAct : int { ACT_LOCK = 1000, ACT_JUNK, ACT_HOTBAR_CLEAR };

	const char* promptId(int pr)
	{
		switch ( pr )
		{
			case PROMPT_EQUIP: return "equip"; case PROMPT_UNEQUIP: return "unequip"; case PROMPT_SPELL_EQUIP: return "spellequip";
			case PROMPT_APPRAISE: return "appraise"; case PROMPT_INTERACT: return "use"; case PROMPT_INTERACT_SPELLBOOK_HOTBAR: return "usebook";
			case PROMPT_EAT: return "eat"; case PROMPT_CONSUME: return "consume"; case PROMPT_CONSUME_ALTERNATE: return "consume2";
			case PROMPT_INSPECT: return "inspect"; case PROMPT_INSPECT_ALTERNATE: return "inspect2"; case PROMPT_DROP: return "drop";
			case PROMPT_TINKER: return "tinker"; case PROMPT_COOK: return "cook"; case PROMPT_SCEPTER_CHARGE: return "charge";
			case PROMPT_APX_BREAK: return "break"; case ACT_LOCK: return "lock"; case ACT_JUNK: return "junk";
			default: return "other";
		}
	}

	std::vector<Act> actionsFor(int p, State& S, Item* it)
	{
		std::vector<Act> out;
		if ( !it ) { return out; }
		const bool locked = S.locked.count(it->uid) > 0;
		for ( auto pr : getContextMenuOptionsForItem(p, it) )
		{
			if ( pr == PROMPT_DROPDOWN || pr == PROMPT_CLEAR_HOTBAR_SLOT || pr == PROMPT_SELL || pr == PROMPT_BUY || pr == PROMPT_STORE_CHEST
				|| pr == PROMPT_RETRIEVE_CHEST || pr == PROMPT_RETRIEVE_CHEST_ALL || pr == PROMPT_STORE_CHEST_ALL || pr == PROMPT_GRAB
				|| pr == PROMPT_UNEQUIP_FOR_DROP || pr == PROMPT_SPELL_QUICKCAST || pr == PROMPT_SPELL_CHANGE_FOCUS ) { continue; } // modes of P2/P3
			Act a; a.prompt = pr; a.id = promptId(pr);
			const char* l = getContextMenuLangEntry(p, pr, *it);
			a.label = l ? l : a.id;
			if ( locked && (pr == PROMPT_DROP || pr == PROMPT_APX_BREAK) ) { a.enabled = false; }
			out.push_back(a);
		}
		bool hasDrop = false; for ( auto& a : out ) { hasDrop |= a.prompt == PROMPT_DROP; }
		if ( !hasDrop ) { Act a; a.prompt = PROMPT_DROP; a.id = "drop"; a.label = getContextMenuLangEntry(p, PROMPT_DROP, *it); a.enabled = !locked; out.push_back(a); }
		{ Act a; a.prompt = ACT_LOCK; a.id = "lock"; a.label = locked ? "Unlock" : "Lock"; out.push_back(a); }
		{ Act a; a.prompt = ACT_JUNK; a.id = "junk"; a.label = S.junk.count(it->uid) ? "Not junk" : "Mark junk"; out.push_back(a); }
		return out;
	}

	// the selected item's actions, rebuilt when the item, the inventory or the lock/junk marks change, or every 25 ticks
	const std::vector<Act>& selActs(int p, State& S)
	{
		const unsigned long long k = mix(mix(mix(mix(S.sel, S.sig), S.locked.size() * 131 + S.junk.size()), (unsigned long long)(ticks / 25)), (unsigned long long)S.actions);
		if ( k != S.actsKey ) { S.actsKey = k; S.acts = actionsFor(p, S, ownItem(p, S.sel)); }
		return S.acts;
	}

	std::string itemState(int p, Uint32 uid)
	{
		Item* it = ownItem(p, uid);
		if ( !it ) { return "gone"; }
		char b[96];
		snprintf(b, sizeof b, "%s count=%d", itemIsEquipped(it, p) ? "equipped" : "carried", (int)it->count);
		return b;
	}

	void logEvent(int p, const char* ev, const std::string& more)
	{
		printlog("[apx-ui] %s p=%d %s", ev, p, more.c_str());
	}

	void doAction(int p, State& S, Uint32 uid, int prompt)
	{
		Item* it = ownItem(p, uid);
		if ( !it ) { return; }
		Player& pl = *players[p];
		const int type = it->type;
		const std::string before = itemState(p, uid);
		if ( prompt == ACT_LOCK ) { if ( !S.locked.erase(uid) ) { S.locked.insert(uid); } }
		else if ( prompt == ACT_JUNK ) { if ( !S.junk.erase(uid) ) { S.junk.insert(uid); } S.viewKey = ~0ULL; }
		else if ( prompt == PROMPT_DROP )
		{
			if ( S.locked.count(uid) ) { S.lastResult = "locked"; return; }
			if ( pl.paperDoll.isItemOnDoll(*it) || itemIsEquipped(it, p) )
			{
				// Barony's own drop path for worn items (the vanilla context menu does the same)
				pl.inventoryUI.activateItemContextMenuOption(it, PROMPT_UNEQUIP_FOR_DROP);
				pl.paperDoll.updateSlots();
				it = ownItem(p, uid);
				if ( it && !itemIsEquipped(it, p) )
				{
					bool droppedAll = false;
					while ( it && it->count > 1 ) { droppedAll = dropItem(it, p); if ( droppedAll ) { it = nullptr; } }
					if ( !droppedAll && it ) { dropItem(it, p); }
				}
			}
			else { pl.inventoryUI.activateItemContextMenuOption(it, PROMPT_DROP); }
		}
		else { pl.inventoryUI.activateItemContextMenuOption(it, (ItemContextMenuPrompts)prompt); }
		const std::string after = itemState(p, uid);
		++S.actions;
		S.lastAction = promptId(prompt);
		S.lastResult = after;
		char b[200];
		snprintf(b, sizeof b, "action=%s type=%d uid=%u before=\"%s\" after=\"%s\"", promptId(prompt), type, (unsigned)uid, before.c_str(), after.c_str());
		logEvent(p, "ui_action", b);
		S.sig = ~0ULL; // the view model rebuilds now
		if ( after == "gone" ) { S.sel = 0; }
	}

	void assignHotbar(int p, State& S, int slot, Uint32 uid)
	{
		if ( slot < 0 || slot >= (int)NUM_HOTBAR_SLOTS ) { return; }
		auto& hb = players[p]->hotbar.slots();
		for ( auto& h : hb ) { if ( uid && h.item == uid ) { h.item = 0; h.resetLastItem(); } } // one slot per item, as Barony's drag does
		hb[slot].item = uid; // the hotbar binding array, as Barony's inventory drag and number keys set it
		hb[slot].resetLastItem();
		if ( Item* it = ownItem(p, uid) ) { hb[slot].storeLastItem(it); }
		++S.actions;
		S.lastAction = uid ? "hotbar" : "hotbar_clear";
		char b[64]; snprintf(b, sizeof b, "slot%d=%u", slot + 1, (unsigned)hb[slot].item);
		S.lastResult = b;
		logEvent(p, "ui_action", std::string("action=") + S.lastAction + " " + b);
	}

	void closeWindow(int p)
	{
		players[p]->closeAllGUIs(CLOSEGUI_ENABLE_SHOOTMODE, CLOSEGUI_CLOSE_ALL);
	}

	// ------------------------------------------------------------------ drawing helpers
	ImVec2 S2(float x, float y) { const float s = (float)scale(); return ImVec2(x * s, y * s); }
	float SZ(float v) { return v * (float)scale(); }

	bool idButton(const char* id, const char* label, ImVec2 size, bool enabled = true)
	{
		ImGui::PushID(id);
		if ( !enabled ) { ImGui::BeginDisabled(); }
		const bool r = ImGui::Button(label, size);
		if ( !enabled ) { ImGui::EndDisabled(); }
		recordLastItem(id);
		ImGui::PopID();
		return r && enabled;
	}

	void drawIcon(ImDrawList* dl, void* tex, ImVec2 a, ImVec2 b)
	{
		if ( tex ) { dl->AddImage((ImTextureID)tex, a, b); }
	}

	void focusFrame(ImDrawList* dl, ImVec2 a, ImVec2 b)
	{
		dl->AddRect(ImVec2(a.x - 2, a.y - 2), ImVec2(b.x + 2, b.y + 2), IM_COL32(255, 240, 160, 255), 0.f, 0, SZ(2));
	}

	// draw-call batching: grids draw backgrounds (channel 0), item art (1) and marks/text (2) into split
	// channels, so the font-atlas geometry of every cell merges into two draw commands per grid
	bool g_split = false;
	void ch(ImDrawList* dl, int c) { if ( g_split ) { dl->ChannelsSetCurrent(c); } }
	void splitBegin(ImDrawList* dl) { dl->ChannelsSplit(3); g_split = true; dl->ChannelsSetCurrent(2); }
	void splitEnd(ImDrawList* dl) { if ( g_split ) { dl->ChannelsMerge(); g_split = false; } }

	void cellVisual(ImDrawList* dl, const Cell& c, ImVec2 a, ImVec2 b, bool selected, bool hovered, bool locked, bool junk)
	{
		ch(dl, 0);
		dl->AddRectFilled(a, b, hovered ? COL_CELL_HOVER : COL_CELL);
		const float pad = SZ(3);
		ch(dl, 1);
		drawIcon(dl, c.tex, ImVec2(a.x + pad, a.y + pad), ImVec2(b.x - pad, b.y - pad));
		ch(dl, 2);
		dl->AddRect(a, b, rarityCol(c.rarity, c.rarity ? 255 : 120), 0.f, 0, SZ(1));
		ImFont* f = fontSmall(); const float fs = f->FontSize;
		if ( c.rarity > 0 ) { dl->AddText(f, fs, ImVec2(a.x + SZ(2), a.y + SZ(1)), rarityCol(c.rarity), RARITY_MARK[c.rarity]); }
		if ( !c.identified ) { dl->AddText(f, fs, ImVec2(b.x - fs - SZ(1), a.y + SZ(1)), IM_COL32(255, 255, 255, 255), "?"); }
		else if ( c.curseKnown ) { dl->AddText(f, fs, ImVec2(b.x - fs - SZ(1), a.y + SZ(1)), COL_BAD, "!"); }
		if ( locked ) { dl->AddText(f, fs, ImVec2(b.x - fs - SZ(1), a.y + fs + SZ(2)), COL_GOLD_HI, "L"); }
		if ( junk ) { dl->AddText(f, fs, ImVec2(a.x + SZ(2), a.y + fs + SZ(2)), COL_DIM, "j"); }
		// condition pip (Barony Status: BROKEN 0 .. EXCELLENT 4)
		static const ImU32 pip[5] = { IM_COL32(200, 50, 40, 255), IM_COL32(220, 120, 40, 255), IM_COL32(220, 200, 60, 255), IM_COL32(140, 200, 80, 255), IM_COL32(80, 230, 110, 255) };
		const ImVec2 pa(a.x + SZ(2), b.y - SZ(5));
		dl->AddRectFilled(pa, ImVec2(pa.x + SZ(3), pa.y + SZ(3)), pip[std::max(0, std::min(4, c.status))]);
		if ( c.equipped ) { dl->AddText(f, fs, ImVec2(a.x + SZ(7), b.y - fs - SZ(1)), COL_GOLD_HI, "E"); }
		if ( c.count > 1 ) { char n[16]; snprintf(n, sizeof n, "%d", c.count); const ImVec2 ts = f->CalcTextSizeA(fs, 1e9f, 0.f, n); dl->AddText(f, fs, ImVec2(b.x - ts.x - SZ(2), b.y - fs - SZ(1)), COL_TEXT, n); }
		if ( selected ) { dl->AddRect(ImVec2(a.x - 1, a.y - 1), ImVec2(b.x + 1, b.y + 1), COL_GOLD_HI, 0.f, 0, SZ(2)); }
	}

	// drag-and-drop payload: an item uid, plus where it came from
	struct Drag { Uint32 uid; int from; int fromSlot; };
	enum From : int { FROM_BAG, FROM_EQUIP, FROM_HOTBAR };

	void dragSource(const Cell* c, Uint32 uid, int from, int fromSlot)
	{
		if ( ImGui::BeginDragDropSource(ImGuiDragDropFlags_SourceAllowNullID) )
		{
			Drag d{ uid, from, fromSlot };
			ImGui::SetDragDropPayload("APXITEM", &d, sizeof d);
			if ( c && c->tex ) { ImGui::Image((ImTextureID)c->tex, S2(32, 32)); ImGui::SameLine(); }
			ImGui::TextUnformatted(c ? c->name.c_str() : "item");
			ImGui::EndDragDropSource();
		}
	}
	const Drag* dropTarget()
	{
		const Drag* out = nullptr;
		if ( ImGui::BeginDragDropTarget() )
		{
			if ( const ImGuiPayload* pl = ImGui::AcceptDragDropPayload("APXITEM") ) { out = static_cast<const Drag*>(pl->Data); }
			ImGui::EndDragDropTarget();
		}
		return out;
	}

	// ------------------------------------------------------------------ columns
	void refreshStats(int p, State& S)
	{
		Stat* st = stats[p];
		Entity* me = players[p]->entity;
		const unsigned long long sk = mix(S.sig, (unsigned long long)(ticks / 10)); // Barony's numbers, refreshed 5 times a second
		if ( sk != S.statKey )
		{
			S.statKey = sk;
			AttackHoverText_t atk;
			S.atk = me ? displayAttackPower(p, atk) : 0; S.ac = me ? AC(st) : 0;
			S.str = statGetSTR(st, me); S.dex = statGetDEX(st, me); S.con = statGetCON(st, me);
			S.intl = statGetINT(st, me); S.per = statGetPER(st, me); S.chr = statGetCHR(st, me);
			S.carried = apx_inv::carriedMass(p); S.limit = std::max<Sint32>(1, apx_inv::massLimit(p));
		}
	}

	void drawEquipment(int p, State& S, bool focusedWin)
	{
		ImDrawList* dl = ImGui::GetWindowDrawList();
		ImGui::PushFont(fontHead()); ImGui::TextUnformatted("Equipment"); ImGui::PopFont();
		const float row = SZ(40); // two wrapped text lines per slot
		const float lineH = ImGui::GetTextLineHeightWithSpacing();
		const float statsH = lineH * 5 + ImGui::GetStyle().ItemSpacing.y * 2 + SZ(6);
		ImGui::BeginChild("inv/equip/slots", ImVec2(0, -statsH), false);
		dl = ImGui::GetWindowDrawList(); // the child's own list (clipped to the child)
		splitBegin(dl);
		for ( int s = 0; s < NUM_SLOTS; ++s )
		{
			Item* w = worn(p, SLOTS[s].slot);
			const Cell* c = w ? cellOf(S, w->uid) : nullptr;
			char id[48]; snprintf(id, sizeof id, "inv/equip/%s", SLOTS[s].id);
			ImGui::PushID(id);
			const ImVec2 a = ImGui::GetCursorScreenPos();
			const float wdt = ImGui::GetContentRegionAvail().x;
			ImGui::InvisibleButton("slot", ImVec2(wdt, row));
			recordLastItem(id);
			const bool hov = ImGui::IsItemHovered();
			if ( ImGui::IsItemClicked(ImGuiMouseButton_Left) && w ) { S.sel = w->uid; S.zone = Z_EQUIP; S.slotIdx = s; }
			if ( hov && w && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left) ) { doAction(p, S, w->uid, PROMPT_UNEQUIP); }
			if ( ImGui::IsItemClicked(ImGuiMouseButton_Right) && w ) { S.sel = w->uid; S.quickUid = w->uid; S.openQuick = true; }
			if ( w ) { dragSource(c, w->uid, FROM_EQUIP, s); }
			if ( const Drag* d = dropTarget() )
			{
				Item* di = ownItem(p, d->uid);
				if ( di && slotOf(di) == SLOTS[s].slot && !itemIsEquipped(di, p) ) { S.sel = d->uid; doAction(p, S, d->uid, PROMPT_EQUIP); }
				else if ( di ) { S.lastAction = "equip"; S.lastResult = "does not fit that slot"; }
			}
			const ImVec2 b(a.x + wdt, a.y + row);
			ch(dl, 0);
			dl->AddRectFilled(a, b, hov ? COL_CELL_HOVER : COL_CELL);
			const ImVec2 ia(a.x + SZ(2), a.y + SZ(2)), ib(a.x + SZ(34) - SZ(2), a.y + SZ(34) - SZ(2));
			dl->AddRect(ia, ib, c ? rarityCol(c->rarity) : IM_COL32(90, 70, 40, 255));
			ch(dl, 1);
			if ( c ) { drawIcon(dl, c->tex, ImVec2(ia.x + 1, ia.y + 1), ImVec2(ib.x - 1, ib.y - 1)); }
			ch(dl, 2);
			ImFont* f = fontBody();
			{	// "Slot: full name", wrapped at word boundaries (never cut mid-word), two lines
				const float tx = a.x + SZ(38), ww = std::max(SZ(40), b.x - tx - SZ(10));
				const std::string txt = c ? std::string(SLOTS[s].label) + ": " + c->name : std::string(SLOTS[s].label);
				dl->AddText(f, f->FontSize, ImVec2(tx, a.y + SZ(2)), c ? rarityCol(c->rarity) : COL_DIM, txt.c_str(), nullptr, ww);
			}
			if ( w && S.sel == w->uid ) { dl->AddRect(a, b, COL_GOLD_HI, 0.f, 0, SZ(2)); }
			if ( focusedWin && S.zone == Z_EQUIP && S.slotIdx == s ) { focusFrame(dl, a, b); }
			ImGui::PopID();
		}
		splitEnd(dl);
		Stat* st = stats[p];
		Entity* me = players[p]->entity;
		// bonuses: every effect line Barony writes for each worn item
		unsigned long long bs = 1;
		for ( int s = 0; s < NUM_SLOTS; ++s ) { const Item* w = worn(p, SLOTS[s].slot); bs = mix(bs, w ? (w->uid * 31ULL + w->status * 7ULL + (unsigned)(w->beatitude + 100)) : 0); }
		bs = mix(bs, (unsigned)st->STR); bs = mix(bs, (unsigned)st->DEX);
		if ( bs != S.bonusSig ) { S.bonusSig = bs; S.bonuses.clear(); S.bonusNext = 0; }
		// at most one worn item's lines per frame (a first view of an item costs Barony's text rendering)
		while ( S.bonusNext < NUM_SLOTS )
		{
			Item* w = worn(p, SLOTS[S.bonusNext++].slot);
			if ( !w ) { continue; }
			Lines L = harvestCached(p, w);
			for ( auto& l : L.icons ) { S.bonuses.push_back(l); }
			break;
		}
		if ( ImGui::CollapsingHeader("Bonuses from worn items##inv/equip/bonuses", ImGuiTreeNodeFlags_DefaultOpen) )
		{
			recordLastItem("inv/equip/bonuses");
			if ( S.bonuses.empty() ) { ImGui::TextDisabled("none"); }
			for ( auto& l : S.bonuses ) { ImGui::TextWrapped("%s", l.c_str()); }
		}
		ImGui::EndChild();
		// stats (Barony's own numbers), pinned below the scrolling slots so they are never clipped
		ImGui::Separator();
		ImGui::Text("ATK %d   AC %d", S.atk, S.ac);
		ImGui::Text("HP %d/%d   MP %d/%d", st->HP, st->MAXHP, st->MP, st->MAXMP);
		ImGui::Text("STR %d DEX %d CON %d", S.str, S.dex, S.con);
		ImGui::Text("INT %d PER %d CHR %d", S.intl, S.per, S.chr);
		ImGui::Text("Load %s / %s", apx_inv::massText(S.carried).c_str(), apx_inv::massText(S.limit).c_str());
	}

	void drawBag(int p, State& S, bool focusedWin, bool keyboardPlayer)
	{
		// toolbar: search, categories, sort
		ImGui::PushFont(fontHead()); ImGui::TextUnformatted("Bag"); ImGui::PopFont();
		ImGui::SameLine();
		ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x);
		if ( S.focusSearch ) { ImGui::SetKeyboardFocusHere(); S.focusSearch = false; }
		ImGui::PushID("inv/bag/search");
		if ( ImGui::InputTextWithHint("##search", "search  ( / )", S.search, sizeof S.search) ) { S.viewKey = ~0ULL; }
		recordLastItem("inv/bag/search");
		ImGui::PopID();
		for ( int c = 0; c < CAT_MAX; ++c )
		{
			char id[40]; snprintf(id, sizeof id, "inv/bag/cat/%s", CAT_IDS[c]);
			const bool on = S.cat == c;
			if ( on ) { ImGui::PushStyleColor(ImGuiCol_Button, ImGui::GetStyleColorVec4(ImGuiCol_ButtonActive)); }
			if ( idButton(id, CAT_NAMES[c], ImVec2(0, 0)) ) { S.cat = c; S.viewKey = ~0ULL; }
			if ( on ) { ImGui::PopStyleColor(); }
			if ( c + 1 < CAT_MAX )
			{
				const float next = ImGui::CalcTextSize(CAT_NAMES[c + 1]).x + ImGui::GetStyle().FramePadding.x * 2;
				if ( ImGui::GetItemRectMax().x + ImGui::GetStyle().ItemSpacing.x + next < ImGui::GetWindowPos().x + ImGui::GetWindowContentRegionMax().x ) { ImGui::SameLine(); }
			}
		}
		ImGui::TextUnformatted("Sort:"); ImGui::SameLine();
		for ( int s = 0; s < SORT_MAX; ++s )
		{
			char id[40]; snprintf(id, sizeof id, "inv/bag/sort/%s", lower(SORT_NAMES[s]).c_str());
			const bool on = S.sort == s;
			if ( on ) { ImGui::PushStyleColor(ImGuiCol_Button, ImGui::GetStyleColorVec4(ImGuiCol_ButtonActive)); }
			if ( idButton(id, SORT_NAMES[s], ImVec2(0, 0)) ) { if ( on ) { S.sortDesc = !S.sortDesc; } else { S.sort = s; S.sortDesc = false; } S.viewKey = ~0ULL; }
			if ( on ) { ImGui::PopStyleColor(); }
			if ( s + 1 < SORT_MAX ) { ImGui::SameLine(); }
		}
		// the grid
		const float footer = ImGui::GetFrameHeightWithSpacing() * 2 + SZ(4);
		ImGui::BeginChild("inv/bag/grid", ImVec2(0, -footer), true);
		{ const ImVec2 ga = ImGui::GetWindowPos(); recordRect("inv/bag/grid", ga, ImVec2(ga.x + ImGui::GetWindowWidth(), ga.y + ImGui::GetWindowHeight())); }
		const float gap = SZ(4);
		const float avail = ImGui::GetContentRegionAvail().x - ImGui::GetStyle().ScrollbarSize; // room for the scrollbar
		S.cols = std::max(1, (int)((avail + gap) / (SZ(60) + gap)));            // about 60 px cells, whole columns
		const float cell = std::floor((avail - gap * (S.cols - 1)) / (float)S.cols); // stretched so the row fills the panel
		const ImVec2 gridOrigin = ImGui::GetCursorScreenPos();
		const float gridViewH = ImGui::GetWindowHeight();
		ImDrawList* dl = ImGui::GetWindowDrawList();
		const int n = (int)S.shown.size();
		const int rows = (n + S.cols - 1) / S.cols;
		if ( S.scrollToSel )
		{
			const int si = shownIndex(S, S.sel);
			if ( si >= 0 ) { const float y = (si / S.cols) * (cell + gap); if ( y < ImGui::GetScrollY() || y + cell > ImGui::GetScrollY() + ImGui::GetWindowHeight() - SZ(8) ) { ImGui::SetScrollY(std::max(0.f, y - cell)); } }
			S.scrollToSel = false;
		}
		std::set<int> typeSeen; // QA aliases: inv/bag/type/<ItemType> (first cell of that type), inv/bag/index/<n> (n-th shown)
		ImGuiListClipper clip;
		splitBegin(dl);
		clip.Begin(rows, cell + gap);
		while ( clip.Step() )
		{
			for ( int r = clip.DisplayStart; r < clip.DisplayEnd; ++r )
			{
				for ( int k = 0; k < S.cols; ++k )
				{
					const int i = r * S.cols + k;
					if ( i >= n ) { break; }
					const Cell& c = S.cells[S.shown[i]];
					if ( k ) { ImGui::SameLine(0, gap); }
					char id[48]; snprintf(id, sizeof id, "inv/bag/cell/%u", (unsigned)c.uid);
					ImGui::PushID(id);
					const ImVec2 a = ImGui::GetCursorScreenPos();
					ImGui::InvisibleButton("c", ImVec2(cell, cell));
					recordLastItem(id);
					{
						char alias[48];
						if ( typeSeen.insert(c.type).second ) { snprintf(alias, sizeof alias, "inv/bag/type/%d", c.type); recordLastItem(alias); }
						snprintf(alias, sizeof alias, "inv/bag/index/%d", i); recordLastItem(alias);
					}
					const bool hov = ImGui::IsItemHovered();
					if ( ImGui::IsItemClicked(ImGuiMouseButton_Left) ) { S.sel = c.uid; S.zone = Z_BAG; logEvent(p, "ui_select", std::string("uid=") + std::to_string(c.uid)); }
					if ( hov && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left) )
					{
						S.sel = c.uid;
						auto acts = actionsFor(p, S, ownItem(p, c.uid));
						if ( !acts.empty() && acts[0].enabled ) { doAction(p, S, c.uid, acts[0].prompt); }
					}
					if ( ImGui::IsItemClicked(ImGuiMouseButton_Right) ) { S.sel = c.uid; S.quickUid = c.uid; S.openQuick = true; }
					dragSource(&c, c.uid, FROM_BAG, -1);
					const ImVec2 b(a.x + cell, a.y + cell);
					cellVisual(dl, c, a, b, S.sel == c.uid, hov, S.locked.count(c.uid) > 0, S.junk.count(c.uid) > 0);
					if ( focusedWin && S.zone == Z_BAG && S.sel == c.uid ) { focusFrame(dl, a, b); }
					ImGui::PopID();
				}
			}
		}
		splitEnd(dl);
		{	// empty slots fill the visible panel so the bag reads as a grid, not one lonely row (placeholders: no input)
			const int visRows = (int)(gridViewH / (cell + gap)) + 1;
			const int total = std::max(rows, visRows) * S.cols;
			for ( int i = n; i < total && i < n + S.cols * 40; ++i )
			{
				const float y0 = gridOrigin.y + (i / S.cols) * (cell + gap);
				if ( y0 > gridOrigin.y + ImGui::GetScrollY() + gridViewH ) { break; }
				const ImVec2 a(gridOrigin.x + (i % S.cols) * (cell + gap), y0);
				dl->AddRectFilled(a, ImVec2(a.x + cell, a.y + cell), IM_COL32(255, 255, 255, 10));
			}
		}
		if ( n == 0 ) { ImGui::TextDisabled(S.cells.empty() ? "Your bag is empty." : "Nothing matches."); }
		ImGui::EndChild();
		// dropping a worn item on the bag unequips it (Barony's unequip)
		if ( const Drag* d = dropTarget() ) { if ( d->from == FROM_EQUIP ) { doAction(p, S, d->uid, PROMPT_UNEQUIP); } }
		// footer: weight bar with the movement penalty in words, gold, the material pouch
		const Sint32 carried = S.carried, limit = std::max<Sint32>(1, S.limit);
		const float frac = std::min(1.f, (float)carried / (float)limit);
		const char* words = frac < 0.5f ? "unburdened" : frac < 0.75f ? "burdened: a little slower" : frac < 1.f ? "heavily burdened: much slower" : "overloaded: cannot move";
		char lbl[96]; snprintf(lbl, sizeof lbl, "%s / %s  %s", apx_inv::massText(carried).c_str(), apx_inv::massText(limit).c_str(), words);
		ImGui::PushStyleColor(ImGuiCol_PlotHistogram, frac < 0.75f ? ImVec4(0.55f, 0.45f, 0.20f, 1.f) : ImVec4(0.75f, 0.25f, 0.18f, 1.f));
		ImGui::ProgressBar(frac, ImVec2(-1, 0), lbl);
		recordLastItem("inv/bag/weight");
		ImGui::PopStyleColor();
		const auto& pv = apx_bench::provider();
		const long long pouch = pv.pouchGrams ? pv.pouchGrams(p) : 0;
		ImGui::Text("Gold %d", (int)stats[p]->GOLD);
		ImGui::SameLine();
		char pl[48]; snprintf(pl, sizeof pl, "Materials %lld g", pouch);
		if ( idButton("inv/bag/pouch", pl, ImVec2(0, 0)) ) { S.cat = CAT_MATERIALS; S.viewKey = ~0ULL; }
		ImGui::SameLine();
		ImGui::TextDisabled("%d items", (int)S.cells.size());
		(void)keyboardPlayer;
	}

	void drawDetails(int p, State& S, bool focusedWin)
	{
		Item* it = ownItem(p, S.sel);
		const Cell* c = it ? cellOf(S, it->uid) : nullptr;
		const std::vector<Act> acts = selActs(p, S);
		const float btnH = ImGui::GetFrameHeight();
		const float actionsH = it ? (btnH * 2.2f + ImGui::GetFrameHeightWithSpacing() * ((acts.size() + 2) / 3) + SZ(8)) : 0.f;
		ImGui::BeginChild("inv/details/body", ImVec2(0, -actionsH), false);
		if ( !it || !c )
		{
			ImGui::PushFont(fontHead()); ImGui::TextUnformatted("Details"); ImGui::PopFont();
			ImGui::TextDisabled("Select an item.");
			ImGui::EndChild();
			return;
		}
		// rebuild Barony's lines only when the item (or its state) changes
		const unsigned long long ds = mix(mix(mix(it->uid, (unsigned)it->count), (unsigned)it->status), (unsigned)(it->beatitude + 100) + (it->identified ? 1000 : 0));
		if ( S.detUid != it->uid || S.detSig != ds ) { S.detUid = it->uid; S.detSig = ds; S.det = harvestCached(p, it); }
		ImGui::PushFont(fontHead());
		ImGui::PushStyleColor(ImGuiCol_Text, ImGui::ColorConvertU32ToFloat4(rarityCol(c->rarity)));
		ImGui::TextWrapped("%s", c->name.c_str());
		ImGui::PopStyleColor();
		ImGui::PopFont();
		const ImVec2 ia = ImGui::GetCursorScreenPos();
		ImGui::Dummy(S2(64, 64));
		ImDrawList* dl = ImGui::GetWindowDrawList();
		dl->AddRectFilled(ia, ImVec2(ia.x + SZ(64), ia.y + SZ(64)), COL_CELL);
		dl->AddRect(ia, ImVec2(ia.x + SZ(64), ia.y + SZ(64)), rarityCol(c->rarity));
		drawIcon(dl, c->tex, ImVec2(ia.x + SZ(4), ia.y + SZ(4)), ImVec2(ia.x + SZ(60), ia.y + SZ(60)));
		ImGui::SameLine();
		ImGui::BeginGroup();
		const ItemEquippableSlot sl = slotOf(it);
		ImGui::Text("%s%s%s", catNameOf(it), sl != NO_EQUIP ? " - " : "", sl != NO_EQUIP ? ItemTooltips.getItemSlotName(sl).c_str() : "");
		ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(rarityCol(c->rarity)), "%s%s", RARITY_NAMES[c->rarity], c->identified ? "" : "  (unidentified)");
		if ( c->equipped ) { ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(COL_GOLD_HI), "Equipped"); }
		ImGui::EndGroup();
		// primary stats with compare-to-equipped deltas
		Item* cmp = (sl != NO_EQUIP && !c->equipped) ? worn(p, sl) : nullptr;
		auto delta = [&](const char* what, int mine, int theirs) {
			ImGui::Text("%s %d", what, mine);
			if ( cmp ) { ImGui::SameLine(); const int d = mine - theirs; ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(d > 0 ? COL_GOOD : d < 0 ? COL_BAD : COL_DIM), "(%+d vs worn)", d); }
		};
		if ( itemCategory(it) == WEAPON || itemCategory(it) == MAGICSTAFF || itemCategory(it) == THROWN ) { delta("Attack", it->weaponGetAttack(stats[p]), cmp ? cmp->weaponGetAttack(stats[p]) : 0); }
		if ( itemCategory(it) == ARMOR || (sl == EQUIPPABLE_IN_SLOT_SHIELD && itemCategory(it) != WEAPON) ) { delta("Armour", it->armorGetAC(stats[p]), cmp ? cmp->armorGetAC(stats[p]) : 0); }
		if ( cmp ) { ImGui::TextDisabled("worn: %s", displayName(cmp).c_str()); }
		// Barony's own effect lines
		if ( ImGui::CollapsingHeader("Effects##inv/details/effects", ImGuiTreeNodeFlags_DefaultOpen) )
		{
			recordLastItem("inv/details/effects");
			for ( auto& l : S.det.icons ) { ImGui::TextWrapped("%s", l.c_str()); }
			for ( auto& l : S.det.attributes ) { ImGui::TextWrapped("%s", l.c_str()); }
			if ( S.det.icons.empty() && S.det.attributes.empty() ) { ImGui::TextDisabled("none"); }
			if ( !S.det.description.empty() ) { ImGui::Spacing(); for ( auto& l : S.det.description ) { ImGui::PushStyleColor(ImGuiCol_Text, ImGui::ColorConvertU32ToFloat4(COL_DIM)); ImGui::TextWrapped("%s", l.c_str()); ImGui::PopStyleColor(); } }
		}
		// components and affixes (05b sections 4 and 7, from the F6 provider)
		const auto& pv = apx_bench::provider();
		std::vector<apx_bench::PartRow> parts = pv.parts ? pv.parts(it) : std::vector<apx_bench::PartRow>();
		apx_bench::PartRow comp;
		const bool loose = pv.componentOf && pv.componentOf(it, comp) == 1;
		if ( loose ) { parts.insert(parts.begin(), comp); }
		if ( !parts.empty() && ImGui::CollapsingHeader("Components##inv/details/components", ImGuiTreeNodeFlags_DefaultOpen) )
		{
			recordLastItem("inv/details/components");
			for ( auto& pr : parts )
			{
				ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(rarityCol(pr.rarity)), "%s: %s %s", pr.slotName.c_str(), pr.material.c_str(), pr.name.c_str());
				ImGui::Indent();
				ImGui::TextDisabled("%s, quality %d%s%s", RARITY_NAMES[std::max(0, std::min(5, pr.rarity))], pr.quality, pr.maker.empty() ? "" : ", by ", pr.maker.c_str());
				if ( !pr.affixesKnown && !pr.affixes.empty() ) { ImGui::TextDisabled("affixes: unknown"); }
				else { for ( auto& a : pr.affixes ) { ImGui::TextWrapped("%s", a.c_str()); } }
				char ib[32]; snprintf(ib, sizeof ib, "integrity %d%%", (int)(pr.integrity * 100.f + 0.5f));
				ImGui::ProgressBar(std::max(0.f, std::min(1.f, pr.integrity)), ImVec2(-1, SZ(10)), ib);
				if ( !pr.fits.empty() ) { std::string f; for ( auto& x : pr.fits ) { f += (f.empty() ? "" : ", ") + x; } ImGui::TextDisabled("fits: %s", f.c_str()); }
				ImGui::Unindent();
			}
			const int sockets = pv.sockets ? pv.sockets(it) : 0;
			if ( sockets > 0 ) { ImGui::Text("Sockets: %d", sockets); }
		}
		if ( ImGui::CollapsingHeader("Value and weight##inv/details/value", ImGuiTreeNodeFlags_DefaultOpen) )
		{
			recordLastItem("inv/details/value");
			ImGui::Text("Weight %s   Value %d gold", apx_inv::massText(it->getWeight()).c_str(), it->sellValue(p));
		}
		ImGui::EndChild();
		// action buttons: Barony's own options for this item, primary first
		ImGui::Separator();
		bool first = true; int col = 0;
		for ( int k = 0; k < (int)acts.size(); ++k )
		{
			const Act& a = acts[k];
			char id[64]; snprintf(id, sizeof id, "inv/details/action/%s", a.id.c_str());
			ImVec2 size = first ? ImVec2(-1, btnH * 1.6f) : ImVec2((ImGui::GetContentRegionAvail().x - ImGui::GetStyle().ItemSpacing.x * (col == 0 ? 2 : 0)) / (col == 0 ? 3.f : 1.f), 0);
			if ( !first )
			{
				const float w3 = (ImGui::GetWindowContentRegionMax().x - ImGui::GetWindowContentRegionMin().x - ImGui::GetStyle().ItemSpacing.x * 2) / 3.f;
				size = ImVec2(w3, 0);
				if ( col > 0 ) { ImGui::SameLine(); }
			}
			const ImVec2 pa = ImGui::GetCursorScreenPos();
			if ( idButton(id, a.label.c_str(), size, a.enabled) )
			{
				if ( a.prompt == PROMPT_DROP && c->rarity >= 2 && !S.confirmDrop ) { S.confirmDrop = true; S.confirmUid = it->uid; }
				else { doAction(p, S, it->uid, a.prompt); }
			}
			if ( focusedWin && S.zone == Z_ACTIONS && S.actionIdx == k ) { focusFrame(ImGui::GetWindowDrawList(), ImGui::GetItemRectMin(), ImGui::GetItemRectMax()); }
			(void)pa;
			if ( first ) { first = false; col = 0; } else { col = (col + 1) % 3; }
		}
	}

	void drawHotbar(int p, State& S, bool focusedWin)
	{
		auto& hb = players[p]->hotbar.slots();
		const float sz = SZ(40), gap = SZ(4);
		ImDrawList* dl = ImGui::GetWindowDrawList();
		ImGui::TextUnformatted("Hotbar"); ImGui::SameLine();
		splitBegin(dl);
		for ( int k = 0; k < (int)NUM_HOTBAR_SLOTS; ++k )
		{
			if ( k ) { ImGui::SameLine(0, gap); }
			char id[32]; snprintf(id, sizeof id, "inv/hotbar/%d", k + 1);
			ImGui::PushID(id);
			const ImVec2 a = ImGui::GetCursorScreenPos();
			ImGui::InvisibleButton("h", ImVec2(sz, sz));
			recordLastItem(id);
			const bool hov = ImGui::IsItemHovered();
			Item* it = ownItem(p, hb[k].item);
			const Cell* c = it ? cellOf(S, it->uid) : nullptr;
			if ( ImGui::IsItemClicked(ImGuiMouseButton_Left) )
			{
				if ( S.sel && S.zone != Z_HOTBAR && ownItem(p, S.sel) ) { assignHotbar(p, S, k, S.sel); }
				else if ( it ) { S.sel = it->uid; }
				S.hotbarIdx = k;
			}
			if ( ImGui::IsItemClicked(ImGuiMouseButton_Right) && hb[k].item ) { assignHotbar(p, S, k, 0); }
			if ( it ) { dragSource(c, it->uid, FROM_HOTBAR, k); }
			if ( const Drag* d = dropTarget() )
			{
				if ( d->from == FROM_HOTBAR && d->fromSlot >= 0 && d->fromSlot != k ) { const Uint32 other = hb[k].item; assignHotbar(p, S, k, d->uid); if ( other ) { assignHotbar(p, S, d->fromSlot, other); } }
				else { assignHotbar(p, S, k, d->uid); }
			}
			const ImVec2 b(a.x + sz, a.y + sz);
			ch(dl, 0);
			dl->AddRectFilled(a, b, hov ? COL_CELL_HOVER : COL_CELL);
			ch(dl, 1);
			if ( c ) { drawIcon(dl, c->tex, ImVec2(a.x + SZ(3), a.y + SZ(3)), ImVec2(b.x - SZ(3), b.y - SZ(3))); }
			ch(dl, 2);
			dl->AddRect(a, b, c ? rarityCol(c->rarity) : IM_COL32(90, 70, 40, 255));
			char n[4]; snprintf(n, sizeof n, "%d", (k + 1) % 10);
			dl->AddText(fontSmall(), fontSmall()->FontSize, ImVec2(a.x + SZ(2), a.y + SZ(1)), COL_DIM, n);
			if ( focusedWin && S.zone == Z_HOTBAR && S.hotbarIdx == k ) { focusFrame(dl, a, b); }
			ImGui::PopID();
		}
		splitEnd(dl);
	}

	void drawQuickMenu(int p, State& S, bool padPlayer)
	{
		char pid[32]; snprintf(pid, sizeof pid, "inv/quick##%d", p);
		if ( S.openQuick ) { ImGui::OpenPopup(pid); S.openQuick = false; S.quickIdx = 0; if ( padPlayer ) { S.zone = Z_QUICK; } }
		if ( ImGui::BeginPopup(pid) )
		{
			g_popupOpen = true;
			if ( ImGui::IsKeyPressed(ImGuiKey_Escape) ) { ImGui::CloseCurrentPopup(); }
			Item* it = ownItem(p, S.quickUid);
			if ( !it ) { ImGui::CloseCurrentPopup(); ImGui::EndPopup(); if ( S.zone == Z_QUICK ) { S.zone = Z_BAG; } return; }
			const Cell* c = cellOf(S, it->uid);
			ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(rarityCol(c ? c->rarity : 0)), "%s", c ? c->name.c_str() : "item");
			ImGui::Separator();
			auto acts = actionsFor(p, S, it);
			for ( int k = 0; k < (int)acts.size(); ++k )
			{
				char id[64]; snprintf(id, sizeof id, "inv/quick/%s", acts[k].id.c_str());
				ImGui::PushID(id);
				const bool hl = S.zone == Z_QUICK && S.quickIdx == k;
				if ( ImGui::Selectable(acts[k].label.c_str(), hl, acts[k].enabled ? 0 : ImGuiSelectableFlags_Disabled) ) { doAction(p, S, it->uid, acts[k].prompt); ImGui::CloseCurrentPopup(); S.zone = Z_BAG; }
				recordLastItem(id);
				ImGui::PopID();
			}
			if ( S.zone == Z_QUICK && S.quickIdx < 0 ) { ImGui::CloseCurrentPopup(); S.zone = Z_BAG; }
			ImGui::EndPopup();
		}
		else if ( S.zone == Z_QUICK ) { S.zone = Z_BAG; }
	}

	void drawConfirm(int p, State& S)
	{
		char pid[32]; snprintf(pid, sizeof pid, "Drop this item?##inv/confirm/%d", p);
		if ( S.confirmDrop ) { ImGui::OpenPopup(pid); S.confirmDrop = false; }
		if ( ImGui::BeginPopupModal(pid, nullptr, ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoSavedSettings) )
		{
			g_popupOpen = true;
			if ( ImGui::IsKeyPressed(ImGuiKey_Escape) ) { ImGui::CloseCurrentPopup(); }
			const Cell* c = cellOf(S, S.confirmUid);
			ImGui::Text("Drop %s?", c ? c->name.c_str() : "it");
			if ( idButton("inv/confirm/yes", "Drop", S2(90, 0)) ) { doAction(p, S, S.confirmUid, PROMPT_DROP); ImGui::CloseCurrentPopup(); }
			ImGui::SameLine();
			if ( idButton("inv/confirm/no", "Keep", S2(90, 0)) ) { ImGui::CloseCurrentPopup(); }
			ImGui::EndPopup();
		}
	}

	// ------------------------------------------------------------------ focus model (keyboard + per-player gamepad)
	void moveSel(State& S, int d)
	{
		if ( S.shown.empty() ) { S.sel = 0; return; }
		int i = shownIndex(S, S.sel);
		if ( i < 0 ) { i = 0; } else { i = std::max(0, std::min((int)S.shown.size() - 1, i + d)); }
		S.sel = S.cells[S.shown[i]].uid;
		S.scrollToSel = true;
	}

	void primary(int p, State& S)
	{
		auto acts = actionsFor(p, S, ownItem(p, S.sel));
		if ( !acts.empty() && acts[0].enabled ) { doAction(p, S, S.sel, acts[0].prompt); }
	}

	bool focusInput(int p, State& S, const std::string& a)
	{
		Item* selItem = ownItem(p, S.sel);
		const int nActs = (int)actionsFor(p, S, selItem).size();
		if ( a == "b" )
		{
			if ( S.zone == Z_QUICK || S.zone == Z_ACTIONS || S.zone == Z_HOTBAR || S.zone == Z_EQUIP ) { S.zone = Z_BAG; if ( S.zone == Z_QUICK ) { S.quickIdx = -1; } return true; }
			closeWindow(p); return true;
		}
		if ( a == "lb" || a == "rb" ) { S.cat = (S.cat + (a == "rb" ? 1 : CAT_MAX - 1)) % CAT_MAX; S.viewKey = ~0ULL; return true; }
		if ( a == "y" ) { if ( selItem ) { S.zone = Z_ACTIONS; S.actionIdx = 0; } return true; }
		if ( a == "x" ) { if ( selItem ) { S.quickUid = S.sel; S.openQuick = true; } return true; }
		switch ( S.zone )
		{
			case Z_BAG:
				if ( a == "left" ) { const int i = shownIndex(S, S.sel); if ( i >= 0 && i % S.cols == 0 ) { S.zone = Z_EQUIP; } else { moveSel(S, -1); } }
				else if ( a == "right" ) { moveSel(S, +1); }
				else if ( a == "up" ) { moveSel(S, -S.cols); }
				else if ( a == "down" ) { const int i = shownIndex(S, S.sel); if ( i >= 0 && i + S.cols >= (int)S.shown.size() ) { S.zone = Z_HOTBAR; } else { moveSel(S, S.cols); } }
				else if ( a == "a" ) { primary(p, S); }
				else if ( a == "delete" ) { if ( selItem ) { const Cell* c = cellOf(S, S.sel); if ( c && c->rarity >= 2 ) { S.confirmDrop = true; S.confirmUid = S.sel; } else { doAction(p, S, S.sel, PROMPT_DROP); } } }
				else { return false; }
				return true;
			case Z_EQUIP:
			{
				if ( a == "up" ) { S.slotIdx = std::max(0, S.slotIdx - 1); }
				else if ( a == "down" ) { S.slotIdx = std::min(NUM_SLOTS - 1, S.slotIdx + 1); }
				else if ( a == "right" ) { S.zone = Z_BAG; }
				else if ( a == "a" ) { if ( Item* w = worn(p, SLOTS[S.slotIdx].slot) ) { doAction(p, S, w->uid, PROMPT_UNEQUIP); } }
				else { return false; }
				if ( Item* w = worn(p, SLOTS[S.slotIdx].slot) ) { S.sel = w->uid; }
				return true;
			}
			case Z_ACTIONS:
				if ( a == "left" || a == "up" ) { S.actionIdx = std::max(0, S.actionIdx - 1); }
				else if ( a == "right" || a == "down" ) { S.actionIdx = std::min(std::max(0, nActs - 1), S.actionIdx + 1); }
				else if ( a == "a" ) { auto acts = actionsFor(p, S, selItem); if ( S.actionIdx < (int)acts.size() && acts[S.actionIdx].enabled ) { doAction(p, S, S.sel, acts[S.actionIdx].prompt); } S.zone = Z_BAG; }
				else { return false; }
				return true;
			case Z_HOTBAR:
				if ( a == "left" ) { S.hotbarIdx = std::max(0, S.hotbarIdx - 1); }
				else if ( a == "right" ) { S.hotbarIdx = std::min((int)NUM_HOTBAR_SLOTS - 1, S.hotbarIdx + 1); }
				else if ( a == "up" ) { S.zone = Z_BAG; }
				else if ( a == "a" ) { if ( selItem ) { assignHotbar(p, S, S.hotbarIdx, S.sel); } }
				else { return false; }
				return true;
			case Z_QUICK:
			{
				auto acts = actionsFor(p, S, ownItem(p, S.quickUid));
				if ( a == "up" ) { S.quickIdx = std::max(0, S.quickIdx - 1); }
				else if ( a == "down" ) { S.quickIdx = std::min((int)acts.size() - 1, S.quickIdx + 1); }
				else if ( a == "a" ) { if ( S.quickIdx >= 0 && S.quickIdx < (int)acts.size() && acts[S.quickIdx].enabled ) { doAction(p, S, S.quickUid, acts[S.quickIdx].prompt); } S.quickIdx = -1; }
				else { return false; }
				return true;
			}
		}
		return false;
	}

	void readKeyboard(int p, State& S)
	{
		if ( ImGui::GetIO().WantTextInput )
		{
			if ( ImGui::IsKeyPressed(ImGuiKey_Escape) || ImGui::IsKeyPressed(ImGuiKey_Enter) ) { ImGui::ClearActiveID(); }
			return;
		}
		if ( ImGui::IsKeyPressed(ImGuiKey_LeftArrow) ) { focusInput(p, S, "left"); }
		if ( ImGui::IsKeyPressed(ImGuiKey_RightArrow) ) { focusInput(p, S, "right"); }
		if ( ImGui::IsKeyPressed(ImGuiKey_UpArrow) ) { focusInput(p, S, "up"); }
		if ( ImGui::IsKeyPressed(ImGuiKey_DownArrow) ) { focusInput(p, S, "down"); }
		if ( ImGui::IsKeyPressed(ImGuiKey_Enter) || ImGui::IsKeyPressed(ImGuiKey_KeypadEnter) ) { focusInput(p, S, "a"); }
		if ( ImGui::IsKeyPressed(ImGuiKey_Delete) ) { focusInput(p, S, "delete"); }
		if ( ImGui::IsKeyPressed(ImGuiKey_Slash) ) { S.focusSearch = true; }
	}

	void readGamepad(int p, State& S)
	{
		Input& in = Input::inputs[p];
		static const char* map[][2] = { { "MenuUp", "up" }, { "MenuDown", "down" }, { "MenuLeft", "left" }, { "MenuRight", "right" },
			{ "MenuConfirm", "a" }, { "MenuCancel", "b" }, { "MenuAlt1", "x" }, { "MenuAlt2", "y" }, { "MenuPageLeft", "lb" }, { "MenuPageRight", "rb" } };
		for ( auto& m : map )
		{
			if ( in.consumeBinaryToggle(m[0]) ) { focusInput(p, S, m[1]); }
		}
	}

	void readHotbarKeys(int p, State& S)
	{
		// 1-0 while an item is selected: assign it to that hotbar slot (consumed before the HUD would use the slot)
		if ( !ownItem(p, S.sel) || ImGui::GetIO().WantTextInput ) { return; }
		Input& in = Input::inputs[p];
		for ( int k = 0; k < (int)NUM_HOTBAR_SLOTS; ++k )
		{
			char b[32]; snprintf(b, sizeof b, "Hotbar Slot %d", k + 1);
			if ( in.binaryToggle(b) ) { in.consumeBinaryToggle(b); assignHotbar(p, S, k, S.sel); }
		}
	}

	void damageWarning(int p, State& S, ImVec2 a, ImVec2 b)
	{
		Stat* st = stats[p];
		if ( S.lastHP >= 0 && st->HP < S.lastHP )
		{
			const int loss = S.lastHP - st->HP;
			S.flashUntil = SDL_GetTicks() + 1200; S.flashLoss = loss;
			Player::soundCancel();
			logEvent(p, "ui_damage", "loss=" + std::to_string(loss) + " hp=" + std::to_string(st->HP));
			const char* knob = getenv("APX_UI_CLOSE_ON_HIT");
			const bool closeOnHit = knob ? knob[0] == '1' : (multiplayer == SINGLE && !splitscreen); // section 4 + 10: single player only (ASSUMED)
			if ( closeOnHit && st->MAXHP > 0 && loss * 5 > st->MAXHP ) { S.lastHP = st->HP; closeWindow(p); return; }
		}
		S.lastHP = st->HP;
		if ( SDL_TICKS_PASSED(SDL_GetTicks(), S.flashUntil) ) { return; }
		ImDrawList* fg = ImGui::GetForegroundDrawList();
		const int alpha = 120 + (int)(135 * ((S.flashUntil - SDL_GetTicks()) / 1200.f));
		fg->AddRect(a, b, IM_COL32(230, 40, 30, alpha), 0.f, 0, SZ(4));
		char t[64]; snprintf(t, sizeof t, "-%d HP  (%d/%d)", S.flashLoss, st->HP, st->MAXHP);
		ImFont* f = fontHead();
		const ImVec2 ts = f->CalcTextSizeA(f->FontSize, 1e9f, 0.f, t);
		fg->AddRectFilled(ImVec2((a.x + b.x - ts.x) / 2 - SZ(6), a.y + SZ(4)), ImVec2((a.x + b.x + ts.x) / 2 + SZ(6), a.y + SZ(8) + ts.y), IM_COL32(60, 0, 0, 220));
		fg->AddText(f, f->FontSize, ImVec2((a.x + b.x - ts.x) / 2, a.y + SZ(6)), IM_COL32(255, 210, 200, 255), t);
	}

	void drawWindow(int p)
	{
		State& S = g_st[p];
		Player& pl = *players[p];
		const bool kbm = inputs.bPlayerUsingKeyboardControl(p);
		const bool pad = !kbm || inputs.hasController(p);
		// the view model: rebuilt only when the inventory signature moves
		const unsigned long long sig = invSig(p);
		if ( sig != S.sig ) { S.sig = sig; rebuild(p, S); S.viewKey = ~0ULL; }
		const unsigned long long vk = mix(mix(mix(sig, (unsigned)S.cat), (unsigned)(S.sort * 2 + S.sortDesc)), std::hash<std::string>()(S.search));
		if ( vk != S.viewKey ) { S.viewKey = vk; refilter(S); }
		refreshStats(p, S);
		if ( S.sel && !ownItem(p, S.sel) ) { S.sel = 0; }
		if ( !S.sel && !S.shown.empty() && pad ) { S.sel = S.cells[S.shown[0]].uid; }

		// placement: inside this player's view, above the HUD bars and hotbar (invariant 5)
		const view_t& cam = pl.camera();
		const float hudBottom = SZ(165); // Barony HUD at the bottom of each view: message lines, hotbar, HP/MP bars (measured at 1280x720)
		const float margin = SZ(8);
		ImVec2 pos((float)cam.winx + margin, (float)cam.winy + margin);
		ImVec2 size((float)cam.winw - margin * 2, (float)cam.winh - margin - hudBottom);
		size.y = std::max(size.y, SZ(200));
		ImGui::SetNextWindowPos(pos, ImGuiCond_Always);
		ImGui::SetNextWindowSize(size, ImGuiCond_Always);
		ImGuiWindowFlags fl = ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoCollapse
			| ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse | ImGuiWindowFlags_NoFocusOnAppearing
			| ImGuiWindowFlags_NoBringToFrontOnFocus;
		if ( !kbm ) { fl |= ImGuiWindowFlags_NoInputs; } // only the keyboard-and-mouse player's window takes the mouse; pads use the focus model
		char wid[32]; snprintf(wid, sizeof wid, "##apxinv%d", p);
		ImGui::PushFont(fontBody());
		ImGui::Begin(wid, nullptr, fl);
		{
			const ImVec2 wa = ImGui::GetWindowPos();
			recordRect("inv/window", wa, ImVec2(wa.x + ImGui::GetWindowWidth(), wa.y + ImGui::GetWindowHeight()));
		}
		if ( kbm ) { readKeyboard(p, S); }
		if ( pad && !kbm ) { readGamepad(p, S); }
		readHotbarKeys(p, S);

		const float W = ImGui::GetContentRegionAvail().x;
		const float hotbarH = SZ(40) + ImGui::GetStyle().ItemSpacing.y * 2 + SZ(4);
		const float colH = ImGui::GetContentRegionAvail().y - hotbarH;
		const bool narrow = size.x < 900.f;
		const float gap = ImGui::GetStyle().ItemSpacing.x;
		if ( !narrow )
		{
			const float leftW = std::max(SZ(220), W * 0.24f), rightW = std::max(SZ(260), W * 0.30f);
			ImGui::BeginChild("inv/col/equipment", ImVec2(leftW, colH), true);
			drawEquipment(p, S, pad);
			ImGui::EndChild();
			ImGui::SameLine(0, gap);
			ImGui::BeginChild("inv/col/bag", ImVec2(W - leftW - rightW - gap * 2, colH), true, ImGuiWindowFlags_NoScrollbar);
			drawBag(p, S, pad, kbm);
			ImGui::EndChild();
			ImGui::SameLine(0, gap);
			ImGui::BeginChild("inv/col/details", ImVec2(rightW, colH), true, ImGuiWindowFlags_NoScrollbar);
			drawDetails(p, S, pad);
			ImGui::EndChild();
		}
		else
		{
			// narrow views (split-screen): two columns, Equipment as a tab beside Details (section 2)
			const float rightW = std::max(SZ(200), W * 0.42f);
			ImGui::BeginChild("inv/col/bag", ImVec2(W - rightW - gap, colH), true, ImGuiWindowFlags_NoScrollbar);
			drawBag(p, S, pad, kbm);
			ImGui::EndChild();
			ImGui::SameLine(0, gap);
			ImGui::BeginChild("inv/col/details", ImVec2(rightW, colH), true, ImGuiWindowFlags_NoScrollbar);
			if ( idButton("inv/tab/details", "Details", ImVec2(0, 0)) ) { S.narrowEquipTab = false; }
			ImGui::SameLine();
			if ( idButton("inv/tab/equipment", "Equipment", ImVec2(0, 0)) || (S.zone == Z_EQUIP) ) { S.narrowEquipTab = true; }
			if ( S.zone == Z_BAG && pad && !kbm ) { S.narrowEquipTab = false; }
			if ( S.narrowEquipTab ) { ImGui::BeginChild("inv/col/equipment", ImVec2(0, 0), false); drawEquipment(p, S, pad); ImGui::EndChild(); }
			else { drawDetails(p, S, pad); }
			ImGui::EndChild();
		}
		drawHotbar(p, S, pad);
		drawQuickMenu(p, S, pad && !kbm);
		drawConfirm(p, S);
		const ImVec2 wa = ImGui::GetWindowPos();
		damageWarning(p, S, wa, ImVec2(wa.x + ImGui::GetWindowWidth(), wa.y + ImGui::GetWindowHeight()));
		ImGui::End();
		ImGui::PopFont();
	}
} // namespace

	bool replacesVanilla(int player)
	{
		if ( player < 0 || player >= MAXPLAYERS || !players[player] || !contextReady() ) { return false; }
		if ( getenv("APX_VANILLA_INVENTORY") && getenv("APX_VANILLA_INVENTORY")[0] == '1' ) { return false; } // control trial knob: Barony's window back
		return plainInventory(player);
	}

	bool anyOpen()
	{
		for ( int p = 0; p < MAXPLAYERS; ++p ) { if ( g_st[p].open ) { return true; } }
		return false;
	}

	// ------------------------------------------------------------------ headless self-test (selftest-all INVENTORY stage)
	// Drives the window's model and every action by the same functions the clicks call (doAction/assignHotbar/refilter),
	// each through Barony's own item paths, and asserts the game effect. Clicking real widgets is the QA driver's job
	// (tools/apx/qa/inv_play.txt); this runs where no window exists.
	bool selfTest(std::string& fields)
	{
		const int p = 0;
		if ( !stats[p] || !players[p] ) { fields = "\"window\":\"no_player\""; return false; }
		State& S = g_st[p];
		const bool wasOpen = S.open; S.open = false;
		int pass = 0, total = 0;
		std::ostringstream o;
		auto chk = [&](const char* name, bool ok) { ++total; if ( ok ) { ++pass; } o << (total > 1 ? "," : "") << "\"" << name << "\":" << (ok ? 1 : 0); };
		Item* sword = newItem(IRON_SWORD, EXCELLENT, 0, 1, 7, true, &stats[p]->inventory);
		Item* apple = newItem(FOOD_APPLE, EXCELLENT, 0, 2, 7, true, &stats[p]->inventory);
		const Uint32 swordUid = sword->uid, appleUid = apple->uid;
		const int savedCat = S.cat, savedSort = S.sort; char savedSearch[64]; memcpy(savedSearch, S.search, sizeof savedSearch);
		S.search[0] = 0; S.cat = CAT_ALL; S.sort = SORT_NAME;
		rebuild(p, S); refilter(S);
		chk("model_has_items", cellOf(S, swordUid) && cellOf(S, appleUid));
		S.cat = CAT_WEAPONS; refilter(S);
		chk("category_weapons", shownIndex(S, swordUid) >= 0 && shownIndex(S, appleUid) < 0);
		S.cat = CAT_ALL; snprintf(S.search, sizeof S.search, "apple"); refilter(S);
		chk("search_apple", shownIndex(S, appleUid) >= 0 && shownIndex(S, swordUid) < 0);
		S.search[0] = 0; refilter(S);
		bool sorted = true;
		for ( size_t i = 1; i < S.shown.size(); ++i ) { sorted &= S.cells[S.shown[i - 1]].lname <= S.cells[S.shown[i]].lname; }
		chk("sort_name", sorted);
		// every action the details buttons list must exist for these items
		auto has = [&](Item* it, int prompt) { for ( auto& a : actionsFor(p, S, it) ) { if ( a.prompt == prompt ) { return true; } } return false; };
		chk("buttons_equip_drop", has(sword, PROMPT_EQUIP) && has(sword, PROMPT_DROP) && has(apple, PROMPT_DROP));
		// equip / unequip change the game
		doAction(p, S, swordUid, PROMPT_EQUIP);
		chk("equip", ownItem(p, swordUid) && itemIsEquipped(ownItem(p, swordUid), p));
		doAction(p, S, swordUid, PROMPT_UNEQUIP);
		chk("unequip", ownItem(p, swordUid) && !itemIsEquipped(ownItem(p, swordUid), p));
		// hotbar assign, then clear
		assignHotbar(p, S, 2, appleUid);
		chk("hotbar_assign", players[p]->hotbar.slots()[2].item == appleUid);
		assignHotbar(p, S, 2, 0);
		chk("hotbar_clear", players[p]->hotbar.slots()[2].item == 0);
		// use (eat) consumes one apple
		int eatPrompt = -1; for ( auto& a : actionsFor(p, S, apple) ) { if ( a.prompt == PROMPT_EAT || a.prompt == PROMPT_CONSUME || a.prompt == PROMPT_INTERACT ) { eatPrompt = a.prompt; break; } }
		const Sint32 savedHunger = stats[p]->HUNGER; stats[p]->HUNGER = 400; // hungry enough to eat
		const Sint32 hungerBefore = stats[p]->HUNGER;
		if ( eatPrompt >= 0 ) { doAction(p, S, appleUid, eatPrompt); }
		Item* a2 = ownItem(p, appleUid);
		chk("use_eat", eatPrompt >= 0 && a2 && a2->count == 1 && stats[p]->HUNGER > hungerBefore);
		stats[p]->HUNGER = savedHunger;
		// a locked item refuses to drop, an unlocked one leaves the bag
		doAction(p, S, swordUid, ACT_LOCK);
		doAction(p, S, swordUid, PROMPT_DROP);
		chk("lock_blocks_drop", ownItem(p, swordUid) != nullptr && S.lastResult != "gone");
		doAction(p, S, swordUid, ACT_LOCK);
		doAction(p, S, swordUid, PROMPT_DROP);
		chk("drop_sword", ownItem(p, swordUid) == nullptr);
		for ( int k = 0; k < 3 && ownItem(p, appleUid); ++k ) { doAction(p, S, appleUid, PROMPT_DROP); } // Barony drops one of a stack per click
		chk("drop_apple", ownItem(p, appleUid) == nullptr);
		S.locked.clear(); S.search[0] = 0; S.cat = savedCat; S.sort = savedSort; memcpy(S.search, savedSearch, sizeof savedSearch);
		S.sel = 0; S.sig = ~0ULL; S.open = wasOpen;
		fields = "\"window\":{" + o.str() + "}";
		return pass == total;
	}

	bool popupOpen() { return g_popupWasOpen || g_popupOpen; }

	bool mouseOwned()
	{
		for ( int p = 0; p < MAXPLAYERS; ++p ) { if ( g_st[p].open && inputs.bPlayerUsingKeyboardControl(p) ) { return true; } }
		return false;
	}

	bool wantsKey(const SDL_Event& e)
	{
		if ( e.type != SDL_KEYDOWN ) { return false; }
		bool kbmOpen = false;
		for ( int p = 0; p < MAXPLAYERS; ++p ) { kbmOpen |= g_st[p].open && inputs.bPlayerUsingKeyboardControl(p); }
		if ( !kbmOpen ) { return false; }
		switch ( e.key.keysym.scancode )
		{
			// our keys while the window is open (movement keys still reach the game: you can walk)
			case SDL_SCANCODE_UP: case SDL_SCANCODE_DOWN: case SDL_SCANCODE_LEFT: case SDL_SCANCODE_RIGHT:
			case SDL_SCANCODE_RETURN: case SDL_SCANCODE_KP_ENTER: case SDL_SCANCODE_DELETE: case SDL_SCANCODE_SLASH:
				return true;
			case SDL_SCANCODE_ESCAPE:
				return g_popupWasOpen; // closes our quick menu / confirm, not the whole screen
			default:
				return false;
		}
	}

	void hideVanilla(int player)
	{
		Player& pl = *players[player];
		if ( pl.inventoryUI.frame ) { pl.inventoryUI.frame->setDisabled(true); }
		if ( pl.characterSheet.sheetFrame ) { pl.characterSheet.sheetFrame->setDisabled(true); }
		if ( pl.inventoryUI.tooltipContainerFrame ) { pl.inventoryUI.tooltipContainerFrame->setDisabled(true); }
		if ( pl.inventoryUI.interactFrame ) { pl.inventoryUI.interactFrame->setDisabled(true); }
		auto* ui = inputs.getUIInteraction(player);
		ui->itemMenuOpen = false;
		ui->selectedItem = nullptr;
	}

	// one cold item per frame while the window is closed (the cost lands on a play frame, never on the window)
	void warmTick(int p)
	{
		Stat* st = stats[p];
		static Uint32 retryAt = 0;
		if ( !st || !players[p] || !players[p]->entity || intro || !gameUIFrame[p] || ticks < retryAt ) { return; }
		for ( node_t* n = st->inventory.first; n; n = n->next )
		{
			Item* it = static_cast<Item*>(n->element);
			if ( !it || it->type < 0 || it->type >= NUMITEMS || itemCategory(it) == SPELL_CAT ) { continue; }
			if ( g_lineCache.count(lineKey(p, it)) ) { continue; }
			const unsigned m0 = g_cacheMisses;
			Lines L = harvestCached(p, it); itemIcon(p, *it, nullptr, nullptr);
			if ( g_cacheMisses != m0 && !L.ok ) { retryAt = ticks + 100; ++g_warmFail; } else { ++g_warmed; }
			return;
		}
	}

	void draw()
	{
		g_popupWasOpen = g_popupOpen;
		g_popupOpen = false;
		for ( int p = 0; p < MAXPLAYERS; ++p )
		{
			State& S = g_st[p];
			const bool want = replacesVanilla(p);
			if ( want != S.open )
			{
				S.open = want;
				logEvent(p, want ? "ui_open" : "ui_close", "");
				if ( want ) { S.lastHP = stats[p] ? stats[p]->HP : -1; S.sig = ~0ULL; S.zone = Z_BAG; hideVanilla(p); }
			}
			if ( S.open ) { drawWindow(p); } else { warmTick(p); }
		}
	}

	bool padAction(int player, const std::string& action)
	{
		if ( player < 0 || player >= MAXPLAYERS || !g_st[player].open ) { return false; }
		return focusInput(player, g_st[player], action);
	}

	std::string status()
	{
		std::ostringstream o;
		for ( int p = 0; p < MAXPLAYERS; ++p )
		{
			const State& S = g_st[p];
			if ( !S.open ) { continue; }
			const Cell* c = cellOf(S, S.sel);
			o << "ui_inv p" << p << " open 1 items " << S.cells.size() << " shown " << S.shown.size() << " cat " << CAT_IDS[S.cat]
				<< " sort " << SORT_NAMES[S.sort] << " sel " << S.sel << " sel_name \"" << (c ? c->name : "") << "\" zone " << S.zone
				<< " actions " << S.actions << " last " << S.lastAction << " result \"" << S.lastResult << "\"\n";
			// measured: no vanilla item frame is live while ours is open (disabled frames are never drawn and take no input)
			const Player& pl = *players[p];
			auto live = [](const Frame* f) { return f && !f->isDisabled() ? 1 : 0; };
			o << "vanilla p" << p << " inventory_frame_live " << live(pl.inventoryUI.frame) << " sheet_live " << live(pl.characterSheet.sheetFrame)
				<< " tooltip_live " << live(pl.inventoryUI.tooltipContainerFrame) << " context_menu_live " << live(pl.inventoryUI.interactFrame) << "\n";
		}
		return o.str();
	}

	std::string dump()
	{
		std::ostringstream o;
		o << status();
		o << "rebuilds " << g_rebuilds << " harvests " << g_harvestCalls << " cache_hits " << g_cacheHits << " cache_misses " << g_cacheMisses << " warmed " << g_warmed << " warm_fail " << g_warmFail << "\n";
		for ( int p = 0; p < MAXPLAYERS; ++p )
		{
			const State& S = g_st[p];
			if ( !S.open ) { continue; }
			for ( int i : S.shown ) { const Cell& c = S.cells[i]; o << "cell p" << p << " uid " << c.uid << " type " << c.type << " count " << c.count << " rarity " << c.rarity << (c.equipped ? " E" : "") << " \"" << c.name << "\"\n"; }
			auto& hb = players[p]->hotbar.slots();
			o << "hotbar p" << p;
			for ( auto& h : hb ) { o << ' ' << h.item; }
			o << "\n";
			for ( auto& a : S.acts ) { o << "action " << a.id << " label=\"" << a.label << "\" enabled=" << a.enabled << "\n"; }
			for ( auto& l : S.det.icons ) { o << "detail_icon \"" << l << "\"\n"; }
			for ( auto& l : S.det.attributes ) { o << "detail_attr \"" << l << "\"\n"; }
			for ( auto& l : S.det.description ) { o << "detail_desc \"" << l << "\"\n"; }
			for ( auto& l : S.bonuses ) { o << "bonus \"" << l << "\"\n"; }
		}
		return o.str();
	}
} // namespace inv
} // namespace ui
} // namespace apx
