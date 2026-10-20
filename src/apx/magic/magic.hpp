// apx/magic/magic.hpp -- apx::magic, our own player spell system (spec 04b section 7, D93 "Rewrite casting now"; phase S1).
//
// Replaces Barony's player casting path: castSpellInit's mana check, the spellcasting animation manager's circle/throw stages, the
// per-frame hand pose and the mana drip. Barony code that stays: the spell tables (spell_t, spellbooks teach), its rangefinder as the
// AIM SOLVER (cast_animation[p].setRangeFinderLocation, read-only), and the named effect functions behind the vanilla spells that are
// not four-pick spells (castSpell in trap mode: no animation, no mana, no skill-up there; this module did all three).
//
//   bool beginCast(int player, SpellRef)   from castSpellInit (every hotbar, quick-cast, spellbook and tome route goes there)
//   void tick(int player)                  once per 50 Hz sim tick from actPlayer: cast time, concentration, release, hands state
//   void frame()                           once per game tick: stamps, pending emissions, projectile cost sample, periodic summary
//   bool poseHand(Entity*, bool left, int) the first-person cast hands (called from actLeft/RightHandMagic; true = pose written)
//
// Pipeline: choose -> aim solve -> contradiction grade (table) -> mana = base x area x affinity x factor (halved near a matching element) ->
// concentration check (fail under 20 percent, no mana taken) -> cast time (verb x modifier, stretched as concentration falls) with the
// ground sigil telegraph and cast hands -> release: bolts through apx::proj, area stamps on tiles, self verbs through status effects,
// vanilla non-pick spells through Barony's effect function. Contradictions: HARD backfires on the caster at 25 percent (else fizzles),
// SOFT halves strength. The cast log (every stage, one line) is in memory, in the "[apx-magic]" log, and in APX_MAGIC_LOG=<dir>/magic.jsonl.
//
// Data: data/apx/magic/magic.json (tables), data/apx/magic/spells.json (the 223 vanilla rows; tools/apx/gen_spells.py). APX_MAGIC_DATA=<dir>.
// Knob: APX_MAGIC_OLD=1 runs Barony's casting (the CONTROL).
#pragma once
#include "magic_core.hpp"
#include <string>

class Entity;
struct spell_t;

namespace apx
{
namespace magic
{
	struct SpellRef
	{
		spell_t* spell = nullptr;        // a vanilla spell (a row of spells.json), or
		bool book = false, tome = false;
		bool hasPicks = false;           // free picks (a wand, the console, the gate)
		core::Picks picks;
	};

	bool active();                       // false when APX_MAGIC_OLD=1
	bool beginCast(int player, const SpellRef& ref);
	bool casting(int player);
	void tick(int player);
	void frame();
	bool poseHand(Entity* hand, bool left, int player);
	void clearLevel();                   // level change: stamps and pending emissions

	// ---- instruments ---------------------------------------------------------------------------------------
	struct Counters
	{
		long long begun = 0, released = 0, failedMana = 0, failedConc = 0, failedAim = 0, interrupted = 0, fizzled = 0, backfired = 0, softHalved = 0;
		long long boltsNative = 0, areas = 0, selfs = 0, legacyCalls = 0, stampHits = 0, echoes = 0, chains = 0, triggers = 0;
		double manaSpent = 0.0;
	};
	const Counters& counters();
	float concentration(int player);                 // percent
	void setConcentration(int player, float pct);    // gate set-up
	int costOf(const core::Picks& p, bool nearbyElement);   // the engine's table (magic.json)
	const core::CostTable& costTable();
	int rowCount();
	bool rowInfo(int index, int* id, std::string* name, core::Picks* p, int* deliver);   // deliver: 0 native proj, 1 legacy
	void log(const char* fmt, ...);                  // "[apx-magic]" + the in-memory cast log
	std::string castLogTail(int n);
}
}
