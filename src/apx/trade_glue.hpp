// trade_glue.hpp -- BT1 trade, engine side (card BT1, D73). The rules are src/apx/trade_rules.hpp (pure, unit-tested).
// Knob: APX_TRADE=0 restores vanilla shops byte for byte (stock, prices, who buys what). Default ON.
#pragma once

class Entity;
class Item;
class Stat;

namespace apx
{
namespace tradeglue
{
	struct Counters
	{
		int bundles = 0, shops = 0, hamlet = 0, rowsRealized = 0, rowsPendingPouch = 0, rowsPendingService = 0;
		int classRefused = 0, purseRefused = 0, sold = 0, soldGold = 0, peddlers = 0;
	};
	extern Counters counters;

	bool on();                                    // APX_TRADE is not "0"
	void markHaven(Entity* merchant);             // apx-gen bridge: this shopkeeper is the floor's haven merchant
	// initShopkeeper: replaces the vanilla stock (returns true when it did). Sets the store type, stock and purse.
	bool initShop(Entity* my, Stat* myStats, int shopLevel);
	// shop window / sell and buy paths (host and client compute the same numbers from the item and the shop type)
	int buyValue(const Item* item, int player, int vanilla);
	int sellValue(const Item* item, int player, int vanilla);
	bool sellable(int player, const Item* item);   // own trade only (class by main material, via form_map)
	bool purseRefuses(int player, const Item* item); // host/single player: the offer exceeds what the merchant has left
	// peddlers (director event): a small stock, attackable, rewards a rescue
	Entity* spawnPeddler(double xTiles, double yTiles);
	bool isPeddler(const Entity* e);
	void setDiscount(const char* trade, int pct);   // host-side token earned by a rescue (D73 rule 6)
	int typeByName(const char* name);              // ItemType index or -1
	const char* typeName(int type);
	void floorChanged();                           // resets per-floor state (haven set, peddler set)
	void logSummary();                             // one line for the log / tests
} // namespace tradeglue
} // namespace apx
