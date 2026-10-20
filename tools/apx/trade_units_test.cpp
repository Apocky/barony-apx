// trade_units_test.cpp -- offline tests of src/apx/trade_rules.hpp and its embedded tables (card BT1, D73). Seconds, no Barony.
// Build: cmake -S tools/apx/units -B build/units && cmake --build build/units --config Release --target trade_units_test
// APX_TRADE_UNITS_BREAK=clamp|buyany|vanilla: the controls. Each must make at least one check fail (exit 1).
#include "apx/trade_rules.hpp"
#include <cstdio>
#include <cstdlib>
#include <map>
#include <random>
#include <set>

using namespace apx::trade;

static const char* const kTypeNames[] = {
#include "apx/parts_typenames.inc"
};
static int typeIndex(const char* n) { for ( int i = 0; i < (int)(sizeof(kTypeNames) / sizeof(kTypeNames[0])); ++i ) { if ( !std::strcmp(kTypeNames[i], n) ) { return i; } } return -1; }

static int fails = 0, checks = 0;
#define CHECK(c, ...) do { ++checks; if ( !(c) ) { ++fails; std::printf("FAIL line %d: ", __LINE__); std::printf(__VA_ARGS__); std::printf("\n"); } } while ( 0 )

static std::string hashStock(const std::vector<StockRow>& v)
{
	std::string s;
	for ( auto& r : v ) { s += r.cls + "/" + r.id + "/" + r.detail + "/" + std::to_string(r.units) + "/" + std::to_string(r.basePrice) + ";"; }
	return s;
}

int main()
{
	const char* b = std::getenv("APX_TRADE_UNITS_BREAK");
	std::string brk = b ? b : "";
	knobs().noClamp = brk == "clamp";
	knobs().buyAnything = brk == "buyany";
	knobs().vanillaStock = brk == "vanilla";

	// 1. price rule: clamp, monotone in demand, anti-monotone in supply
	{
		std::mt19937 rng(11);
		int outside = 0, notMono = 0;
		for ( int i = 0; i < 100000; ++i )
		{
			int base = 1 + rng() % 500, d = 1 + rng() % 400, s = 1 + rng() % 400;
			double f = priceFactor(d, s);
			if ( f < 0.25 - 1e-9 || f > 4.0 + 1e-9 ) { ++outside; }
			if ( unitPrice(base, d + 1, s) < unitPrice(base, d, s) ) { ++notMono; }
			if ( unitPrice(base, d, s + 1) > unitPrice(base, d, s) ) { ++notMono; }
		}
		CHECK(outside == 0, "price factor outside 0.25..4 in %d of 100000", outside);
		CHECK(notMono == 0, "price not monotone in %d cases", notMono);
		CHECK(marketPrice(100, 0, 1) == 100, "neutral market price is the base price, got %d", marketPrice(100, 0, 1));
		CHECK(marketPrice(100, 1000, 1) == 400, "heavy buying caps at 4x, got %d", marketPrice(100, 1000, 1));
		CHECK(marketPrice(100, 0, 1000) == 25, "oversupply floors at 0.25x, got %d", marketPrice(100, 0, 1000));
	}
	// 2. offer is about a third and never above it by more than 1; the purse is finite
	{
		int bad = 0;
		for ( int p = 1; p < 20000; ++p ) { if ( offerFor(p) > p / 3 + 1 || offerFor(p) < 1 ) { ++bad; } }
		CHECK(bad == 0, "offer above sell/3+1 or below 1 in %d prices", bad);
		const TradeRow* smith = findTrade("smith");
		CHECK(smith != nullptr, "smith exists");
		if ( smith )
		{
			CHECK(purseFor(1, *smith) < purseFor(9, *smith), "purse grows with depth");
			int purse = purseFor(2, *smith), paid = 0, refusedPurse = 0, n = 0;
			while ( n < 1000 ) { int r = sellToMerchant(*smith, "metal", 90, purse); if ( r > 0 ) { paid += r; } else if ( r == -1 ) { ++refusedPurse; break; } else { break; } ++n; }
			CHECK(refusedPurse == 1, "a finite purse must run out (paid %d)", paid);
			CHECK(paid <= purseFor(2, *smith), "paid %d more than the purse %d", paid, purseFor(2, *smith));
		}
	}
	// 3. merchants buy only their own trade
	{
		int leaks = 0, tested = 0;
		for ( auto& t : kTrades )
			for ( auto cn : { "metal", "wood", "hide", "fibre", "bone", "stone", "shard", "other", "provision", "reagent", "paper", "gem" } )
			{
				bool want = std::find(t.buys.begin(), t.buys.end(), cn) != t.buys.end();
				int purse = 1000000;
				int r = sellToMerchant(t, cn, 99, purse);
				++tested;
				if ( !want && r != 0 ) { ++leaks; }
				if ( want && r <= 0 ) { ++leaks; }
			}
		CHECK(leaks == 0, "%d of %d (trade, class) pairs wrong: out-of-trade bought or in-trade refused", leaks, tested);
		CHECK(std::string(classOfItem(typeIndex("IRON_SWORD"))) == "metal", "IRON_SWORD class %s", classOfItem(typeIndex("IRON_SWORD")));
		CHECK(std::string(classOfItem(typeIndex("FOOD_BREAD"))) == "provision", "FOOD_BREAD class");
		CHECK(std::string(classOfItem(typeIndex("WOODEN_SHIELD"))) == "wood", "WOODEN_SHIELD class %s", classOfItem(typeIndex("WOODEN_SHIELD")));
		CHECK(std::string(classOfItem(typeIndex("GEM_RUBY"))) == "gem", "GEM_RUBY class");
		int unclassified = 0; for ( int i = 0; i < (int)(sizeof(kTypeNames) / sizeof(kTypeNames[0])); ++i ) { if ( std::string(classOfItem(i)) == "none" ) { ++unclassified; } }
		CHECK(unclassified == 0, "%d item types without a class", unclassified);
		CHECK((int)(sizeof(kItemClass) / sizeof(kItemClass[0])) == (int)(sizeof(kTypeNames) / sizeof(kTypeNames[0])), "item class table covers all item types");
	}
	// 4. stock: 6..14 rows, no finished goods but at most one rare, food and light where promised, deterministic, varied
	{
		int sizeBad = 0, finished = 0, rareOver = 0, promiseBad = 0, nondet = 0, rows = 0;
		std::set<std::string> distinct; int builds = 0;
		for ( uint32_t seed = 1; seed <= 1000; ++seed )
			for ( int floor = 1; floor <= 10; ++floor )
				for ( auto& t : kTrades )
				{
					auto s = buildStock(t, seed * 2654435761u, floor);
					rows += (int)s.size();
					if ( (int)s.size() < kMinRows || (int)s.size() > kMaxRows ) { ++sizeBad; }
					int rare = 0, prov = 0, light = 0;
					for ( auto& r : s )
					{
						if ( r.cls == "rare" ) { ++rare; } else if ( r.cls == "finished" ) { ++finished; }
						if ( r.cls == "provision" ) { ++prov; } if ( r.cls == "light" ) { ++light; }
						if ( r.cls != "rare" && r.cls != "finished" && r.cls != "material" && r.cls != "reagent" && r.cls != "component" && r.cls != "service" && r.cls != "provision" && r.cls != "light" ) { ++finished; }
						if ( r.units <= 0 || (r.cls != "rare" && r.basePrice < 1) ) { ++sizeBad; }
					}
					if ( rare > 1 ) { ++rareOver; }
					if ( (t.id == "provisioner" || t.id == "general") && (prov < 1 || light < 1) ) { ++promiseBad; }
					if ( hashStock(s) != hashStock(buildStock(t, seed * 2654435761u, floor)) ) { ++nondet; }
					if ( floor == 3 && t.id == "smith" ) { distinct.insert(hashStock(s)); }
					++builds;
				}
		CHECK(sizeBad == 0, "%d stocks with a bad size or non-positive units/price", sizeBad);
		CHECK(finished == 0, "%d finished-goods rows (only one rare piece is allowed)", finished);
		CHECK(rareOver == 0, "%d shops with more than one rare piece", rareOver);
		CHECK(promiseBad == 0, "%d provisioner/general stocks without food or light", promiseBad);
		CHECK(nondet == 0, "%d stocks differ on a second build of the same inputs", nondet);
		CHECK(distinct.size() >= 950, "1000 smith seeds gave only %d distinct stocks", (int)distinct.size());
		std::printf("info: %d stocks, mean %.2f rows\n", builds, (double)rows / builds);
		auto hs = buildHamletStock(77, 3);
		std::set<std::string> cl; for ( auto& r : hs ) { cl.insert(r.cls); }
		CHECK(cl.count("service") && cl.count("provision") && cl.count("material") && cl.count("reagent") && cl.count("component"), "Hamlet misses a stock class");
		CHECK((int)hs.size() <= kHamletMaxRows && hs.size() > 20, "Hamlet size %d", (int)hs.size());
	}
	// 5. floor shops: general store rate 0.5 (95 percent half-width stop), never the same trade twice running, none on floor 1
	{
		const int N = 100000;
		int gen = 0, repeat = 0, g1 = 0, gDisallowed = 0;
		std::string prev;
		for ( int i = 0; i < N; ++i )
		{
			uint32_t seed = (uint32_t)(i * 2246822519u + 12345);
			int floor = 2 + (i % 9);
			FloorShops f = floorShops(seed, floor, prev, true);
			if ( f.general ) { ++gen; }
			if ( f.haven == prev ) { ++repeat; }
			if ( floorShops(seed, 1, prev, true).general ) { ++g1; }
			if ( floorShops(seed, floor, prev, false).general ) { ++gDisallowed; }
			prev = (i % 10 == 9) ? "" : f.haven;
		}
		double rate = (double)gen / N, half = 1.96 * std::sqrt(0.25 / N);
		CHECK(std::fabs(rate - 0.5) <= half + 0.002, "general store rate %.4f, half-width %.4f", rate, half);
		CHECK(repeat == 0, "%d floors repeated the previous haven trade", repeat);
		CHECK(g1 == 0, "%d floor-1 general stores", g1);
		CHECK(gDisallowed == 0, "%d general stores on excluded floors", gDisallowed);
		std::printf("info: general rate %.4f +- %.4f over %d floors\n", rate, half, N);
	}
	// 6. peddler
	{
		int sizeBad = 0, rareBad = 0, disc = 0, N = 10000;
		for ( int i = 0; i < N; ++i )
		{
			auto s = peddlerStock(i * 7919u, 1 + i % 9);
			if ( s.size() < 3 || s.size() > 5 ) { ++sizeBad; }
			for ( auto& r : s ) { if ( r.cls == "rare" || r.cls == "finished" || r.cls == "service" ) { ++rareBad; } }
			if ( peddlerReward(i * 7919u, 1 + i % 9).discount ) { ++disc; }
		}
		CHECK(sizeBad == 0, "%d peddler stocks outside 3..5 rows", sizeBad);
		CHECK(rareBad == 0, "%d peddler rows are finished goods or services", rareBad);
		CHECK(std::fabs((double)disc / N - 0.5) < 0.03, "peddler reward split %.3f", (double)disc / N);
	}

	std::printf("%s: %d checks, %d failed%s%s\n", fails == 0 ? "PASS" : "FAIL", checks, fails, brk.empty() ? "" : " (control: ", brk.empty() ? "" : (brk + ")").c_str());
	return fails == 0 ? 0 : 1;
}
