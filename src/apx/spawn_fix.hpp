// apx/spawn_fix.hpp -- the floor-start death fixes (QA s1 seed 4242, s2 seed 777; tools/apx/spawn_gate.py):
//   1. D72 placement: a group's members are drawn from kinds that are not sworn enemies of each other (a fodder
//      swarm drew rat AND skeleton per member: sworn enemies in one den brawled at once). apx_gen_bridge.cpp.
//   2. The director's ambush pack: same rule (core.build_pack takes a compatibility test), and each spawn is
//      tagged with the tier of its director cost (1 fodder, 3 regular, 10 elite) so D74 reads it (untagged
//      kobolds fell back to the MAXHP tier: regular, vanilla damage). director.lua, lua_layer.cpp api_spawn.
//   3. Creature hits on players: the BV1 variant multiple applies before the D74 tier rule (it came after and
//      lifted fodder over the cap); after F4c's zone and stagger multipliers the fodder depth cap still holds,
//      and a blow below elite tier deals at most a third of the player's max HP (head 2.5x on 31 = 78 on a
//      40 HP character was a one-shot). entity.cpp, apx_combat.hpp.
//   4. Downed: a solo player gets back up after the revive time (no ally can channel; hits used to restart
//      nothing and the bleed-out was certain: a slow death loop); a bleed-out names the last creature that
//      hit the player as the killer (was "dies randomly"). apx_combat.hpp.
// Every fix counts in apx::spawnfix::C and prints "[apx-spawnfix]" rows. APX_SPAWN_LEGACY=1 brings every old
// behaviour back (the gate's control run, which must fail).
#pragma once

#include <cstdlib>

namespace apx
{
namespace spawnfix
{
	inline bool legacy()
	{
		static const int on = [] { const char* v = std::getenv("APX_SPAWN_LEGACY"); return (v && v[0] && v[0] != '0') ? 1 : 0; }();
		return on != 0;
	}
	struct Counters
	{
		long long d72Redraws = 0;      // a member's kind redrawn: the first draw was a sworn enemy of its group
		long long d72Fallbacks = 0;    // no compatible kind in the tier: the group's anchor kind used
		long long d72ArrivalMoves = 0;  // groups moved out of the arrival radius
		long long directorTags = 0;    // director spawns tagged with their cost tier
		long long fodderCapAfterZone = 0, playerBlowClamps = 0, variantBeforeTier = 0;
		long long soloRecovers = 0, killersNamed = 0, obituariesKept = 0;
	};
	inline Counters& C() { static Counters c; return c; }
} // namespace spawnfix
} // namespace apx
