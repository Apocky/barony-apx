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
#include "parts_engine.hpp"
#include "../apx_bench_api.hpp"
#include "../apx_inv_logic.hpp"
namespace apx_inv // the bench screen's rows and actions (apx_bench.cpp); the probe drives them as the UI does
{
	std::vector<apx_logic::Entry> benchEntries(int player, int tab, int station, Item* selectedOwn, uint32_t selectedForm, std::string& head);
	bool benchActivate(int player, int tab, int station, Item* selectedOwn, const apx_logic::Entry& row, uint32_t& selectedForm, std::string& msg);
}
#include "../collision.hpp"
#include <unordered_set>
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
				// use: the row of this furniture type with a use verb; destroy: its "destroyed" row (-1 = any type)
				const bool destroyRow = p.verb == "destroyed";
				if ( forUse && !destroyRow && p.furnitureType == e->furnitureType ) return &p;
				if ( !forUse && destroyRow && (p.furnitureType == e->furnitureType || p.furnitureType == -1) ) return &p;
			}
			else if ( p.behavior == "collider" && e->behavior == &actColliderDecoration )
			{
				// destroy: the "break" row of this collider; use (apx::interact): the row with a label
				if ( (forUse ? !p.label.empty() : p.verb == "break")
					&& EditorEntityData_t::getColliderIndexFromName(p.collider) == e->colliderDamageTypes && e->colliderDamageTypes != 0 ) return &p;
			}
		}
		return nullptr;
	}
	inline bool hasUseVerb(Entity* e) { return rowFor(e, true) != nullptr; }
	inline std::vector<Uint32>& restSpawned() { static std::vector<Uint32> v; return v; }
	inline std::unordered_set<Uint32>& flipped() { static std::unordered_set<Uint32> s; return s; }
	inline std::unordered_set<Uint32>& darkTorches() { static std::unordered_set<Uint32> s; return s; }
	inline bool torchDark(Entity* e) { return e && enabled() && darkTorches().count(e->getUID()); }
	inline bool hostileInSight(Entity* me);
	// the world prompt for player pn (D81): the prop's verb; a table offers Flip while a hostile is in sight
	inline const char* labelFor(Entity* e, int pn)
	{
		static std::unordered_set<Uint32> seen;
		if ( std::getenv("APX_PROPS_QA") && e && seen.insert(e->getUID()).second ) printlog("[apx-props] qa prompt asked for uid=%u", e->getUID());
		if ( e && e->behavior == &actTorch && enabled() ) return torchDark(e) ? "Relight the sconce" : nullptr;
		const PropRow* p = rowFor(e, true);
		if ( !p || p->label.empty() ) return nullptr;
		if ( p->verb == "bench" && players[pn] && players[pn]->entity )
		{
			if ( flipped().count(e->getUID()) ) return "Flipped table (cover)";
			if ( tableFlips(hostileInSight(players[pn]->entity), false) ) return "Flip for cover";
		}
		return p->label.c_str();
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
	struct Counters { int rests = 0, restsDone = 0, restsBroken = 0, healed = 0, sits = 0, sitTicks = 0, benches = 0, reads = 0, breaks = 0, skeletons = 0, skeletonsFodder = 0, trinkets = 0, scrap = 0, nothing = 0, yields = 0, encounters = 0, yieldGrams = 0, flips = 0, torchesTaken = 0, torchesRelit = 0, encounterKinds = 0, searches = 0, snuffs = 0, rattles = 0; };
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
			printlog("[apx-props] sit player=%d", pn);
			return true;
		}
		if ( p->verb == "bench" && tableFlips(hostileInSight(me), flipped().count(e->getUID()) > 0) )
		{
			// BP1-2: on its side it blocks sight lines (LINETRACE_IGNORE_ENTITIES honours BLOCKSIGHT) and bodies
			flipped().insert(e->getUID());
			e->roll = PI / 2;
			e->flags[BLOCKSIGHT] = true;
			e->flags[PASSABLE] = false;
			e->furnitureHealth += 10;
			C().flips++;
			messagePlayer(pn, MESSAGE_INTERACTION, "You heave the table over for cover.");
			printlog("[apx-props] flip table uid=%u player=%d", e->getUID(), pn);
			return true;
		}
		if ( p->verb == "bench" && flipped().count(e->getUID()) ) { messagePlayer(pn, MESSAGE_INTERACTION, "The table is on its side."); return true; }
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

	inline void giveOutcome(const PropRow& p, Entity* e, bool asBreak = true)
	{
		int k = rollOutcome(p.outcomes, (int)(local_rng.rand() % 1000));
		if ( asBreak ) C().breaks++;
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
		if ( !o.yield.empty() )
		{
			// "pouch:<material>:<grams>" into the F6 pouch of the nearest player within 4 tiles (who broke it)
			char mat[40] = {};
			int grams = 0;
			if ( sscanf(o.yield.c_str(), "pouch:%39[^:]:%d", mat, &grams) == 2 )
			{
				int best = -1;
				double bd = 4 * 16.0;
				for ( int i = 0; i < MAXPLAYERS; ++i )
					if ( players[i] && players[i]->entity ) { double d = std::hypot(players[i]->entity->x - e->x, players[i]->entity->y - e->y); if ( d <= bd ) { bd = d; best = i; } }
				if ( best >= 0 && apxPartsLedgerGive(best, mat, grams) )
				{
					C().yields++;
					C().yieldGrams += grams;
					messagePlayer(best, MESSAGE_INVENTORY, "You salvage %d g of %s.", grams, mat);
					printlog("[apx-props] yield %d g %s -> pouch of player %d", grams, mat, best);
				}
				else printlog("[apx-props] yield %s: no player in reach or unknown material", o.yield.c_str());
			}
		}
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
		if ( !me ) return false;
		for ( node_t* n = map.creatures ? map.creatures->first : nullptr; n; n = n->next )
		{
			Entity* e = (Entity*)n->element;
			if ( !e || e->behavior != &actMonster || e->monsterAllyGetPlayerLeader() ) continue;
			Stat* s = e->getStats();
			if ( s && s->HP > 0 && std::hypot(e->x - me->x, e->y - me->y) < 6 * 16.0 ) return true;
		}
		return false;
	}

	// BP1-2 wall torches: taking one leaves a dark sconce (vanilla removed it); a torch in hand relights it
	inline bool torchTaken(Entity* e)   // called where vanilla removed the torch; true = keep the sconce
	{
		if ( !e || !enabled() ) return false;
		darkTorches().insert(e->getUID());
		e->removeLightField();
		C().torchesTaken++;
		printlog("[apx-props] torch taken uid=%u", e->getUID());
		return true;
	}
	inline bool torchUse(Entity* e, int pn) // true = handled (a dark sconce)
	{
		if ( !torchDark(e) || !stats[pn] ) return false;
		Item* t = stats[pn]->shield;
		if ( !t || t->type != TOOL_TORCH ) { messagePlayer(pn, MESSAGE_INTERACTION, "An empty sconce. Hold a torch to relight it."); return true; }
		stats[pn]->shield = nullptr;
		consumeItem(t, pn);
		darkTorches().erase(e->getUID());
		e->skill[0] = 0; // TORCH_LIGHTING: the next tick adds the light again
		C().torchesRelit++;
		messagePlayer(pn, MESSAGE_INTERACTION, "You set your torch in the sconce.");
		printlog("[apx-props] torch relit uid=%u player=%d", e->getUID(), pn);
		return true;
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
					std::vector<int> kinds = apx::tiers::fodderTypes(); // the floor's fodder band (D72/D75)
					for ( int i = 0; i < n; ++i )
					{
						int k = kinds.empty() ? (int)RAT : kinds[local_rng.rand() % kinds.size()];
						if ( Entity* m = summonMonster((Monster)k, (long)me->x, (long)me->y, false) ) { apx::tiers::tag(m, apx::combat::Tier::Fodder); C().encounterKinds++; restSpawned().push_back(m->getUID()); }
					}
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
		// QA setup (APX_PROPS_QA=1): a bed, a chair, a table and a bone cage are placed in a row in front of
		// player 0 on floor start, nothing else is done: a tester walks up and uses them with real input
		static const char* qa = std::getenv("APX_PROPS_QA");
		static int qaDone = -1;
		if ( qa && qa[0] && qa[0] != '0' && pn == 0 && qaDone != currentlevel && ::ticks > 60 )
		{
			qaDone = currentlevel;
			// an arc 24 units away: chair at -50 deg, table straight ahead, bed at +50 deg (relative to the view),
			// the bone cage behind; the tester turns to each without walking into furniture
			// APX_PROPS_QA=name[:d]: that prop straight ahead, centre distance d units (default per prop). Names: chair table bed
			// bunkbed podium torch (dark sconce) cage candelabra maiden; 1 = chair, table, bed in an arc and the cage behind.
			// Colliders sit on tile centres, so the player is moved along the line to the prop to make the centre distance d.
			std::string w = qa;
			real_t wantD = -1;
			if ( size_t c = w.find(':'); c != std::string::npos ) { wantD = std::atof(w.c_str() + c + 1); w = w.substr(0, c); }
			auto at = [&](real_t deg, int type, real_t d) {
				real_t a = me->yaw + deg * PI / 180.0, fx = std::cos(a), fy = std::sin(a);
				Entity* f = newEntity(0, 1, map.entities, nullptr);
				// as maps.cpp builds them (table 271 5x4 focalz -3, chair 272 2x2 -5, bed 627 5x4); z 8 = on the floor
				f->behavior = &actFurniture; f->furnitureType = type; f->flags[BURNABLE] = true; f->z = 8;
				f->sprite = type == FURNITURE_TABLE ? 271 : type == FURNITURE_CHAIR ? 272 : 627;
				f->focalz = type == FURNITURE_CHAIR ? -5 : -3;
				f->sizex = type == FURNITURE_CHAIR ? 2 : 5; f->sizey = type == FURNITURE_CHAIR ? 2 : 4;
				const real_t r = d > 0 ? d : type == FURNITURE_CHAIR ? 14 : 18;
				f->x = me->x + fx * r; f->y = me->y + fy * r;
				if ( type == FURNITURE_BED || type == FURNITURE_BUNKBED ) f->z = 4;
				printlog("[apx-props] qa placed furniture type %d at %.0f,%.0f centre distance %.1f", type, f->x, f->y, r);
			};
			if ( w == "chair" ) at(0, FURNITURE_CHAIR, wantD);
			else if ( w == "table" ) at(0, FURNITURE_TABLE, wantD);
			else if ( w == "bed" ) at(0, FURNITURE_BED, wantD);
			else if ( w == "bunkbed" ) at(0, FURNITURE_BUNKBED, wantD);
			else if ( w == "podium" ) at(0, FURNITURE_PODIUM, wantD);
			else if ( w == "torch" )
			{
				Entity* f = newEntity(3, 1, map.entities, nullptr);
				f->behavior = &actTorch; f->z = -1; f->flags[PASSABLE] = true;
				const real_t r = wantD > 0 ? wantD : 20;
				f->x = me->x + std::cos(me->yaw) * r; f->y = me->y + std::sin(me->yaw) * r;
				darkTorches().insert(f->getUID());
				printlog("[apx-props] qa placed dark sconce uid=%u at %.0f,%.0f centre distance %.1f", f->getUID(), f->x, f->y, r);
			}
			else if ( w == "1" ) { at(-50, FURNITURE_CHAIR, -1); at(0, FURNITURE_TABLE, -1); at(50, FURNITURE_BED, -1); }
			const char* cname = w == "cage" || w == "1" ? "hanging_cage_skel" : w == "candelabra" ? "candelabra" : w == "maiden" ? "iron_maiden" : nullptr;
			if ( cname )
			{
				int idx = EditorEntityData_t::getColliderIndexFromName(cname);
				// the first free floor tile ahead (farthest first when a distance is wanted; behind for "1"), then beside it
				Entity* c = nullptr;
				const real_t dir = w == "1" ? -1 : 1;
				const int k0 = wantD > 0 ? 3 : 1, k1 = wantD > 0 ? 1 : 3, ks = wantD > 0 ? -1 : 1;
				for ( int k = k0; k != k1 + ks && !c; k += ks )
					for ( int side : { 0, 1, -1 } )
					{
						real_t a = me->yaw + side * PI / 2, dd = 16.0 * k;
						real_t px = me->x + std::cos(me->yaw) * dir * dd + (side ? std::cos(a) * 16 : 0), py = me->y + std::sin(me->yaw) * dir * dd + (side ? std::sin(a) * 16 : 0);
						if ( (c = Entity::createBreakableCollider(idx, px, py, nullptr)) ) break;
					}
				if ( c && w != "1" )
				{
					const real_t ang = std::atan2(c->y - me->y, c->x - me->x), d0 = std::hypot(c->x - me->x, c->y - me->y);
					if ( wantD > 0 && wantD < d0 ) { me->x = c->x - std::cos(ang) * wantD; me->y = c->y - std::sin(ang) * wantD; } // setup: stand wantD from its centre on the same line
					me->yaw = ang; // setup: face it, the tester only uses or swings
				}
				if ( c ) printlog("[apx-props] qa placed %s uid=%u at %.0f,%.0f centre distance %.1f, player faces it (yaw %.2f)", cname, c->getUID(), c->x, c->y, std::hypot(c->x - me->x, c->y - me->y), me->yaw);
				else printlog("[apx-props] qa could not place %s", cname);
			}
		}
		static const char* probe = std::getenv("APX_PROPS_PROBE");
		if ( probe && probe[0] == '1' && pn == 0 ) probeStep(me);
	}

	// ---------------------------------------------------------------- the headless check (card BP1-1)
	struct Probe { int phase = 0; Uint32 t0 = 0; Entity* bed = nullptr; Entity* chair = nullptr; Entity* table = nullptr; Entity* podium = nullptr; Entity* table2 = nullptr; int hp0 = 0; int cages = 0; std::string labels; int encKindsFodder = 0, encKinds = 0, fodderBand = 0; std::string flipLabel; int losBefore = -1, losAfter = -1; int torchFound = 0, torchKept = 0, torchDarkAfterTake = 0, gotTorch = 0, relit = 0; int woodGrams = -1, gramsNoLedger = -2, paidOk = 0, paidStatus = -1, paidLedgerDelta = -1, repairRow = 0, repairOk = 0, statusBefore = -1, statusAfter = -1, repairGrams = -1; std::string repairWhy; };
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
			P.table2 = makeFurniture(FURNITURE_TABLE, me, 40);
			P.podium = makeFurniture(FURNITURE_PODIUM, me, -8);
			for ( Entity* e : { P.bed, P.chair, P.table, P.podium } ) { const char* l = labelFor(e, 0); P.labels += std::string(l ? l : "Inspect") + "|"; }
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
			ps(0).encounterPm = 1000; // the probe forces the wandering group to test its kinds
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
			// the wandering group came from the floor's fodder band, tagged fodder; then clear it
			auto band = apx::tiers::fodderTypes();
			P.fodderBand = (int)band.size();
			for ( Uint32 u : restSpawned() ) // only what the rest itself spawned
				if ( Entity* m = uidToEntity(u) ) if ( m->getStats() )
				{
					++P.encKinds;
					if ( std::find(band.begin(), band.end(), (int)m->getStats()->type) != band.end() && apx::tiers::tierOf(m) == apx::combat::Tier::Fodder ) ++P.encKindsFodder;
				}
			for ( node_t* n = map.creatures ? map.creatures->first : nullptr; n; n = n->next )
				if ( Entity* m = (Entity*)n->element ) if ( m->behavior == &actMonster && m->getStats() && m->getStats()->HP > 0 ) m->setHP(0);
			P.phase = 4; P.t0 = ::ticks;
			return;
		}
		if ( P.phase == 4 && ::ticks - P.t0 >= 5 )
		{
			use(P.table, 0);
			use(P.podium, 0);
			// flip: a rat in sight makes the second table offer Flip; sight line measured before and after
			Entity* rat = summonMonster(RAT, (long)me->x - 48, (long)me->y, false);
			const char* fl = labelFor(P.table2, 0);
			P.flipLabel = fl ? fl : "";
			real_t ang = atan2(P.table2->y - me->y, P.table2->x - me->x);
			lineTrace(me, me->x, me->y, ang, 64, LINETRACE_IGNORE_ENTITIES, false);
			P.losBefore = hit.entity == P.table2;
			use(P.table2, 0);
			lineTrace(me, me->x, me->y, ang, 64, LINETRACE_IGNORE_ENTITIES, false);
			P.losAfter = hit.entity == P.table2;
			if ( rat ) rat->setHP(0);
			// torch: take (the sconce stays dark), then relight it with the torch in hand
			Entity* torch = nullptr;
			for ( node_t* n = map.entities->first; n && !torch; n = n->next ) if ( Entity* e = (Entity*)n->element ) if ( e->behavior == &actTorch ) torch = e;
			if ( torch )
			{
				P.torchFound = 1;
				Uint32 tu = torch->getUID();
				selectedEntity[0] = torch; inrange[0] = true; torch->ticks = std::max<Uint32>(torch->ticks, 5); actTorch(torch); selectedEntity[0] = nullptr;
				P.torchKept = uidToEntity(tu) != nullptr;
				P.torchDarkAfterTake = torchDark(torch);
				for ( node_t* n = st->inventory.first; n; n = n->next ) if ( Item* it = (Item*)n->element ) if ( it->type == TOOL_TORCH ) { P.gotTorch = 1; st->shield = it; break; }
				if ( P.torchKept ) // vanilla (APX_PROPS=0) removed the torch entity: nothing to relight
				{
					selectedEntity[0] = torch; inrange[0] = true; actTorch(torch); selectedEntity[0] = nullptr;
					P.relit = !torchDark(torch);
				}
			}
			// repair through the bench provider (F6): salvage swords into the ledger, then mend a worn one
			if ( apxPartsEnabled() )
			{
				Item* sw = newItem(IRON_SWORD, WORN, 0, 1, 0, true, nullptr);
				apxPartsBind(sw, 0);
				P.statusBefore = (int)sw->status;
				std::string why, mat;
				for ( int k = 0; k < 20 && apx_bench::provider().repairCost(0, sw, why, mat) < 0 && sw->apx.nParts; ++k )
				{
					Item* s2 = newItem(IRON_SWORD, WORN, 0, 1, (Uint32)k + 1, true, &st->inventory); // salvage consumes it from the pack
					apxPartsBind(s2, 0);
					std::string w2;
					apxPartsSalvageItem(0, s2, -1, w2);
				}
				P.repairGrams = apx_bench::provider().repairCost(0, sw, why, mat);
				// through the bench screen: the Repair tab at a FIELD bench lists the one-step repair row (U1i names)
				std::string head, msg;
				uint32_t form = 0;
				auto rows = apx_inv::benchEntries(0, apx_logic::TAB_REPAIR, apx_logic::FIELD, sw, form, head);
				for ( auto& row : rows )
					if ( !row.header && row.row.kind == apx_logic::ROW_REPAIR_STEP )
					{
						P.repairRow = 1;
						P.repairOk = apx_inv::benchActivate(0, apx_logic::TAB_REPAIR, apx_logic::FIELD, sw, row, form, msg);
						P.repairWhy = msg;
						break;
					}
				P.statusAfter = (int)sw->status;
				{ std::string w4, m4; Item probeCopy = *sw; probeCopy.status = (Status)P.statusBefore; P.gramsNoLedger = apx_bench::provider().repairGrams(&probeCopy, w4, m4); }
				long long l0 = apxPartsLedgerGrams(0);
				std::string w3;
				P.paidOk = apx_bench::provider().repairPaid(0, sw, w3);
				P.paidStatus = (int)sw->status;
				P.paidLedgerDelta = (int)(apxPartsLedgerGrams(0) - l0);
				free(sw);
			}
			// a broken chair pays its wood into the pouch (the real furniture death path in actFurniture)
			{
				Entity* ch = makeFurniture(FURNITURE_CHAIR, me, 8);
				ch->actFurniture(); // init
				long long g0 = apxPartsLedgerGrams(0);
				ch->furnitureHealth = 0;
				ch->actFurniture(); // dies: gibs, then apx::props::onDestroyed
				P.woodGrams = (int)(apxPartsLedgerGrams(0) - g0);
			}
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
			char row[2600];
			snprintf(row, sizeof row,
				"{\"probe\":\"props\",\"enabled\":%d,\"labels\":\"%s\",\"rests\":%d,\"rests_done\":%d,\"rests_broken\":%d,\"healed\":%d,\"hp0\":%d,\"hp1\":%d,"
				"\"sits\":%d,\"sit_ticks\":%d,\"stamina_after_sit\":%.1f,\"benches\":%d,\"field_bench_near\":%d,\"reads\":%d,\"read_id\":\"%s\",\"cages\":%d,\"breaks\":%d,"
				"\"skeletons\":%d,\"skeletons_fodder\":%d,\"trinkets\":%d,\"nothing\":%d,\"encounters\":%d,\"enc_creatures\":%d,\"enc_fodder_band\":%d,\"fodder_band_kinds\":%d,"
				"\"flip_label\":\"%s\",\"flips\":%d,\"los_before\":%d,\"los_after\":%d,\"torch_found\":%d,\"torch_kept\":%d,\"torch_dark\":%d,\"got_torch\":%d,\"relit\":%d,"
				"\"wood_grams\":%d,\"grams_no_ledger\":%d,\"paid_ok\":%d,\"paid_status\":%d,\"paid_ledger_delta\":%d,\"repair_row\":%d,\"repair_ok\":%d,\"repair_grams\":%d,\"status_before\":%d,\"status_after\":%d,\"repair_why\":\"%s\"}",
				(int)enabled(), P.labels.c_str(), c.rests, c.restsDone, c.restsBroken, c.healed, P.hp0, st->HP, c.sits, c.sitTicks, (double)apx::get(0).value,
				c.benches, (int)nearFieldBench(0), c.reads, lastReadId().c_str(), P.cages, c.breaks, c.skeletons, c.skeletonsFodder, c.trinkets, c.nothing, c.encounters,
				P.encKinds, P.encKindsFodder, P.fodderBand, P.flipLabel.c_str(), c.flips, P.losBefore, P.losAfter, P.torchFound, P.torchKept, P.torchDarkAfterTake, P.gotTorch, P.relit,
				P.woodGrams, P.gramsNoLedger, P.paidOk, P.paidStatus, P.paidLedgerDelta, P.repairRow, P.repairOk, P.repairGrams, P.statusBefore, P.statusAfter, P.repairWhy.c_str());
			printlog("[apx-props] %s", row);
			if ( const char* o = std::getenv("APX_PROPS_OUT") ) if ( FILE* f = std::fopen(o, "ab") ) { std::fprintf(f, "%s\n", row); std::fclose(f); }
			std::exit(0);
		}
	}
} // namespace props
} // namespace apx
