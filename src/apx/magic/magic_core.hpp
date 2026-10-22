// apx/magic/magic_core.hpp -- the pure rules of apx::magic (spec 04b section 7, 13-ALCHEMY-MAGIC section 5, 06 section 5.2).
// No engine headers: unit-tested offline by tools/apx/magic_test.cpp. Engine glue is magic.cpp.
//
//   four picks      VERB x SHAPE x ELEMENT x MODIFIER = 12 x 7 x 10 x 8 = 6,720 spells
//   mana            base(verb) x area(shape) x affinity(element) x factor(modifier), halved near a matching element
//   contradictions  a table: HARD backfires on the caster at 25 percent, SOFT halves strength; nothing is a hidden roll
//   concentration   a 0..100 percent pool: each cast spends some, it recovers with time, casts fail below 20 percent,
//                   cast time stretches as it falls; damage never drains it (04 section 4.2)
// The numbers live in data/apx/magic/magic.json; the defaults here are the same table (the offline test and the build with no
// data directory use them; the python gate recomputes the formula from the JSON, independently of this code).
#pragma once
#include <cmath>
#include <cstdint>

namespace apx
{
namespace magic
{
namespace core
{
	enum Verb : int { V_BOLT = 0, V_WALL, V_PIT, V_BURST, V_BEAM, V_WARD, V_LIFT, V_PULL, V_BIND, V_MEND, V_SENSE, V_VEIL, V_COUNT };
	enum Shape : int { S_POINT = 0, S_LINE, S_CONE, S_SPHERE, S_RING, S_PLANE, S_TRAIL, S_COUNT };
	enum Element : int { E_FIRE = 0, E_ICE, E_STONE, E_IRON, E_ACID, E_LIGHTNING, E_LIGHT, E_SHADOW, E_VOID, E_LIFE, E_COUNT };
	enum Modifier : int { M_NONE = 0, M_TRIGGER, M_TIMER, M_CHAIN, M_ECHO, M_SILENT, M_ANCHORED, M_INVERTED, M_COUNT };

	static const char* const kVerbNames[V_COUNT] = { "bolt", "wall", "pit", "burst", "beam", "ward", "lift", "pull", "bind", "mend", "sense", "veil" };
	static const char* const kShapeNames[S_COUNT] = { "point", "line", "cone", "sphere", "ring", "plane", "trail" };
	static const char* const kElementNames[E_COUNT] = { "fire", "ice", "stone", "iron", "acid", "lightning", "light", "shadow", "void", "life" };
	static const char* const kModifierNames[M_COUNT] = { "none", "trigger", "timer", "chain", "echo", "silent", "anchored", "inverted" };

	struct Picks
	{
		int verb = V_BOLT, shape = S_POINT, element = E_FIRE, modifier = M_NONE;
		bool valid() const
		{
			return verb >= 0 && verb < V_COUNT && shape >= 0 && shape < S_COUNT && element >= 0 && element < E_COUNT && modifier >= 0 && modifier < M_COUNT;
		}
	};

	// ---- mana ---------------------------------------------------------------------------------------------
	struct CostTable
	{
		float base[V_COUNT] = { 5.f, 8.f, 8.f, 9.f, 6.f, 7.f, 6.f, 6.f, 7.f, 6.f, 3.f, 5.f };
		float area[S_COUNT] = { 1.0f, 1.2f, 1.8f, 2.2f, 2.6f, 2.0f, 1.6f };
		float affinity[E_COUNT] = { 1.0f, 1.0f, 1.1f, 1.2f, 1.1f, 1.3f, 0.9f, 1.0f, 1.5f, 1.2f };
		float factor[M_COUNT] = { 1.0f, 1.4f, 1.2f, 1.8f, 1.7f, 1.1f, 1.3f, 1.0f };
		float nearby = 0.5f;           // multiplier when a matching element source is within nearbyTiles
		float nearbyTiles = 4.f;
	};
	inline float costFloat(const CostTable& t, const Picks& p, bool nearbyElement)
	{
		float c = t.base[p.verb] * t.area[p.shape] * t.affinity[p.element] * t.factor[p.modifier];
		return nearbyElement ? c * t.nearby : c;
	}
	// whole mana points, never below 1
	inline int costMana(const CostTable& t, const Picks& p, bool nearbyElement)
	{
		const int c = (int)std::ceil(costFloat(t, p, nearbyElement) - 1e-4f);
		return c < 1 ? 1 : c;
	}

	// ---- contradictions -----------------------------------------------------------------------------------
	enum Grade : int { G_NONE = 0, G_SOFT = 1, G_HARD = 2 };
	struct Rule { int verb, shape, element, modifier; Grade grade; const char* why; };   // -1 matches anything
	// The authored table (first match wins; HARD rows come first). 13-ALCHEMY-MAGIC section 6 gives the two examples:
	// a silent beam of lightning, and a verb whose element undoes it.
	static const Rule kDefaultRules[] = {
		// hard: the picks undo each other
		{ V_MEND,  -1, E_VOID,      -1,          G_HARD, "mend with void: nothing to mend" },
		{ V_MEND,  -1, E_ACID,      -1,          G_HARD, "mend with acid eats what it closes" },
		{ V_WARD,  -1, E_VOID,      -1,          G_HARD, "a ward of void has nothing to hold" },
		{ V_VEIL,  -1, E_LIGHT,     -1,          G_HARD, "a veil of light hides nothing" },
		{ V_BEAM,  -1, E_LIGHTNING, M_SILENT,    G_HARD, "lightning cannot be silent" },
		{ V_BOLT,  -1, E_LIGHTNING, M_SILENT,    G_HARD, "lightning cannot be silent" },
		{ V_BOLT,  S_PLANE, -1,     -1,          G_HARD, "a bolt is one body, a plane is a sheet" },
		{ V_BOLT,  S_RING,  -1,     M_ANCHORED,  G_HARD, "a ring of bolts cannot stay anchored" },
		{ V_WARD,  -1,  -1,         M_TRIGGER,   G_HARD, "a ward is never struck first: nothing to trigger on" },
		{ V_SENSE, -1,  -1,         M_TRIGGER,   G_HARD, "sensing does not hit anything" },
		{ V_WALL,  S_POINT, -1,     -1,          G_HARD, "a wall needs width" },
		{ V_BEAM,  S_RING,  -1,     -1,          G_HARD, "a beam is straight" },
		{ V_PIT,   -1, E_LIGHT,     -1,          G_HARD, "a pit of light has no depth" },
		// soft: strength halves
		{ V_MEND,  -1, E_SHADOW,    -1,          G_SOFT, "shadow resists mending" },
		{ V_MEND,  -1, E_FIRE,      M_NONE,      G_SOFT, "fire mends by cautery, badly" },
		{ V_SENSE, -1, E_VOID,      -1,          G_SOFT, "void dulls sensing" },
		{ V_VEIL,  -1, E_FIRE,      -1,          G_SOFT, "fire glows through a veil" },
		{ V_VEIL,  -1, E_LIGHTNING, -1,          G_SOFT, "lightning crackles through a veil" },
		{ V_WALL,  S_SPHERE, -1,    -1,          G_SOFT, "a wall on a sphere shell leaks" },
		{ V_BEAM,  S_POINT, -1,     -1,          G_SOFT, "a beam without length is a spark" },
		{ V_BOLT,  S_SPHERE, -1,    M_ANCHORED,  G_SOFT, "an anchored sphere does not fly" },
		{ V_BOLT,  -1,  -1,         M_ANCHORED,  G_SOFT, "an anchored bolt does not fly far" },
		{ -1,      -1, E_LIGHTNING, M_SILENT,    G_SOFT, "lightning is loud" },
		{ V_BEAM,  -1,  -1,         M_TIMER,     G_SOFT, "a beam does not wait" },
		{ V_WARD,  -1,  -1,         M_CHAIN,     G_SOFT, "a ward does not chain" },
		{ V_WARD,  S_LINE,  -1,     -1,          G_SOFT, "a ward is a place, a line is a path" },
		{ V_MEND,  -1,  -1,         M_TRIGGER,   G_SOFT, "mending on a hit is uncertain" },
		{ V_LIFT,  S_CONE,  -1,     -1,          G_SOFT, "a cone lifts unevenly" },
		{ V_BIND,  S_TRAIL, -1,     -1,          G_SOFT, "a trail binds only what steps in it" },
		{ V_SENSE, -1, -1,          M_INVERTED,  G_SOFT, "inverted sensing blinds, weakly" },
	};
	static const int kDefaultRuleCount = (int)(sizeof(kDefaultRules) / sizeof(kDefaultRules[0]));

	struct Contradiction { Grade grade = G_NONE; const char* why = ""; int rule = -1; };
	inline bool ruleMatches(const Rule& r, const Picks& p)
	{
		return (r.verb < 0 || r.verb == p.verb) && (r.shape < 0 || r.shape == p.shape) && (r.element < 0 || r.element == p.element)
			&& (r.modifier < 0 || r.modifier == p.modifier);
	}
	inline Contradiction contradiction(const Rule* rules, int n, const Picks& p)
	{
		Contradiction best;
		for ( int i = 0; i < n; ++i )
		{
			if ( ruleMatches(rules[i], p) && rules[i].grade > best.grade ) { best.grade = rules[i].grade; best.why = rules[i].why; best.rule = i; }
		}
		return best;
	}
	inline Contradiction contradiction(const Picks& p) { return contradiction(kDefaultRules, kDefaultRuleCount, p); }

	// What a contradiction does at resolution: strength multiplier, and whether the backfire roll is taken.
	inline float strengthFor(Grade g) { return g == G_SOFT ? 0.5f : 1.0f; }
	static const float kBackfireChance = 0.25f;      // HARD: the op runs on the caster's cell with this chance (else it fizzles)

	// ---- concentration ------------------------------------------------------------------------------------
	struct ConcTable
	{
		float spend[V_COUNT] = { 8.f, 14.f, 12.f, 14.f, 10.f, 12.f, 10.f, 10.f, 12.f, 10.f, 6.f, 8.f };   // percent per cast, by verb
		float spendMod[M_COUNT] = { 0.f, 3.f, 2.f, 6.f, 6.f, 0.f, 1.f, 0.f };                              // percent added by modifier
		float regenPerSecond = 8.f;                // percent per second
		float failBelow = 20.f;                    // casts fail when the pool is under this
		float timeStretch = 0.5f;                  // cast time x (1 + stretch x (100 - pool) / 100)
	};
	struct Concentration
	{
		float pct = 100.f;
		void regen(const ConcTable& t, float seconds) { pct = std::fmin(100.f, pct + t.regenPerSecond * seconds); }
		bool canCast(const ConcTable& t) const { return pct >= t.failBelow; }
		void spend(const ConcTable& t, const Picks& p) { pct = std::fmax(0.f, pct - t.spend[p.verb] - t.spendMod[p.modifier]); }
	};
	// ms of cast time: verb base x modifier x concentration stretch
	struct TimeTable
	{
		float ms[V_COUNT] = { 350.f, 700.f, 650.f, 600.f, 450.f, 500.f, 400.f, 400.f, 500.f, 450.f, 300.f, 400.f };
		float modMul[M_COUNT] = { 1.f, 1.1f, 1.f, 1.3f, 1.4f, 1.f, 1.1f, 1.f };
	};
	inline float castMs(const TimeTable& tt, const ConcTable& ct, const Picks& p, float concPct)
	{
		return tt.ms[p.verb] * tt.modMul[p.modifier] * (1.f + ct.timeStretch * (100.f - concPct) / 100.f);
	}

	// Barony speaks in 50 Hz ticks
	inline int msToTicks(float ms) { const int t = (int)std::ceil(ms / 20.f - 1e-4f); return t < 1 ? 1 : t; }

	// Ricochet (04b section 6): a projectile glancing a wall under this angle (to the wall's surface) bounces instead of stopping.
	static const float kRicochetDeg = 20.f;
	// angle in degrees between a velocity and a wall surface; axisX = the wall's normal is the x axis (a wall running along y)
	inline float wallAngleDeg(float vx, float vy, bool axisX)
	{
		const float n = axisX ? std::fabs(vx) : std::fabs(vy);       // component into the wall
		const float t = axisX ? std::fabs(vy) : std::fabs(vx);       // component along the wall
		return std::atan2(n, t) * 57.29577951f;
	}
} // namespace core
} // namespace magic
} // namespace apx
