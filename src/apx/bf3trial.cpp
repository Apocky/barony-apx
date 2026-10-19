// apx/bf3trial.cpp -- BF3 trial: natural attack limits on vs off at 3x, measured in the real game loop.
//
//   barony.exe -quickstart=barbarian -apx-out=<dir> -apx-seed=<n> -apx-bf3trial
//   APX_BF3_ARMS     default "off,on,on,off,off,on,on,off" (4 counterbalanced pairs). on = every knob on;
//                    off = the four limits off (reach, interleave, allyavoid, ring) with tactics, packs and
//                    morale on (as the S.A.M runs); vanilla = APX_TACTICS off (nothing of BF3 acts).
//   APX_BF3_N 12 (3 packs of 4 = 3x a 4-pack), APX_BF3_PACK 4, APX_BF3_KIND skeleton (bronze sword),
//   APX_BF3_SETTLE 100, APX_BF3_MEASURE 1000 (ticks, as the S.A.M runs), APX_BF3_FLOOR 1,
//   APX_BF3_MORALE 1 (after the arms: kill a pack leader with morale on, then off as the control).
// Rows: <apx-out>/bf3-arms.jsonl (one per arm), bf3-morale.jsonl (one per morale arm), bf3-run.jsonl.
// Human input: every keyboard, mouse, text, controller and touch EVENT is dropped and counted while the
// trial runs; the test character's position and facing are pinned every tick (a polled gamepad cannot
// move it); damage dealt to trial creatures by anything is counted (contamination flag). The test
// character never dies: each hit on it is counted, then negated, as in the S.A.M runs.
#include "tactics_int.hpp"
#include "telemetry.hpp"
#include "../items.hpp"
#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <queue>
#include <string>
#include <vector>

namespace apx
{
namespace bf3
{
	bool on = false;
	int exitCode = 0;

	enum Phase { P_WAIT_GAME, P_LOAD, P_WAIT_LOAD, P_PREP, P_CLEAR, P_SPAWN, P_SETTLE, P_MEASURE, P_CLEAN,
		P_M_SPAWN, P_M_SETTLE, P_M_OBSERVE, P_M_CLEAN, P_DONE };
	static Phase ph = P_WAIT_GAME;
	static Uint32 phT = 0;
	static std::vector<std::string> arms;
	static size_t armI = 0;
	static int nCreatures = 12, packSize = 4, settleTicks = 100, measureTicks = 1000, floorWanted = 1, moraleTest = 1;
	static std::string kindName = "skeleton";
	static Monster kind = SKELETON;
	static double pinX = 0, pinY = 0, pinYaw = 0;
	static bool pinned = false;
	static int pTX = -1, pTY = -1;
	static std::vector<std::pair<int, int>> spawnTiles;   // angle-sorted ring 4..7 tiles out
	static std::vector<Uint32> trialUids;
	static Uint32 leaderUid = 0;
	static int moraleArm = 0;
	static bool measuring = false, absorbing = false, scripted = false;
	static Uint32 killTick = 0, firstRegroup = 0;
	static double distAtKill = 0, distAt3s = 0;
	static int retreatAt3s = 0, shouldRetreatAt3s = 0, membersAtKill = 0;
	static long long inKeys = 0, inButtons = 0, inMotion = 0, inOther = 0;

	struct Arm
	{
		long long dmg = 0, foreignDmg = 0, hits = 0, attempts = 0, thin = 0, wide = 0, playerMoved = 0, creatureDmg = 0;
		long long targetLostTicks = 0, ticks = 0, overflowTicks = 0, inReachSum = 0, attackersSum = 0, near6Sum = 0;
		long long attackersMax = 0, inReachMax = 0, lost = 0, standoffN = 0, spreadN = 0, waitPairN = 0;
		double standoffSum = 0, spreadSum = 0, waitPairSum = 0;
		long long keys0 = 0, buttons0 = 0, motion0 = 0, other0 = 0;
		long long stWait = 0, stAttack = 0, stPath = 0, stHunt = 0, stOther = 0, targetPlayer = 0, overlapTicks = 0, playerAttackTicks = 0;
		double distSum = 0; long long distN = 0;
		std::vector<Uint32> hitTicks, attemptTicks;
		std::vector<int> hist = std::vector<int>(13, 0);
		int slots = 0, spawned = 0;
	};
	static Arm A;

	static int envInt(const char* n, int d) { const char* e = getenv(n); return (e && *e) ? atoi(e) : d; }

	void init(int argc, char** argv)
	{
		for ( int i = 1; i < argc; ++i ) { if ( argv[i] && !strncmp(argv[i], "-apx-bf3trial", 13) ) { on = true; } }
		if ( !on ) { return; }
		const char* e = getenv("APX_BF3_ARMS");
		std::string spec = (e && *e) ? e : "off,on,on,off,off,on,on,off";
		size_t p = 0;
		while ( p <= spec.size() )
		{
			size_t q = spec.find(',', p);
			if ( q == std::string::npos ) { q = spec.size(); }
			if ( q > p ) { arms.push_back(spec.substr(p, q - p)); }
			p = q + 1;
		}
		nCreatures = envInt("APX_BF3_N", 12);
		packSize = std::max(1, envInt("APX_BF3_PACK", 4));
		settleTicks = envInt("APX_BF3_SETTLE", 100);
		measureTicks = envInt("APX_BF3_MEASURE", 1000);
		floorWanted = envInt("APX_BF3_FLOOR", 1);
		moraleTest = envInt("APX_BF3_MORALE", 1);
		e = getenv("APX_BF3_KIND");
		if ( e && *e ) { kindName = e; }
		for ( int i = 1; i < NUMMONSTERS; ++i ) { if ( !_stricmp(monstertypename[i], kindName.c_str()) ) { kind = (Monster)i; } }
		printlog("[apx] bf3 trial: %d arms (%s), %d %s in packs of %d, settle %d, measure %d ticks, floor %d, morale %d",
			(int)arms.size(), spec.c_str(), nCreatures, kindName.c_str(), packSize, settleTicks, measureTicks, floorWanted, moraleTest);
	}

	bool swallowInput(const SDL_Event& ev)
	{
		if ( !on ) { return false; }
		switch ( ev.type )
		{
			case SDL_KEYDOWN: case SDL_KEYUP: case SDL_TEXTINPUT: case SDL_TEXTEDITING:
				++inKeys; return true;
			case SDL_MOUSEBUTTONDOWN: case SDL_MOUSEBUTTONUP: case SDL_MOUSEWHEEL:
			case SDL_CONTROLLERBUTTONDOWN: case SDL_CONTROLLERBUTTONUP: case SDL_JOYBUTTONDOWN: case SDL_JOYBUTTONUP:
				++inButtons; return true;
			case SDL_MOUSEMOTION:
				++inMotion; return true;
			case SDL_CONTROLLERAXISMOTION: case SDL_JOYAXISMOTION: case SDL_JOYHATMOTION: case SDL_JOYBALLMOTION:
			case SDL_FINGERDOWN: case SDL_FINGERUP: case SDL_FINGERMOTION:
				++inOther; return true;
			default:
				return false;
		}
	}

	static bool isTrial(Entity* e)
	{
		return e && std::find(trialUids.begin(), trialUids.end(), e->getUID()) != trialUids.end();
	}

	bool absorbPlayerDamage(Entity* player, int amount)
	{
		if ( !on || !absorbing || amount >= 0 || !players[0] || player != players[0]->entity ) { return false; }
		if ( measuring )
		{
			// a hit with no trial creature within melee distance (a trap, a stray) is counted apart
			double nearest = 1e9;
			for ( Uint32 u : trialUids ) { if ( Entity* c = uidToEntity(u) ) { nearest = std::min(nearest, hypot(c->x - player->x, c->y - player->y)); } }
			if ( nearest > STRIKERANGE + 16 ) { A.foreignDmg += -amount; }
			else { A.dmg += -amount; ++A.hits; A.hitTicks.push_back(ticks); }
		}
		return true;
	}

	void onCreatureHP(Entity* e, int oldHP, int newHP)
	{
		if ( (measuring || ph == P_M_OBSERVE) && !scripted && newHP < oldHP && isTrial(e) ) { A.creatureDmg += oldHP - newHP; }
	}

	void onSwingAttempt(Entity* my, Entity* target, double dist)
	{
		(void)dist;
		if ( !measuring || !players[0] || target != players[0]->entity || !isTrial(my) ) { return; }
		++A.attempts;
		A.attemptTicks.push_back(ticks);
		A.standoffSum += hypot(my->x - target->x, my->y - target->y);
		++A.standoffN;
	}

	void onSwingThin(Entity* my, Entity* target)
	{
		if ( measuring && players[0] && target == players[0]->entity && isTrial(my) ) { ++A.thin; }
	}

	void onSwingWide(Entity* my, Entity* target)
	{
		if ( measuring && players[0] && target == players[0]->entity && isTrial(my) ) { ++A.wide; }
	}

	// ---------------------------------------------------------------- arena
	static bool clear3(int x, int y)
	{
		for ( int dx = -1; dx <= 1; ++dx ) { for ( int dy = -1; dy <= 1; ++dy ) { if ( !tactics::walkable(x + dx, y + dy) ) { return false; } } }
		return true;
	}

	static int openCount(int cx, int cy, int R, std::vector<std::pair<int, int>>* out)
	{
		int W = 2 * R + 1, n = 0;
		std::vector<char> seen((size_t)W * W, 0);
		std::queue<std::pair<int, int>> q;
		q.push({ cx, cy });
		seen[R + R * W] = 1;
		while ( !q.empty() )
		{
			auto c = q.front(); q.pop();
			++n;
			if ( out ) { out->push_back(c); }
			static const int dx[4] = { 1, -1, 0, 0 }, dy[4] = { 0, 0, 1, -1 };
			for ( int k = 0; k < 4; ++k )
			{
				int x = c.first + dx[k], y = c.second + dy[k];
				if ( abs(x - cx) > R || abs(y - cy) > R ) { continue; }
				char& s = seen[(x - cx + R) + (y - cy + R) * W];
				if ( s || !tactics::walkable(x, y) ) { continue; }
				s = 1;
				q.push({ x, y });
			}
		}
		return n;
	}

	static void chooseArena(Entity* pe)
	{
		int best = -1;
		for ( int y = 2; y < (int)map.height - 2; ++y )
		{
			for ( int x = 2; x < (int)map.width - 2; ++x )
			{
				if ( !clear3(x, y) ) { continue; }
				int n = openCount(x, y, 5, nullptr);
				if ( n > best ) { best = n; pTX = x; pTY = y; }
			}
		}
		pinX = pTX * 16.0 + 8; pinY = pTY * 16.0 + 8; pinYaw = 0;
		pe->x = pinX; pe->y = pinY; pe->yaw = pinYaw;
		pinned = true;
		std::vector<std::pair<int, int>> reach;
		openCount(pTX, pTY, 8, &reach);
		spawnTiles.clear();
		for ( auto& t : reach )
		{
			double d = hypot(t.first - pTX, t.second - pTY);
			if ( d < 4 || d > 7 ) { continue; }
			if ( !tactics::wallClear(pe, pinX, pinY, t.first * 16.0 + 8, t.second * 16.0 + 8) ) { continue; }
			spawnTiles.push_back(t);
		}
		std::sort(spawnTiles.begin(), spawnTiles.end(), [](const std::pair<int, int>& a, const std::pair<int, int>& b) {
			double aa = atan2(a.second - pTY, a.first - pTX), ab = atan2(b.second - pTY, b.first - pTX);
			return aa < ab || (aa == ab && hypot(a.first - pTX, a.second - pTY) < hypot(b.first - pTX, b.second - pTY));
		});
		printlog("[apx] bf3 arena: tile %d,%d open %d (radius 5), %d spawn tiles 4-7 tiles out", pTX, pTY, best, (int)spawnTiles.size());
	}

	static void removeQuietly(Entity* m)
	{
		Stat* st = m->getStats();
		if ( !st ) { return; }
		Item* eq[] = { st->weapon, st->shield, st->helmet, st->breastplate, st->gloves, st->shoes, st->cloak, st->ring, st->amulet, st->mask };
		for ( Item* it : eq ) { if ( it ) { it->isDroppable = false; } }
		for ( node_t* n = st->inventory.first; n; n = n->next ) { Item* it = (Item*)n->element; if ( it ) { it->isDroppable = false; } }
		st->GOLD = 0;
		m->setHP(0);
	}

	static void killAllMonsters()
	{
		for ( node_t* nd = map.creatures ? map.creatures->first : nullptr; nd; nd = nd->next )
		{
			Entity* e = (Entity*)nd->element;
			if ( e && e->behavior == &actMonster && e->getStats() && e->getStats()->HP > 0 ) { removeQuietly(e); }
		}
	}

	// arm spec "kind[:N]": kind sets the knobs, N the creature count (default APX_BF3_N)
	static std::string armKind(const std::string& a) { size_t c = a.find(':'); return c == std::string::npos ? a : a.substr(0, c); }
	static int armN(const std::string& a) { size_t c = a.find(':'); return c == std::string::npos ? nCreatures : std::max(1, atoi(a.c_str() + c + 1)); }

	static void setArmKnobs(const std::string& spec)
	{
		std::string a = armKind(spec);
		tactics::K = tactics::Knobs();
		tactics::setProbeUs(a == "probe" ? 1000.0 : 0.0); // probe: every knob on plus a 1 ms busy-wait per decide (control)
		if ( a == "off" ) { tactics::K.reach = tactics::K.interleave = tactics::K.allyavoid = tactics::K.ring = false; }
		else if ( a == "vanilla" ) { tactics::K.tactics = false; }
		else if ( a == "nomorale" ) { tactics::K.morale = false; }
		else if ( a == "noring" ) { tactics::K.ring = false; }        // control for BF3-4: waiters bunch
		else if ( a == "noreach" ) { tactics::K.reach = false; tactics::K.ring = false; }
		else if ( a == "noally" ) { tactics::K.allyavoid = false; }
		else if ( a == "nojitter" ) { tactics::K.interleave = false; }
		tactics::refresh();
	}

	static Entity* spawnOne(int tx, int ty, Uint32 packId, uint8_t role, double hx, double hy, Entity* pe)
	{
		Entity* m = summonMonster(kind, tx * 16 + 8, ty * 16 + 8, true);
		if ( !m ) { return nullptr; }
		if ( Stat* st = m->getStats() )
		{
			if ( kind == SKELETON || kind == GOBLIN || kind == HUMAN )
			{
				st->weapon = newItem(BRONZE_SWORD, EXCELLENT, 0, 1, MONSTER_ITEM_UNDROPPABLE_APPEARANCE, false, nullptr);
			}
		}
		tactics::assignPack(m, packId, role, hx, hy);
		m->monsterAcquireAttackTarget(*pe, MONSTER_STATE_PATH);
		trialUids.push_back(m->getUID());
		return m;
	}

	static void spawnPacks(Entity* pe, int n, int pack, bool flankers)
	{
		trialUids.clear();
		int got = (int)spawnTiles.size();
		if ( got == 0 ) { return; }
		std::vector<std::pair<int, int>> pick;
		for ( int i = 0; i < n; ++i ) { pick.push_back(spawnTiles[(size_t)((long long)i * got / n)]); }
		for ( int p0 = 0; p0 < n; p0 += pack )
		{
			int p1 = std::min(n, p0 + pack);
			double hx = 0, hy = 0;
			for ( int i = p0; i < p1; ++i ) { hx += pick[i].first * 16.0 + 8; hy += pick[i].second * 16.0 + 8; }
			hx /= (p1 - p0); hy /= (p1 - p0);
			for ( int i = p0; i < p1; ++i )
			{
				uint8_t role = i == p0 ? tactics::ROLE_LEADER : (flankers && i == p0 + 1 && p1 - p0 >= 3 ? tactics::ROLE_FLANKER : tactics::ROLE_BRUTE);
				Entity* m = spawnOne(pick[i].first, pick[i].second, (Uint32)(1 + p0 / pack), role, hx, hy, pe);
				if ( m && i == p0 ) { leaderUid = m->getUID(); }
			}
		}
	}

	static void wake(Entity* pe)
	{
		for ( Uint32 u : trialUids )
		{
			Entity* c = uidToEntity(u);
			if ( !c ) { continue; }
			c->setEffect(EFF_ASLEEP, false, 0, false);
			c->monsterAcquireAttackTarget(*pe, MONSTER_STATE_PATH);
		}
	}

	static void cleanup()
	{
		tactics::onLevelChange();
		for ( Uint32 u : trialUids ) { if ( Entity* c = uidToEntity(u) ) { if ( c->getStats() && c->getStats()->HP > 0 ) { removeQuietly(c); } } }
		trialUids.clear();
	}

	// circular spread of swing-timer phases (mod HITRATE) among attackers in reach, in ticks
	static void samplePhases(Entity* pe)
	{
		double sx = 0, sy = 0;
		int n = 0;
		for ( Uint32 u : trialUids )
		{
			Entity* c = uidToEntity(u);
			if ( !c || (Uint32)c->monsterTarget != pe->getUID() || c->monsterState != MONSTER_STATE_ATTACK ) { continue; }
			if ( hypot(c->x - pe->x, c->y - pe->y) >= STRIKERANGE + tactics::bodyR(pe) ) { continue; }
			double ph = 2 * PI * (((c->monsterHitTime % HITRATE) + HITRATE) % HITRATE) / HITRATE;
			sx += cos(ph); sy += sin(ph); ++n;
		}
		if ( n < 2 ) { return; }
		double R = std::max(0.01, hypot(sx, sy) / n);
		A.spreadSum += HITRATE / (2 * PI) * sqrt(-2 * log(R));
		++A.spreadN;
	}

	static void sample(Entity* pe)
	{
		int inReach = 0, attackers = 0, near6 = 0;
		std::vector<Entity*> waiters;
		for ( Uint32 u : trialUids )
		{
			Entity* c = uidToEntity(u);
			Stat* cs = c ? c->getStats() : nullptr;
			if ( !c || !cs || cs->HP <= 0 ) { ++A.lost; continue; }
			double d = hypot(c->x - pe->x, c->y - pe->y);
			if ( (Uint32)c->monsterTarget != pe->getUID() ) { ++A.targetLostTicks; } else { ++A.targetPlayer; }
			switch ( c->monsterState )
			{
				case MONSTER_STATE_WAIT: ++A.stWait; break;
				case MONSTER_STATE_ATTACK: ++A.stAttack; break;
				case MONSTER_STATE_PATH: ++A.stPath; break;
				case MONSTER_STATE_HUNT: ++A.stHunt; break;
				default: ++A.stOther; break;
			}
			A.distSum += d; ++A.distN;
			if ( d < tactics::bodyR(pe) + tactics::bodyR(c) - 0.5 ) { ++A.overlapTicks; }
			if ( d < 6 * 16.0 ) { ++near6; }
			if ( d < STRIKERANGE + tactics::bodyR(pe) )
			{
				++inReach;
				if ( c->monsterState == MONSTER_STATE_ATTACK && (Uint32)c->monsterTarget == pe->getUID() ) { ++attackers; }
			}
			else if ( d < 6 * 16.0 ) { waiters.push_back(c); }
		}
		++A.ticks;
		if ( pe->skill[9] != 0 ) { ++A.playerAttackTicks; } // PLAYER_ATTACK (actplayer.cpp): the test character swung
		A.inReachSum += inReach; A.attackersSum += attackers; A.near6Sum += near6;
		A.inReachMax = std::max<long long>(A.inReachMax, inReach);
		A.attackersMax = std::max<long long>(A.attackersMax, attackers);
		A.hist[std::min(attackers, 12)]++;
		if ( attackers > A.slots ) { ++A.overflowTicks; }
		if ( ticks % 10 == 0 )
		{
			samplePhases(pe);
			if ( waiters.size() >= 2 )
			{
				double s = 0; int k = 0;
				for ( size_t i = 0; i < waiters.size(); ++i ) { for ( size_t j = i + 1; j < waiters.size(); ++j ) { s += hypot(waiters[i]->x - waiters[j]->x, waiters[i]->y - waiters[j]->y); ++k; } }
				A.waitPairSum += s / k; ++A.waitPairN;
			}
		}
	}

	static double shareNear(std::vector<Uint32> v, Uint32 gap, bool bothSides)
	{
		if ( v.empty() ) { return 0; }
		std::sort(v.begin(), v.end());
		int n = 0;
		for ( size_t i = 0; i < v.size(); ++i )
		{
			bool prev = i > 0 && v[i] - v[i - 1] <= gap;
			bool next = bothSides && i + 1 < v.size() && v[i + 1] - v[i] <= gap;
			if ( prev || next ) { ++n; }
		}
		return (double)n / v.size();
	}

	static void writeArmRow(const std::string& armSpec)
	{
		using namespace tactics;
		double secs = measureTicks / (double)TICKS_PER_SECOND;
		double dp50, dp99, mp50, mp99, tp99;
		timingPercentiles(dp50, dp99, mp50, mp99, tp99);
		long long keys = inKeys - A.keys0, buttons = inButtons - A.buttons0, motion = inMotion - A.motion0, other = inOther - A.other0;
		// CONTAMINATED_* voids a row; INFO_* is behaviour worth reading (friendly fire is the engine's own:
		// a monster's blow hurts a packmate that steps into it; friendlyFireProtection covers players' allies only)
		std::string flags;
		if ( A.playerAttackTicks > 0 ) { flags += "CONTAMINATED_PLAYER_ATTACKED;"; }
		if ( A.playerMoved > 0 ) { flags += "CONTAMINATED_PLAYER_MOVED;"; }
		if ( keys + buttons + other > 0 ) { flags += "INFO_INPUT_REJECTED;"; }
		if ( A.foreignDmg > 0 ) { flags += "INFO_FOREIGN_DAMAGE;"; }
		if ( A.creatureDmg > 0 ) { flags += "INFO_FRIENDLY_FIRE;"; }
		if ( A.lost > 0 ) { flags += "INFO_CREATURES_LOST;"; }
		if ( A.spawned < armN(arms[armI]) ) { flags += "SPAWN_SHORT;"; }
		ApxRow r;
		commonFields(r);
		r.str("trial", "bf3-limits").str("arm_kind", armSpec).str("arm_base", armKind(armSpec)).i("arm", (long long)armI).str("knobs", knobString())
			.d("population_x", armN(armSpec) / (double)packSize).d("probe_us", armKind(armSpec) == "probe" ? 1000.0 : 0.0)
			.str("kind", kindName).str("weapon", "bronze_sword").i("creatures", A.spawned).i("pack_size", packSize)
			.str("pack_template", "trial-3x4").i("arena_x", pTX).i("arena_y", pTY).d("window_s", secs)
			.i("dmg", A.dmg).d("dmg_pm", A.dmg / (secs / 60.0)).d("dmg_in_per_player_per_min", A.dmg / (secs / 60.0))
			.i("dmg_foreign", A.foreignDmg).i("hits", A.hits).d("burst_share", shareNear(A.hitTicks, 5, true))
			.i("swing_attempts", A.attempts).d("attempt_lockstep_share", shareNear(A.attemptTicks, 5, false))
			.d("standoff_mean", A.standoffN ? A.standoffSum / A.standoffN : 0).i("blocked_thin", A.thin).i("blocked_wide", A.wide)
			.d("swing_phase_spread_ticks", A.spreadN ? A.spreadSum / A.spreadN : 0)
			.i("reach_slots", A.slots).d("reach_slots_mean", C.reach_slots_n ? (double)C.reach_slots_sum / C.reach_slots_n : 0)
			.i("reach_overflow", A.overflowTicks).i("reach_overflow_creatures", C.reach_overflow_creatures).i("reach_assign", C.reach_assign)
			.d("attackers_mean", A.ticks ? (double)A.attackersSum / A.ticks : 0).i("attackers_max_per_target", A.attackersMax)
			.d("in_reach_mean", A.ticks ? (double)A.inReachSum / A.ticks : 0).i("in_reach_max", A.inReachMax)
			.d("near6_mean", A.ticks ? (double)A.near6Sum / A.ticks : 0).intList("attackers_hist", A.hist)
			.i("ally_blocked", C.ally_blocked).i("reposition", C.reposition).i("gap_wait", C.gap_wait)
			.i("keep_engaged", C.keep_engaged).i("promoted", C.promoted)
			.i("target_dropped_by_tactics", C.target_dropped_by_tactics).i("target_lost_ticks", A.targetLostTicks)
			.i("ring_holds", C.ring_holds).i("ring_assign", C.ring_assign).i("ring_slot_fail", C.ring_slot_fail).i("feints", C.feints)
			.i("stuck", C.stuck).i("stuck_episodes", C.stuck_episodes).i("steer_ticks", C.steer_ticks).i("steer_holds", C.steer_holds)
			.d("waiters_pair_dist_mean", A.waitPairN ? A.waitPairSum / A.waitPairN : 0)
			.i("flank_started", C.flank_started).i("flank_arrived", C.flank_arrived).i("flank_failed", C.flank_failed)
			.i("retreats", C.retreats).i("regroups", C.regroups).i("morale_breaks", C.morale_breaks).i("morale_events", C.morale_events)
			.i("calls_for_help", 0).i("calls_answered", 0).i("surrenders", C.surrenders_offered).i("packs_formed", C.packs_formed)
			.i("flow_builds", 0).i("flow_cells_updated", 0).i("flow_fallback_astar", 0)
			.d("decide_us_p50", dp50).d("decide_us_p99", dp99).d("move_us_p50", mp50).d("move_us_p99", mp99).d("tick_us_p99", tp99)
			.i("decides", C.decides).i("tactics_calls", C.tactics_calls).i("swing_resets_jittered", C.swing_resets)
			.d("jitter_mean", C.swing_resets ? (double)C.jitter_sum / C.swing_resets : 0)
			.i("deaths", 0).str("outcome", "survived_damage_negated")
			.i("input_keys", keys).i("input_buttons", buttons).i("input_motion", motion).i("input_other", other)
			.i("player_moved", A.playerMoved).i("player_attack_ticks", A.playerAttackTicks)
			.i("creature_dmg", A.creatureDmg).i("friendly_fire_dmg", A.creatureDmg).i("creatures_lost_ticks", A.lost)
			.d("st_wait", A.ticks ? (double)A.stWait / A.ticks : 0).d("st_attack", A.ticks ? (double)A.stAttack / A.ticks : 0)
			.d("st_path", A.ticks ? (double)A.stPath / A.ticks : 0).d("st_hunt", A.ticks ? (double)A.stHunt / A.ticks : 0)
			.d("st_other", A.ticks ? (double)A.stOther / A.ticks : 0).d("targeting_player_mean", A.ticks ? (double)A.targetPlayer / A.ticks : 0)
			.d("dist_mean", A.distN ? A.distSum / A.distN : 0).i("overlap_ticks", A.overlapTicks).i("pack_reacquire", C.pack_reacquire)
			.str("flags", flags);
		write("bf3-arms.jsonl", r);
		printlog("[apx] bf3 arm %d %s: dmg %lld (%.1f/min) hits %lld attackers mean %.2f max %lld slots %d input %lld/%lld/%lld",
			(int)armI, armSpec.c_str(), A.dmg, A.dmg / (secs / 60.0), A.hits, A.ticks ? (double)A.attackersSum / A.ticks : 0,
			A.attackersMax, A.slots, keys, buttons, motion);
	}

	static double meanDist(Entity* pe)
	{
		double s = 0; int n = 0;
		for ( Uint32 u : trialUids )
		{
			if ( u == leaderUid ) { continue; }
			Entity* c = uidToEntity(u);
			if ( c && c->getStats() && c->getStats()->HP > 0 ) { s += hypot(c->x - pe->x, c->y - pe->y); ++n; }
		}
		return n ? s / n : 0;
	}

	static void writeMoraleRow()
	{
		using namespace tactics;
		ApxRow r;
		commonFields(r);
		r.str("trial", "bf3-morale").str("arm_kind", moraleArm == 0 ? "morale_on" : "morale_off_control").str("knobs", knobString())
			.str("kind", kindName).i("members_at_kill", membersAtKill).i("retreating_at_3s", retreatAt3s)
			.i("should_retreat_at_3s", shouldRetreatAt3s).d("dist_at_kill", distAtKill).d("dist_at_3s", distAt3s)
			.i("retreats", C.retreats).i("morale_breaks", C.morale_breaks).i("regroups", C.regroups)
			.d("first_regroup_s", firstRegroup ? (firstRegroup - killTick) / (double)TICKS_PER_SECOND : -1)
			.i("home_moved", C.home_moved).i("surrenders", C.surrenders_offered).i("morale_events", C.morale_events)
			.i("creature_dmg", A.creatureDmg).i("input_keys", inKeys - A.keys0).i("input_buttons", inButtons - A.buttons0);
		write("bf3-morale.jsonl", r);
		printlog("[apx] bf3 morale %s: retreating at 3 s %d of %d, dist %.1f -> %.1f, regroups %lld", moraleArm == 0 ? "on" : "off",
			retreatAt3s, membersAtKill, distAtKill, distAt3s, C.regroups);
	}

	static void go(Phase p) { ph = p; phT = ticks; }

	static void finish(int code)
	{
		ApxRow r;
		commonFields(r);
		r.str("trial", "bf3-run").i("arms", (long long)arms.size()).i("arms_done", (long long)armI).i("exit", code)
			.i("input_keys", inKeys).i("input_buttons", inButtons).i("input_motion", inMotion).i("input_other", inOther);
		write("bf3-run.jsonl", r);
		exitCode = code;
		absorbing = false;
		ph = P_DONE;
		mainloop = 0;
	}

	void onGameTick()
	{
		if ( !on || ph == P_DONE ) { return; }
		Entity* pe = players[0] ? players[0]->entity : nullptr;
		if ( measuring ) { tactics::endOfGameTick(); }
		if ( pinned && pe )
		{
			if ( fabs(pe->x - pinX) > 0.5 || fabs(pe->y - pinY) > 0.5 || fabs(pe->yaw - pinYaw) > 0.01 ) { ++A.playerMoved; }
			pe->x = pinX; pe->y = pinY; pe->yaw = pinYaw; pe->vel_x = 0; pe->vel_y = 0;
		}
		if ( ph >= P_PREP && !pe ) { printlog("[apx] bf3: the test character is gone"); finish(5); return; }
		if ( !pe && ph != P_WAIT_GAME && ph != P_WAIT_LOAD ) { return; } // a floor is loading
		switch ( ph )
		{
			case P_WAIT_GAME:
				if ( pe && !loading && ticks > 100 ) { go(P_LOAD); }
				else if ( ticks > 50 * 600 ) { finish(2); }
				break;
			case P_LOAD:
				currentlevel = floorWanted - 1;
				loadnextlevel = true;
				go(P_WAIT_LOAD);
				break;
			case P_WAIT_LOAD:
				if ( pe && !loadnextlevel && !loading && currentlevel == floorWanted && ticks - phT > 100 ) { go(P_PREP); }
				else if ( ticks - phT > 50 * 120 ) { finish(3); }
				break;
			case P_PREP:
				chooseArena(pe);
				absorbing = true;
				killAllMonsters();
				go(P_CLEAR);
				break;
			case P_CLEAR:
				if ( ticks - phT >= 60 ) { go(armI < arms.size() ? P_SPAWN : (moraleTest ? P_M_SPAWN : P_DONE)); if ( ph == P_DONE ) { finish(0); } }
				break;
			case P_SPAWN:
				setArmKnobs(arms[armI]);
				tactics::onLevelChange();
				A = Arm();
				A.keys0 = inKeys; A.buttons0 = inButtons; A.motion0 = inMotion; A.other0 = inOther;
				spawnPacks(pe, armN(arms[armI]), packSize, true);
				A.spawned = (int)trialUids.size();
				A.slots = trialUids.empty() ? 0 : tactics::reachSlotsFor(pe, uidToEntity(trialUids[0]));
				go(P_SETTLE);
				break;
			case P_SETTLE:
				if ( ticks - phT == 2 ) { wake(pe); }
				if ( ticks - phT >= (Uint32)settleTicks )
				{
					tactics::resetCounters();
					tactics::timingReset();
					long long k0 = A.keys0, b0 = A.buttons0, m0 = A.motion0, o0 = A.other0;
					int slots = A.slots, spawned = A.spawned;
					A = Arm();
					A.keys0 = k0; A.buttons0 = b0; A.motion0 = m0; A.other0 = o0; A.slots = slots; A.spawned = spawned;
					measuring = true;
					go(P_MEASURE);
				}
				break;
			case P_MEASURE:
				sample(pe);
				if ( ticks - phT >= (Uint32)measureTicks )
				{
					measuring = false;
					writeArmRow(arms[armI]);
					cleanup();
					++armI;
					go(P_CLEAN);
				}
				break;
			case P_CLEAN:
				if ( ticks - phT >= 60 )
				{
					if ( armI < arms.size() ) { go(P_SPAWN); }
					else if ( moraleTest && moraleArm < 2 ) { go(P_M_SPAWN); }
					else { finish(0); }
				}
				break;
			case P_M_SPAWN:
				setArmKnobs(moraleArm == 0 ? "on" : "nomorale");
				tactics::onLevelChange();
				tactics::resetCounters();
				A = Arm();
				A.keys0 = inKeys; A.buttons0 = inButtons; A.motion0 = inMotion; A.other0 = inOther;
				spawnPacks(pe, packSize, packSize, false);
				firstRegroup = 0;
				go(P_M_SETTLE);
				break;
			case P_M_SETTLE:
				if ( ticks - phT == 2 ) { wake(pe); }
				if ( ticks - phT >= 150 )
				{
					Entity* l = uidToEntity(leaderUid);
					membersAtKill = 0;
					for ( Uint32 u : trialUids ) { Entity* c = uidToEntity(u); if ( u != leaderUid && c && c->getStats() && c->getStats()->HP > 0 ) { ++membersAtKill; } }
					distAtKill = meanDist(pe);
					if ( l && l->getStats() )
					{
						scripted = true;
						l->modHP(-l->getStats()->HP); // through the HP choke point, so the morale hook sees a leader die
						scripted = false;
					}
					killTick = ticks;
					go(P_M_OBSERVE);
				}
				break;
			case P_M_OBSERVE:
				if ( ticks - killTick == 3 * TICKS_PER_SECOND )
				{
					retreatAt3s = shouldRetreatAt3s = 0;
					for ( Uint32 u : trialUids )
					{
						Entity* c = uidToEntity(u);
						if ( u == leaderUid || !c || !c->getStats() || c->getStats()->HP <= 0 ) { continue; }
						if ( tactics::stanceOf(c) == tactics::STANCE_RETREAT || tactics::stanceOf(c) == tactics::STANCE_REGROUP ) { ++retreatAt3s; }
						if ( c->shouldRetreat(*c->getStats()) ) { ++shouldRetreatAt3s; }
					}
					distAt3s = meanDist(pe);
				}
				if ( !firstRegroup && tactics::C.regroups > 0 ) { firstRegroup = ticks; }
				if ( ticks - killTick >= 45 * TICKS_PER_SECOND )
				{
					writeMoraleRow();
					cleanup();
					++moraleArm;
					go(P_CLEAN);
				}
				break;
			default:
				break;
		}
	}
}
}
