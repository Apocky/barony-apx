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
#include "flow.hpp"
#include "../items.hpp"
#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <queue>
#include <unordered_map>
#include <string>
#include <vector>

namespace apx
{
namespace bf3
{
	bool on = false;
	int exitCode = 0;

	enum Phase { P_WAIT_GAME, P_LOAD, P_WAIT_LOAD, P_PREP, P_CLEAR, P_SPAWN, P_SETTLE, P_MEASURE, P_CLEAN,
		P_M_SPAWN, P_M_SETTLE, P_M_OBSERVE, P_M_CLEAN, P_H_SPAWN, P_H_SETTLE, P_H_OBSERVE, P_FT_LOAD, P_FT_WAIT, P_FT_RUN, P_DONE };
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
	static int homeAt5s = -1; // members home (within 2 tiles of it, or regrouping) 5 s after the kill
	static long long inKeys = 0, inButtons = 0, inMotion = 0, inOther = 0;
	// calls for help (BF3-6b): pack B idles out of sight of pack A; pack A's leader is "hit" by the test character
	static int helpTest = 1, helpArm = 0, hsX = -1, hsY = -1, hsBfs = -1;
	static bool hsSearched = false;
	static std::vector<Uint32> helpUids;
	static Uint32 callTick = 0, firstHelpArrival = 0;
	static double helpMinDist = 1e9;
	// help is judged inside the time help needs: pack B's walk from its spot (BFS steps at its own speed) plus 5 s;
	// monsters also wander and find a fight on their own later (s5-route control: 2 of 4 by 26.3 s of 30)
	static double helpWindowS = 0;
	static int within4Window = -1;
	static std::vector<Uint32> helpAcquired;
	static Uint32 firstHelpAcquire = 0;
	// flow self-test (BF3-5a): APX_BF3_FLOWTEST pairs per floor over APX_BF3_FLOWFLOORS (default 1,2,3,4,5)
	static int flowTest = 0;
	static int dumpCreatures = 0;
	static std::vector<int> ftFloors;
	static size_t ftI = 0;

	struct Arm
	{
		long long dmg = 0, foreignDmg = 0, hits = 0, attempts = 0, thin = 0, wide = 0, playerMoved = 0, creatureDmg = 0;
		long long dmgHits = 0, dmgOther = 0, otherEvents = 0;    // damage from swings vs damage over time (bleeding etc.)
		int overflowLogs = 0;                                   // overflow ticks logged in this arm (first 6)
		long long unslottedInReachSum = 0;                      // creatures in reach the slot gate holds (they cannot strike)
		int acStart = -1, acEnd = -1, gearFixes = 0;            // the character's armour class; gear repaired at the arm's start
		long long targetLostTicks = 0, ticks = 0, overflowTicks = 0, inReachSum = 0, attackersSum = 0, near6Sum = 0;
		long long attackersMax = 0, inReachMax = 0, lost = 0, standoffN = 0, spreadN = 0, waitPairN = 0;
		double standoffSum = 0, spreadSum = 0, waitPairSum = 0;
		long long keys0 = 0, buttons0 = 0, motion0 = 0, other0 = 0;
		long long stWait = 0, stAttack = 0, stPath = 0, stHunt = 0, stOther = 0, targetPlayer = 0, overlapTicks = 0, playerAttackTicks = 0;
		double distSum = 0; long long distN = 0;
		std::vector<Uint32> hitTicks, attemptTicks;
		std::vector<int> hist = std::vector<int>(13, 0);
		std::unordered_map<Uint32, std::pair<int, int>> swing;   // per creature: swing attempts, swings stopped by a body
		double nnSum = 0; long long nnN = 0, waiterSamples = 0, waiterOverlaps = 0;
		long long contactSum = 0, contactSamples = 0, contactMax = 0;
		long long traceAlly = 0, traceNone = 0, traceOther = 0, traceOtherPlayerOwned = 0;
		long long pinCorrections = 0, foreignSeen = 0, foreignRemoved = 0;
		// reach-slot holders per sampled tick: in reach, or out of it by engine state, and out of it standing still 1 s+
		long long holderIn = 0, holderOut = 0, holderOutAttack = 0, holderOutHunt = 0, holderOutPath = 0, holderOutWait = 0, holderOutStill = 0;
		long long holderFalseArrive = 0; // out of reach, standing as "at its goal" while more than HOLD_EPS + 1 from it
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
		helpTest = envInt("APX_BF3_HELP", 1);
		flowTest = envInt("APX_BF3_FLOWTEST", 0);
		dumpCreatures = envInt("APX_BF3_DUMP", 0);
		{
			const char* ff = getenv("APX_BF3_FLOWFLOORS");
			std::string fs = (ff && *ff) ? ff : "1,2,3,4,5";
			size_t q0 = 0;
			while ( q0 <= fs.size() )
			{
				size_t q1 = fs.find(',', q0);
				if ( q1 == std::string::npos ) { q1 = fs.size(); }
				if ( q1 > q0 ) { ftFloors.push_back(atoi(fs.substr(q0, q1 - q0).c_str())); }
				q0 = q1 + 1;
			}
		}
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

	// The test character is restored at every arm. Damage per hit drifted across a run and between runs (s12: skeleton
	// 6.3 -> 9.3 per hit, rats 7.0; s13: 6.3 -> 4.8, rats 2.0 then 5.9 two arms later): its armour wears and breaks
	// under thousands of blows and effects (bleeding) carried from one arm into the next. A snapshot taken before the
	// first fight is put back at every arm: attributes, skills, effects, and each equipped item (re-created if broken).
	struct GearSnap { bool has = false; int type = 0; int status = 0; Sint16 beatitude = 0; Uint32 appearance = 0; bool identified = true; };
	static bool snapTaken = false;
	static Sint32 snapAttr[11];
	static Sint32 snapProf[NUMPROFICIENCIES];
	static Uint8 snapEff[NUMEFFECTS];
	static Sint32 snapEffT[NUMEFFECTS];
	static GearSnap snapGear[10];
	static Item** gearSlot(Stat* st, int i)
	{
		Item** s[10] = { &st->helmet, &st->breastplate, &st->gloves, &st->shoes, &st->shield, &st->weapon, &st->cloak, &st->amulet, &st->ring, &st->mask };
		return s[i];
	}
	static void snapCharacter(Stat* st)
	{
		Sint32 a[11] = { st->STR, st->DEX, st->CON, st->INT, st->PER, st->CHR, st->EXP, st->LVL, st->MAXHP, st->MAXMP, st->HUNGER };
		for ( int i = 0; i < 11; ++i ) { snapAttr[i] = a[i]; }
		for ( int i = 0; i < NUMPROFICIENCIES; ++i ) { snapProf[i] = st->getProficiency(i); }
		for ( int i = 0; i < NUMEFFECTS; ++i ) { snapEff[i] = st->getEffectActive(i); snapEffT[i] = st->EFFECTS_TIMERS[i]; }
		for ( int i = 0; i < 10; ++i )
		{
			Item* it = *gearSlot(st, i);
			snapGear[i] = GearSnap();
			if ( it )
			{
				snapGear[i].has = true; snapGear[i].type = (int)it->type; snapGear[i].status = (int)it->status;
				snapGear[i].beatitude = it->beatitude; snapGear[i].appearance = it->appearance; snapGear[i].identified = it->identified;
			}
		}
		snapTaken = true;
		printlog("[apx] bf3 character snapshot: AC %d, STR %d DEX %d CON %d, LVL %d", AC(st), st->STR, st->DEX, st->CON, st->LVL);
	}
	static int restoreCharacter(Stat* st) // the gear repaired or re-created
	{
		if ( !snapTaken ) { return 0; }
		st->STR = snapAttr[0]; st->DEX = snapAttr[1]; st->CON = snapAttr[2]; st->INT = snapAttr[3]; st->PER = snapAttr[4]; st->CHR = snapAttr[5];
		st->EXP = snapAttr[6]; st->LVL = snapAttr[7]; st->MAXHP = snapAttr[8]; st->MAXMP = snapAttr[9]; st->HUNGER = snapAttr[10];
		st->HP = st->MAXHP; st->MP = st->MAXMP;
		for ( int i = 0; i < NUMPROFICIENCIES; ++i ) { st->setProficiencyUnsafe(i, snapProf[i]); }
		for ( int i = 0; i < NUMEFFECTS; ++i ) { st->setEffectValueUnsafe(i, snapEff[i]); st->EFFECTS_TIMERS[i] = snapEffT[i]; }
		int fixes = 0;
		for ( int i = 0; i < 10; ++i )
		{
			const GearSnap& g = snapGear[i];
			if ( !g.has ) { continue; }
			Item** slot = gearSlot(st, i);
			if ( !*slot || (int)(*slot)->type != g.type )
			{
				*slot = newItem((ItemType)g.type, (Status)g.status, g.beatitude, 1, g.appearance, g.identified, &st->inventory);
				++fixes;
			}
			else if ( (int)(*slot)->status != g.status || (*slot)->beatitude != g.beatitude )
			{
				(*slot)->status = (Status)g.status;
				(*slot)->beatitude = g.beatitude;
				++fixes;
			}
		}
		return fixes;
	}
	static std::string gearString(Stat* st) // each slot's status (4 excellent ... 0 broken, - empty)
	{
		std::string s;
		for ( int i = 0; i < 10; ++i ) { Item* it = *gearSlot(st, i); s += it ? std::to_string((int)it->status) : std::string("-"); }
		return s;
	}
	static void freshCharacter(Entity* pe)
	{
		Stat* st = pe ? pe->getStats() : nullptr;
		if ( !st ) { return; }
		if ( !snapTaken ) { snapCharacter(st); }
		A.gearFixes = restoreCharacter(st);
	}

	Entity* attackInFlight = nullptr; // set by Entity::attack (apx::bf3::AttackScope)

	bool absorbPlayerDamage(Entity* player, int amount)
	{
		if ( !on || !absorbing || amount >= 0 || !players[0] || player != players[0]->entity ) { return false; }
		if ( measuring )
		{
			// a hit with no trial creature within melee distance (a trap, a stray) is counted apart
			double nearest = 1e9;
			for ( Uint32 u : trialUids ) { if ( Entity* c = uidToEntity(u) ) { nearest = std::min(nearest, hypot(c->x - player->x, c->y - player->y)); } }
			// a blow: the damage comes inside a trial creature's Entity::attack (the strike lands about 9 ticks after the
			// swing starts, in a later attack() call; s14 tagged swings by their start tick and missed nearly all)
			bool swing = attackInFlight && isTrial(attackInFlight);
			if ( nearest > STRIKERANGE + 16 ) { A.foreignDmg += -amount; }
			else if ( swing ) { A.dmg += -amount; A.dmgHits += -amount; ++A.hits; A.hitTicks.push_back(ticks); }
			else { A.dmg += -amount; A.dmgOther += -amount; ++A.otherEvents; } // bleeding and other damage over time
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
		++A.swing[my->getUID()].first;
		A.attemptTicks.push_back(ticks);
		A.standoffSum += hypot(my->x - target->x, my->y - target->y);
		++A.standoffN;
	}

	void onSwingThin(Entity* my, Entity* target)
	{
		if ( measuring && players[0] && target == players[0]->entity && isTrial(my) ) { ++A.thin; ++A.swing[my->getUID()].second; }
	}

	void onSwingWide(Entity* my, Entity* target)
	{
		if ( measuring && players[0] && target == players[0]->entity && isTrial(my) ) { ++A.wide; ++A.swing[my->getUID()].second; }
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
		// the engine moves an entity between spatial-grid cells (TileEntityList) only when it changes tile inside its
		// own behaviour call; a teleport from outside must re-register it, or every line trace that searches the grid
		// (findEntityInLine: monster swings, sight) misses the character from most directions
		TileEntityList.updateEntity(*pe);
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
		// the arena's walls, to read routes and stuck creatures against: '#' not walkable, '.' floor, 'P' the character
		for ( int y = pTY - 8; y <= pTY + 8; ++y )
		{
			std::string row;
			for ( int x = pTX - 8; x <= pTX + 8; ++x ) { row += (x == pTX && y == pTY) ? 'P' : (tactics::walkable(x, y) ? '.' : '#'); }
			printlog("[apx] bf3 arena row %+d %s", y - pTY, row.c_str());
		}
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
	// the arm's monster: a third field (monstertypename, e.g. "on:12:rat"), else the run's APX_BF3_KIND
	static Monster armKindMon = SKELETON;
	static std::string armKindName = "skeleton";
	static bool armedKind(Monster m) { return m == SKELETON || m == GOBLIN || m == HUMAN; } // these get the bronze sword
	static void setArmMonster(const std::string& a)
	{
		armKindMon = kind; armKindName = kindName;
		size_t c1 = a.find(':'), c2 = c1 == std::string::npos ? c1 : a.find(':', c1 + 1);
		if ( c2 == std::string::npos ) { return; }
		std::string m = a.substr(c2 + 1);
		for ( int i = 1; i < NUMMONSTERS; ++i )
		{
			if ( !_stricmp(monstertypename[i], m.c_str()) ) { armKindMon = (Monster)i; armKindName = monstertypename[i]; return; }
		}
		printlog("[apx] bf3 arm spec %s: unknown monster '%s', using %s", a.c_str(), m.c_str(), kindName.c_str());
	}
	static int armN(const std::string& a) { size_t c = a.find(':'); return c == std::string::npos ? nCreatures : std::max(1, atoi(a.c_str() + c + 1)); }

	static void setArmKnobs(const std::string& spec)
	{
		std::string a = armKind(spec);
		tactics::K = tactics::Knobs();
		tactics::setProbeUs(a == "probe" ? 1000.0 : 0.0); // probe: every knob on plus a 1 ms busy-wait per decide (control)
		if ( a == "off" ) { tactics::K.reach = tactics::K.interleave = tactics::K.allyavoid = tactics::K.ring = false; }
		else if ( a == "vanilla" ) { tactics::K.tactics = false; }
		else if ( a == "nomorale" ) { tactics::K.morale = false; }
		else if ( a == "nopacks" ) { tactics::K.packs = false; }   // control for calls for help and flanks
		else if ( a == "noring" ) { tactics::K.ring = false; }        // control for BF3-4: waiters bunch
		else if ( a == "noreach" ) { tactics::K.reach = false; tactics::K.ring = false; }
		else if ( a == "noally" ) { tactics::K.allyavoid = false; }
		else if ( a == "nojitter" ) { tactics::K.interleave = false; }
		else if ( a == "nodiag" ) { tactics::K.astardiag = false; }   // control for the A* diagonal fix: upstream's test
		flow::enabled = a != "noflow";                             // control for flow fields: the engine's A* for every path
		tactics::setElbow(a == "slots5" ? 16.0 : -1.0);          // slots5: two body widths of swing room -> 5 slots
		tactics::refresh();
	}

	static Entity* spawnOne(int tx, int ty, Uint32 packId, uint8_t role, double hx, double hy, Entity* pe)
	{
		Entity* m = summonMonster(armKindMon, tx * 16 + 8, ty * 16 + 8, true);
		if ( !m ) { return nullptr; }
		if ( Stat* st = m->getStats() )
		{
			if ( armedKind(armKindMon) )
			{
				st->weapon = newItem(BRONZE_SWORD, EXCELLENT, 0, 1, MONSTER_ITEM_UNDROPPABLE_APPEARANCE, false, nullptr);
			}
		}
		tactics::assignPack(m, packId, role, hx, hy);
		m->monsterAcquireAttackTarget(*pe, MONSTER_STATE_PATH);
		trialUids.push_back(m->getUID());
		return m;
	}

	// spreadN: spawn tiles are taken as if spreadN creatures were spread round the arena (default n). A pack's home is
	// the centroid of its spawn tiles, so a lone pack spread all round had its home on the character (s9-1 morale:
	// members "home" within 2 tiles, regrouping in the fight); one pack takes the arc it would hold among three.
	static void spawnPacks(Entity* pe, int n, int pack, bool flankers, int spreadN = 0)
	{
		trialUids.clear();
		int got = (int)spawnTiles.size();
		if ( got == 0 ) { return; }
		const int sn = spreadN > 0 ? spreadN : n;
		std::vector<std::pair<int, int>> pick;
		for ( int i = 0; i < n; ++i ) { pick.push_back(spawnTiles[(size_t)((long long)i * got / sn)]); }
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

	static std::vector<int> bfsFromArena()
	{
		int W = (int)map.width, H = (int)map.height;
		std::vector<int> dist((size_t)W * H, -1);
		std::queue<std::pair<int, int>> q;
		q.push({ pTX, pTY });
		dist[pTX + (size_t)pTY * W] = 0;
		static const int dx[4] = { 1, -1, 0, 0 }, dy[4] = { 0, 0, 1, -1 };
		while ( !q.empty() )
		{
			auto c = q.front(); q.pop();
			for ( int k = 0; k < 4; ++k )
			{
				int x = c.first + dx[k], y = c.second + dy[k];
				if ( x < 0 || y < 0 || x >= W || y >= H || dist[x + (size_t)y * W] >= 0 || !tactics::walkable(x, y) ) { continue; }
				dist[x + (size_t)y * W] = dist[c.first + (size_t)c.second * W] + 1;
				q.push({ x, y });
			}
		}
		return dist;
	}

	// BF3-5 cost gate: creatures spread over the floor 10-60 steps from the arena, packs of four, all on the character
	static void spawnFar(Entity* pe, int n)
	{
		trialUids.clear();
		std::vector<int> dist = bfsFromArena();
		std::vector<std::pair<int, int>> far;
		for ( int y = 1; y < (int)map.height - 1; ++y )
		{
			for ( int x = 1; x < (int)map.width - 1; ++x )
			{
				int d = dist[x + (size_t)y * map.width];
				if ( d >= 10 && d <= 60 ) { far.push_back({ x, y }); }
			}
		}
		if ( far.empty() ) { return; }
		for ( int i = 0; i < n; ++i )
		{
			auto t = far[(size_t)((long long)i * far.size() / n)];
			Uint32 packId = 1 + i / packSize;
			uint8_t role = (i % packSize) == 0 ? tactics::ROLE_LEADER : tactics::ROLE_BRUTE;
			Entity* m = spawnOne(t.first, t.second, packId, role, t.first * 16.0 + 8, t.second * 16.0 + 8, pe);
			if ( m && (i % packSize) == 0 ) { leaderUid = m->getUID(); }
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

	static std::vector<Uint32> foreignUids;
	static void removeForeign(Entity* pe)
	{
		for ( node_t* nd = map.creatures ? map.creatures->first : nullptr; nd; nd = nd->next )
		{
			Entity* c = (Entity*)nd->element;
			Stat* cs = c ? c->getStats() : nullptr;
			if ( !c || c->behavior != &actMonster || !cs || cs->HP <= 0 || isTrial(c) ) { continue; }
			if ( std::find(foreignUids.begin(), foreignUids.end(), c->getUID()) == foreignUids.end() )
			{
				foreignUids.push_back(c->getUID());
				++A.foreignSeen;
				printlog("[apx] bf3 foreign monster %s at %.1f tiles (removed: %s)", monstertypename[cs->type],
					hypot(c->x - pe->x, c->y - pe->y) / 16.0, hypot(c->x - pe->x, c->y - pe->y) <= 16 * 16.0 ? "yes" : "no");
			}
			if ( hypot(c->x - pe->x, c->y - pe->y) <= 16 * 16.0 ) { removeQuietly(c); ++A.foreignRemoved; }
		}
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
			if ( tactics::TacticsState* ts = tactics::find(c) )
			{
				if ( ts->reachIdx >= 0 && ts->slotTarget == pe->getUID() )
				{
					if ( d < STRIKERANGE + tactics::bodyR(pe) ) { ++A.holderIn; }
					else
					{
						++A.holderOut;
						if ( c->monsterState == MONSTER_STATE_ATTACK ) { ++A.holderOutAttack; }
						else if ( c->monsterState == MONSTER_STATE_HUNT ) { ++A.holderOutHunt; }
						else if ( c->monsterState == MONSTER_STATE_PATH ) { ++A.holderOutPath; }
						else { ++A.holderOutWait; }
						if ( ts->stillSince && ticks - ts->stillSince >= (Uint32)TICKS_PER_SECOND ) { ++A.holderOutStill; }
						if ( ts->lastSteer == tactics::STEER_AT_GOAL && hypot(ts->goalX - c->x, ts->goalY - c->y) > tactics::HOLD_EPS + 1 ) { ++A.holderFalseArrive; }
					}
				}
			}
			if ( d < tactics::bodyR(pe) + tactics::bodyR(c) - 0.5 ) { ++A.overlapTicks; }
			if ( d < 6 * 16.0 ) { ++near6; }
			if ( d < STRIKERANGE + tactics::bodyR(pe) )
			{
				++inReach;
				// an attacker can strike: the slot gate (tactics::swingAllowed) holds the ones placed without a reach slot
				if ( c->monsterState == MONSTER_STATE_ATTACK && (Uint32)c->monsterTarget == pe->getUID() )
				{
					if ( tactics::slotGateHolds(c, pe) ) { ++A.unslottedInReachSum; }
					else { ++attackers; }
				}
			}
			else if ( d < 6 * 16.0 ) { waiters.push_back(c); }
		}
		++A.ticks;
		if ( pe->skill[9] != 0 ) { ++A.playerAttackTicks; } // PLAYER_ATTACK (actplayer.cpp): the test character swung
		if ( ticks % 10 == 0 ) { removeForeign(pe); }
		A.inReachSum += inReach; A.attackersSum += attackers; A.near6Sum += near6;
		A.inReachMax = std::max<long long>(A.inReachMax, inReach);
		A.attackersMax = std::max<long long>(A.attackersMax, attackers);
		A.hist[std::min(attackers, 12)]++;
		if ( attackers > A.slots ) { ++A.overflowTicks; }
		if ( attackers > A.slots && A.slots > 0 && A.overflowLogs < 6 )
		{
			++A.overflowLogs;
			for ( Uint32 u : trialUids )
			{
				Entity* c = uidToEntity(u);
				Stat* cs = c ? c->getStats() : nullptr;
				if ( !c || !cs || cs->HP <= 0 ) { continue; }
				double d = hypot(c->x - pe->x, c->y - pe->y);
				if ( d >= STRIKERANGE + tactics::bodyR(pe) || c->monsterState != MONSTER_STATE_ATTACK || (Uint32)c->monsterTarget != pe->getUID()
					|| tactics::slotGateHolds(c, pe) ) { continue; }
				tactics::TacticsState* s = tactics::find(c);
				printlog("[apx] bf3 overflow arm %d t %u uid %u d %.1f reach %d ring %d placed_here %d stance %d flank %d steer %d atk %d decided_ago %d",
					(int)armI, ticks - phT, u, d, s ? s->reachIdx : -9, s ? s->ringIdx : -9, s ? (s->slotTarget == pe->getUID() ? 1 : 0) : -9,
					s ? (int)s->stance : -9, s ? (int)s->flank : -9, s ? (int)s->lastSteer : -9, (int)c->monsterAttack, s ? (int)(ticks - s->decideTick) : -9);
			}
		}
		if ( ticks % 5 == 0 )
		{
			// in contact: in reach and the engine's own swing line (thin ray, packmates block) reaches the target
			hit_t saved = hit;
			int contact = 0;
			for ( Uint32 u : trialUids )
			{
				Entity* c = uidToEntity(u);
				if ( !c || !c->getStats() || c->getStats()->HP <= 0 || (Uint32)c->monsterTarget != pe->getUID() ) { continue; }
				double d = hypot(c->x - pe->x, c->y - pe->y);
				if ( d >= STRIKERANGE + tactics::bodyR(pe) ) { continue; }
				lineTrace(c, c->x, c->y, atan2(pe->y - c->y, pe->x - c->x), STRIKERANGE + 8, LINETRACE_ATK_CHECK_FRIENDLYFIRE, false);
				if ( hit.entity == pe ) { ++contact; }
				else if ( !hit.entity ) { ++A.traceNone; }
				else if ( hit.entity->behavior == &actMonster ) { ++A.traceAlly; }
				else
				{
					++A.traceOther;
					if ( hit.entity->parent == pe->getUID() ) { ++A.traceOtherPlayerOwned; } // a limb or held item of the target
				}
			}
			hit = saved;
			A.contactSum += contact; ++A.contactSamples;
			A.contactMax = std::max<long long>(A.contactMax, contact);
		}
		if ( ticks % 10 == 0 )
		{
			samplePhases(pe);
			if ( waiters.size() >= 2 )
			{
				double s = 0; int k = 0;
				for ( size_t i = 0; i < waiters.size(); ++i ) { for ( size_t j = i + 1; j < waiters.size(); ++j ) { s += hypot(waiters[i]->x - waiters[j]->x, waiters[i]->y - waiters[j]->y); ++k; } }
				A.waitPairSum += s / k; ++A.waitPairN;
				// bunching: nearest neighbour of each waiting creature (same-kind bodies overlap natively)
				for ( size_t i = 0; i < waiters.size(); ++i )
				{
					double nn = 1e9;
					for ( size_t j = 0; j < waiters.size(); ++j ) { if ( j != i ) { nn = std::min(nn, hypot(waiters[i]->x - waiters[j]->x, waiters[i]->y - waiters[j]->y)); } }
					A.nnSum += nn; ++A.nnN; ++A.waiterSamples;
					if ( nn < 2 * tactics::bodyR(waiters[i]) ) { ++A.waiterOverlaps; }
				}
			}
		}
	}

	// creatures with at least one swing that no body stopped: the attackers in contact with the target
	static int contactAttackers()
	{
		int n = 0;
		for ( auto& kv : A.swing ) { if ( kv.second.first > kv.second.second ) { ++n; } }
		return n;
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
		double dp50, dp99, mp50, mp99, tp99, fpm, apm, fb50, fb99;
		timingPercentiles(dp50, dp99, mp50, mp99, tp99);
		flow::pathUsMeans(fpm, apm);
		flow::buildUsPercentiles(fb50, fb99);
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
			.str("kind", armKindName).str("weapon", armedKind(armKindMon) ? "bronze_sword" : "natural").str("group_kind", "emergent")
			.i("creatures", A.spawned).i("pack_size", packSize)
			.str("pack_template", "trial-3x4").i("arena_x", pTX).i("arena_y", pTY).d("window_s", secs)
			.i("dmg", A.dmg).d("dmg_pm", A.dmg / (secs / 60.0)).d("dmg_in_per_player_per_min", A.dmg / (secs / 60.0))
			.i("dmg_hits", A.dmgHits).i("dmg_other", A.dmgOther).i("other_events", A.otherEvents)
			.d("dmg_per_hit", A.hits ? (double)A.dmgHits / A.hits : 0).i("ac_start", A.acStart)
			.i("ac_end", (players[0] && players[0]->entity && players[0]->entity->getStats()) ? AC(players[0]->entity->getStats()) : -1)
			.str("gear_end", (players[0] && players[0]->entity && players[0]->entity->getStats()) ? gearString(players[0]->entity->getStats()) : std::string("?"))
			.i("gear_fixes", A.gearFixes)
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
			.d("waiters_nn_mean", A.nnN ? A.nnSum / A.nnN : 0).d("waiters_overlap_share", A.waiterSamples ? (double)A.waiterOverlaps / A.waiterSamples : 0)
			.i("contact_attackers", contactAttackers()).d("blocked_share", A.attempts ? (double)(A.thin + A.wide) / A.attempts : 0)
			.d("contact_mean", A.contactSamples ? (double)A.contactSum / A.contactSamples : 0).i("contact_max", A.contactMax)
			.i("trace_ally", A.traceAlly).i("trace_none", A.traceNone).i("trace_other", A.traceOther).i("trace_other_player_owned", A.traceOtherPlayerOwned)
			.i("pin_corrections", A.pinCorrections).i("foreign_seen", A.foreignSeen).i("foreign_removed", A.foreignRemoved)
			.i("holder_in", A.holderIn).i("holder_out", A.holderOut).i("holder_out_attack", A.holderOutAttack).i("holder_out_hunt", A.holderOutHunt)
			.i("holder_out_path", A.holderOutPath).i("holder_out_wait", A.holderOutWait).i("holder_out_still", A.holderOutStill)
			.i("holder_false_arrive", A.holderFalseArrive)
			.i("flank_fail_target", C.flank_fail_target).i("flank_fail_progress", C.flank_fail_progress).i("flank_fail_time", C.flank_fail_time)
			.i("flank_fail_nogoal", C.flank_fail_nogoal).i("flank_fail_noroute", C.flank_fail_noroute)
			.i("route_blocked_replans", C.route_blocked_replans).i("breakables_hit", C.breakables_hit).i("goal_moved_out", C.goal_moved_out).i("flank_park", C.flank_park).i("no_path_holds", C.no_path_holds).i("arrival_decides", C.arrival_decides).i("swings_held_no_slot", C.swings_held_no_slot)
			.d("in_reach_unslotted_mean", A.ticks ? (double)A.unslottedInReachSum / A.ticks : 0)
			.i("goal_routes", C.goal_routes).i("goal_route_fail", C.goal_route_fail).i("goal_route_steps", C.goal_route_steps)
			.i("arc_fallback", C.arc_fallback).i("step_out", C.step_out).i("doors_opened", C.doors_opened).i("door_bumps_locked", C.door_bumps_locked)
			.i("routed_engaged", C.routed_engaged).i("routed_promoted", C.routed_promoted)
			.i("flank_started", C.flank_started).i("flank_arrived", C.flank_arrived).i("flank_failed", C.flank_failed)
			.i("retreats", C.retreats).i("regroups", C.regroups).i("morale_breaks", C.morale_breaks).i("morale_events", C.morale_events)
			.i("calls_for_help", C.calls_for_help).i("calls_answered", C.calls_answered).i("route_around", C.route_around).i("reach_full", C.reach_full).i("surrenders", C.surrenders_offered).i("packs_formed", C.packs_formed)
			.i("flow_builds", flow::ST.flow_builds).i("flow_cells_updated", flow::ST.flow_cells_updated)
			.i("flow_fallback_astar", flow::ST.flow_fallback_astar).i("flow_paths", flow::ST.flow_paths)
			.i("astar_paths", flow::ST.astar_paths).i("astar_null", flow::ST.astar_null).i("grid_builds", flow::ST.grid_builds)
			.d("flow_path_us_mean", fpm).d("astar_path_us_mean", apm).d("flow_build_us_p50", fb50).d("flow_build_us_p99", fb99)
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
			.str("kind", kindName).str("group_kind", "spawned").i("members_at_kill", membersAtKill).i("retreating_at_3s", retreatAt3s)
			.i("should_retreat_at_3s", shouldRetreatAt3s).d("dist_at_kill", distAtKill).d("dist_at_3s", distAt3s).i("home_at_5s", homeAt5s)
			.i("retreats", C.retreats).i("morale_breaks", C.morale_breaks).i("regroups", C.regroups)
			.d("first_regroup_s", firstRegroup ? (firstRegroup - killTick) / (double)TICKS_PER_SECOND : -1)
			.i("home_moved", C.home_moved).i("surrenders", C.surrenders_offered).i("morale_events", C.morale_events)
			.i("creature_dmg", A.creatureDmg).i("input_keys", inKeys - A.keys0).i("input_buttons", inButtons - A.buttons0);
		write("bf3-morale.jsonl", r);
		printlog("[apx] bf3 morale %s: retreating at 3 s %d of %d, dist %.1f -> %.1f, regroups %lld", moraleArm == 0 ? "on" : "off",
			retreatAt3s, membersAtKill, distAtKill, distAt3s, C.regroups);
	}

	static void go(Phase p) { ph = p; phT = ticks; }

	// one frame per arm kind ("on", "noring", "off") at the middle of its first measured window
	static std::string shotWanted, shotGiven;
	static std::vector<std::string> shotKinds;
	const char* screenshotWanted()
	{
		if ( shotWanted.empty() ) { return nullptr; }
		shotGiven = shotWanted;
		shotWanted.clear();
		return shotGiven.c_str();
	}

	// BF3-5a: the field against the engine's A* on random pairs of walkable tiles of this floor
	static void flowTestFloor(Entity* pe, int floorNo)
	{
		std::vector<std::pair<int, int>> open;
		for ( int y = 1; y < (int)map.height - 1; ++y ) { for ( int x = 1; x < (int)map.width - 1; ++x ) { if ( tactics::walkable(x, y) ) { open.push_back({ x, y }); } } }
		Uint32 rs = 2654435761u * (Uint32)(floorNo + 1) ^ (seedBase ? seedBase : 77u);
		auto rnd = [&rs]() { rs ^= rs << 13; rs ^= rs >> 17; rs ^= rs << 5; return rs; };
		int pairs = 0, fieldOk = 0, astarOk = 0, bothOk = 0, stepsEqual = 0, stepsWithin1 = 0, stepsDiffMax = 0, costEqual = 0;
		int fieldCheaper = 0, fieldDearer = 0, doorPairs = 0, doorWithin1 = 0, chaseFail = 0, astarOnly = 0, fieldOnly = 0;
		long long fieldDiag = 0, astarDiag = 0, astarDiagNeg = 0, astarCornerCut = 0, fieldSteps = 0, astarSteps = 0;
		long long upOk = 0, upDiag = 0, upSteps = 0, upCost = 0, fixCost = 0, upBoth = 0;
		std::vector<double> build;
		// the walker: one of the trial's own kind (the A*'s rules depend on who walks: a player walker skips different
		// entities). The floor's monsters were removed at load (P_FT_WAIT), so none blocks either side.
		Entity* probe = open.empty() ? nullptr : summonMonster(kind, open[0].first * 16 + 8, open[0].second * 16 + 8, true);
		if ( !probe ) { printlog("[apx] bf3 flowtest floor %d: no probe monster, using the character", floorNo); probe = pe; }
		for ( int i = 0; i < flowTest && open.size() > 1; ++i )
		{
			auto s = open[rnd() % open.size()], g = open[rnd() % open.size()];
			if ( s == g ) { continue; }
			flow::Compare c = flow::compare(probe, s.first, s.second, g.first, g.second);
			++pairs;
			build.push_back(c.buildUs);
			fieldOk += c.fieldOk; astarOk += c.astarOk;
			upOk += c.upOk;
			if ( c.upOk ) { upDiag += c.upDiag; }
			if ( c.upOk && c.astarOk ) { ++upBoth; upSteps += c.upSteps; upCost += c.upCost; fixCost += c.astarCost; }
			if ( c.astarOk && !c.fieldOk ) { ++astarOnly; }
			if ( c.fieldOk && !c.astarOk ) { ++fieldOnly; }
			list_t* chase = generatePath(s.first, s.second, g.first, g.second, probe, nullptr, GeneratePathTypes::GENERATE_PATH_TO_HUNT_MONSTER_TARGET);
			if ( chase ) { list_FreeAll(chase); free(chase); } else { ++chaseFail; }
			if ( !(c.fieldOk && c.astarOk) ) { continue; }
			++bothOk;
			int sd = abs(c.fieldSteps - c.astarSteps);
			stepsDiffMax = std::max(stepsDiffMax, sd);
			stepsEqual += sd == 0; stepsWithin1 += sd <= 1;
			costEqual += c.fieldCost == c.astarCost; fieldCheaper += c.fieldCost < c.astarCost; fieldDearer += c.fieldCost > c.astarCost;
			if ( c.door ) { ++doorPairs; doorWithin1 += sd <= 1; }
			fieldDiag += c.fieldDiag; astarDiag += c.astarDiag; astarDiagNeg += c.astarDiagNeg; astarCornerCut += c.astarCornerCut;
			fieldSteps += c.fieldSteps; astarSteps += c.astarSteps;
		}
		if ( probe != pe ) { removeQuietly(probe); }
		std::sort(build.begin(), build.end());
		double b50 = build.empty() ? 0 : build[(size_t)(0.5 * (build.size() - 1) + 0.5)], b99 = build.empty() ? 0 : build[(size_t)(0.99 * (build.size() - 1) + 0.5)];
		ApxRow r;
		commonFields(r);
		r.str("trial", "bf3-flowtest").i("floor_no", floorNo).i("map_w", (long long)map.width).i("map_h", (long long)map.height)
			.i("walkable", (long long)open.size()).i("pairs", pairs).i("field_ok", fieldOk).i("astar_ok", astarOk).i("both_ok", bothOk)
			.i("astar_only", astarOnly).i("steps_equal", stepsEqual).i("steps_within1", stepsWithin1).i("steps_diff_max", stepsDiffMax)
			.i("cost_equal", costEqual).i("cost_field_cheaper", fieldCheaper).i("cost_field_dearer", fieldDearer)
			.i("door_pairs", doorPairs).i("door_within1", doorWithin1).i("chase_astar_fail", chaseFail)
			.i("field_only", fieldOnly).i("field_steps", fieldSteps).i("astar_steps", astarSteps).i("field_diag_steps", fieldDiag)
			.i("astar_diag_steps", astarDiag).i("astar_diag_neg_x", astarDiagNeg).i("astar_corner_cuts", astarCornerCut)
			.str("probe", probe == pe ? "character" : "monster").i("astar_diag_fix", tactics::astarDiagOn ? 1 : 0)
			.i("upstream_ok", upOk).i("upstream_diag_steps", upDiag).i("upstream_both_ok", upBoth)
			.i("upstream_steps", upSteps).i("upstream_cost", upCost).i("fixed_cost_same_pairs", fixCost)
			.d("build_us_p50", b50).d("build_us_p99", b99);
		write("bf3-flowtest.jsonl", r);
		printlog("[apx] bf3 flowtest floor %d: %d pairs, both ok %d, steps equal %d within1 %d (max diff %d), field dearer %d, "
			"astar-only %d, chase A* fails %d, build p50 %.0f us p99 %.0f us", floorNo, pairs, bothOk, stepsEqual, stepsWithin1,
			stepsDiffMax, fieldDearer, astarOnly, chaseFail, b50, b99);
	}

	// the nearest tile 14-30 steps from the arena that no tile of the fight can see (the engine's own alert reaches
	// only allies the victim can see) with room for a pack of four
	static void findHiddenSpot()
	{
		hsSearched = true;
		int W = (int)map.width, H = (int)map.height;
		std::vector<int> dist((size_t)W * H, -1);
		std::queue<std::pair<int, int>> q;
		q.push({ pTX, pTY });
		dist[pTX + (size_t)pTY * W] = 0;
		static const int dx[4] = { 1, -1, 0, 0 }, dy[4] = { 0, 0, 1, -1 };
		while ( !q.empty() )
		{
			auto c = q.front(); q.pop();
			for ( int k = 0; k < 4; ++k )
			{
				int x = c.first + dx[k], y = c.second + dy[k];
				if ( x < 0 || y < 0 || x >= W || y >= H || dist[x + (size_t)y * W] >= 0 || !tactics::walkable(x, y) ) { continue; }
				dist[x + (size_t)y * W] = dist[c.first + (size_t)c.second * W] + 1;
				q.push({ x, y });
			}
		}
		std::vector<std::pair<double, double>> views;   // every walkable tile of the fight: within 5 tiles of the arena
		for ( int y = pTY - 8; y <= pTY + 8; ++y )
		{
			for ( int x = pTX - 8; x <= pTX + 8; ++x )
			{
				if ( x < 0 || y < 0 || x >= W || y >= H || hypot(x - pTX, y - pTY) > 8 || dist[x + (size_t)y * W] < 0 ) { continue; }
				views.push_back({ x * 16.0 + 8, y * 16.0 + 8 });
			}
		}
		int best = 1 << 30;
		for ( int y = 1; y < H - 1; ++y )
		{
			for ( int x = 1; x < W - 1; ++x )
			{
				int bd = dist[x + (size_t)y * W];
				if ( bd < 14 || bd > 30 || bd >= best ) { continue; }
				int room = 0;
				for ( int k = 0; k < 4; ++k ) { if ( tactics::walkable(x + dx[k], y + dy[k]) ) { ++room; } }
				if ( room < 3 ) { continue; }
				bool seen = false;   // the spot and its four neighbours, from every tile of the fight
				for ( int k = -1; k < 4 && !seen; ++k )
				{
					int sx = k < 0 ? x : x + dx[k], sy = k < 0 ? y : y + dy[k];
					if ( !tactics::walkable(sx, sy) ) { continue; }
					for ( auto& v : views ) { if ( tactics::wallClear(nullptr, v.first, v.second, sx * 16.0 + 8, sy * 16.0 + 8) ) { seen = true; break; } }
				}
				if ( seen ) { continue; }
				best = bd; hsX = x; hsY = y; hsBfs = bd;
			}
		}
		printlog("[apx] bf3 help: hidden spot %d,%d at %d steps from the arena", hsX, hsY, hsBfs);
	}

	static void spawnHelpers()
	{
		helpUids.clear();
		std::vector<std::pair<int, int>> tiles{ { hsX, hsY } };
		static const int dx[8] = { 1, -1, 0, 0, 1, 1, -1, -1 }, dy[8] = { 0, 0, 1, -1, 1, -1, 1, -1 };
		for ( int k = 0; k < 8 && tiles.size() < (size_t)packSize; ++k ) { if ( tactics::walkable(hsX + dx[k], hsY + dy[k]) ) { tiles.push_back({ hsX + dx[k], hsY + dy[k] }); } }
		for ( auto& t : tiles )
		{
			Entity* m = summonMonster(kind, t.first * 16 + 8, t.second * 16 + 8, true);
			if ( !m ) { continue; }
			if ( Stat* st = m->getStats() ) { if ( kind == SKELETON || kind == GOBLIN || kind == HUMAN ) { st->weapon = newItem(BRONZE_SWORD, EXCELLENT, 0, 1, MONSTER_ITEM_UNDROPPABLE_APPEARANCE, false, nullptr); } }
			tactics::assignPack(m, 2, tactics::ROLE_BRUTE, hsX * 16.0 + 8, hsY * 16.0 + 8);
			helpUids.push_back(m->getUID());
			trialUids.push_back(m->getUID());
		}
	}

	static void writeHelpRow(const char* reason)
	{
		using namespace tactics;
		Entity* pe = players[0] ? players[0]->entity : nullptr;
		int within4 = 0, targeting = 0;
		for ( Uint32 u : helpUids )
		{
			Entity* c = uidToEntity(u);
			if ( !c || !pe ) { continue; }
			if ( hypot(c->x - pe->x, c->y - pe->y) <= 4 * 16.0 ) { ++within4; }
			if ( (Uint32)c->monsterTarget == pe->getUID() ) { ++targeting; }
		}
		ApxRow r;
		commonFields(r);
		r.str("trial", "bf3-help").str("arm_kind", helpArm == 0 ? "help_on" : "help_off_nopacks").str("knobs", knobString())
			.str("reason", reason).str("kind", kindName).str("group_kind", "spawned").i("spot_x", hsX).i("spot_y", hsY).i("spot_steps", hsBfs).i("helpers", (long long)helpUids.size())
			.i("calls_for_help", C.calls_for_help).i("calls_answered", C.calls_answered).i("helpers_sent", C.helpers_sent)
			.i("helpers_arrived", C.helpers_arrived).i("b_within4_end", within4).i("b_targeting_player_end", targeting)
			.d("window_s", helpWindowS).i("b_within4_window", within4Window).i("b_acquired", (long long)helpAcquired.size())
			.d("b_first_acquire_s", firstHelpAcquire ? (firstHelpAcquire - callTick) / (double)TICKS_PER_SECOND : -1)
			.d("b_first_arrival_s", firstHelpArrival ? (firstHelpArrival - callTick) / (double)TICKS_PER_SECOND : -1)
			.d("b_min_dist_tiles", helpMinDist < 1e8 ? helpMinDist / 16.0 : -1)
			.i("input_keys", inKeys - A.keys0).i("input_buttons", inButtons - A.buttons0);
		write("bf3-help.jsonl", r);
		printlog("[apx] bf3 help %s: calls %lld answered %lld sent %lld, pack B within 4 tiles %d of %d at %.1f s (%d at 30 s), first arrival %.1f s",
			helpArm == 0 ? "on" : "off", C.calls_for_help, C.calls_answered, C.helpers_sent, within4Window, (int)helpUids.size(), helpWindowS,
			within4, firstHelpArrival ? (firstHelpArrival - callTick) / (double)TICKS_PER_SECOND : -1.0);
	}

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
			bool moved = pe->x != pinX || pe->y != pinY;
			pe->x = pinX; pe->y = pinY; pe->yaw = pinYaw; pe->vel_x = 0; pe->vel_y = 0;
			if ( moved ) { TileEntityList.updateEntity(*pe); ++A.pinCorrections; } // keep the grid cell right (see chooseArena)
		}
		if ( ph >= P_PREP && !pe ) { printlog("[apx] bf3: the test character is gone"); finish(5); return; }
		if ( !pe && ph != P_WAIT_GAME && ph != P_WAIT_LOAD ) { return; } // a floor is loading
		switch ( ph )
		{
			case P_WAIT_GAME:
				if ( pe && !loading && ticks > 100 && flowTest > 0 ) { go(P_FT_LOAD); }
				else if ( pe && !loading && ticks > 100 ) { go(P_LOAD); }
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
				if ( ticks - phT >= 60 ) { go(armI < arms.size() ? P_SPAWN : (moraleTest ? P_M_SPAWN : (helpTest ? P_H_SPAWN : P_DONE))); if ( ph == P_DONE ) { finish(0); } }
				break;
			case P_SPAWN:
				setArmKnobs(arms[armI]);
				setArmMonster(arms[armI]);
				tactics::onLevelChange();
				A = Arm();
				freshCharacter(pe);
				A.keys0 = inKeys; A.buttons0 = inButtons; A.motion0 = inMotion; A.other0 = inOther;
				if ( armKind(arms[armI]) == "flow" || armKind(arms[armI]) == "noflow" ) { spawnFar(pe, armN(arms[armI])); }
				else { spawnPacks(pe, armN(arms[armI]), packSize, true); }
				flow::resetStats();
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
					flow::resetStats();
					long long k0 = A.keys0, b0 = A.buttons0, m0 = A.motion0, o0 = A.other0;
					int slots = A.slots, spawned = A.spawned, fixes = A.gearFixes;
					A = Arm();
					A.keys0 = k0; A.buttons0 = b0; A.motion0 = m0; A.other0 = o0; A.slots = slots; A.spawned = spawned; A.gearFixes = fixes;
					if ( Stat* pst = pe ? pe->getStats() : nullptr ) { A.acStart = AC(pst); }
					measuring = true;
					go(P_MEASURE);
				}
				break;
			case P_MEASURE:
				sample(pe);
				if ( dumpCreatures && ticks != phT && (ticks - phT) % (Uint32)(measureTicks / 4) == 0 && ticks - phT < (Uint32)measureTicks )
				{
					for ( Uint32 u : trialUids )
					{
						Entity* c = uidToEntity(u);
						Stat* cs = c ? c->getStats() : nullptr;
						if ( !c || !cs ) { printlog("[apx] bf3 dump arm %d uid %u gone", (int)armI, u); continue; }
						tactics::TacticsState* s = tactics::find(c);
						printlog("[apx] bf3 dump arm %d t %u uid %u hp %d d %.1f ang %.2f st %d tgt %d elig %d reach %d ring %d stance %d flank %d atk %d spec %d ally %d leader %u"
							" steer %d goal_d %.1f goal_ang %.2f goal_gap %.1f goal_clear %d still %u route %d/%d kind %d ok %d at %.1f,%.1f moved %.2f bump %d steered_ago %d",
							(int)armI, ticks - phT, u, cs->HP, hypot(c->x - pe->x, c->y - pe->y), atan2(c->y - pe->y, c->x - pe->x), (int)c->monsterState,
							(Uint32)c->monsterTarget == pe->getUID() ? 1 : 0, tactics::eligible(c, *cs) ? 1 : 0, s ? s->reachIdx : -9, s ? s->ringIdx : -9,
							s ? (int)s->stance : -9, s ? (int)s->flank : -9, (int)c->monsterAttack, (int)c->monsterSpecialState, (int)c->monsterAllyIndex, cs->leader_uid,
							s ? (int)s->lastSteer : -9, s ? hypot(s->goalX - pe->x, s->goalY - pe->y) : -1.0, s ? atan2(s->goalY - pe->y, s->goalX - pe->x) : 0.0,
							s ? hypot(s->goalX - c->x, s->goalY - c->y) : -1.0, s ? (tactics::wallClear(c, c->x, c->y, s->goalX, s->goalY) ? 1 : 0) : -9,
							s && s->stillSince ? ticks - s->stillSince : 0u, s ? (int)s->routeAt : -9, s ? (int)s->route.size() : -9, s ? (int)s->routeKind : -9, s ? (int)s->routeOk : -9,
							c->x, c->y, s ? s->lastMoved : -1.0, s ? s->lastBump : -9, s ? (int)(ticks - s->steeredTick) : -9);
					}
				}
				if ( ticks - phT == (Uint32)(measureTicks / 2) )
				{
					std::string k = armKind(arms[armI]);
					if ( (k == "on" || k == "noring" || k == "off") && armN(arms[armI]) == nCreatures
						&& std::find(shotKinds.begin(), shotKinds.end(), k) == shotKinds.end() )
					{
						shotKinds.push_back(k);
						shotWanted = "bf3-shot-" + k + ".png";
						printlog("[apx] bf3 screenshot %s (arm %d)", shotWanted.c_str(), (int)armI);
					}
				}
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
					else if ( helpTest && helpArm < 2 ) { go(P_H_SPAWN); }
					else { finish(0); }
				}
				break;
			case P_M_SPAWN:
				setArmKnobs(moraleArm == 0 ? "on" : "nomorale");
				setArmMonster("");
				tactics::onLevelChange();
				tactics::resetCounters();
				A = Arm();
				freshCharacter(pe);
				A.keys0 = inKeys; A.buttons0 = inButtons; A.motion0 = inMotion; A.other0 = inOther;
				spawnPacks(pe, packSize, packSize, false, 3 * packSize);
				firstRegroup = 0;
				homeAt5s = -1;
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
				if ( dumpCreatures && ticks != killTick && (ticks - killTick) % TICKS_PER_SECOND == 0 && ticks - killTick <= 4 * TICKS_PER_SECOND )
				{
					for ( Uint32 u : trialUids )
					{
						Entity* c = uidToEntity(u);
						if ( u == leaderUid || !c || !c->getStats() || c->getStats()->HP <= 0 ) { continue; }
						tactics::TacticsState* s = tactics::find(c);
						printlog("[apx] bf3 dump morale %d t %u uid %u d %.1f ang %.2f st %d tgt %d stance %d morale %d steer %d home_d %.1f route %d/%d kind %d moved %.2f bump %d steered_ago %d at %.1f,%.1f",
							moraleArm, ticks - killTick, u, hypot(c->x - pe->x, c->y - pe->y), atan2(c->y - pe->y, c->x - pe->x), (int)c->monsterState,
							(Uint32)c->monsterTarget == pe->getUID() ? 1 : 0, s ? (int)s->stance : -9, s ? (int)s->morale : -999, s ? (int)s->lastSteer : -9,
							s ? hypot(s->homeX - c->x, s->homeY - c->y) : -1.0, s ? (int)s->routeAt : -9, s ? (int)s->route.size() : -9,
							s ? (int)s->routeKind : -9, s ? s->lastMoved : -1.0, s ? s->lastBump : -9, s ? (int)(ticks - s->steeredTick) : -9, c->x, c->y);
					}
				}
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
				if ( ticks - killTick == 5 * TICKS_PER_SECOND )
				{
					homeAt5s = 0;
					for ( Uint32 u : trialUids )
					{
						Entity* c = uidToEntity(u);
						if ( u == leaderUid || !c || !c->getStats() || c->getStats()->HP <= 0 ) { continue; }
						tactics::TacticsState* ms = tactics::find(c);
						if ( ms && (ms->stance == tactics::STANCE_REGROUP || hypot(ms->homeX - c->x, ms->homeY - c->y) <= 2 * 16.0) ) { ++homeAt5s; }
					}
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
			case P_FT_LOAD:
				if ( ftI >= ftFloors.size() ) { if ( arms.empty() ) { finish(0); } else { go(P_LOAD); } break; } // then the arms on floor 1
				currentlevel = ftFloors[ftI] - 1;
				loadnextlevel = true;
				go(P_FT_WAIT);
				break;
			case P_FT_WAIT:
				if ( pe && !loadnextlevel && !loading && currentlevel == ftFloors[ftI] && ticks - phT > 100 ) { killAllMonsters(); go(P_FT_RUN); }
				else if ( ticks - phT > 50 * 120 ) { finish(3); }
				break;
			case P_FT_RUN:
				if ( ticks - phT < 10 ) { break; } // the removed monsters leave the entity list first
				flowTestFloor(pe, ftFloors[ftI]);
				++ftI;
				go(P_FT_LOAD);
				break;
			case P_H_SPAWN:
				setArmKnobs(helpArm == 0 ? "on" : "nopacks");
				setArmMonster("");
				tactics::onLevelChange();
				tactics::resetCounters();
				A = Arm();
				freshCharacter(pe);
				A.keys0 = inKeys; A.buttons0 = inButtons; A.motion0 = inMotion; A.other0 = inOther;
				if ( !hsSearched ) { findHiddenSpot(); }
				firstHelpArrival = 0; helpMinDist = 1e9; callTick = 0;
				if ( hsX < 0 ) { writeHelpRow("no_hidden_spot"); ++helpArm; go(P_CLEAN); break; }
				spawnPacks(pe, packSize, packSize, false);
				spawnHelpers();
				go(P_H_SETTLE);
				break;
			case P_H_SETTLE:
				if ( ticks - phT == 2 )
				{
					for ( Uint32 u : trialUids )
					{
						Entity* c = uidToEntity(u);
						if ( !c ) { continue; }
						c->setEffect(EFF_ASLEEP, false, 0, false);
						if ( std::find(helpUids.begin(), helpUids.end(), u) == helpUids.end() ) { c->monsterAcquireAttackTarget(*pe, MONSTER_STATE_PATH); }
					}
				}
				// the call comes from the fight: once pack A's leader is within 3 tiles of the character (or after 10 s)
				if ( ticks - phT >= 100 && (ticks - phT >= 500 || (uidToEntity(leaderUid) && hypot(uidToEntity(leaderUid)->x - pe->x, uidToEntity(leaderUid)->y - pe->y) <= 3 * 16.0)) )
				{
					if ( Entity* v = uidToEntity(leaderUid) ) { v->alertAlliesOnBeingHit(pe, nullptr); } // the engine's own call
					callTick = ticks;
					helpAcquired.clear(); firstHelpAcquire = 0; within4Window = -1; helpWindowS = 0;
					for ( Uint32 u : helpUids )
					{
						Entity* h = uidToEntity(u);
						if ( !h ) { continue; }
						double v = .045 * (h->monsterGetDexterityForMovement() + 10) * TICKS_PER_SECOND; // units per second
						helpWindowS = std::min(hsBfs * 16.0 / std::max(v, 1.0) + 5.0, 28.0); // one walk + 5 s, inside the 30 s observation
						break;
					}
					go(P_H_OBSERVE);
				}
				break;
			case P_H_OBSERVE:
				for ( Uint32 u : helpUids )
				{
					Entity* c = uidToEntity(u);
					if ( !c ) { continue; }
					double d = hypot(c->x - pe->x, c->y - pe->y);
					helpMinDist = std::min(helpMinDist, d);
					if ( !firstHelpArrival && d <= 4 * 16.0 ) { firstHelpArrival = ticks; }
					if ( (Uint32)c->monsterTarget == pe->getUID() && std::find(helpAcquired.begin(), helpAcquired.end(), u) == helpAcquired.end() )
					{
						helpAcquired.push_back(u);
						if ( !firstHelpAcquire ) { firstHelpAcquire = ticks; }
						printlog("[apx] bf3 help %s: B uid %u took the character as target %.1f s after the call, %.1f tiles away, state %d",
							helpArm == 0 ? "on" : "off", u, (ticks - callTick) / (double)TICKS_PER_SECOND, d / 16.0, (int)c->monsterState);
					}
				}
				if ( within4Window < 0 && helpWindowS > 0 && ticks - callTick >= (Uint32)(helpWindowS * TICKS_PER_SECOND) )
				{
					within4Window = 0;
					for ( Uint32 u : helpUids )
					{
						Entity* c = uidToEntity(u);
						if ( c && hypot(c->x - pe->x, c->y - pe->y) <= 4 * 16.0 ) { ++within4Window; }
					}
				}
				if ( ticks - callTick >= 30 * TICKS_PER_SECOND )
				{
					writeHelpRow("");
					cleanup();
					helpUids.clear();
					++helpArm;
					go(P_CLEAN);
				}
				break;
			default:
				break;
		}
	}
}
}
