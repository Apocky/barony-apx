// apx/variants_glue.hpp -- creature variants (card BV1 V1-3): the engine side of src/apx/variants.hpp.
// Hooks: actmonster.cpp after a monster's init (onMonsterInit), before the death drops (onDeath), every tick (tick: client look,
// selftest), entity.cpp creature damage (scaleDamage). The roll uses its own hash stream, never the engine RNG.
//
// Knobs (environment): APX_VARIANTS=0 vanilla spawns and drops exactly; APX_VARIANTS_FLAT=1 the flat-list control;
// APX_VARIANT_FORCE=<node id> every spawn of that node's base becomes that node; APX_VARIANTS_SELFTEST=1 the scripted run.
// Net: the host puts the node index + 1 in the monster's skill[58] and sends it (serverUpdateEntitySkill); clients apply the look.
// Not done here (named in the card report): traits (on-hit effects), light, per-entity impact material (F4b keys by species).
#pragma once

#include "../game.hpp"
#include "../net.hpp"
#include "../items.hpp"
#include "parts_engine.hpp"
#include "variants.hpp"
#include <cstdlib>
#include <string>
#include <vector>
#include <unordered_map>

namespace apx
{
namespace vglue
{
	namespace V = apx::variants;
	static const int kSlot = 58;                 // monster skill slot carrying node index + 1
	static const int kMatDropGrams = 100;        // ASSUMED grams credited per material drop
	static const real_t kMatDropRadius = 192.0;  // the nearest player within 12 tiles gets the material

	inline bool envOn(const char* n, bool dflt) { const char* v = std::getenv(n); return v ? v[0] != '0' : dflt; }
	inline bool on() { static const bool b = envOn("APX_VARIANTS", true); return b; }
	inline bool flat() { static const bool b = envOn("APX_VARIANTS_FLAT", false); return b; }
	inline bool selftest() { static const bool b = envOn("APX_VARIANTS_SELFTEST", false); return b; }
	inline int& forceNode() { static int f = -2; return f; } // -2 not read yet, -1 none, else a node index
	inline int forced()
	{
		if ( forceNode() == -2 )
		{
			const char* v = std::getenv("APX_VARIANT_FORCE");
			forceNode() = v ? V::find(v) : -1;
			if ( v && forceNode() < 0 ) { printlog("[apx-variant] APX_VARIANT_FORCE=%s is not a variant id", v); }
		}
		return forceNode();
	}

	struct Counters { int applied = 0, clientApplied = 0, deaths = 0, itemDrops = 0, matCredits = 0, matLost = 0, itemFail = 0, compPending = 0; };
	inline Counters& counters() { static Counters c; return c; }

	struct Applied { int node; double dmg; bool look; };
	inline std::unordered_map<Uint32, Applied>& applied() { static std::unordered_map<Uint32, Applied> m; return m; }

	// monster type -> variant base id (only the bases with a tree)
	inline const char* baseIdFor(int type)
	{
		switch ( type )
		{
			case RAT: return "rat";
			case SKELETON: return "skeleton";
			default: return nullptr;
		}
	}

	inline uint32_t& spawnCounter() { static uint32_t c = 0; return c; }
	inline uint64_t floorSeed() { return (static_cast<uint64_t>(mapseed) << 16) ^ (static_cast<uint64_t>(currentlevel) << 8) ^ (secretlevel ? 1u : 0u); }
	inline void floorChanged() { spawnCounter() = 0; applied().clear(); }

	inline int itemTypeByName(const char* name)
	{
		static const char* const names[] = {
#include "parts_typenames.inc"
		};
		for ( int i = 0; i < static_cast<int>(sizeof(names) / sizeof(names[0])); ++i ) { if ( std::strcmp(names[i], name) == 0 ) { return i; } }
		return -1;
	}

	inline vec4 tintOf(const V::VNode& n)
	{
		unsigned r = 255, g = 255, b = 255;
		if ( std::strlen(n.tint) == 7 ) { sscanf(n.tint + 1, "%02x%02x%02x", &r, &g, &b); }
		return vec4(r / 255.f * 0.45f, g / 255.f * 0.45f, b / 255.f * 0.45f, 0.f); // additive light on the model (the D79 palette stand-in)
	}

	// the look: tint and scale on the entity and its body parts (the same on host and clients)
	inline void applyLook(Entity* my, const V::VNode& n)
	{
		const vec4 t = tintOf(n);
		auto one = [&](Entity* e)
		{
			if ( !e ) { return; }
			e->lightBonus = t;
			e->scalex *= n.scale; e->scaley *= n.scale; e->scalez *= n.scale;
		};
		one(my);
		for ( Entity* part : my->bodyparts ) { one(part); }
	}

	inline void logNode(const char* who, Entity* my, int idx)
	{
		const V::VNode& n = V::node(idx);
		Stat* st = my->getStats();
		printlog("[apx-variant] %s id=%s node=%d uid=%u hp=%d dex=%d material=%s tier_shift=%d traits=%s tint=%s scale=%.2f loot_rows=%d",
			who, n.id, idx, my->getUID(), st ? st->MAXHP : -1, st ? st->DEX : -1, n.material, n.tierShift, n.traits, n.tint, n.scale, n.lootCount);
	}

	// server: after the monster's species init, roll and apply
	inline void onMonsterInit(Entity* my, Stat* st)
	{
		if ( !on() || multiplayer == CLIENT || !my || !st ) { return; }
		const char* base = baseIdFor(st->type);
		if ( !base || applied().count(my->getUID()) ) { return; }
		int idx = -1;
		if ( forced() >= 0 && V::rootOf(forced()) == V::find(base) ) { idx = forced(); }
		else { idx = V::roll(floorSeed(), spawnCounter(), base, currentlevel + 1, flat()); }
		++spawnCounter();
		if ( idx < 0 || V::node(idx).level == 0 ) { return; } // the base itself: vanilla, untouched
		const V::VNode& n = V::node(idx);
		st->MAXHP = std::max(1, static_cast<int>(st->MAXHP * n.hp + 0.5));
		st->HP = st->MAXHP;
		st->OLDHP = st->HP;
		st->DEX = std::max(1, static_cast<int>(st->DEX * n.speed + 0.5)); // movement reads DEX
		if ( st->name[0] == 0 ) { snprintf(st->name, sizeof(st->name), "%s", n.id); }
		applied()[my->getUID()] = { idx, n.dmg, true };
		applyLook(my, n);
		my->skill[kSlot] = idx + 1;
		if ( multiplayer == SERVER ) { serverUpdateEntitySkill(my, kSlot); }
		++counters().applied;
		logNode("spawn", my, idx);
	}

	// every tick, every monster: a client applies the look of the id the host sent
	inline void tick(Entity* my)
	{
		if ( !on() || multiplayer != CLIENT || !my || my->skill[kSlot] <= 0 ) { return; }
		auto it = applied().find(my->getUID());
		if ( it != applied().end() ) { return; }
		const int idx = my->skill[kSlot] - 1;
		if ( idx < 0 || idx >= V::nodeCount() ) { return; }
		applied()[my->getUID()] = { idx, V::node(idx).dmg, true };
		applyLook(my, V::node(idx));
		++counters().clientApplied;
		logNode("client", my, idx);
	}

	// a creature's blow, scaled by its variant (after the D74 tier damage)
	inline int scaleDamage(Entity* attacker, int damage)
	{
		if ( !on() || !attacker || attacker->behavior != &actMonster ) { return damage; }
		auto it = applied().find(attacker->getUID());
		if ( it == applied().end() ) { return damage; }
		return std::max(1, static_cast<int>(damage * it->second.dmg + 0.5));
	}

	// server: before the death drops, draw the variant's resolved loot table
	inline void onDeath(Entity* my, Stat* st)
	{
		if ( !on() || multiplayer == CLIENT || !my ) { return; }
		auto it = applied().find(my->getUID());
		if ( it == applied().end() ) { return; }
		++counters().deaths;
		const V::VNode& n = V::node(it->second.node);
		uint64_t s = V::spawnStream(floorSeed(), my->getUID(), "loot");
		std::string got;
		for ( const char* id : V::drops(it->second.node, s) )
		{
			got += id; got += ' ';
			if ( std::strncmp(id, "item:", 5) == 0 )
			{
				const int type = itemTypeByName(id + 5);
				if ( type < 0 ) { ++counters().itemFail; continue; }
				Item* item = newItem(static_cast<ItemType>(type), SERVICABLE, 0, 1, 0, false, nullptr);
				Entity* e = dropItemMonster(item, my, st);
				if ( e ) { ++counters().itemDrops; } else { ++counters().itemFail; }
			}
			else if ( std::strncmp(id, "comp:", 5) == 0 )
			{
				++counters().compPending; // D86: components are first-class items; the entry is data-only until F6's component item lands
			}
			else if ( std::strncmp(id, "mat:", 4) == 0 )
			{
				int best = -1; real_t bd = kMatDropRadius * kMatDropRadius;
				for ( int p = 0; p < MAXPLAYERS; ++p )
				{
					if ( !players[p] || !players[p]->entity ) { continue; }
					const real_t dx = players[p]->entity->x - my->x, dy = players[p]->entity->y - my->y;
					if ( dx * dx + dy * dy < bd ) { bd = dx * dx + dy * dy; best = p; }
				}
				if ( best >= 0 && apxPartsLedgerGive(best, id + 4, kMatDropGrams) ) { ++counters().matCredits; } else { ++counters().matLost; }
			}
		}
		printlog("[apx-variant] death id=%s uid=%u drops=%s", n.id, my->getUID(), got.c_str());
	}

	// ---- scripted run (APX_VARIANTS_SELFTEST): every node, kills in batches, drop log ----
	inline void selftestStep()
	{
		if ( !selftest() || !players[0] || !players[0]->entity ) { return; }
		static Uint32 t = 0;
		static bool started = false;
		++t;
		Entity* me = players[0]->entity;
		const int N = V::nodeCount();
		const int perNode = 70, batch = 10;
		if ( t == 5 ) { stats[0]->HP = stats[0]->MAXHP = 99999; started = true; printlog("[apx-variant] selftest start nodes=%d kills_per_node=%d", N, perNode); }
		if ( !started ) { return; }
		static std::vector<Uint32> live;
		const Uint32 step = 24; // ticks per batch
		const Uint32 t0 = 20;
		if ( t >= t0 && (t - t0) % step == 0 )
		{
			const int k = static_cast<int>((t - t0) / step);
			const int node = k / (perNode / batch), rep = k % (perNode / batch);
			(void)rep;
			// kill the previous batch (HP to 0: the engine runs the death path, drops included)
			for ( Uint32 uid : live ) { Entity* e = uidToEntity(uid); if ( e && e->getStats() ) { e->getStats()->HP = 0; } }
			live.clear();
			if ( node < N )
			{
				const V::VNode& n = V::node(node);
				const int root = V::rootOf(node);
				const char* base = V::node(root).id;
				const int type = std::strcmp(base, "rat") == 0 ? RAT : SKELETON;
				forceNode() = node;
				for ( int i = 0; i < batch; ++i )
				{
					const real_t a = (i / static_cast<real_t>(batch)) * 2 * PI;
					Entity* m = summonMonster(static_cast<Monster>(type), me->x + cos(a) * 48.0, me->y + sin(a) * 48.0, true);
					if ( m ) { live.push_back(m->getUID()); }
				}
				forceNode() = -1;
				(void)n;
			}
			else if ( node == N )
			{
				Counters& c = counters();
				printlog("[apx-variant] selftest_done applied=%d deaths=%d itemDrops=%d matCredits=%d matLost=%d itemFail=%d compPending=%d", c.applied, c.deaths, c.itemDrops, c.matCredits, c.matLost, c.itemFail, c.compPending);
			}
		}
	}
}
}
