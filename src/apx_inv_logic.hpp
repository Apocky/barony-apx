// apx: pure logic of our inventory (slice U1, D57). No Barony includes: unit-tested offline by
// tools/apx/tests/u1_logic_test.cpp. The engine glue (apx_inventory*.cpp) fills these structs.
#pragma once
#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

namespace apx_logic
{
	enum Kind : int { WEAPONS, ARMOUR, TOOLS, CONSUMABLES, MATERIALS, BOOKS, GOLD, KIND_MAX };
	inline const char* kindName(int k)
	{
		static const char* n[KIND_MAX] = { "Weapons", "Armour", "Tools", "Consumables", "Materials", "Books", "Gold" };
		return k >= 0 && k < KIND_MAX ? n[k] : "?";
	}

	struct Row
	{
		uint32_t uid = 0; int type = -1; int kind = 0; int count = 0; int32_t mass = 0;
		int status = 0; int beatitude = 0; bool identified = true; bool equipped = false;
		int price = -1;   // shop rows only
		std::string name;
	};
	struct Entry { bool header = false; Row row; };

	// a header per kind, then that kind's rows sorted by name (stable)
	inline std::vector<Entry> entries(std::vector<Row> rows)
	{
		std::stable_sort(rows.begin(), rows.end(), [](const Row& a, const Row& b) {
			return a.kind != b.kind ? a.kind < b.kind : a.name < b.name; });
		std::vector<Entry> out;
		int kind = -1;
		for ( auto& r : rows )
		{
			if ( r.kind != kind )
			{
				kind = r.kind;
				Entry h; h.header = true; h.row.kind = kind; h.row.name = kindName(kind);
				out.push_back(h);
			}
			Entry e; e.row = r;
			out.push_back(e);
		}
		return out;
	}

	// move the selection by d rows, skipping headers; d == 0 snaps a header selection to the next row
	inline int move(const std::vector<Entry>& e, int sel, int d)
	{
		const int n = (int)e.size();
		if ( n == 0 ) { return 0; }
		auto hdr = [&](int i) { return i >= 0 && i < n && e[i].header; };
		sel = std::max(0, std::min(n - 1, sel));
		int step = d < 0 ? -1 : 1, k = d < 0 ? -d : d;
		if ( k == 0 && hdr(sel) ) { k = 1; step = 1; }
		for ( int i = 0; i < k; ++i )
		{
			int next = sel + step;
			while ( next >= 0 && next < n && hdr(next) ) { next += step; }
			if ( next < 0 || next >= n ) { break; }
			sel = next;
		}
		if ( hdr(sel) ) // only headers in that direction: look the other way
		{
			for ( int i = sel; i < n; ++i ) { if ( !hdr(i) ) { return i; } }
			for ( int i = sel; i >= 0; --i ) { if ( !hdr(i) ) { return i; } }
		}
		return sel;
	}

	inline long long massSum(const std::vector<Row>& r)
	{
		long long s = 0;
		for ( auto& x : r ) { s += x.mass; }
		return s;
	}
	inline std::string checkRows(const std::vector<Row>& r, long long carried)
	{
		long long s = massSum(r);
		return s == carried ? std::string() : "rows sum " + std::to_string(s) + " != carried " + std::to_string(carried);
	}

	// taking into the pack obeys the limit; giving away is always allowed
	inline bool canTake(long long carried, long long itemMass, long long limit) { return carried + itemMass <= limit; }

	inline std::string massText(long long units)
	{
		char b[32];
		snprintf(b, sizeof(b), "%lld.%lld kg", units / 10, (units < 0 ? -units : units) % 10);
		return b;
	}

	// ---------------------------------------------------------------- stations (step 5)
	enum Station : int { NONE, FORGE, ANVIL, CRUCIBLE, WORKBENCH, ALEMBIC, STATION_MAX };
	inline const char* stationName(int s)
	{
		static const char* n[STATION_MAX] = { "none", "Forge", "Anvil", "Crucible", "Workbench", "Alembic" };
		return s >= 0 && s < STATION_MAX ? n[s] : "none";
	}
	// decoration model index (models/models.txt line - 1) -> station; forge has no base-game model
	inline int stationOfModel(int model)
	{
		switch ( model )
		{
			case 1617: return WORKBENCH;  // models/decorations/workbench.vox
			case 1621: return ANVIL;      // models/decorations/divine_anvil.vox
			case 1622: return CRUCIBLE;   // models/decorations/cauldron.vox
			default: return NONE;
		}
	}
	// the operations a station offers (spec 08 bench: FORGE, ASSEMBLE, SMELT, ALLOY, ... names as in 08)
	inline std::vector<std::string> stationOps(int s)
	{
		switch ( s )
		{
			case FORGE: return { "Smelt", "Alloy" };
			case ANVIL: return { "Forge", "Repair" };
			case CRUCIBLE: return { "Melt", "Cast" };
			case WORKBENCH: return { "Assemble", "Salvage" };
			case ALEMBIC: return { "Distil", "Infuse" };
			default: return {};
		}
	}

	// ---------------------------------------------------------------- inspect (step 3)
	inline const char* conditionWord(int status)
	{
		static const char* w[] = { "broken", "decrepit", "worn", "serviceable", "excellent" };
		return status >= 0 && status <= 4 ? w[status] : "?";
	}
	inline std::vector<std::string> inspectLines(const Row& r)
	{
		std::vector<std::string> l;
		l.push_back(r.name);
		l.push_back(std::string("Kind: ") + kindName(r.kind));
		l.push_back("Mass: " + massText(r.mass) + (r.count > 1 ? " (" + std::to_string(r.count) + " pieces)" : ""));
		if ( r.type >= 0 )
		{
			l.push_back(std::string("Condition: ") + conditionWord(r.status));
			l.push_back(r.identified ? "Beatitude: " + std::string(r.beatitude > 0 ? "+" : "") + std::to_string(r.beatitude) : "Beatitude: unknown");
		}
		l.push_back("Form: known when F6 lands");
		return l;
	}
}
