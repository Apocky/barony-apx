// apx/wild_enmity.hpp -- who fights whom at the floor start and after (Apocky, 2026-10-19: "curb creature on creature
// fighting unless it makes sense, otherwise creatures will clear themselves out before the player can even initiate
// combat"). Two rules, both in Entity::checkEnemy (entity.cpp):
//
// 1. LOAD GRACE. Until the player has fully loaded in (the level's fade-in is done: fadealpha 0, not fading out, the player
//    entity exists) plus kGraceTicks (ASSUMED 3 s), no monster counts anything as an enemy: no creature targets the player
//    and no creature fights another. "[apx-loadgrace] ended" marks the moment. Knob APX_LOAD_GRACE=0 (the control).
//    Before: creatures woke and fought while the screen was still fading in (QA s1: the brawl started 1 s into the floor).
//
// 2. WILD ENMITY. Two wild creatures (actMonster, no leader) fight only when it makes sense: their pair is in the explicit
//    table apx/wild_hostility.inc (monsters vs humans, obvious enemies, predator and prey; a reason per row), or one provoked the other (a
//    blow landed between them: friendly fire in a crowd, a sweep), or one belongs to a director-scripted event (tagged
//    by the director), or one is a shopkeeper defending its shop. Followers and players keep Barony's tables. Vanilla
//    Barony's swornenemies table alone made every rat and skeleton on a floor fight on sight; the D72 packs put them in
//    rooms side by side, and some 30 creatures brawled at the start (QA s1 seed 4242). A shopkeeper defends only its shop
//    (kShopRadius tiles of where it first stood) unless provoked. Knob APX_WILD_ENMITY=vanilla brings
//    the table back for wild creatures (the control: it reproduces the brawl). APX_SPAWN_LEGACY=1 turns both rules off.
#pragma once

#include "../main.hpp"
#include "../monster.hpp"
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <unordered_map>
#include <unordered_set>

namespace apx
{
namespace wild
{
	constexpr int kGraceTicks = 3 * 50; // ASSUMED: 3 s after the fade-in ends
	constexpr int kShopRadius = 5;      // ASSUMED: tiles around where a shopkeeper first stood (its shop)

	struct Counters
	{
		long long graceRefusals = 0;   // enemy checks refused during the load grace
		long long wildBlocked = 0;     // wild-on-wild enemy checks refused (the vanilla table said enemy, nothing provoked it)
		long long provokedAllowed = 0; // wild-on-wild allowed: provoked
		long long eventAllowed = 0;    // wild-on-wild allowed: a director event creature
		long long provocations = 0;    // blows that provoked a pair
		long long tableAllowed = 0;    // wild-on-wild allowed: the pair is in the hostility table
		long long shopDefence = 0;     // a shopkeeper turned on a creature inside its shop
		long long shopIgnored = 0;     // a shopkeeper let a creature outside its shop be
	};
	inline Counters& C() { static Counters c; return c; }

	inline bool envOff(const char* name)
	{
		const char* v = std::getenv(name);
		return v && v[0] == '0';
	}
	inline bool graceOn() { static const bool on = !envOff("APX_LOAD_GRACE"); return on; }
	inline bool wildRuleOn()
	{
		static const bool on = [] { const char* v = std::getenv("APX_WILD_ENMITY"); return !(v && !std::strcmp(v, "vanilla")); }();
		return on;
	}

	// ---- load grace state (updated once per game tick from gameLogic)
	struct Grace
	{
		uint32_t lastTicks = 0;
		bool loaded = false, ended = false;
		uint32_t loadedAt = 0, endedAt = 0;
		int floor = -1;
	};
	inline Grace& G() { static Grace g; return g; }

	// provocations: a creature's uid -> the uids it may fight (cleared with the floor)
	inline std::unordered_map<uint32_t, std::unordered_set<uint32_t>>& provoked()
	{
		static std::unordered_map<uint32_t, std::unordered_set<uint32_t>> m;
		return m;
	}
	inline void provoke(uint32_t a, uint32_t b)
	{
		if ( !a || !b || a == b ) return;
		auto& m = provoked();
		if ( m[a].insert(b).second ) ++C().provocations;
		m[b].insert(a);
	}
	inline bool isProvoked(uint32_t a, uint32_t b)
	{
		auto& m = provoked();
		auto it = m.find(a);
		return it != m.end() && it->second.count(b) > 0;
	}
	// a shopkeeper's home: where it stood at its first enemy check (cleared with the floor)
	inline std::unordered_map<uint32_t, std::pair<double, double>>& homes() { static std::unordered_map<uint32_t, std::pair<double, double>> m; return m; }
	inline std::pair<double, double> home(uint32_t uid, double x, double y)
	{
		auto it = homes().find(uid);
		if ( it == homes().end() ) it = homes().emplace(uid, std::make_pair(x, y)).first;
		return it->second;
	}
	// ---- the explicit hostility table
	struct Pair { int a, b; const char* cls; const char* reason; };
	inline const Pair* table(int& n)
	{
		static const Pair rows[] = {
#include "wild_hostility.inc"
		};
		n = (int)(sizeof(rows) / sizeof(rows[0]));
		return rows;
	}
	// the row for a pair (either order), else nullptr; each pair's first use is logged with its reason
	inline const Pair* hostile(int a, int b)
	{
		int n = 0;
		const Pair* rows = table(n);
		for ( int i = 0; i < n; ++i )
		{
			if ( (rows[i].a == a && rows[i].b == b) || (rows[i].a == b && rows[i].b == a) )
			{
				static std::unordered_set<int> logged;
				if ( logged.insert(i).second ) printlog("[apx-wild] pair %s-%s hostile (%s): %s", monstertypename[rows[i].a], monstertypename[rows[i].b], rows[i].cls, rows[i].reason);
				return &rows[i];
			}
		}
		return nullptr;
	}
	// writes the table to <run folder>/wild_hostility.txt once
	inline void dumpTable()
	{
		static bool done = false;
		if ( done ) return;
		done = true;
		int n = 0;
		const Pair* rows = table(n);
		if ( FILE* f = std::fopen("wild_hostility.txt", "w") )
		{
			std::fprintf(f, "# apx wild hostility table (src/apx/wild_hostility.inc): wild pairs not listed are neutral unless provoked or in a director event\n");
			std::fprintf(f, "# rule on=%d (APX_WILD_ENMITY=vanilla: Barony's swornenemies table instead)\n", (int)wildRuleOn());
			for ( int i = 0; i < n; ++i ) std::fprintf(f, "%s\t%s\t%s\t%s\n", monstertypename[rows[i].a], monstertypename[rows[i].b], rows[i].cls, rows[i].reason);
			std::fclose(f);
		}
		printlog("[apx-wild] hostility table: %d pairs written to wild_hostility.txt (rule on=%d)", n, (int)wildRuleOn());
	}
} // namespace wild
} // namespace apx
