// apx/tactics_pack.cpp -- BF3 packs and roles, flank route B (route A's tiles penalised in A*), morale,
// retreat and regroup (card BF3-6b and BF3-7). Calls for help (alertAlliesOnBeingHit) are NOT built here.
#include "tactics_int.hpp"
#include <algorithm>
#include <cmath>

namespace apx
{
namespace tactics
{
	static Uint32 gPackSeq = 1000;      // level packs; the BF3 trial uses ids below 1000
	static std::vector<char> gPenalty;  // route-A tiles of the flank being planned (index x + y * map.width)
	static bool gPenaltyOn = false;

	void packsReset() { gPackSeq = 1000; gPenalty.clear(); gPenaltyOn = false; }

	int pathPenalty(int x, int y)
	{
		if ( !gPenaltyOn ) { return 0; }
		if ( x < 0 || y < 0 || x >= (int)map.width || y >= (int)map.height ) { return 0; }
		return gPenalty[x + y * map.width] ? FLANK_PENALTY : 0;
	}

	// F5 fills this (parley). It must never hold or harm anyone (A4, PRIME_DIRECTIVE).
	static void onSurrender(Entity* my) { (void)my; }

	void assignPack(Entity* e, Uint32 packId, uint8_t role, double homeX, double homeY)
	{
		TacticsState& s = S[e->getUID()];
		s = TacticsState();
		s.self = e;
		s.seen = true;
		s.packId = packId;
		s.role = role;
		s.homeX = homeX;
		s.homeY = homeY;
		s.morale = MORALE_START;
	}

	static void formPack(Entity* my, Stat& st)
	{
		std::vector<Entity*> members{ my };
		for ( size_t i = 0; i < members.size() && members.size() < 8; ++i )
		{
			Entity* m = members[i];
			Stat* ms = m->getStats();
			for ( node_t* nd = map.creatures ? map.creatures->first : nullptr; nd && members.size() < 8; nd = nd->next )
			{
				Entity* c = (Entity*)nd->element;
				if ( !c || c->behavior != &actMonster || std::find(members.begin(), members.end(), c) != members.end() ) { continue; }
				Stat* cs = c->getStats();
				if ( !cs || cs->HP <= 0 || !eligible(c, *cs) ) { continue; }
				TacticsState* o = find(c);
				if ( o && o->packId ) { continue; }
				bool linked = (cs->leader_uid && cs->leader_uid == m->getUID()) || (ms && ms->leader_uid && ms->leader_uid == c->getUID());
				bool near = cs->type == st.type && hypot(c->x - m->x, c->y - m->y) <= 4 * TILE;
				if ( linked || near ) { members.push_back(c); }
			}
		}
		Entity* leader = members[0];
		for ( Entity* m : members )
		{
			Stat* a = m->getStats();
			Stat* b = leader->getStats();
			if ( a->MAXHP > b->MAXHP || (a->MAXHP == b->MAXHP && m->getUID() < leader->getUID()) ) { leader = m; }
		}
		Entity* flanker = nullptr;
		if ( members.size() >= 3 )
		{
			for ( Entity* m : members )
			{
				if ( m == leader ) { continue; }
				if ( !flanker || m->getDEX() > flanker->getDEX() || (m->getDEX() == flanker->getDEX() && m->getUID() < flanker->getUID()) ) { flanker = m; }
			}
		}
		double hx = 0, hy = 0;
		for ( Entity* m : members ) { hx += m->x; hy += m->y; }
		hx /= members.size(); hy /= members.size();
		Uint32 id = ++gPackSeq;
		for ( Entity* m : members )
		{
			TacticsState& s = S[m->getUID()];
			if ( s.self != m ) { s = TacticsState(); s.self = m; }
			if ( !s.seen ) { s.seen = true; s.morale = MORALE_START; }
			s.packId = id;
			s.role = m == leader ? ROLE_LEADER : (m == flanker ? ROLE_FLANKER : ROLE_BRUTE);
			s.homeX = hx; s.homeY = hy;
		}
		++C.packs_formed;
	}

	void moraleEvent(Entity* e, TacticsState& s, int delta)
	{
		(void)e;
		++C.morale_events;
		int m = std::max(0, std::min(100, (int)s.morale + delta));
		s.morale = (int8_t)m;
		if ( s.stance == STANCE_NORMAL && m < MORALE_BREAK )
		{
			s.stance = STANCE_RETREAT;
			s.retreatStart = ticks;
			s.lastHomePath = 0;
			s.reachIdx = -1; s.ringIdx = -1; s.ringSlot = 0;
			if ( s.flank == 1 ) { s.flank = 3; ++C.flank_failed; }
			s.route.clear(); s.routeKind = ROUTE_NONE;
			++C.morale_breaks;
			++C.retreats;
		}
	}

	static void onDeath(Entity* e, TacticsState& s)
	{
		if ( !s.packId ) { return; }
		bool leader = s.role == ROLE_LEADER;
		for ( auto& kv : S )
		{
			if ( kv.first == e->getUID() || kv.second.packId != s.packId ) { continue; }
			Entity* m = uidToEntity(kv.first);
			if ( !m || kv.second.self != m ) { continue; }
			Stat* ms = m->getStats();
			if ( !ms || ms->HP <= 0 ) { continue; }
			if ( leader ) { moraleEvent(m, kv.second, -40); }
			else if ( hypot(m->x - e->x, m->y - e->y) <= 8 * TILE && wallClear(m, m->x, m->y, e->x, e->y) ) { moraleEvent(m, kv.second, -8); }
		}
	}

	void onHPChange(Entity* e, int oldHP, int newHP)
	{
		if ( bf3::on ) { bf3::onCreatureHP(e, oldHP, newHP); }
		if ( !active || !K.morale || multiplayer == CLIENT || e->behavior != &actMonster ) { return; }
		TacticsState* s = find(e);
		Stat* st = e->getStats();
		if ( !s || !st ) { return; }
		if ( newHP <= 0 && oldHP > 0 ) { onDeath(e, *s); return; }
		if ( !s->lowHp && newHP * 10 < st->MAXHP * 3 && oldHP * 10 >= st->MAXHP * 3 )
		{
			s->lowHp = true;
			moraleEvent(e, *s, -15);
		}
	}

	// a player within 8 tiles that closed in by more than 2 units since the last decide (0.2 s)
	static bool pursued(Entity* my, TacticsState& s)
	{
		double best = 1e9;
		for ( int p = 0; p < MAXPLAYERS; ++p )
		{
			if ( players[p] && players[p]->entity ) { best = std::min(best, hypot(players[p]->entity->x - my->x, players[p]->entity->y - my->y)); }
		}
		bool closing = best < s.lastThreatDist - 2.0;
		s.lastThreatDist = best;
		return best <= 8 * TILE && closing;
	}

	static bool inBackArc(Entity* my, Entity* target)
	{
		return angDiff(atan2(my->y - target->y, my->x - target->x), target->yaw + PI) <= PI / 3; // back 120 degrees
	}

	static void flankFail(TacticsState& s)
	{
		s.flank = 3;
		++C.flank_failed;
		s.route.clear();
		s.routeKind = ROUTE_NONE;
	}

	static void copyPath(list_t* p, std::vector<int>& out)
	{
		out.clear();
		for ( node_t* n = p ? p->first : nullptr; n; n = n->next )
		{
			pathnode_t* pn = (pathnode_t*)n->element;
			if ( pn ) { out.push_back(pn->x + pn->y * (int)map.width); }
		}
	}

	static void freePath(list_t* p)
	{
		if ( p ) { list_FreeAll(p); free(p); }
	}

	static void flankDecide(Entity* my, TacticsState& s, Entity* target)
	{
		Uint32 tuid = target->getUID();
		double d = hypot(target->x - my->x, target->y - my->y);
		if ( s.flank == 1 )
		{
			if ( s.flankTarget != tuid ) { flankFail(s); return; }
			if ( d <= 2 * TILE && inBackArc(my, target) )
			{
				s.flank = 2;
				++C.flank_arrived;
				s.route.clear(); s.routeKind = ROUTE_NONE;
				s.reachIdx = -1; // the next assignment gives it the free slot nearest its bearing: a rear slot
				return;
			}
			if ( hypot(my->x - s.progressX, my->y - s.progressY) > 8 ) { s.progressX = my->x; s.progressY = my->y; s.progressTick = ticks; }
			else if ( ticks - s.progressTick > 75 ) { flankFail(s); return; }
			if ( ticks - s.flankStart > 10 * TICKS_PER_SECOND ) { flankFail(s); }
			return;
		}
		if ( s.flank != 0 && s.flankTarget == tuid ) { return; } // one try per target
		if ( d > 10 * TILE || d < 2.5 * TILE ) { return; }
		int mates = 0;
		for ( auto& kv : S )
		{
			if ( kv.first == my->getUID() || kv.second.packId != s.packId ) { continue; }
			Entity* m = uidToEntity(kv.first);
			Stat* ms = m ? m->getStats() : nullptr;
			if ( m && ms && ms->HP > 0 && (Uint32)m->monsterTarget == tuid ) { ++mates; }
		}
		if ( mates < 2 ) { return; }
		s.flankTarget = tuid;
		// goal: an open tile in the back arc, 1.5 to 2.5 tiles out, in clear sight of the target
		static const double offs[] = { 0, 0.35, -0.35, 0.7, -0.7, 0.95, -0.95 };
		static const double rads[] = { 2.0, 1.5, 2.5 };
		double back = target->yaw + PI;
		int gx = -1, gy = -1;
		for ( double r : rads )
		{
			for ( double o : offs )
			{
				int tx = (int)floor((target->x + r * TILE * cos(back + o)) / 16);
				int ty = (int)floor((target->y + r * TILE * sin(back + o)) / 16);
				if ( walkable(tx, ty) && wallClear(target, target->x, target->y, tx * 16 + 8, ty * 16 + 8) ) { gx = tx; gy = ty; break; }
			}
			if ( gx >= 0 ) { break; }
		}
		if ( gx < 0 ) { flankFail(s); return; }
		HitSave hs;
		int mx = (int)floor(my->x / 16), my_ = (int)floor(my->y / 16);
		list_t* a = generatePath(mx, my_, (int)floor(target->x / 16), (int)floor(target->y / 16), my, target, GeneratePathTypes::GENERATE_PATH_TO_HUNT_MONSTER_TARGET);
		gPenalty.assign((size_t)map.width * map.height, 0);
		std::vector<int> routeA;
		copyPath(a, routeA);
		freePath(a);
		for ( int t : routeA ) { gPenalty[t] = 1; }
		gPenaltyOn = true;
		list_t* b = generatePath(mx, my_, gx, gy, my, nullptr, GeneratePathTypes::GENERATE_PATH_TO_HUNT_MONSTER_TARGET);
		gPenaltyOn = false;
		copyPath(b, s.route);
		freePath(b);
		if ( s.route.empty() ) { flankFail(s); return; }
		s.routeAt = 0;
		s.routeKind = ROUTE_FLANK;
		s.flank = 1;
		s.flankStart = s.progressTick = ticks;
		s.progressX = my->x; s.progressY = my->y;
		s.reachIdx = -1; s.ringIdx = -1; s.ringSlot = 0;
		++C.flank_started;
	}

	void packDecide(Entity* my, Stat& st, TacticsState& s, Entity* target)
	{
		if ( K.packs && !s.packId ) { formPack(my, st); }
		if ( K.morale )
		{
			if ( my->flags[BURNING] ) { if ( !s.burning ) { s.burning = true; moraleEvent(my, s, -10); } }
			else { s.burning = false; }
			if ( target && s.stance == STANCE_NORMAL && !s.outnumbered && s.packId )
			{
				int mates = 0, foes = 0;
				for ( auto& kv : S )
				{
					if ( kv.second.packId != s.packId || kv.second.stance != STANCE_NORMAL ) { continue; }
					Entity* m = uidToEntity(kv.first);
					Stat* ms = m ? m->getStats() : nullptr;
					if ( m && ms && ms->HP > 0 && (Uint32)m->monsterTarget == target->getUID()
						&& hypot(m->x - target->x, m->y - target->y) <= 8 * TILE ) { ++mates; }
				}
				for ( int p = 0; p < MAXPLAYERS; ++p )
				{
					if ( players[p] && players[p]->entity && hypot(players[p]->entity->x - target->x, players[p]->entity->y - target->y) <= 8 * TILE ) { ++foes; }
				}
				if ( mates >= 2 * std::max(1, foes) ) { s.outnumbered = true; moraleEvent(my, s, +5); }
			}
			if ( s.stance == STANCE_RETREAT )
			{
				if ( hypot(s.homeX - my->x, s.homeY - my->y) <= 2 * TILE )
				{
					s.stance = STANCE_REGROUP; s.lastRecover = ticks; s.route.clear(); s.routeKind = ROUTE_NONE;
				}
				else if ( ticks - s.retreatStart > 20 * TICKS_PER_SECOND )
				{
					s.homeX = my->x; s.homeY = my->y; ++C.home_moved; // home unreachable: regroup where it stands
					s.stance = STANCE_REGROUP; s.lastRecover = ticks; s.route.clear(); s.routeKind = ROUTE_NONE;
				}
				else if ( my->monsterState != MONSTER_STATE_ATTACK && (my->monsterState == MONSTER_STATE_PATH || ticks - s.lastHomePath >= 2 * TICKS_PER_SECOND) )
				{
					s.lastHomePath = ticks; // out of the fight: the engine's own path walking takes it home
					if ( my->monsterSetPathToLocation((int)floor(s.homeX / 16), (int)floor(s.homeY / 16), 1, GeneratePathTypes::GENERATE_PATH_DEFAULT) )
					{
						my->monsterState = MONSTER_STATE_HUNT;
						serverUpdateEntitySkill(my, 0);
					}
				}
			}
			if ( s.stance == STANCE_REGROUP )
			{
				if ( my->monsterState == MONSTER_STATE_PATH || my->monsterState == MONSTER_STATE_HUNT )
				{
					my->monsterState = MONSTER_STATE_WAIT; // stay at home while regrouping
					serverUpdateEntitySkill(my, 0);
				}
				bool chased = pursued(my, s);
				if ( !chased && ticks - s.lastRecover >= (Uint32)TICKS_PER_SECOND ) { s.lastRecover = ticks; if ( s.morale < 100 ) { ++s.morale; } }
				if ( s.morale > MORALE_RETURN && !chased )
				{
					s.stance = STANCE_NORMAL;
					s.lowHp = false; s.outnumbered = false;
					++C.regroups;
				}
			}
			if ( s.stance != STANCE_NORMAL && !s.surrendered && s.morale < MORALE_BREAK - 30 && st.HP * 4 < st.MAXHP )
			{
				s.surrendered = true;
				++C.surrenders_offered;
				onSurrender(my);
			}
		}
		if ( K.packs && target && s.role == ROLE_FLANKER && s.stance == STANCE_NORMAL ) { flankDecide(my, s, target); }
	}

	static bool followRoute(Entity* my, TacticsState& s, double speed, double& dist2, double fx, double fy)
	{
		while ( s.routeAt < s.route.size() )
		{
			int t = s.route[s.routeAt];
			double wx = (t % (int)map.width) * 16.0 + 8, wy = (t / (int)map.width) * 16.0 + 8;
			if ( hypot(wx - my->x, wy - my->y) <= 6.0 ) { ++s.routeAt; continue; }
			return moveTo(my, s, wx, wy, speed, dist2);
		}
		return moveTo(my, s, fx, fy, speed, dist2);
	}

	bool packSteer(Entity* my, Stat& st, TacticsState& s, Entity* target, double d, double speed, double& dist2)
	{
		(void)st;
		if ( K.morale && s.stance == STANCE_RETREAT )
		{
			if ( s.routeKind != ROUTE_HOME )
			{
				HitSave hs;
				list_t* p = generatePath((int)floor(my->x / 16), (int)floor(my->y / 16), (int)floor(s.homeX / 16), (int)floor(s.homeY / 16),
					my, nullptr, GeneratePathTypes::GENERATE_PATH_DEFAULT);
				copyPath(p, s.route);
				freePath(p);
				s.routeAt = 0;
				s.routeKind = ROUTE_HOME;
			}
			if ( !followRoute(my, s, speed, dist2, s.homeX, s.homeY) ) { stand(my, target, speed); }
			else { s.faceMove = true; }
			return true;
		}
		if ( K.morale && s.stance == STANCE_REGROUP )
		{
			stand(my, target, speed);
			return true;
		}
		if ( K.packs && s.flank == 1 && s.routeKind == ROUTE_FLANK )
		{
			double bx = target->x + 1.5 * TILE * cos(target->yaw + PI), by = target->y + 1.5 * TILE * sin(target->yaw + PI);
			if ( !followRoute(my, s, speed, dist2, bx, by) ) { stand(my, target, speed); }
			s.faceTarget = d < 3 * TILE;
			return true;
		}
		return false;
	}
}
}
