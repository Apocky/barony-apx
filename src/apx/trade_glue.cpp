// trade_glue.cpp -- BT1 trade, engine side (card BT1, D73). Pure rules: trade_rules.hpp.
#include "../main.hpp"
#include "../game.hpp"
#include "../stat.hpp"
#include "../entity.hpp"
#include "../items.hpp"
#include "../monster.hpp"
#include "../net.hpp"
#include "../player.hpp"
#include "../shops.hpp"
#include "trade_glue.hpp"
#include "trade_rules.hpp"
#include "parts_engine.hpp"

#include <cstdlib>
#include <cstring>
#include <map>
#include <set>
#include <string>

namespace apx
{
namespace tradeglue
{
	Counters counters;

namespace
{
	const char* const TYPE_NAMES[] = {
#include "parts_typenames.inc"
	};
	static_assert(sizeof(TYPE_NAMES) / sizeof(TYPE_NAMES[0]) == NUMITEMS, "parts_typenames.inc is stale: run tools/apx/gen_forms.py");

	constexpr int kDemandRef = 2;       // demand reference: a stack of 2 sells at the base price (price = base x demand / supply, supply = stack size)
	std::set<Uint32> havenUids, peddlerUids;
	std::string prevHaven, floorHaven;
	int floorKey = -1;
	bool floorGeneralTaken = false;
	std::map<std::string, int> discountPct;

	int typeByNameImpl_(const std::string& n)
	{
		for ( int i = 0; i < NUMITEMS; ++i ) { if ( n == TYPE_NAMES[i] ) { return i; } }
		return -1;
	}
	bool isHamlet() { return !strcmp(map.name, "Mages Guild"); }

	// vanilla store type -> trade (the Hamlet keeps its vanilla shopkeepers, one trade each)
	const char* tradeForStoreType(int storeType)
	{
		static const char* const t[] = { "smith", "smith", "fence", "scribe", "alchemist", "alchemist", "provisioner", "general", "smith", "general" };
		return (storeType >= 0 && storeType <= 9) ? t[storeType] : "general";
	}
	const trade::TradeRow* tradeOfPlayerShop(int player)
	{
		if ( player < 0 || player >= MAXPLAYERS ) { return nullptr; }
		int st = shopkeepertype[player];
		if ( st == 10 ) { return nullptr; }   // the mysterious merchant stays vanilla
		return trade::findTrade(tradeForStoreType(st));
	}
	bool materialIsMagic(const std::string& id) { return id == "glass" || id == "obsidian" || id == "quartz" || id == "aethite"; }

	void addItem(Stat* st, const std::string& typeName, int count, Status status, bool identified)
	{
		int t = typeByNameImpl_(typeName);
		if ( t < 0 || count < 1 ) { ++counters.rowsPendingPouch; return; }
		newItem(static_cast<ItemType>(t), status, 0, (Sint16)std::min(count, 30000), 0, identified, &st->inventory);
		++counters.rowsRealized;
	}
	// puts a loose item (built by F6 in no inventory) on the shopkeeper's shelf, the way newItem(..., &inventory) does
	void attach(Stat* st, Item* item)
	{
		node_t* node = list_AddNodeLast(&st->inventory);
		node->element = item;
		node->deconstructor = &defaultDeconstructor;
		node->size = sizeof(Item);
		item->node = node;
	}
	// one stock row -> items. Material lots and reagents become F6 bundles (D85: buying one puts the grams in the buyer's pouch),
	// food, light and the rare piece stay vanilla items. Components wait for F6's single-part roll and services for a service UI:
	// those rows are COUNTED (rows_pending_*), never dropped silently.
	void realize(Stat* st, const trade::StockRow& r)
	{
		if ( r.cls == "provision" || r.cls == "light" ) { addItem(st, r.id, r.units, EXCELLENT, true); return; }
		if ( r.cls == "rare" ) { addItem(st, r.id, 1, SERVICABLE, true); return; }
		if ( r.cls == "material" || r.cls == "reagent" )
		{
			int mi = apxPartsMaterialIndex(r.id.c_str());
			Item* b = mi >= 0 ? apxPartsMakeBundle(mi, r.units) : nullptr;
			if ( b ) { attach(st, b); ++counters.rowsRealized; ++counters.bundles; return; }
			++counters.rowsPendingPouch; return;
		}
		if ( r.cls == "service" ) { ++counters.rowsPendingService; return; }
		++counters.rowsPendingPouch;   // component rows
	}
}

	int typeByName(const char* n) { return n ? typeByNameImpl_(n) : -1; }
	const char* typeName(int t) { return (t >= 0 && t < NUMITEMS) ? TYPE_NAMES[t] : ""; }

	bool on()
	{
		static int cached = -1;
		if ( cached < 0 ) { const char* e = std::getenv("APX_TRADE"); cached = (e && e[0] == '0') ? 0 : 1; }
		return cached == 1;
	}
	void markHaven(Entity* m) { if ( m ) { havenUids.insert(m->getUID()); } }
	bool isPeddler(const Entity* e) { return e && peddlerUids.count(e->getUID()) > 0; }
	void setDiscount(const char* t, int pct) { discountPct[t ? t : ""] = std::max(0, std::min(90, pct)); }
	void floorChanged() { floorKey = -1; }   // uids are never reused, so the haven and peddler sets need no clearing

	bool initShop(Entity* my, Stat* st, int shopLevel)
	{
		if ( !on() || !my || !st || my->monsterStoreType == 10 ) { return false; }
		const bool hamlet = isHamlet();
		const bool peddler = peddlerUids.count(my->getUID()) > 0;
		const bool haven = havenUids.count(my->getUID()) > 0;
		const int key = currentlevel * 1000003 + (int)mapseed;
		if ( key != floorKey )
		{
			floorKey = key; floorGeneralTaken = false;
			bool allowed = strncmp(map.name, "Underworld", 10) && strncmp(map.name, "Hell", 4) && strncmp(map.filename, "fortress", 8);
			trade::FloorShops fs = trade::floorShops(mapseed, currentlevel, prevHaven, allowed);
			floorHaven = fs.haven;
			prevHaven = fs.haven;
		}
		const trade::TradeRow* t = nullptr;
		if ( peddler ) { t = trade::findTrade("provisioner"); }
		else if ( hamlet ) { t = trade::findTrade(tradeForStoreType(my->monsterStoreType)); }
		else if ( haven ) { t = trade::findTrade(floorHaven); }
		else { t = trade::findTrade("general"); }
		if ( !t ) { return false; }
		std::vector<trade::StockRow> rows;
		if ( peddler ) { rows = trade::peddlerStock(mapseed, currentlevel); }
		else { rows = trade::buildStock(*t, mapseed ^ (hamlet ? (Uint32)(my->getUID() * 2654435761u) : 0u), currentlevel); }
		for ( auto& r : rows ) { realize(st, r); }
		if ( !hamlet ) { my->monsterStoreType = t->storeType; }   // the Hamlet keeps its vanilla shop names
		int purse = trade::purseFor(currentlevel, *t) * (hamlet ? 3 : 1) / (peddler ? 2 : 1);
		st->GOLD = purse;
		++counters.shops; if ( hamlet ) { ++counters.hamlet; }
		printlog("[apx-trade] shop uid=%u trade=%s role=%s floor=%d rows=%d realized_total=%d pending_pouch=%d pending_service=%d purse=%d",
			(unsigned)my->getUID(), t->id.c_str(), peddler ? "peddler" : hamlet ? "hamlet" : haven ? "haven" : "general", currentlevel, (int)rows.size(),
			counters.rowsRealized, counters.rowsPendingPouch, counters.rowsPendingService, purse);
		(void)shopLevel;
		return true;
	}

	int buyValue(const Item* item, int player, int vanilla)
	{
		if ( !on() || !item || !tradeOfPlayerShop(player) ) { return vanilla; }
		double v = vanilla;
		if ( apxPartsIsBundle(item) ) { return std::max(1, (int)std::lround(apxPartsBundleValue(item) * (double)trade::kLotMarkup)); }   // a lot is one of a kind: no stack factor
		if ( !isHamlet() ) { v *= trade::priceFactor(kDemandRef, std::max(1, (int)item->count)); }
		if ( !item->playerSoldItemToShop )
		{
			for ( auto& r : trade::kRareItems ) { int ti = typeByNameImpl_(r); if ( ti == (int)item->type ) { v *= trade::kRareMarkup; break; } }
		}
		const trade::TradeRow* t = tradeOfPlayerShop(player);
		auto d = discountPct.find(t->id);
		if ( d != discountPct.end() ) { v = v * (100 - d->second) / 100.0; }
		return std::max(1, (int)std::lround(v));
	}
	int sellValue(const Item* item, int player, int vanilla)
	{
		if ( !on() || !item || player < 0 || !tradeOfPlayerShop(player) ) { return vanilla; }
		if ( apxPartsIsBundle(item) ) { return trade::offerFor(std::max(1, (int)std::lround(apxPartsBundleValue(item) * (double)trade::kLotMarkup))); }
		return trade::offerFor(vanilla);   // about a third of what the item is worth (D73 rule 3)
	}
	bool sellable(int player, const Item* item)
	{
		const trade::TradeRow* t = tradeOfPlayerShop(player);
		if ( !on() || !item || !t ) { return true; }   // caller keeps the vanilla answer
		int idx = (int)item->type;
		std::string cls = trade::classOfItem(idx);
		if ( apxPartsIsBundle(item) || apxPartsIsComponent(item) )   // the carrier type is GEM_GLASS: class by the material inside
		{
			int mat = apxPartsIsBundle(item) ? apxPartsBundleMaterial(item) : (int)item->apx.parts[0].material;
			if ( mat >= 0 && mat < (int)trade::kMats.size() ) { cls = trade::kMats[mat].cat; }
		}
		bool ok = trade::canBuyClass(*t, cls);
		if ( !ok ) { ++counters.classRefused; }
		return ok;
	}
	bool purseRefuses(int player, const Item* item)
	{
		if ( !on() || !item || multiplayer == CLIENT || !tradeOfPlayerShop(player) ) { return false; }
		Entity* shop = uidToEntity(shopkeeper[player]);
		if ( !shop || !shop->getStats() ) { return false; }
		if ( shop->getStats()->GOLD < item->sellValue(player) ) { ++counters.purseRefused; return true; }
		return false;
	}

	Entity* spawnPeddler(double xt, double yt)
	{
		if ( multiplayer == CLIENT || xt < 1 || yt < 1 || xt >= map.width - 1 || yt >= map.height - 1 ) { return nullptr; }
		// flag first: initShopkeeper runs on the entity's first behaviour tick, after summonMonster returns
		Entity* e = summonMonster(SHOPKEEPER, (long)(xt * 16.0), (long)(yt * 16.0), false);
		if ( !e ) { return nullptr; }
		peddlerUids.insert(e->getUID());
		++counters.peddlers;
		return e;
	}
	void logSummary()
	{
		printlog("[apx-trade] summary shops=%d hamlet=%d rows_realized=%d pending_pouch=%d pending_service=%d class_refused=%d purse_refused=%d peddlers=%d",
			counters.shops, counters.hamlet, counters.rowsRealized, counters.rowsPendingPouch, counters.rowsPendingService,
			counters.classRefused, counters.purseRefused, counters.peddlers);
	}
} // namespace tradeglue
} // namespace apx
