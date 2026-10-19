// apx/tactics.cpp -- BF3 core: knobs, per-creature state, reach slots, ring, wind-up interleave, ally
// avoidance, steering in the ATTACK state and the timing probe. Design and knobs: tactics.hpp.
#include "tactics_int.hpp"
#include "flow.hpp"
#include "../engine/audio/sound.hpp"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace apx
{
namespace tactics
{
	Knobs K;
	bool active = false;
	bool astarDiagOn = false;
	Counters C;
	std::unordered_map<Uint32, TacticsState> S;
	std::unordered_map<Uint32, TargetSlots> T;
	static double gElbow = -1;   // APX_ELBOW: swing room between attackers in units; -1 = one body width
	static double gProbeUs = 0;  // APX_TACTICS_PROBE_US: CONTROL for the timing probe (busy-wait per decide)
	static std::vector<float> gDecideUs, gMoveUs, gTickUs;
	static double gTickAcc = 0;
	static const size_t SAMPLE_CAP = 400000;

	static bool envOn(const char* name, bool def)
	{
		const char* v = getenv(name);
		if ( !v || !*v ) { return def; }
		return strcmp(v, "0") != 0;
	}

	void init()
	{
		K.tactics = envOn("APX_TACTICS", true);
		K.reach = envOn("APX_REACH", true);
		K.interleave = envOn("APX_INTERLEAVE", true);
		K.allyavoid = envOn("APX_ALLYAVOID", true);
		K.ring = envOn("APX_RING", true);
		K.packs = envOn("APX_PACKS", true);
		K.morale = envOn("APX_MORALE", true);
		K.astardiag = envOn("APX_ASTAR_DIAG", true);
		const char* e = getenv("APX_ELBOW");
		if ( e && *e ) { gElbow = atof(e); }
		e = getenv("APX_TACTICS_PROBE_US");
		if ( e && *e ) { gProbeUs = atof(e); }
		refresh();
		printlog("[apx] tactics knobs: %s elbow=%.1f probe_us=%.0f", knobString().c_str(), gElbow, gProbeUs);
	}

	void refresh() { active = K.tactics; astarDiagOn = K.tactics && K.astardiag; }
	void setProbeUs(double us) { gProbeUs = us; }
	void setElbow(double units) { gElbow = units; }

	std::string knobString()
	{
		char buf[176];
		snprintf(buf, sizeof(buf), "tactics=%d,reach=%d,interleave=%d,allyavoid=%d,ring=%d,flow=%d,morale=%d,packs=%d,astardiag=%d",
			K.tactics ? 1 : 0, K.reach ? 1 : 0, K.interleave ? 1 : 0, K.allyavoid ? 1 : 0, K.ring ? 1 : 0,
			flow::enabled ? 1 : 0, K.morale ? 1 : 0, K.packs ? 1 : 0, K.astardiag ? 1 : 0);
		return buf;
	}

	void onLevelChange()
	{
		S.clear();
		T.clear();
		packsReset();
		flow::onLevelChange();
	}

	void resetCounters() { C = Counters(); }

	void addDecideUs(double us) { if ( gDecideUs.size() < SAMPLE_CAP ) { gDecideUs.push_back((float)us); } gTickAcc += us; }
	void addMoveUs(double us) { if ( gMoveUs.size() < SAMPLE_CAP ) { gMoveUs.push_back((float)us); } gTickAcc += us; }
	void addPathUs(double us) { gTickAcc += us; }
	void endOfGameTick() { if ( gTickUs.size() < SAMPLE_CAP ) { gTickUs.push_back((float)gTickAcc); } gTickAcc = 0; }
	void timingReset() { gDecideUs.clear(); gMoveUs.clear(); gTickUs.clear(); gTickAcc = 0; }

	static double pct(std::vector<float> v, double p)
	{
		if ( v.empty() ) { return 0; }
		size_t i = (size_t)std::min((double)v.size() - 1, std::floor(p * (v.size() - 1) + 0.5));
		std::nth_element(v.begin(), v.begin() + i, v.end());
		return v[i];
	}

	void timingPercentiles(double& dp50, double& dp99, double& mp50, double& mp99, double& tp99)
	{
		dp50 = pct(gDecideUs, 0.5); dp99 = pct(gDecideUs, 0.99);
		mp50 = pct(gMoveUs, 0.5); mp99 = pct(gMoveUs, 0.99);
		tp99 = pct(gTickUs, 0.99);
	}

	// ---------------------------------------------------------------- helpers
	TacticsState* find(Entity* e)
	{
		if ( !e ) { return nullptr; }
		auto it = S.find(e->getUID());
		return (it == S.end() || it->second.self != e) ? nullptr : &it->second;
	}

	double bodyR(Entity* e) { return std::max((double)e->sizex, (double)e->sizey); }

	double angDiff(double a, double b)
	{
		double d = fmod(a - b, 2 * PI);
		if ( d < -PI ) { d += 2 * PI; }
		if ( d > PI ) { d -= 2 * PI; }
		return fabs(d);
	}

	static double elbowFor(double rA) { return gElbow >= 0 ? gElbow : 2 * rA; }

	bool eligible(Entity* my, Stat& st)
	{
		if ( my->behavior != &actMonster || my->monsterAllyIndex >= 0 ) { return false; } // followers: never (A4)
		if ( st.leader_uid )
		{
			Entity* l = uidToEntity(st.leader_uid);
			if ( l && l->behavior == &actPlayer ) { return false; }
		}
		switch ( st.type )
		{
			case NOTHING: case LICH: case LICH_FIRE: case LICH_ICE: case DEVIL: case SHOPKEEPER: case SHADOW:
			case MIMIC: case MINIMIMIC: case DUMMYBOT: case HOLOGRAM: case SENTRYBOT: case SPELLBOT: case GYROBOT:
			case BAT_SMALL: case DUCK_SMALL: case MOTH_SMALL: case REVENANT_SKULL: case MONSTER_ADORCISED_WEAPON:
			case FLAME_ELEMENTAL: case EARTH_ELEMENTAL: case MINOTAUR: case BUGBEAR:
				return false;
			default:
				break;
		}
		return !my->isInertMimic();
	}

	bool walkable(int tx, int ty)
	{
		if ( tx < 1 || ty < 1 || tx >= (int)map.width - 1 || ty >= (int)map.height - 1 ) { return false; }
		int i = ty * MAPLAYERS + tx * MAPLAYERS * map.height;
		if ( !map.tiles[i] || map.tiles[OBSTACLELAYER + i] ) { return false; }
		if ( swimmingtiles && swimmingtiles[map.tiles[i]] ) { return false; }
		if ( lavatiles && lavatiles[map.tiles[i]] ) { return false; }
		return true;
	}

	// walls only. LINETRACE_IGNORE_ENTITIES does NOT skip visible monsters (collision.cpp findEntityInLine skips
	// only non-sight-blocking props and invisible monsters), so a crowd read as walls: reach slots fell from 7 to
	// about 2 and ring slots failed. With no tracing entity, findEntityInLine returns nothing: a pure wall trace.
	bool wallClear(Entity* from, double x0, double y0, double x1, double y1)
	{
		(void)from;
		double len = hypot(x1 - x0, y1 - y0);
		if ( len < 1 ) { return true; }
		HitSave hs;
		double d = lineTrace(nullptr, x0, y0, atan2(y1 - y0, x1 - x0), len, 0, false);
		return d >= len - 0.5;
	}

	// a monster body (radius + swing room) crosses the segment from my to target
	bool corridorBlocked(Entity* my, Entity* target)
	{
		double ax = my->x, ay = my->y, dx = target->x - ax, dy = target->y - ay, L2 = dx * dx + dy * dy;
		if ( L2 < 1 ) { return false; }
		for ( node_t* nd = map.creatures ? map.creatures->first : nullptr; nd; nd = nd->next )
		{
			Entity* o = (Entity*)nd->element;
			if ( !o || o == my || o == target || o->behavior != &actMonster ) { continue; }
			Stat* os = o->getStats();
			if ( !os || os->HP <= 0 ) { continue; }
			double t = ((o->x - ax) * dx + (o->y - ay) * dy) / L2;
			if ( t <= 0 || t >= 1 ) { continue; }
			double px = ax + t * dx - o->x, py = ay + t * dy - o->y, r = bodyR(o) + SWING_HALF;
			if ( px * px + py * py < r * r ) { return true; }
		}
		return false;
	}

	void stand(Entity* my, Entity* target, double speed)
	{
		// the engine's rotate-only idiom (actmonster.cpp ATTACK state): a tiny velocity toward the target
		double tg = atan2(target->y - my->y, target->x - my->x);
		MONSTER_VELX = cos(tg) * .02 * speed;
		MONSTER_VELY = sin(tg) * .02 * speed;
	}

	// The engine opens a door that a chasing creature bumps (actmonster.cpp ATTACK state, same rule: unlocked and
	// INT > -2). Steered movement skips that branch, so the bump is handled here. A locked door (the engine bashes it
	// down) is kept out of this creature's routes for 5 s and its route is replanned round it.
	static void bumpDoor(Entity* my, TacticsState& s)
	{
		Entity* e = hit.entity;
		if ( !e || (e->behavior != &actDoor && e->behavior != &actIronDoor) ) { return; }
		if ( !e->doorLocked && my->getINT() > -2 )
		{
			if ( !e->doorDir && !e->doorStatus ) { e->doorStatus = 1 + (my->x > e->x); playSoundEntity(e, 21, 96); ++C.doors_opened; }
			else if ( e->doorDir && !e->doorStatus ) { e->doorStatus = 1 + (my->y < e->y); playSoundEntity(e, 21, 96); ++C.doors_opened; }
			return;
		}
		++C.door_bumps_locked;
		s.avoidTile = (int)floor(e->x / 16) + (int)floor(e->y / 16) * (int)map.width;
		s.avoidUntil = ticks + 5 * TICKS_PER_SECOND;
		s.routeBuilt = 0; // replan now
	}

	static int gStuckLogs = 0;
	static void logStuck(Entity* my, TacticsState& s, double gx, double gy)
	{
		if ( gStuckLogs >= 80 ) { return; }
		++gStuckLogs;
		printlog("[apx] tactics stuck uid %u at %.1f,%.1f tile %d,%d goal %.1f,%.1f steer %d route %d/%d kind %d bump %d",
			my->getUID(), my->x, my->y, (int)floor(my->x / 16), (int)floor(my->y / 16), gx, gy, (int)s.lastSteer,
			(int)s.routeAt, (int)s.route.size(), (int)s.routeKind, s.lastBump);
		int n = 0;
		for ( node_t* nd = map.entities ? map.entities->first : nullptr; nd && n < 12; nd = nd->next )
		{
			Entity* e = (Entity*)nd->element;
			if ( !e || e == my || fabs(e->x - my->x) > 24 || fabs(e->y - my->y) > 24 ) { continue; }
			++n;
			printlog("[apx] tactics stuck   near sprite %d %s passable %d at %.1f,%.1f size %d,%d", (int)e->sprite,
				e->behavior == &actMonster ? "monster" : e->behavior == &actPlayer ? "player" : e->behavior == &actDoor ? "door"
				: e->behavior == &actDoorFrame ? "doorframe" : "other", e->flags[PASSABLE] ? 1 : 0, e->x, e->y, (int)e->sizex, (int)e->sizey);
		}
	}

	bool moveTo(Entity* my, TacticsState& s, double gx, double gy, double speed, double& dist2)
	{
		double dx = gx - my->x, dy = gy - my->y, len = hypot(dx, dy);
		if ( len <= HOLD_EPS ) { s.stillSince = 0; s.stuckCounted = false; return false; }
		double v = std::min(speed, len);
		MONSTER_VELX = dx / len * v;
		MONSTER_VELY = dy / len * v;
		double ox = my->x, oy = my->y;
		dist2 = clipMove(&my->x, &my->y, MONSTER_VELX, MONSTER_VELY, my);
		s.lastBump = hit.entity ? (int)hit.entity->sprite : -1; // clipMove clears hit first: this move's own contact
		bumpDoor(my, s);
		++C.steer_moves;
		++C.steer_ticks;
		s.lastMoved = hypot(my->x - ox, my->y - oy);
		if ( s.lastMoved < 0.05 )
		{
			if ( !s.stillSince ) { s.stillSince = ticks; }
			else if ( ticks - s.stillSince >= (Uint32)TICKS_PER_SECOND )
			{
				if ( !s.stuckCounted ) { s.stuckCounted = true; ++C.stuck_episodes; logStuck(my, s, gx, gy); }
				++C.stuck; // ticks stuck (no movement for 1 s while steering toward a goal)
			}
		}
		else { s.stillSince = 0; s.stuckCounted = false; }
		return true;
	}

	// The reach zone holds slot holders only: a creature without a reach slot walks round it at keepOut() and leaves it
	// radially; a slot holder at the wrong angle steps out, walks round and enters radially at its own slot angle.
	void routeAround(Entity* target, Entity* my, double& gx, double& gy, bool ownSlot)
	{
		double keepR = keepOut(bodyR(target));
		double mx = my->x - target->x, myy = my->y - target->y, dMe = hypot(mx, myy);
		double aMe = atan2(myy, mx), aG = atan2(gy - target->y, gx - target->x);
		double dA = remainder(aG - aMe, 2 * PI);
		if ( ownSlot )
		{
			if ( fabs(dA) <= 0.35 ) { return; }   // in line with its slot: straight in
			if ( dMe < keepR - 1 )
			{
				gx = target->x + cos(aMe) * (keepR + HOLD_EPS + 1); gy = target->y + sin(aMe) * (keepR + HOLD_EPS + 1);
				++C.route_around;
				return;
			}
		}
		else
		{
			if ( dMe < keepR - 1 )
			{
				gx = target->x + cos(aMe) * (keepR + HOLD_EPS + 1); gy = target->y + sin(aMe) * (keepR + HOLD_EPS + 1);
				++C.route_around;
				return;
			}
			double sx = gx - my->x, sy = gy - my->y, L2 = sx * sx + sy * sy;
			double t = L2 > 0 ? std::max(0.0, std::min(1.0, ((target->x - my->x) * sx + (target->y - my->y) * sy) / L2)) : 0;
			if ( hypot(my->x + t * sx - target->x, my->y + t * sy - target->y) >= keepR ) { return; }
		}
		double step = std::min(fabs(dA), 0.6) * (dA < 0 ? -1 : 1);
		double r = std::max(dMe, keepR);
		gx = target->x + cos(aMe + step) * r; gy = target->y + sin(aMe + step) * r;
		++C.route_around;
	}

	// The straight way to a goal crosses a wall, or the reach zone: a slot holder off its slot's bearing (it enters
	// radially), anyone else whose segment passes within keepOut of the target. Such a walk is routed (planGoalRoute).
	bool needsRoute(Entity* target, Entity* my, double gx, double gy, bool ownSlot)
	{
		if ( !wallClear(my, my->x, my->y, gx, gy) ) { return true; }
		if ( ownSlot )
		{
			double dA = remainder(atan2(gy - target->y, gx - target->x) - atan2(my->y - target->y, my->x - target->x), 2 * PI);
			return fabs(dA) > 0.35;
		}
		double sx = gx - my->x, sy = gy - my->y, L2 = sx * sx + sy * sy;
		double t = L2 > 0 ? std::max(0.0, std::min(1.0, ((target->x - my->x) * sx + (target->y - my->y) * sy) / L2)) : 0;
		return hypot(my->x + t * sx - target->x, my->y + t * sy - target->y) < keepOut(bodyR(target));
	}

	// ---------------------------------------------------------------- reach slots and ring
	static void slotGeometry(Entity* target, TargetSlots& ts, double rA)
	{
		double rT = bodyR(target), w = 2 * rA + elbowFor(rA);
		if ( !ts.assigned ) { ts.base = target->yaw; } // world-fixed per engagement: a turning target does not spin the crowd
		ts.R = rT + CHASE;                              // where a vanilla melee creature stops: trace distance CHASE
		ts.n = std::max(1, (int)floor(2 * PI * ts.R / w));
		ts.ringR = RING_R;
		ts.k = std::max(1, (int)floor(2 * PI * ts.ringR / w));
		ts.valid.assign(ts.n, 0);
		ts.ringValid.assign(ts.k, 0);
		ts.validN = ts.ringValidN = 0;
		for ( int i = 0; i < ts.n; ++i )
		{
			double a = ts.base + i * 2 * PI / ts.n, px = target->x + ts.R * cos(a), py = target->y + ts.R * sin(a);
			if ( walkable((int)floor(px / 16), (int)floor(py / 16)) && wallClear(target, target->x, target->y, px, py) ) { ts.valid[i] = 1; ++ts.validN; }
		}
		for ( int j = 0; j < ts.k; ++j )
		{
			double a = ts.base + j * 2 * PI / ts.k, px = target->x + ts.ringR * cos(a), py = target->y + ts.ringR * sin(a);
			if ( walkable((int)floor(px / 16), (int)floor(py / 16)) && wallClear(target, target->x, target->y, px, py) ) { ts.ringValid[j] = 1; ++ts.ringValidN; }
		}
	}

	int reachSlotsFor(Entity* target, Entity* attacker)
	{
		TargetSlots ts;
		slotGeometry(target, ts, attacker ? bodyR(attacker) : 4.0);
		return ts.validN;
	}

	struct Cand { double d; Uint32 uid; Entity* e; TacticsState* s; };

	static void assignTarget(Entity* target)
	{
		Uint32 tuid = target->getUID();
		TargetSlots& ts = T[tuid];
		std::vector<Cand> cand;
		double rA = 0;
		for ( node_t* nd = map.creatures ? map.creatures->first : nullptr; nd; nd = nd->next )
		{
			Entity* c = (Entity*)nd->element;
			if ( !c || c->behavior != &actMonster || (Uint32)c->monsterTarget != tuid ) { continue; }
			Stat* cs = c->getStats();
			TacticsState* s = find(c);
			if ( !cs || cs->HP <= 0 || !s ) { continue; }
			double d = hypot(c->x - target->x, c->y - target->y);
			if ( s->stance != STANCE_NORMAL || s->flank == 1 || !eligible(c, *cs) || c->hasRangedWeapon() || d > ENGAGE_R + TILE )
			{
				s->reachIdx = -1; s->ringIdx = -1; s->ringSlot = 0;
				continue;
			}
			cand.push_back({ d, c->getUID(), c, s });
			rA = std::max(rA, bodyR(c));
		}
		slotGeometry(target, ts, rA > 0 ? rA : 4.0);
		ts.assignTick = ticks;
		ts.assigned = true;
		++C.reach_slots_n;
		C.reach_slots_sum += ts.validN;
		std::sort(cand.begin(), cand.end(), [](const Cand& a, const Cand& b) { return a.d < b.d || (a.d == b.d && a.uid < b.uid); });
		std::vector<Uint32> holder(ts.n, 0), ringHolder(ts.k, 0);
		// room is bodies, not slot indices: count every monster on this target already inside reach (holders,
		// creatures stepping out, arrivals) and admit creatures from outside only into the remaining room
		const double reachR = bodyR(target) + STRIKERANGE;
		int inZone = 0;
		for ( node_t* nd = map.creatures ? map.creatures->first : nullptr; nd; nd = nd->next )
		{
			Entity* c = (Entity*)nd->element;
			Stat* cs = c ? c->getStats() : nullptr;
			if ( c && c->behavior == &actMonster && cs && cs->HP > 0 && (Uint32)c->monsterTarget == tuid
				&& hypot(c->x - target->x, c->y - target->y) < reachR ) { ++inZone; }
		}
		int admitted = 0;
		// 1. holders keep a still-valid slot (hysteresis) unless blocked by an ally for too long
		for ( auto& c : cand )
		{
			TacticsState& s = *c.s;
			bool keep = s.slotTarget == tuid && s.reachIdx >= 0 && s.reachIdx < ts.n && ts.valid[s.reachIdx] && !holder[s.reachIdx];
			if ( keep && !s.wantReposition ) { holder[s.reachIdx] = c.uid; }
			else if ( !keep ) { s.reachIdx = -1; }
		}
		// 2. the rest, nearest first: the free slot closest to the creature's own bearing
		for ( auto& c : cand )
		{
			TacticsState& s = *c.s;
			if ( s.reachIdx >= 0 && holder[s.reachIdx] == c.uid ) { continue; }
			double bear = atan2(c.e->y - target->y, c.e->x - target->x);
			int best = -1;
			double bestA = 1e9;
			for ( int i = 0; i < ts.n; ++i )
			{
				if ( !ts.valid[i] || holder[i] || (s.wantReposition && i == s.reachIdx) ) { continue; }
				double a = angDiff(ts.base + i * 2 * PI / ts.n, bear);
				if ( a < bestA ) { bestA = a; best = i; }
			}
			if ( s.wantReposition )
			{
				if ( best >= 0 ) { ++C.reposition; }
				else
				{
					++C.gap_wait; // no other room: keep the slot and wait for the line to clear
					if ( s.reachIdx >= 0 && !holder[s.reachIdx] ) { best = s.reachIdx; }
				}
				s.wantReposition = false;
				s.blockedSince = 0;
			}
			bool inside = c.d < reachR;
			if ( best >= 0 && !inside && inZone + admitted >= ts.validN ) { best = -1; ++C.reach_full; } // no room yet: wait on the ring
			if ( best >= 0 )
			{
				if ( s.reachIdx != best ) { ++C.reach_assign; }
				if ( !inside ) { ++admitted; }
				holder[best] = c.uid;
				s.reachIdx = best;
				s.ringIdx = -1; s.ringSlot = 0;
			}
			else
			{
				s.reachIdx = -1;
				++C.reach_overflow_creatures;
			}
			s.slotTarget = tuid;
		}
		// 3. the overflow waits on the ring, front slots first (cost: walk + up to one tile for the back)
		for ( auto& c : cand )
		{
			TacticsState& s = *c.s;
			s.slotTarget = tuid;
			if ( s.reachIdx >= 0 || !K.ring ) { s.ringIdx = -1; s.ringSlot = 0; continue; }
			int best = -1;
			double bestCost = 1e18;
			for ( int j = 0; j < ts.k; ++j )
			{
				if ( !ts.ringValid[j] || ringHolder[j] ) { continue; }
				double a = ts.base + j * 2 * PI / ts.k;
				double px = target->x + ts.ringR * cos(a), py = target->y + ts.ringR * sin(a);
				double cost = hypot(px - c.e->x, py - c.e->y) + TILE * angDiff(a, target->yaw) / PI;
				if ( s.ringIdx == j ) { cost -= TILE; }
				if ( cost < bestCost ) { bestCost = cost; best = j; }
			}
			if ( best >= 0 )
			{
				if ( s.ringIdx != best ) { ++C.ring_assign; }
				ringHolder[best] = c.uid;
				s.ringIdx = best;
				s.ringSlot = best + 1;
			}
			else
			{
				s.ringIdx = -1; s.ringSlot = 0;
				++C.ring_slot_fail;
			}
		}
	}

	// The point a managed creature walks to: its reach slot, its ring slot (feints aside), or with neither the queue point
	// just outside reach behind the reach slot nearest its bearing (card BF3-3: "keeps pathing toward a free slot or
	// waits behind"; creatures from one side queue together). False when the creature is not placed on this target.
	bool goalFor(Entity* my, TacticsState& s, Entity* target, double& gx, double& gy, bool& ownSlot)
	{
		ownSlot = false;
		auto it = T.find(target->getUID());
		if ( it == T.end() || !it->second.assigned || s.slotTarget != (Uint32)target->getUID() ) { return false; }
		TargetSlots& ts = it->second;
		if ( s.reachIdx >= 0 && s.reachIdx < ts.n )
		{
			double a = ts.base + s.reachIdx * 2 * PI / ts.n;
			gx = target->x + ts.R * cos(a); gy = target->y + ts.R * sin(a);
			ownSlot = true;
			return true;
		}
		if ( K.ring && s.ringIdx >= 0 && s.ringIdx < ts.k )
		{
			double a = ts.base + s.ringIdx * 2 * PI / ts.k;
			gx = target->x + ts.ringR * cos(a); gy = target->y + ts.ringR * sin(a);
			return true;
		}
		double waitR = keepOut(bodyR(target)) + 2;
		double bear = atan2(my->y - target->y, my->x - target->x), bestA = 1e9, a = bear;
		for ( int i = 0; i < ts.n; ++i )
		{
			double ai = ts.base + i * 2 * PI / ts.n;
			if ( ts.valid[i] && angDiff(ai, bear) < bestA ) { bestA = angDiff(ai, bear); a = ai; }
		}
		gx = target->x + cos(a) * waitR; gy = target->y + sin(a) * waitR;
		return true;
	}

	// a placed creature cut off from its target by a wall: it still has a way to its goal (a clear line or a route)
	static bool routedToGoal(Entity* my, TacticsState& s, Entity* target)
	{
		double gx, gy;
		bool own;
		if ( !goalFor(my, s, target, gx, gy, own) ) { return false; }
		return wallClear(my, my->x, my->y, gx, gy) || planGoalRoute(my, s, target, gx, gy);
	}

	// Every tick, not only at decide: the engine's ATTACK state sends a creature to PATH whenever its own packmates
	// block its line (and to WAIT, which drops the target, when a HUNT path ends). Between decides that walked managed
	// creatures back into the crowd. A creature that holds a reach slot, a ring slot or a flank stays in its fight.
	static void holdInFight(Entity* my, TacticsState& s)
	{
		if ( s.stance != STANCE_NORMAL ) { return; }
		bool flanking = K.packs && s.flank == 1;
		bool placed = K.reach && (s.reachIdx >= 0 || s.ringIdx >= 0);
		if ( !flanking && !placed ) { return; }
		Uint32 tu = flanking ? s.flankTarget : s.slotTarget;
		Entity* t = tu ? uidToEntity(tu) : nullptr;
		Stat* ts = t ? t->getStats() : nullptr;
		if ( !t || !ts || ts->HP <= 0 ) { return; }
		if ( (Uint32)my->monsterTarget != tu )
		{
			my->monsterAcquireAttackTarget(*t, MONSTER_STATE_ATTACK); // the engine released it in WAIT
			if ( (Uint32)my->monsterTarget != tu ) { return; }
			++C.pack_reacquire;
		}
		if ( my->monsterState == MONSTER_STATE_PATH || my->monsterState == MONSTER_STATE_HUNT || my->monsterState == MONSTER_STATE_WAIT )
		{
			double d = hypot(t->x - my->x, t->y - my->y);
			bool clear = d <= ENGAGE_R + TILE && wallClear(my, my->x, my->y, t->x, t->y);
			// behind a wall: a slot or ring holder with a way to its slot walks it (steer), not the engine's path into
			// the crowd (and a failed chase path no longer leaves it standing out of the fight)
			bool routed = !flanking && !clear && d <= ENGAGE_R + TILE && routedToGoal(my, s, t);
			if ( flanking || clear || routed )
			{
				my->monsterState = MONSTER_STATE_ATTACK;
				serverUpdateEntitySkill(my, 0);
				++C.promoted;
				if ( routed ) { ++C.routed_promoted; }
			}
		}
	}

	// ---------------------------------------------------------------- hooks
	void tick(Entity* my)
	{
		if ( !active ) { return; }
		if ( multiplayer == CLIENT ) { return; }
		++C.tactics_calls;
		Stat* st = my->getStats();
		if ( !st || st->HP <= 0 || !eligible(my, *st) ) { return; }
		Uint32 uid = my->getUID();
		TacticsState& s = S[uid];
		if ( s.self != my ) { s = TacticsState(); s.self = my; }
		if ( !s.seen ) { s.seen = true; s.homeX = my->x; s.homeY = my->y; s.morale = MORALE_START; }
		holdInFight(my, s);
		if ( (ticks + uid) % DECIDE != 0 ) { return; }
		Timer tm;
		HitSave hs;
		++C.decides;
		s.decideTick = ticks;
		Entity* target = my->monsterTarget ? uidToEntity((Uint32)my->monsterTarget) : nullptr;
		if ( target && target->behavior != &actPlayer && target->behavior != &actMonster ) { target = nullptr; }
		if ( !target && K.packs && s.packId && s.stance == STANCE_NORMAL )
		{
			// one pack, one fight: a member whose target was released (the engine releases it every tick in WAIT,
			// e.g. when its own packmates block its sight) takes up the pack's target again
			if ( Entity* pt = packTarget(my, s) )
			{
				my->monsterAcquireAttackTarget(*pt, MONSTER_STATE_PATH);
				if ( (Uint32)my->monsterTarget == pt->getUID() ) { target = pt; ++C.pack_reacquire; }
			}
		}
		packDecide(my, *st, s, target);
		bool managed = false;
		if ( target && K.reach && s.stance == STANCE_NORMAL && s.flank != 1 && !my->hasRangedWeapon() )
		{
			double d = hypot(target->x - my->x, target->y - my->y);
			if ( d <= ENGAGE_R + TILE )
			{
				managed = true;
				// a creature hunting along a path near its target, with only bodies (no wall) between: engage it
				if ( (my->monsterState == MONSTER_STATE_HUNT || my->monsterState == MONSTER_STATE_PATH)
					&& !my->shouldRetreat(*st) && wallClear(my, my->x, my->y, target->x, target->y) )
				{
					my->monsterState = MONSTER_STATE_ATTACK;
					serverUpdateEntitySkill(my, 0);
					++C.promoted;
				}
				TargetSlots& ts = T[target->getUID()];
				if ( !ts.assigned || ticks - ts.assignTick >= (Uint32)DECIDE ) { assignTarget(target); }
				if ( s.reachIdx >= 0 && K.allyavoid && d < bodyR(target) + STRIKERANGE + 4 )
				{
					if ( corridorBlocked(my, target) )
					{
						if ( !s.blockedSince ) { s.blockedSince = ticks; }
						else if ( ticks - s.blockedSince > (Uint32)BLOCK_TICKS ) { s.wantReposition = true; }
					}
					else { s.blockedSince = 0; s.wantReposition = false; }
				}
				if ( s.ringIdx >= 0 ) { ++C.ring_holds; }
			}
		}
		if ( !managed && s.stance == STANCE_NORMAL && s.flank != 1 ) { s.reachIdx = -1; s.ringIdx = -1; s.ringSlot = 0; }
		if ( gProbeUs > 0 ) { while ( tm.us() < gProbeUs ) {} } // CONTROL only: the probe must see this
		addDecideUs(tm.us());
	}

	bool keepEngaged(Entity* my, Entity* target, double& dist)
	{
		if ( !active || multiplayer == CLIENT || !target ) { return false; }
		TacticsState* s = find(my);
		if ( !s || s->stance != STANCE_NORMAL ) { return false; }
		bool flanking = s->flank == 1;
		if ( !K.reach && !flanking ) { return false; }
		Stat* st = my->getStats();
		if ( !st || !eligible(my, *st) || my->hasRangedWeapon() || my->shouldRetreat(*st) ) { return false; }
		double d = hypot(target->x - my->x, target->y - my->y);
		if ( d > ENGAGE_R + TILE && !flanking ) { return false; }
		Entity* blocker = hit.entity;
		if ( !flanking && !(blocker && blocker->behavior == &actMonster) )
		{
			// a wall or door between: a slot or ring holder with a way to its slot keeps walking it (steer); the engine
			// gets a distance it cannot swing at (a whip reaches 1.5 x STRIKERANGE), so no blow lands on the wall
			bool placed = K.reach && (s->reachIdx >= 0 || s->ringIdx >= 0);
			if ( placed && routedToGoal(my, *s, target) )
			{
				dist = std::max(d - bodyR(target), 2.0 * STRIKERANGE);
				hit.entity = target;
				++C.routed_engaged;
				return true;
			}
			return false; // vanilla re-paths
		}
		hit_t saved = hit;
		double td = lineTraceTarget(my, my->x, my->y, atan2(target->y - my->y, target->x - my->x), d + 2 * TILE, 0, false, target);
		if ( hit.entity != target )
		{
			if ( !flanking ) { hit = saved; return false; }
			td = std::max(0.0, d - bodyR(target));
		}
		dist = td;
		hit.entity = target;
		++C.keep_engaged;
		return true;
	}

	bool steer(Entity* my, Stat& st, Entity* target, double dist, double weightratio, double& dist2)
	{
		(void)dist;
		if ( !active || multiplayer == CLIENT || !target ) { return false; }
		TacticsState* sp = find(my);
		if ( !sp ) { return false; }
		TacticsState& s = *sp;
		if ( !eligible(my, st) || my->hasRangedWeapon() || my->monsterSpecialState != 0 ) { return false; }
		if ( st.getEffectActive(EFF_KNOCKBACK) || st.getEffectActive(EFF_ROOTED) || monsterIsImmobileTurret(my, &st) ) { return false; }
		Timer tm;
		double d = hypot(target->x - my->x, target->y - my->y);
		double speed = .045 * (my->monsterGetDexterityForMovement() + 10) * weightratio;
		s.faceTarget = false;
		s.faceMove = false;
		s.lastSteer = STEER_PACK;
		if ( !packSteer(my, st, s, target, d, speed, dist2) )
		{
			if ( !K.reach || my->shouldRetreat(st) || d > ENGAGE_R + TILE || s.slotTarget != (Uint32)target->getUID() ) { return false; }
			auto it = T.find(target->getUID());
			if ( it == T.end() || !it->second.assigned ) { return false; }
			TargetSlots& ts = it->second;
			if ( my->monsterAttack != 0 )
			{
				// mid-swing (wind-up 9 ticks, then the blow): stand and face the target, or the blow lands on a packmate
				s.lastSteer = STEER_SWING;
				stand(my, target, speed);
				++C.steer_holds;
				s.steeredTick = ticks;
				addMoveUs(tm.us());
				return true;
			}
			double gx = my->x, gy = my->y;
			bool ownSlot = false;
			goalFor(my, s, target, gx, gy, ownSlot);
			bool ring = !ownSlot && K.ring && s.ringIdx >= 0 && s.ringIdx < ts.k;
			if ( ring && ticks >= s.nextFeint )
			{
				if ( s.nextFeint ) { s.feintUntil = ticks + FEINT_TICKS; ++C.feints; }
				s.nextFeint = ticks + FEINT_PERIOD + (my->getUID() % 25);
			}
			s.goalX = gx; s.goalY = gy;
			s.faceTarget = ownSlot ? d < 2 * TILE : true;
			bool moved = false;
			const bool inZone = d < keepOut(bodyR(target)) - 1;
			if ( ownSlot && inZone && wallClear(my, my->x, my->y, gx, gy)
				&& angDiff(atan2(gy - target->y, gx - target->x), atan2(my->y - target->y, my->x - target->x)) <= PI / 2 )
			{
				// already inside reach, on its slot's side: straight to the slot (no step out and back in)
				s.lastSteer = STEER_STRAIGHT;
				moved = moveTo(my, s, gx, gy, speed, dist2);
			}
			else if ( !ownSlot && inZone )
			{
				// no slot in reach: out of the zone radially, or by its route when a wall stands in the radial way
				double ox = gx, oy = gy;
				routeAround(target, my, ox, oy, false);
				if ( !wallClear(my, my->x, my->y, ox, oy) && planGoalRoute(my, s, target, gx, gy) )
				{
					s.lastSteer = STEER_ROUTE;
					++C.goal_route_steps;
					moved = followRoute(my, s, speed, dist2, gx, gy, nullptr);
				}
				else
				{
					s.lastSteer = STEER_OUT;
					++C.step_out;
					moved = moveTo(my, s, ox, oy, speed, dist2);
				}
			}
			else if ( needsRoute(target, my, gx, gy, ownSlot) )
			{
				if ( planGoalRoute(my, s, target, gx, gy) )
				{
					// round the zone and any wall by the engine's A* (zone tiles penalised); the slot is entered from its side
					s.lastSteer = STEER_ROUTE;
					++C.goal_route_steps;
					moved = followRoute(my, s, speed, dist2, gx, gy, nullptr);
				}
				else
				{
					routeAround(target, my, gx, gy, ownSlot); // no path to the goal tile (counted): the arc round the zone
					s.lastSteer = STEER_ARC;
					++C.arc_fallback;
					moved = moveTo(my, s, gx, gy, speed, dist2);
				}
			}
			else
			{
				if ( ring && ticks < s.feintUntil ) // feint: a hop in and back
				{
					double a = atan2(gy - target->y, gx - target->x), r = ts.ringR - FEINT_HOP;
					gx = target->x + r * cos(a); gy = target->y + r * sin(a);
				}
				s.lastSteer = STEER_STRAIGHT;
				moved = moveTo(my, s, gx, gy, speed, dist2);
			}
			if ( !moved )
			{
				stand(my, target, speed);
				++C.steer_holds;
				s.faceTarget = false;
				s.lastSteer = STEER_AT_GOAL;
			}
		}
		s.steeredTick = ticks;
		addMoveUs(tm.us());
		return true;
	}

	void face(Entity* my, double& dir)
	{
		if ( !active ) { return; }
		TacticsState* s = find(my);
		if ( !s || s->steeredTick != ticks ) { return; }
		if ( s->faceMove ) { dir = my->yaw - atan2(MONSTER_VELY, MONSTER_VELX); return; } // turn and run (retreat)
		if ( !s->faceTarget ) { return; }
		Entity* t = uidToEntity((Uint32)my->monsterTarget);
		if ( t ) { dir = my->yaw - atan2(t->y - my->y, t->x - my->x); }
	}

	static Uint32 mix(Uint32 a, Uint32 b)
	{
		Uint32 h = a * 2654435761u ^ (b + 0x9e3779b9u + (a << 6) + (a >> 2));
		h ^= h >> 15; h *= 0x2c1b3c6du; h ^= h >> 12; h *= 0x297a2d39u; h ^= h >> 15;
		return h;
	}

	Sint32 swingReset(Entity* my, Entity* target, double dist)
	{
		if ( !active && !bf3::on ) { return 0; }
		if ( bf3::on ) { bf3::onSwingAttempt(my, target, dist); }
		if ( !active || !K.interleave || multiplayer == CLIENT ) { return 0; }
		Stat* st = my->getStats();
		if ( !st || !eligible(my, *st) ) { return 0; }
		int j = (int)(mix(my->getUID(), ticks) % JITTER);
		++C.swing_resets;
		C.jitter_sum += j;
		return -j;
	}

	void onSwingTrace(Entity* my, Entity* target, Entity* hitEntity)
	{
		if ( !active && !bf3::on ) { return; }
		if ( hitEntity == target || !hitEntity || hitEntity->behavior != &actMonster || multiplayer == CLIENT ) { return; }
		if ( active )
		{
			++C.ally_blocked; ++C.ally_blocked_thin;
			TacticsState* s = find(my);
			if ( s && !s->blockedSince ) { s->blockedSince = ticks; }
		}
		if ( bf3::on ) { bf3::onSwingThin(my, target); }
	}

	bool swingLineBlocked(Entity* my, Entity* target)
	{
		if ( !active || !K.allyavoid || multiplayer == CLIENT ) { return false; }
		Stat* st = my->getStats();
		if ( !st || !eligible(my, *st) || my->hasRangedWeapon() ) { return false; }
		if ( !corridorBlocked(my, target) ) { return false; }
		++C.ally_blocked; ++C.ally_blocked_wide;
		TacticsState* s = find(my);
		if ( s && !s->blockedSince ) { s->blockedSince = ticks; }
		if ( bf3::on ) { bf3::onSwingWide(my, target); }
		return true;
	}

	bool moraleBroken(Entity* my)
	{
		if ( !active || !K.morale || my->behavior != &actMonster ) { return false; }
		TacticsState* s = find(my);
		return s && s->stance != STANCE_NORMAL;
	}

	int holdsReachSlot(Entity* e) { TacticsState* s = find(e); return s ? s->reachIdx : -1; }
	int holdsRingSlot(Entity* e) { TacticsState* s = find(e); return s ? s->ringIdx : -1; }
	bool isFlanking(Entity* e) { TacticsState* s = find(e); return s && s->flank == 1; }
	int stanceOf(Entity* e) { TacticsState* s = find(e); return s ? s->stance : -1; }
	int moraleOf(Entity* e) { TacticsState* s = find(e); return s ? s->morale : -1; }
	Uint32 packOf(Entity* e) { TacticsState* s = find(e); return s ? s->packId : 0; }
}
}
