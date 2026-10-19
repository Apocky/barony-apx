// props.hpp -- BP1 engine glue: vanilla props get functions (D83; card handovers/tasks/BP1-props-card.md).
// Rules and the table: props_rules.hpp, props_table.inc (from data/apx/props.json). Hooks:
//   Entity::actFurniture  use loop -> onUse (rest, sit, field bench, read); furniture death -> onDestroyed
//   Entity::colliderOnDestroy -> onColliderBreak (bone cage: skeleton or trinket; empty cage: scrap)
//   player.cpp world prompt -> labelFor (the verb instead of "Inspect", D81)
//   actPlayer -> tick (rest heals and breaks, sitting regen, the probe)
//   U1 nearStation -> nearFieldBench (a table is a FIELD bench: salvage and repair, no crafting)
// Spawns: D72 (1-3, fodder) and D74 (apx::tiers::tag). Server side. APX_PROPS=0 restores vanilla.
// Telemetry: "[apx-props]" lines. APX_PROPS_PROBE=1 runs the headless check (card BP1-1) and exits.
#pragma once

#include "../main.hpp"
#include "../game.hpp"
#include "../stat.hpp"
#include "../entity.hpp"
#include "../items.hpp"
#include "../monster.hpp"
#include "../net.hpp"
#include "../player.hpp"
#include "../mod_tools.hpp"
#include "../apx_stamina.hpp"
#include "props_rules.hpp"
#include "tiers.hpp"
#include "rapidjson/document.h"
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <unordered_map>

namespace apx
{
namespace props
{
#include "props_table.inc"
#include "hollow_enums.inc"

	inline bool enabled() { static const char* v = std::getenv("APX_PROPS"); return !(v && v[0] == '0'); }
	inline int itemNamed(const std::string& s)
	{
		for ( auto& p : kApxItemNames ) if ( s == p.first ) return p.second;
		return -1;
	}
	inline int monsterNamed(const std::string& s)
	{
		for ( auto& p : kApxMonsterNames ) if ( s == p.first ) return p.second;
		return -1;
	}

	inline const PropRow* rowFor(Entity* e, bool forUse)
	{
		if ( !e || !enabled() ) return nullptr;
		for ( auto& p : kPropTable )
		{
			if ( p.behavior == "furniture" && e->behavior == &actFurniture )
			{
				if ( (forUse && p.furnitureType == e->furnitureType) || (!forUse && p.furnitureType == -1) ) return &p;
			}
			else if ( p.behavior == "collider" && e->behavior == &actColliderDecoration && !forUse )
			{
				if ( EditorEntityData_t::getColliderIndexFromName(p.collider) == e->colliderDamageTypes && e->colliderDamageTypes != 0 ) return &p;
			}
		}
		return nullptr;
	}
	inline const char* labelFor(Entity* e)
	{
		const PropRow* p = rowFor(e, true);
		return p && !p->label.empty() ? p->label.c_str() : nullptr;
	}

	struct PlayerState
	{
		Rest rest;
		int restHp = 0, lastHp = -1;
		Entity* seat = nullptr;
		real_t seatX = 0, seatY = 0;
		int sitTicks = 0;
		int encounterPm = 0, encounterMax = 0, hunger = 0;
	};
	inline PlayerState& ps(int pn) { static PlayerState s[MAXPLAYERS]; return s[std::max(0, std::min(MAXPLAYERS - 1, pn))]; }
	struct Counters { int rests = 0, restsDone = 0, restsBroken = 0, healed = 0, sits = 0, sitTicks = 0, benches = 0, reads = 0, breaks = 0, skeletons = 0, skeletonsFodder = 0, trinkets = 0, scrap = 0, nothing = 0, yields = 0, encounters = 0; };
	inline Counters& C() { static Counters c; return c; }
	inline int& fieldBenchUntil(int pn) { static int t[MAXPLAYERS] = {}; return t[std::max(0, std::min(MAXPLAYERS - 1, pn))]; }

	// the H1b history rows a podium reads (APX_HOLLOW_DIR, else data/apx/hollow beside the run)
	inline std::vector<std::pair<std::string, std::string>>& history()
	{
		static std::vector<std::pair<std::string, std::string>> h;
		static bool loaded = false;
		if ( loaded ) return h;
		loaded = true;
		const char* d = std::getenv("APX_HOLLOW_DIR");
		std::string path = std::string(d ? d : "data/apx/hollow") + "/history.json";
		FILE* f = std::fopen(path.c_str(), "rb");
		if ( !f ) return h;
		std::string s;
		char buf[4096];
		size_t n;
		while ( (n = std::fread(buf, 1, sizeof buf, f)) > 0 ) s.append(buf, n);
		std::fclose(f);
		rapidjson::Document doc;
		doc.Parse(s.c_str());
		if ( !doc.HasParseError() && doc.IsArray() )
			for ( auto& r : doc.GetArray() ) h.push_back({ r["id"].GetString(), r["text"].GetString() });
		return h;
	}
	inline std::string lastRead() { static std::string s; return s; }
	inline std::string& lastReadId() { static std::string s; return s; }

	inline Entity* spawnFodder(const std::string& kind, Entity* near)
	{
		int k = monsterNamed(kind);
		if ( k < 0 || !near ) return nullptr;
		Entity* m = summonMonster((Monster)k, (long)near->x, (long)near->y, false);
		if ( m ) apx::tiers::tag(m, apx::combat::Tier::Fodder); // D74: fodder damage
		return m;
	}

	// a use of furniture `e` by player pn (selectedEntity + inrange). true = handled, skip vanilla.
	inline bool onUse(Entity* e, int pn)
	{
		const PropRow* p = rowFor(e, true);
		if ( !p || multiplayer == CLIENT || !players[pn] || !players[pn]->entity ) return false;
		Entity* me = players[pn]->entity;
		Stat* st = me->getStats();
		if ( !st ) return false;
		PlayerState& s = ps(pn);
		if ( p->verb == "rest" )
		{
			if ( s.rest.active ) return true;
			s.rest.start(p->ticks, p->healEvery);
			s.restHp = st->HP; s.lastHp = st->HP; s.hunger = p->hunger; s.encounterPm = p->encounterPm; s.encounterMax = p->encounterMax;
			C().rests++;
			messagePlayer(pn, MESSAGE_INTERACTION, "You lie down to rest.");
			printlog("[apx-props] rest start player=%d hp=%d", pn, st->HP);
			return true;
		}
		if ( p->verb == "sit" )
		{
			s.seat = e; s.seatX = me->x; s.seatY = me->y;
			C().sits++;
			messagePlayer(pn, MESSAGE_INTERACTION, "You sit down and catch your breath.");
			return true;
		}
		if ( p->verb == "bench" )
		{
			fieldBenchUntil(pn) = (int)::ticks + 50 * 60; // the U1 bench shows FIELD while near this table
			C().benches++;
			messagePlayer(pn, MESSAGE_INTERACTION, "A field bench: open your pack to salvage or repair. No crafting here.");
			printlog("[apx-props] field bench player=%d", pn);
			return true;
		}
		if ( p->verb == "read" )
		{
			auto& h = history();
			int k = readPick(e->getUID(), currentlevel, (int)h.size());
			if ( k < 0 ) { messagePlayer(pn, MESSAGE_INTERACTION, "The pages are too faded to read."); return true; }
			messagePlayer(pn, MESSAGE_INTERACTION, "%s", h[k].second.c_str());
			lastReadId() = h[k].first;
			C().reads++;
			printlog("[apx-props] read player=%d history=%s", pn, h[k].first.c_str());
			return true;
		}
		return false;
	}

	inline void giveOutcome(const PropRow& p, Entity* e)
	{
		int k = rollOutcome(p.outcomes, (int)(local_rng.rand() % 1000));
		C().breaks++;
		if ( k < 0 ) { C().nothing++; return; }
		const Outcome& o = p.outcomes[k];
		if ( !o.spawnKind.empty() )
		{
			for ( int i = 0; i < o.spawnN; ++i )
				if ( Entity* m = spawnFodder(o.spawnKind, e) )
				{
					C().skeletons++;
					if ( apx::tiers::tierOf(m) == apx::combat::Tier::Fodder ) C().skeletonsFodder++;
				}
		}
		if ( !o.items.empty() )
		{
			int it = itemNamed(o.items[local_rng.rand() % o.items.size()]);
			if ( it >= 0 )
			{
				dropItemMonster(newItem((ItemType)it, WORN, 0, 1, local_rng.rand(), false, nullptr), e, nullptr);
				(it == (int)TOOL_METAL_SCRAP ? C().scrap : C().trinkets)++;
			}
		}
		if ( !o.yield.empty() ) { C().yields++; printlog("[apx-props] yield %s (given when F6 lands)", o.yield.c_str()); }
	}
	inline void onColliderBreak(Entity* e)
	{
		if ( multiplayer == CLIENT ) return;
		if ( const PropRow* p = rowFor(e, false) ) giveOutcome(*p, e);
	}
	inline void onDestroyed(Entity* e)
	{
		if ( multiplayer == CLIENT ) return;
		if ( const PropRow* p = rowFor(e, false) ) giveOutcome(*p, e);
	}
	inline bool nearFieldBench(int pn)
	{
		if ( !enabled() || !players[pn] || !players[pn]->entity || (int)::ticks > fieldBenchUntil(pn) ) return false;
		Entity* me = players[pn]->entity;
		for ( node_t* n = map.entities ? map.entities->first : nullptr; n; n = n->next )
		{
			Entity* e = (Entity*)n->element;
			if ( e && e->behavior == &actFurniture && e->furnitureType == FURNITURE_TABLE
				&& (e->x - me->x) * (e->x - me->x) + (e->y - me->y) * (e->y - me->y) <= 24.0 * 24.0 ) return true;
		}
		return false;
	}

	inline bool hostileInSight(Entity* me)
	{
		for ( node_t* n = map.creatures ? map.creatures->first : nullptr; n; n = n->next )
		{
			Entity* e = (Entity*)n->element;
			if ( !e || e->behavior != &actMonster || e->monsterAllyGetPlayerLeader() ) continue;
			Stat* s = e->getStats();
			if ( s && s->HP > 0 && std::hypot(e->x - me->x, e->y - me->y) < 6 * 16.0 ) return true;
		}
		return false;
	}

	inline void probeStep(Entity* me);

	inline void tick(Entity* me)
	{
		if ( !me || multiplayer == CLIENT ) return;
		int pn = me->skill[2];
		Stat* st = me->getStats();
		if ( !st ) return;
		PlayerState& s = ps(pn);
		if ( s.rest.active )
		{
			bool hit = s.lastHp >= 0 && st->HP < s.lastHp;
			int add = s.rest.tick(hit, hostileInSight(me));
			if ( add && st->HP < st->MAXHP ) { me->modHP(add); C().healed += add; }
			if ( s.rest.broken ) { C().restsBroken++; messagePlayer(pn, MESSAGE_INTERACTION, "Your rest is broken."); printlog("[apx-props] rest broken player=%d", pn); }
			if ( s.rest.done )
			{
				C().restsDone++;
				st->HUNGER = std::max(0, st->HUNGER - s.hunger);
				printlog("[apx-props] rest done player=%d healed=%d hp=%d", pn, s.rest.healed, st->HP);
				if ( (int)(local_rng.rand() % 1000) < s.encounterPm ) // the director's wandering group (D72: 1-3 fodder)
				{
					int n = 1 + local_rng.rand() % std::max(1, s.encounterMax);
					for ( int i = 0; i < n; ++i ) spawnFodder(currentlevel <= 2 ? "RAT" : "SKELETON", me);
					C().encounters++;
					messagePlayer(pn, MESSAGE_INTERACTION, "Something found you while you slept.");
				}
				s.rest.done = false;
			}
		}
		s.lastHp = st->HP;
		if ( s.seat )
		{
			if ( std::hypot(me->x - s.seatX, me->y - s.seatY) > 2.0 ) s.seat = nullptr; // stood up
			else
			{
				++C().sitTicks;
				auto& sta = apx::get(pn);
				sta.value = std::min(sta.maxValue, sta.value + apx::kRegenPerSecond * 2.0 / 50.0); // x3 with the base regen
			}
		}
		static const char* probe = std::getenv("APX_PROPS_PROBE");
		if ( probe && probe[0] == '1' && pn == 0 ) probeStep(me);
	}

	// ---------------------------------------------------------------- the headless check (card BP1-1)
	struct Probe { int phase = 0; Uint32 t0 = 0; Entity* bed = nullptr; Entity* chair = nullptr; Entity* table = nullptr; Entity* podium = nullptr; int hp0 = 0; int cages = 0; std::string labels; };
	inline Probe& PR() { static Probe p; return p; }
	inline Entity* makeFurniture(int type, Entity* near, int dx)
	{
		Entity* e = newEntity(0, 1, map.entities, nullptr);
		e->behavior = &actFurniture;
		e->furnitureType = type;
		e->furnitureHealth = 15;
		e->x = near->x + dx; e->y = near->y; e->z = 0;
		e->flags[PASSABLE] = true;
		return e;
	}
	inline void use(Entity* e, int pn)
	{
		selectedEntity[pn] = e;
		inrange[pn] = true;
		e->actFurniture();
		selectedEntity[pn] = nullptr;
	}
	inline void probeStep(Entity* me)
	{
		Probe& P = PR();
		Stat* st = me->getStats();
		if ( P.phase == 0 ) { P.t0 = ::ticks; P.phase = 1; return; }
		Uint32 dt = ::ticks - P.t0;
		if ( P.phase == 1 && dt >= 100 )
		{
			P.bed = makeFurniture(FURNITURE_BED, me, 0);
			P.chair = makeFurniture(FURNITURE_CHAIR, me, 0);
			P.table = makeFurniture(FURNITURE_TABLE, me, 8);
			P.podium = makeFurniture(FURNITURE_PODIUM, me, -8);
			for ( Entity* e : { P.bed, P.chair, P.table, P.podium } ) { const char* l = labelFor(e); P.labels += std::string(l ? l : "Inspect") + "|"; }
			// remove nearby hostiles so a rest is not broken by the floor's own creatures
			for ( node_t* n = map.creatures ? map.creatures->first : nullptr; n; n = n->next )
				if ( Entity* e = (Entity*)n->element ) if ( e->behavior == &actMonster ) e->setHP(0);
			st->HP = std::max(1, st->MAXHP / 2);
			P.hp0 = st->HP;
			P.phase = 11; // the bed is used next tick: its first act call initialises it
			return;
		}
		if ( P.phase == 11 )
		{
			use(P.bed, 0);
			P.phase = 2;
			return;
		}
		if ( P.phase == 2 && !ps(0).rest.active ) // the rest ran its 500 ticks (or broke)
		{
			use(P.chair, 0);
			apx::get(0).value = 10;
			P.phase = 3; P.t0 = ::ticks;
			return;
		}
		if ( P.phase == 3 && ::ticks - P.t0 >= 100 )
		{
			use(P.table, 0);
			use(P.podium, 0);
			// 200 bone cages through the real destroy path
			int idx = EditorEntityData_t::getColliderIndexFromName("hanging_cage_skel");
			for ( int i = 0; i < 200; ++i )
			{
				// a free floor tile near the player (the engine refuses an occupied one)
				Entity* c = nullptr;
				for ( int r = 2; r <= 6 && !c; ++r )
					for ( int dx = -r; dx <= r && !c; ++dx )
						for ( int dy = -r; dy <= r && !c; ++dy )
							if ( std::abs(dx) == r || std::abs(dy) == r ) c = Entity::createBreakableCollider(idx, me->x + dx * 16, me->y + dy * 16, nullptr);
				if ( !c ) { if ( i == 0 ) printlog("[apx-props] probe: no cage could be placed (collider index %d)", idx); continue; }
				++P.cages;
				c->colliderOnDestroy();
				list_RemoveNode(c->mynode);
			}
			auto& c = C();
			char row[1400];
			snprintf(row, sizeof row,
				"{\"probe\":\"props\",\"enabled\":%d,\"labels\":\"%s\",\"rests\":%d,\"rests_done\":%d,\"rests_broken\":%d,\"healed\":%d,\"hp0\":%d,\"hp1\":%d,"
				"\"sits\":%d,\"sit_ticks\":%d,\"stamina_after_sit\":%.1f,\"benches\":%d,\"field_bench_near\":%d,\"reads\":%d,\"read_id\":\"%s\",\"cages\":%d,\"breaks\":%d,"
				"\"skeletons\":%d,\"skeletons_fodder\":%d,\"trinkets\":%d,\"nothing\":%d,\"encounters\":%d}",
				(int)enabled(), P.labels.c_str(), c.rests, c.restsDone, c.restsBroken, c.healed, P.hp0, st->HP, c.sits, c.sitTicks, (double)apx::get(0).value,
				c.benches, (int)nearFieldBench(0), c.reads, lastReadId().c_str(), P.cages, c.breaks, c.skeletons, c.skeletonsFodder, c.trinkets, c.nothing, c.encounters);
			printlog("[apx-props] %s", row);
			if ( const char* o = std::getenv("APX_PROPS_OUT") ) if ( FILE* f = std::fopen(o, "ab") ) { std::fprintf(f, "%s\n", row); std::fclose(f); }
			std::exit(0);
		}
	}
} // namespace props
} // namespace apx
