// U1 pure-logic units (T1, offline, no Barony). Build: tools/apx/tests/run_u1_logic.ps1.
// Exit 0 when every case passes; --control flips one expectation and must exit 1.
#include "../../../src/apx_inv_logic.hpp"
#include <cstdio>
#include <cstring>
#include <random>

using namespace apx_logic;
static int g_fail = 0, g_cases = 0;
#define CHECK(c) do { ++g_cases; if ( !(c) ) { ++g_fail; std::printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #c); } } while (0)

static Row row(uint32_t uid, int kind, const char* name, int32_t mass)
{
	Row r; r.uid = uid; r.type = 1; r.kind = kind; r.name = name; r.mass = mass; r.count = 1; r.status = 3;
	return r;
}

int main(int argc, char** argv)
{
	const bool control = argc > 1 && !std::strcmp(argv[1], "--control");

	// entries: headers per kind, rows by name
	std::vector<Row> rs = { row(1, TOOLS, "lockpick", 1), row(2, WEAPONS, "sword", 30), row(3, WEAPONS, "axe", 40), row(4, BOOKS, "tome", 5) };
	auto e = entries(rs);
	CHECK(e.size() == 7);
	CHECK(e[0].header && e[0].row.kind == WEAPONS);
	CHECK(!e[1].header && e[1].row.name == "axe");
	CHECK(e[2].row.name == "sword");
	CHECK(e[3].header && e[3].row.kind == TOOLS);
	CHECK(e[5].header && e[5].row.kind == BOOKS);

	// navigation skips headers both ways and clamps
	CHECK(move(e, 0, 0) == 1);
	CHECK(move(e, 1, 1) == 2);
	CHECK(move(e, 2, 1) == 4);   // over the Tools header
	CHECK(move(e, 4, -1) == 2);
	CHECK(move(e, 6, 5) == 6);
	CHECK(move(e, 1, -3) == 1);
	CHECK(move(e, 2, 3) == 6);
	CHECK(move({}, 0, 1) == 0);

	// mass: 1,000 random row sets sum exactly; one wrong row is caught every time
	std::mt19937 rng(7);
	int sumOk = 0, caught = 0;
	for ( int t = 0; t < 1000; ++t )
	{
		std::vector<Row> v;
		long long carried = 0;
		int n = 1 + rng() % 120;
		for ( int i = 0; i < n; ++i ) { auto r = row(i + 1, rng() % KIND_MAX, "x", rng() % 2000); carried += r.mass; v.push_back(r); }
		sumOk += checkRows(v, carried).empty();
		v[rng() % v.size()].mass += 1;
		caught += !checkRows(v, carried).empty();
	}
	CHECK(sumOk == 1000);
	CHECK(caught == 1000);

	// transfer rule
	CHECK(canTake(100, 20, 120));
	CHECK(!canTake(100, 21, 120));
	CHECK(canTake(0, 0, 0));

	// mass text
	CHECK(massText(123) == "12.3 kg");
	CHECK(massText(5) == "0.5 kg");

	// stations
	CHECK(stationOfModel(1617) == WORKBENCH);
	CHECK(stationOfModel(1621) == ANVIL);
	CHECK(stationOfModel(1622) == CRUCIBLE);
	CHECK(stationOfModel(0) == NONE);
	CHECK(stationOfModel(1618) == NONE);
	CHECK(stationOps(WORKBENCH).size() == 2);
	CHECK(stationOps(NONE).empty());
	for ( int s = FORGE; s < STATION_MAX; ++s ) { CHECK(!stationOps(s).empty()); }

	// inspect
	Row r = row(9, WEAPONS, "iron sword", 25);
	r.beatitude = 2; r.identified = false;
	auto l = inspectLines(r);
	CHECK(l[0] == "iron sword");
	CHECK(l[2] == "Mass: 2.5 kg");
	CHECK(l[3] == "Condition: serviceable");
	CHECK(l[4] == "Beatitude: unknown");   // never leaks an unidentified beatitude
	r.identified = true;
	CHECK(inspectLines(r)[4] == "Beatitude: +2");

	// D81: tabs, column sort inside kind groups, compare deltas
	{
		std::vector<Row> v = { row(1, WEAPONS, "sword", 30), row(2, WEAPONS, "axe", 40), row(3, ARMOUR, "helm", 20), row(4, WEAPONS, "dagger", 5) };
		v[0].power = 7; v[0].powerKind = 'D'; v[0].value = 50; v[0].status = 4;
		v[1].power = 9; v[1].powerKind = 'D'; v[1].value = 30; v[1].status = 1;
		v[3].power = 3; v[3].powerKind = 'D'; v[3].value = 10; v[3].status = 2;
		v[2].power = 2; v[2].powerKind = 'A';
		auto all = entriesSorted(v, 0, COL_NAME, false);
		CHECK(all.size() == 6 && all[1].row.name == "axe" && all[3].row.name == "sword" && all[4].header);
		auto weap = entriesSorted(v, 1 + WEAPONS, COL_WEIGHT, true);
		CHECK(weap.size() == 4 && weap[1].row.name == "axe" && weap[3].row.name == "dagger");
		auto byVal = entriesSorted(v, 1 + WEAPONS, COL_VALUE, true);
		CHECK(byVal[1].row.name == "sword");
		auto byPow = entriesSorted(v, 1 + WEAPONS, COL_POWER, false);
		CHECK(byPow[1].row.name == "dagger" && byPow[3].row.name == "axe");
		auto byCond = entriesSorted(v, 1 + WEAPONS, COL_CONDITION, true);
		CHECK(byCond[1].row.name == "sword");
		CHECK(entriesSorted(v, 1 + BOOKS, COL_NAME, false).empty());
		CHECK(tabCount() == KIND_MAX + 1 && std::string(tabName(0)) == "All" && std::string(tabName(1)) == "Weapons");
		CHECK(compareDelta(v[1], &v[0]) == "+2");
		CHECK(compareDelta(v[3], &v[0]) == "-4");
		CHECK(compareDelta(v[0], &v[0]) == "=");
		CHECK(compareDelta(v[0], nullptr) == "+7");
		CHECK(compareDelta(v[0], &v[2]) == "+7");    // armour is not compared with a weapon
		CHECK(compareDelta(row(9, TOOLS, "x", 1), nullptr).empty());
	}

	// control: one deliberately wrong expectation must fail the run
	if ( control ) { CHECK(stationOfModel(1617) == ANVIL); }

	std::printf("{\"test\":\"u1_logic\",\"cases\":%d,\"failed\":%d,\"control\":%s}\n", g_cases, g_fail, control ? "true" : "false");
	return g_fail ? 1 : 0;
}
