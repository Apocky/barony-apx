// apx/hostility.hpp -- who fights whom (spec 04b s3, s6; D93). Apocky, 2026-10-19: "Skeletons fighting humans makes sense,
// monsters fighting non-monsters makes sense, creatures fighting obvious enemies or predator-prey relationships are fine, but
// not inter-entity combat for its own sake." and "why is there so much combat and chaos at the start before the player even
// fully loads in".
//
//   bool hostile(Entity* a, Entity* b, Reason* why)   does `a` regard `b` as a foe (the AI's target choice asks this)
//
// Creature vs creature is decided here and nowhere else (Entity::checkEnemy hands them to hook()); Barony's swornenemies table
// is not consulted. Rules, in order: calm start (nobody targets anyone until the player has control + calm_seconds, logged),
// provocation (a creature hit by another regards the hitter as a foe for provocation_seconds), followers (they fight what their
// leader fights), shopkeepers (defend a few tiles around where they stood), director events (apx.set_hostile / set_target),
// the table data/apx/combat/hostility.json (a reason per pair); anything else is neutral. Pairs that involve a player after
// leader resolution are left to Barony's player rules (hook() reports handled=false). APX_HOSTILITY_OLD=1: vanilla (control).
// APX_HOSTILITY_GATE=<s>: the player stands still s seconds from load-in; one "[apx-hostility] gate" summary row, then quit.
#pragma once
#include <cstdint>

class Entity;

namespace apx
{
namespace hostility
{
	struct Reason
	{
		const char* cls = "neutral"; // living, enemy, prey, provoked, event, shop, calm, follower, neutral, vanilla
		const char* text = "";
	};
	bool legacy();                                  // APX_HOSTILITY_OLD=1
	bool hostile(Entity* a, Entity* b, Reason* why);
	bool hook(Entity* a, Entity* b, bool* handled); // Entity::checkEnemy
	void onHit(Entity* attacker, Entity* target, int dmg); // provocation + gate counters (from apx::combat_glue::applyHit)
	void eventSet(uint32_t a, uint32_t b, const char* reason); // a director event sets a pair, logged
	void frame();                                   // once per game tick (gameLogic): floor reset, calm start, gate
	bool calmOver();
}
}
