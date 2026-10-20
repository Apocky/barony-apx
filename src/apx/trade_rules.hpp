// trade_rules.hpp -- BT1 pure trade rules (card BT1, D73). No Barony headers: unit-tested offline by
// tools/apx/trade_units_test.cpp. Tables are data/apx/trades.json (+ materials, forms, salvage_materials), embedded by
// tools/apx/gen_trade_inc.py. Every number is ASSUMED until the gold-per-floor gate (BT1-6).
#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <string>
#include <vector>

namespace apx
{
namespace trade
{
	struct MatRow { std::string id; int density, vpk; std::string cat; };      // density kg/m3, value per kg, category
	struct SlotRow { std::string id, role; int volume; };                      // volume cm^3
	struct NamedPrice { std::string id; int price; };
	struct SellGroup { std::string cls; std::vector<std::string> ids, roles, mats; int lo, hi; };
	struct TradeRow { std::string id, name; int storeType; double purseFactor; std::vector<std::string> buys; std::vector<SellGroup> sells; };

	// One line of a shop's stock. units = grams (material, reagent, component) or a count (service 1, provision, light, rare 1).
	// basePrice is for the whole row at neutral demand/supply; markup > 0 marks the rare finished piece (price = gold value x markup).
	struct StockRow { std::string cls, id, detail; int units = 0; int basePrice = 0; int markup = 0; int rarity = 0, affixes = 0; };   // rarity 0 Common .. 4 Legendary (5 Unique never sold); affixes = its affix slots (D86)

#include "trade_table.inc"
#include "trade_itemcls.inc"

	// ---- controls: each makes a gate fail on purpose (tests/units only) ----------------------------------------------
	struct Knobs { bool noClamp = false, buyAnything = false, vanillaStock = false, flatRarity = false; };
	inline Knobs& knobs() { static Knobs k; return k; }

	inline const TradeRow* findTrade(const std::string& id) { for ( auto& t : kTrades ) { if ( t.id == id ) { return &t; } } return nullptr; }
	inline const MatRow* findMat(const std::string& id) { for ( auto& m : kMats ) { if ( m.id == id ) { return &m; } } return nullptr; }

	// ---- price rule (D73): base x demand / supply, clamped 0.25-4 ----------------------------------------------------
	inline double priceFactor(int demand, int supply)
	{
		double r = (double)std::max(1, demand) / (double)std::max(1, supply);
		return knobs().noClamp ? r : std::min(4.0, std::max(0.25, r));
	}
	inline int unitPrice(int base, int demand, int supply) { return std::max(1, (int)std::lround(base * priceFactor(demand, supply))); }
	// demand = 1 + times the party bought this item on this floor; supply = units of it the shop now holds (>= 1)
	inline int marketPrice(int base, int bought, int stockCount) { return unitPrice(base, 1 + bought, std::max(1, stockCount)); }

	// ---- what a merchant pays: about 1/3, from a finite purse --------------------------------------------------------
	inline int offerFor(int sellPrice) { return std::max(1, sellPrice * kOfferPct / 100); }
	inline int purseFor(int depth, const TradeRow& t) { return (int)std::lround((kPurseBase + kPursePerDepth * std::max(0, depth)) * t.purseFactor); }
	inline bool canBuyClass(const TradeRow& t, const std::string& cls)
	{
		if ( knobs().buyAnything ) { return true; }
		return std::find(t.buys.begin(), t.buys.end(), cls) != t.buys.end();
	}
	// result: >0 gold offered (the purse is reduced), 0 = class refused, -1 = purse too small
	inline int sellToMerchant(const TradeRow& t, const std::string& cls, int sellPrice, int& purse)
	{
		if ( !canBuyClass(t, cls) ) { return 0; }
		int off = offerFor(sellPrice);
		if ( off > purse ) { return -1; }
		purse -= off;
		return off;
	}
	inline const char* classOfItem(int typeIndex)
	{
		if ( typeIndex < 0 || typeIndex >= (int)(sizeof(kItemClass) / sizeof(kItemClass[0])) ) { return "none"; }
		return kClassNames[kItemClass[typeIndex]];
	}

	// ---- seeded rng (splitmix64) ---------------------------------------------------------------------------------------
	struct Rng
	{
		uint64_t s;
		explicit Rng(uint64_t seed) : s(seed) {}
		uint64_t next() { uint64_t z = (s += 0x9E3779B97F4A7C15ull); z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull; z = (z ^ (z >> 27)) * 0x94D049BB133111EBull; return z ^ (z >> 31); }
		int below(int n) { return n <= 1 ? 0 : (int)(next() % (uint64_t)n); }
	};
	inline uint64_t stockSeed(uint32_t mapSeed, int floor, const std::string& tag)
	{
		uint64_t h = 1469598103934665603ull;
		auto mix = [&](uint64_t v) { for ( int i = 0; i < 8; ++i ) { h ^= (v >> (i * 8)) & 0xFF; h *= 1099511628211ull; } };
		mix(mapSeed); mix((uint64_t)floor);
		for ( char c : tag ) { h ^= (unsigned char)c; h *= 1099511628211ull; }
		return h;
	}

	// ---- stock ----------------------------------------------------------------------------------------------------------
	constexpr int kMinRows = 6, kMaxRows = 14, kHamletMaxRows = 60;
	inline bool hasRow(const std::vector<StockRow>& v, const std::string& cls, const std::string& id, const std::string& detail)
	{
		for ( auto& r : v ) { if ( r.cls == cls && r.id == id && r.detail == detail ) { return true; } }
		return false;
	}
	inline int lotBase(const MatRow& m, int grams) { return std::max(1, (int)((long long)grams * m.vpk * kLotMarkup / 1000)); }
	// ---- components (D86): first-class items with rarity and affixes; price from rarity (percent of the Common price) ------------
	constexpr int kRarityPct[6] = { 100, 160, 260, 450, 800, 1600 };
	inline int componentPrice(int commonPrice, int rarity)
	{
		if ( knobs().flatRarity ) { return std::max(1, commonPrice); }   // CONTROL: rarity ignored
		rarity = std::max(0, std::min(5, rarity));
		return std::max(1, (int)((long long)commonPrice * kRarityPct[rarity] / 100));
	}
	// rarity a shop's component is rolled at: deeper floors stock rarer ones; Unique (5) is never on a shelf
	inline int rollRarity(int depth, int draw /*0..99 from the rng*/)
	{
		int w[5] = { std::max(20, 60 - 4 * depth), 25, 10 + depth, 4 + depth / 2, 1 + depth / 4 };
		int sum = 0; for ( int x : w ) { sum += x; }
		int r = draw * sum / 100;
		for ( int k = 0; k < 5; ++k ) { if ( r < w[k] ) { return k; } r -= w[k]; }
		return 0;
	}
	// what a merchant pays for a component the player sells: a third of its price, only if its material class is the trade's
	inline int componentOffer(const TradeRow& t, const std::string& materialCategory, int commonPrice, int rarity, int& purse)
	{
		return sellToMerchant(t, materialCategory, componentPrice(commonPrice, rarity), purse);
	}
	inline int servicePrice(const std::string& id) { for ( auto& s : kServices ) { if ( s.id == id ) { return s.price; } } return 0; }

	inline void addGroup(std::vector<StockRow>& out, const SellGroup& g, Rng& rng, bool allowRare, int depth = 1)
	{
		static const int kGrams[4] = { 200, 400, 800, 1600 };
		int n = g.lo + rng.below(g.hi - g.lo + 1);
		if ( g.cls == "rare" )
		{
			if ( !allowRare || rng.below(100) >= kRareChancePct ) { return; }
			StockRow r; r.cls = "rare"; r.id = kRareItems[rng.below((int)kRareItems.size())]; r.units = 1; r.markup = kRareMarkup;
			out.push_back(r); return;
		}
		for ( int k = 0, guard = 0; k < n && guard < 50; ++guard )
		{
			StockRow r;
			if ( g.cls == "material" || g.cls == "reagent" )
			{
				if ( g.ids.empty() ) { return; }
				const std::string& id = g.ids[rng.below((int)g.ids.size())];
				const MatRow* m = findMat(id); if ( !m ) { continue; }
				int grams = kGrams[rng.below(4)];
				r.cls = g.cls; r.id = id; r.detail = m->cat; r.units = grams; r.basePrice = lotBase(*m, grams);
			}
			else if ( g.cls == "component" )
			{
				std::vector<const SlotRow*> pool;
				for ( auto& s : kSlots ) { if ( std::find(g.roles.begin(), g.roles.end(), s.role) != g.roles.end() ) { pool.push_back(&s); } }
				if ( pool.empty() || g.mats.empty() ) { return; }
				const SlotRow* s = pool[rng.below((int)pool.size())];
				const std::string& mid = g.mats[rng.below((int)g.mats.size())];
				const MatRow* m = findMat(mid); if ( !m ) { continue; }
				int grams = std::max(5, (int)((long long)s->volume * m->density / 1000));
				r.cls = "component"; r.id = s->id; r.detail = mid; r.units = grams;
				int common = std::max(1, lotBase(*m, grams) * kWorkPct / 100);
				r.rarity = rollRarity(depth, rng.below(100)); r.affixes = r.rarity;
				r.basePrice = componentPrice(common, r.rarity);
			}
			else if ( g.cls == "service" )
			{
				if ( g.ids.empty() ) { return; }
				const std::string& id = g.ids[rng.below((int)g.ids.size())];
				r.cls = "service"; r.id = id; r.units = 1; r.basePrice = servicePrice(id);
			}
			else if ( g.cls == "provision" || g.cls == "light" )
			{
				const std::vector<NamedPrice>& src = g.cls == "provision" ? kProvisions : kLights;
				const NamedPrice& p = src[rng.below((int)src.size())];
				r.cls = g.cls; r.id = p.id; r.units = 1 + rng.below(g.cls == "provision" ? 4 : 3); r.basePrice = p.price;
			}
			else { return; }
			if ( hasRow(out, r.cls, r.id, r.detail) ) { continue; }
			out.push_back(r); ++k;
		}
	}

	// A merchant's stock: 6..14 rows, no finished goods except at most one rare piece. Same inputs, same stock.
	inline std::vector<StockRow> buildStock(const TradeRow& t, uint32_t mapSeed, int floor)
	{
		std::vector<StockRow> out;
		if ( knobs().vanillaStock )   // CONTROL: vanilla shops stock finished goods; the no-finished-goods gate must fail on this
		{
			for ( int i = 0; i < 10; ++i ) { StockRow r; r.cls = "finished"; r.id = kRareItems[i % kRareItems.size()]; r.units = 1; r.basePrice = 50; out.push_back(r); }
			return out;
		}
		Rng rng(stockSeed(mapSeed, floor, t.id));
		for ( auto& g : t.sells ) { addGroup(out, g, rng, true, floor); }
		// pad to the minimum from the trade's own material groups (never from outside the trade)
		for ( int guard = 0; (int)out.size() < kMinRows && guard < 40; ++guard )
		{
			for ( auto& g : t.sells ) { if ( g.cls == "material" || g.cls == "reagent" ) { SellGroup one = g; one.lo = one.hi = 1; addGroup(out, one, rng, false, floor); break; } }
		}
		while ( (int)out.size() > kMaxRows ) { out.pop_back(); }
		return out;
	}
	// The Hamlet (band great haven): every trade's stock, plain prices (demand = supply is applied by the caller)
	inline std::vector<StockRow> buildHamletStock(uint32_t mapSeed, int floor)
	{
		std::vector<StockRow> out;
		for ( auto& t : kTrades )
		{
			if ( t.id == "general" ) { continue; }
			for ( auto& r : buildStock(t, mapSeed, floor) ) { if ( !hasRow(out, r.cls, r.id, r.detail) && (int)out.size() < kHamletMaxRows ) { out.push_back(r); } }
		}
		return out;
	}

	// ---- which shops a floor gets -------------------------------------------------------------------------------------
	struct FloorShops { std::string haven; bool general = false; };
	// one-trade haven merchant (never the same trade twice running); a general store on about half the floors from floor 2.
	// generalAllowed is false on the floors upstream excludes (Underworld, Hell, fortress).
	inline FloorShops floorShops(uint32_t mapSeed, int floor, const std::string& prev, bool generalAllowed)
	{
		Rng rng(stockSeed(mapSeed, floor, "floor"));
		std::vector<const TradeRow*> five;
		for ( auto& t : kTrades ) { if ( t.id != "general" && t.id != prev ) { five.push_back(&t); } }
		FloorShops f;
		f.haven = five[rng.below((int)five.size())]->id;
		f.general = floor >= 2 && generalAllowed && rng.below(2) == 0;
		return f;
	}

	// ---- peddlers (director relief event, D73 rule 4) ---------------------------------------------------------------------
	inline std::vector<StockRow> peddlerStock(uint32_t seed, int floor)
	{
		std::vector<StockRow> out;
		Rng rng(stockSeed(seed, floor, "peddler"));
		int want = 3 + rng.below(3);
		for ( int guard = 0; (int)out.size() < want && guard < 60; ++guard )
		{
			SellGroup g; g.lo = g.hi = 1;
			switch ( rng.below(4) ) { case 0: g.cls = "provision"; break; case 1: g.cls = "light"; break; case 2: g.cls = "material"; g.ids = { "iron", "wood", "hide", "linen", "coal", "salt" }; break; default: g.cls = "reagent"; g.ids = { "sulfur", "salt", "quartz" }; break; }
			addGroup(out, g, rng, false);
		}
		return out;
	}
	struct PeddlerReward { bool discount = false; int pct = 0; std::string trade; };
	inline PeddlerReward peddlerReward(uint32_t seed, int floor)
	{
		Rng rng(stockSeed(seed, floor, "reward"));
		PeddlerReward r;
		r.discount = rng.below(2) == 0;
		if ( r.discount ) { r.pct = 20; std::vector<const TradeRow*> five; for ( auto& t : kTrades ) { if ( t.id != "general" ) { five.push_back(&t); } } r.trade = five[rng.below((int)five.size())]->id; }
		return r;
	}
} // namespace trade
} // namespace apx
