// apx/sim.cpp -- BF1-4: sim mode, scenario runner and bot players.
//
//   -apx-sim                   (or APX_SIM=1)   fast-forward: every main-loop iteration runs APX_SIM_BATCH (50) real
//                                               gameLogic() ticks, no frame limiter. Drawing still happens once per
//                                               iteration, so no logic that lives in draw paths is skipped.
//   -apx-scenario=<file.json>                   scenario runner (see Scenario below)
//   APX_SIM_TICKS=<n>   APX_SIM_WALL_S=<s>      end of run; a wall-limit timeout exits non-zero (code 3)
//   APX_DRAW_COMPARE... not here: a drawn run is simply the same binary without -apx-sim.
//
// Bots drive the REAL player body: apxBotThink(my) runs at the top of actPlayer and writes the same input
// bindings a device would (Input::apxSet); it never writes HP, position or damage. Aim turns my->yaw
// (what the Turn bindings change; the camera follows it), with a seeded error.
//
// sim mode sets buddhamode (HP never goes below 1) so a wipe is an observable event, not a game over;
// the encounter rows count the lethal hit as a death (`death_model` in the scenario rows = buddha_hp1).
#include "telemetry.hpp"
#include "../main.hpp"
#include "../game.hpp"
#include "../entity.hpp"
#include "../monster.hpp"
#include "../player.hpp"
#include "../stat.hpp"
#include "../input.hpp"
#include "../draw.hpp"
#include "../prng.hpp"
#include "../paths.hpp"
#include "../menu.hpp"
#include "../ui/MainMenu.hpp"
#include "../charclass.hpp"
#include <cmath>
#include <map>
#include <cstdlib>
#include <cstring>
#include <random>
#include <string>
#include <vector>
#include "rapidjson/document.h"


namespace apx
{
	bool simOn = false;
	std::string scenarioName;
	double botSkill = -1;
	int partyN = 1;
	int simExit = 0;
	long floorsLoaded = 0;

	struct Spawn { std::string kind; int n = 1; int ring = 10; int monster = 0; };
	struct Scenario
	{
		int party = 1, floor = 1, encounters = 10, reloadEvery = 25, runs = 10;
		bool walk = false;
		Uint32 seed = 77;
		double skill = 0.7;
		std::vector<Spawn> spawns;
		std::string name;
		bool loaded = false;
	};
	static Scenario S;
	static std::mt19937 rng;
	static int batch = 50;
	static Uint32 simTicksLimit = 0;
	static double simWallLimit = 0;
	static double simStartWall = 0;
	enum State { ST_WAIT_GAME, ST_LOAD, ST_WAIT_LOAD, ST_PLACE, ST_FIGHT, ST_WALK_INIT, ST_WALK, ST_WAIT_DESCENT, ST_DONE };
	static int runIdx = 0;
	static Uint32 walkStart = 0, walkNoMove = 0;
	static Uint32 walkLadderUid = 0;
	static real_t walkLastX = 0, walkLastY = 0;
	static int walkHpMin = 0;
	static long walkEncStart = 0;
	static State st = ST_WAIT_GAME;
	static int encDone = 0, sinceReload = 0;
	static Uint32 stateTick = 0, fightStart = 0, nextFightCheck = 0;
	static long loadSeen = 0, closedAtStart = 0;
	static real_t lastX = 0, lastY = 0;
	static long lastHitsLanded = 0;
	static bool stuckThisEnc = false;
	static long stuckEncounters = 0;
	static bool endDeadline = false;
	static bool stateWaitsForLoad = false;

	void simInit(const char* scenarioPath)
	{
		const char* e = getenv("APX_SIM");
		simOn = (scenarioPath != nullptr) || (e && e[0] == '1');
		if ( !simOn ) { return; }
		const char* b = getenv("APX_SIM_BATCH");
		if ( b && atoi(b) > 0 ) { batch = atoi(b); }
		const char* t = getenv("APX_SIM_TICKS");
		if ( t ) { simTicksLimit = (Uint32)strtoul(t, nullptr, 10); }
		const char* w = getenv("APX_SIM_WALL_S");
		if ( w ) { simWallLimit = atof(w); }
		if ( scenarioPath && *scenarioPath )
		{
			FILE* f = fopen(scenarioPath, "rb");
			if ( !f ) { printlog("[apx] cannot open scenario %s", scenarioPath); simExit = 2; return; }
			std::string txt;
			char buf[4096];
			size_t n;
			while ( (n = fread(buf, 1, sizeof(buf), f)) > 0 ) { txt.append(buf, n); }
			fclose(f);
			rapidjson::Document d;
			d.Parse(txt.c_str());
			if ( d.HasParseError() || !d.IsObject() ) { printlog("[apx] bad scenario json"); simExit = 2; return; }
			if ( d.HasMember("party") ) { S.party = d["party"].GetInt(); }
			if ( d.HasMember("floor") ) { S.floor = d["floor"].GetInt(); }
			if ( d.HasMember("seed") ) { S.seed = (Uint32)d["seed"].GetUint64(); }
			if ( d.HasMember("encounters") ) { S.encounters = d["encounters"].GetInt(); }
			if ( d.HasMember("reload_every") ) { S.reloadEvery = d["reload_every"].GetInt(); }
			if ( d.HasMember("name") ) { S.name = d["name"].GetString(); }
			if ( d.HasMember("mode") && !strcmp(d["mode"].GetString(), "walk") ) { S.walk = true; }
			if ( d.HasMember("runs") ) { S.runs = d["runs"].GetInt(); }
			if ( d.HasMember("bot") && d["bot"].IsObject() && d["bot"].HasMember("skill") ) { S.skill = d["bot"]["skill"].GetDouble(); }
			if ( d.HasMember("spawn") && d["spawn"].IsArray() )
			{
				for ( auto& s : d["spawn"].GetArray() )
				{
					Spawn sp;
					sp.kind = s["kind"].GetString();
					if ( s.HasMember("n") ) { sp.n = s["n"].GetInt(); }
					if ( s.HasMember("ring_tiles") ) { sp.ring = s["ring_tiles"].GetInt(); }
					for ( int i = 1; i < NUMMONSTERS; ++i )
					{
						if ( !_stricmp(monstertypename[i], sp.kind.c_str()) ) { sp.monster = i; }
					}
					if ( !sp.monster ) { printlog("[apx] unknown monster kind %s", sp.kind.c_str()); simExit = 2; return; }
					S.spawns.push_back(sp);
				}
			}
			S.loaded = true;
			if ( !seedBase ) { seedBase = S.seed; }
		}
		rng.seed(S.seed * 2654435761u + 12345u);
		scenarioName = S.name;
		botSkill = S.skill;
		partyN = S.party;
		buddhamode = true;
		simStartWall = wallMs();
		printlog("[apx] sim on: batch %d, scenario '%s', skill %.2f, encounters %d", batch, S.name.c_str(), S.skill, S.encounters);
	}

	bool simActive() { return simOn; }
	int simBatch() { return batch; }

	static void finish(int code)
	{
		simExit = code;
		st = ST_DONE;
		mainloop = 0;
	}

	static bool clearTile(int x, int y)
	{
		if ( x < 2 || y < 2 || x >= (int)map.width - 2 || y >= (int)map.height - 2 ) { return false; }
		for ( int dx = -1; dx <= 1; ++dx )
		{
			for ( int dy = -1; dy <= 1; ++dy )
			{
				int i = (y + dy) * MAPLAYERS + (x + dx) * MAPLAYERS * map.height;
				if ( !map.tiles[i] || map.tiles[i + OBSTACLELAYER] ) { return false; }
				if ( swimmingtiles && swimmingtiles[map.tiles[i]] ) { return false; }
				if ( lavatiles && lavatiles[map.tiles[i]] ) { return false; }
			}
		}
		return true;
	}

	static void killAllMonsters()
	{
		for ( node_t* nd = map.entities->first; nd; nd = nd->next )
		{
			Entity* e = (Entity*)nd->element;
			if ( e && e->behavior == &actMonster ) { e->setHP(0); }
		}
	}

	static void resetPlayer(Entity* pe)
	{
		if ( Stat* s = pe->getStats() )
		{
			pe->setHP(s->MAXHP);
			pe->setMP(s->MAXMP, false);
		}
		clearDowned();
	}

	static bool partyDown()
	{
		for ( int p = 0; p < S.party; ++p ) { if ( !isDowned(p) ) { return false; } }
		return true;
	}

	static void resetParty()
	{
		for ( int p = 0; p < S.party; ++p ) { if ( players[p] && players[p]->entity ) { resetPlayer(players[p]->entity); } }
	}

	static void partySetup()
	{
		// local splitscreen party: the same state the lobby sets for players 1..n-1, then the layout the lobby computes
		if ( S.party <= 1 ) { return; }
		for ( int c = 1; c < S.party && c < MAXPLAYERS; ++c )
		{
			client_disconnected[c] = false;
			client_classes[c] = c % 4; // barbarian, warrior, healer, rogue slots of the class list
			stats[c]->clearStats();
			local_rng.seedBytes(&c, sizeof(c));
			initClass(c);
			players[c]->bSplitscreen = true;
		}
		MainMenu::setupSplitscreen();
		printlog("[apx] party of %d set up", S.party);
	}

	static void normaliseCharacters()
	{
		// the quickstart rolls the character's sex from the clock-seeded stream before -apx-seed applies: pin it
		for ( int p = 0; p < S.party && p < MAXPLAYERS; ++p ) { if ( stats[p] ) { stats[p]->sex = (p % 2) ? FEMALE : MALE; } }
	}

	static void logCharacter(const char* where)
	{
		for ( int p = 0; p < S.party; ++p )
		{
			Stat* s = stats[p];
			if ( !s ) { continue; }
			printlog("[apx] character %d at %s: maxhp %d hp %d mp %d lvl %d str %d dex %d con %d int %d per %d chr %d type %d sex %d ac %d", p, where, (int)s->MAXHP, (int)s->HP,
				(int)s->MAXMP, (int)s->LVL, (int)s->STR, (int)s->DEX, (int)s->CON, (int)s->INT, (int)s->PER, (int)s->CHR, (int)s->type, (int)s->sex, AC(s));
		}
	}

	static void placeAndSpawn()
	{
		static bool logged = false;
		if ( !logged ) { logged = true; logCharacter("first encounter"); }
		Entity* pe = players[0]->entity;
		if ( !pe ) { return; }
		killAllMonsters();
		resetParty();
		// a clear tile for the party, drawn from the scenario seed
		int px = 0, py = 0;
		for ( int tries = 0; tries < 4000; ++tries )
		{
			px = 2 + (int)(rng() % (map.width - 4));
			py = 2 + (int)(rng() % (map.height - 4));
			if ( clearTile(px, py) ) { break; }
		}
		pe->x = px * 16.0 + 8;
		pe->y = py * 16.0 + 8;
		for ( int p = 1; p < S.party; ++p ) { if ( players[p] && players[p]->entity ) { players[p]->entity->x = pe->x + 6.0 * p; players[p]->entity->y = pe->y + 6.0 * (p % 2); } }
		// candidate spawn tiles: walkable tiles by BFS distance from the party (reachable by construction), nearest-to-ring first
		std::vector<std::pair<int, int>> reach; // tile, with dist stored in parallel
		std::vector<int> reachDist;
		{
			const int w = (int)map.width, h = (int)map.height;
			std::vector<int> dist((size_t)w * h, -1);
			std::vector<std::pair<int, int>> q;
			q.push_back({ px, py });
			dist[px + (size_t)py * w] = 0;
			for ( size_t qi = 0; qi < q.size(); ++qi )
			{
				auto c = q[qi];
				int d = dist[c.first + (size_t)c.second * w];
				reach.push_back(c);
				reachDist.push_back(d);
				if ( d >= 24 ) { continue; }
				static const int dx[4] = { 1, -1, 0, 0 }, dy[4] = { 0, 0, 1, -1 };
				for ( int k = 0; k < 4; ++k )
				{
					int nx = c.first + dx[k], ny = c.second + dy[k];
					if ( nx < 2 || ny < 2 || nx >= w - 2 || ny >= h - 2 || dist[nx + (size_t)ny * w] >= 0 ) { continue; }
					if ( !clearTile(nx, ny) ) { continue; }
					dist[nx + (size_t)ny * w] = d + 1;
					q.push_back({ nx, ny });
				}
			}
		}
		std::vector<bool> used(reach.size(), false);
		for ( auto& sp : S.spawns )
		{
			for ( int k = 0; k < sp.n; ++k )
			{
				// pick among the unused tiles whose path distance is closest to the ring distance (ties: seeded random)
				int best = -1, bestScore = 1 << 30;
				for ( int tries = 0; tries < 48; ++tries )
				{
					size_t i = rng() % reach.size();
					if ( used[i] || reachDist[i] < 3 ) { continue; }
					int score = abs(reachDist[i] - sp.ring) * 4 + (int)(rng() % 4);
					if ( score < bestScore ) { bestScore = score; best = (int)i; }
				}
				bool placed = false;
				if ( best >= 0 )
				{
					used[best] = true;
					Entity* m = summonMonster((Monster)sp.monster, (long)(reach[best].first * 16 + 8), (long)(reach[best].second * 16 + 8));
					if ( m )
					{
						m->monsterAcquireAttackTarget(*pe, MONSTER_STATE_PATH);
						placed = true;
					}
				}
				if ( !placed ) { printlog("[apx] could not place %s (reachable tiles %zu)", sp.kind.c_str(), reach.size()); }
			}
		}
		// the game rolls damage from local_rng (seeded from the clock): seed it per encounter so a scenario repeats
		{
			Uint32 s = S.seed * 2654435761u + (Uint32)(encDone + 1) * 40503u + (Uint32)runIdx * 7919u;
			local_rng.seedBytes(&s, sizeof(s));
			Uint32 s2 = s ^ 0x5bd1e995u, s3 = s ^ 0x1b873593u;
			net_rng.seedBytes(&s2, sizeof(s2));
			map_server_rng.seedBytes(&s3, sizeof(s3));
		}
		lastX = pe->x; lastY = pe->y; lastHitsLanded = 0; stuckThisEnc = false;
		closedAtStart = encountersClosed;
		fightStart = ticks;
		nextFightCheck = ticks + 500;
	}

	void simTick()
	{
		if ( !simOn || st == ST_DONE ) { return; }
		if ( simTicksLimit && ticks >= simTicksLimit ) { finish(S.loaded && encDone < S.encounters ? 4 : 0); return; }
		if ( simWallLimit > 0 && (wallMs() - simStartWall) / 1000.0 > simWallLimit )
		{
			printlog("[apx] sim wall limit hit (%.0f s)", simWallLimit);
			closeEncounter("wall_limit");
			finish(3);
			return;
		}
		buddhamode = true; // the game clears it on new-game start; re-assert every tick
		if ( gamePaused ) { pauseGame(1, 0); } // sim runs are unattended: nothing may leave the game paused (focus loss, missing input device)
		if ( !S.loaded ) { return; }
		Entity* pe = players[0] ? players[0]->entity : nullptr;
		switch ( st )
		{
			case ST_WAIT_GAME:
				if ( pe && !loading && ticks > 150 ) { partySetup(); normaliseCharacters(); st = ST_LOAD; stateTick = ticks; }
				break;
			case ST_LOAD:
				closeEncounter("scenario_reload");
				loadSeen = floorsLoaded;
				currentlevel = S.floor - 1;
				if ( S.walk ) { seedBase = S.seed + (Uint32)runIdx * 7919u; }
				loadnextlevel = true;
				sinceReload = 0;
				st = ST_WAIT_LOAD;
				stateTick = ticks;
				break;
			case ST_WAIT_LOAD:
				if ( floorsLoaded > loadSeen && !loadnextlevel && pe && ticks - stateTick > 100 )
					{
						st = S.walk ? ST_WALK_INIT : ST_PLACE; stateTick = ticks; stateWaitsForLoad = true;
					}
				break;
			case ST_PLACE:
				if ( !pe ) { break; }
				if ( ticks - stateTick < 20 && stateWaitsForLoad ) { break; }
				if ( ticks % 100 != 0 ) { break; } // start every encounter on the same tick phase: monster AI keys off ticks
				placeAndSpawn();
				stateWaitsForLoad = false;
				st = ST_FIGHT;
				break;
			case ST_FIGHT:
			{
				if ( !pe ) { break; }
				bool closed = encountersClosed > closedAtStart;
				bool timeout = (ticks - fightStart) > 120 * TICKS_PER_SECOND;
				if ( ticks % 250 == 0 )
				{
					// a pack that lost the party (no line of sight, door) hunts again: the scenario is a pack that engages
					for ( node_t* nd = map.creatures ? map.creatures->first : nullptr; nd; nd = nd->next )
					{
						Entity* m = (Entity*)nd->element;
						if ( m && m->behavior == &actMonster && m->monsterAllyIndex < 0 && m->getStats() && m->getStats()->HP > 0
							&& uidToEntity(m->monsterTarget) != pe )
						{
							m->monsterAcquireAttackTarget(*pe, MONSTER_STATE_PATH);
						}
					}
				}
				if ( ticks >= nextFightCheck )
				{
					nextFightCheck = ticks + 500;
					long hl = 0; // stuck: no position change and no damage dealt over 10 s while a hostile lives
					bool hostile = false;
					for ( node_t* nd = map.creatures ? map.creatures->first : nullptr; nd; nd = nd->next )
					{
						Entity* m = (Entity*)nd->element;
						if ( m && m->behavior == &actMonster && m->getStats() && m->getStats()->HP > 0 && m->monsterAllyIndex < 0 ) { hostile = true; }
					}
					(void)hl;
					if ( hostile && fabs(pe->x - lastX) < 8 && fabs(pe->y - lastY) < 8 && !stuckThisEnc && !isDowned(0) )
					{
						// a bot that stands still while swinging is fighting; require that nothing has been hit either
						long hits = encHitsLanded();
						if ( hits == lastHitsLanded ) { stuckThisEnc = true; ++botsStuck; ++stuckEncounters; }
						lastHitsLanded = hits;
					}
					else { lastHitsLanded = encHitsLanded(); }
					lastX = pe->x; lastY = pe->y;
				}
				if ( timeout )
				{
					if ( encounterOpen() ) { closeEncounter("timeout"); }
					else if ( encountersClosed == closedAtStart ) { noContactRow(); } // keep one row per scenario encounter
					closed = true;
				}
				if ( closed )
				{
					++encDone;
					++sinceReload;
					if ( encDone >= S.encounters ) { finish(0); break; }
					if ( sinceReload >= S.reloadEvery ) { st = ST_LOAD; }
					else { st = ST_PLACE; stateTick = ticks; }
				}
				break;
			}
			case ST_WALK_INIT:
			{
				if ( !pe ) { break; }
				resetParty();
				walkLadderUid = 0;
				for ( node_t* nd = map.entities->first; nd; nd = nd->next )
				{
					Entity* e = (Entity*)nd->element;
					if ( e && e->behavior == &actLadder ) { walkLadderUid = e->getUID(); }
				}
				printlog("[apx] walk init: ladder uid %u, party %d, entities %d%d%d%d, paused %d, level %d", walkLadderUid, S.party, (int)(players[0] && players[0]->entity), (int)(players[1] && players[1]->entity),
					(int)(players[2] && players[2]->entity), (int)(players[3] && players[3]->entity), (int)gamePaused, currentlevel);
				walkStart = ticks; walkNoMove = ticks; walkLastX = pe->x; walkLastY = pe->y;
				walkEncStart = encountersClosed;
				walkHpMin = pe->getStats() ? pe->getStats()->HP : 0;
				loadSeen = floorsLoaded;
				stateWaitsForLoad = false;
				st = ST_WALK;
				break;
			}
			case ST_WALK:
			{
				if ( !pe ) { break; }
				if ( Stat* s = pe->getStats() ) { walkHpMin = std::min(walkHpMin, (int)s->HP); }
				const char* outcome = nullptr;
				if ( currentlevel != S.floor || loadnextlevel ) { outcome = "descended"; }
				else if ( partyDown() ) { outcome = "died"; }
				else if ( ticks - walkStart > 900 * TICKS_PER_SECOND ) { outcome = "timeout"; }
				else
				{
					if ( fabs(pe->x - walkLastX) > 16 || fabs(pe->y - walkLastY) > 16 ) { walkLastX = pe->x; walkLastY = pe->y; walkNoMove = ticks; }
					else if ( ticks - walkNoMove > 20 * TICKS_PER_SECOND ) { outcome = "stuck"; ++botsStuck; }
				}
				if ( outcome )
				{
					bool desc = !strcmp(outcome, "descended");
					if ( !desc ) { closeEncounter("run_end"); }
					ApxRow r;
					commonFields(r);
					r.i("run", runIdx).str("outcome", outcome).d("time_on_floor_s", (ticks - walkStart) / (double)TICKS_PER_SECOND)
						.i("encounters_during", encountersClosed - walkEncStart).i("hp_min", walkHpMin)
						.i("hp_max", pe->getStats() ? pe->getStats()->MAXHP : 0).hex64("floor_hash", lastFloorHash)
						.i("map_seed", mapseed).i("bots_stuck", (long long)botsStuck);
					write("runs.jsonl", r);
					++runIdx;
					if ( runIdx >= S.runs ) { finish(0); break; }
					if ( desc ) { st = ST_WAIT_DESCENT; stateTick = ticks; }
					else { st = ST_LOAD; }
				}
				break;
			}
			case ST_WAIT_DESCENT:
				if ( !loadnextlevel && floorsLoaded > loadSeen && ticks - stateTick > 50 ) { st = ST_LOAD; }
				else if ( ticks - stateTick > 600 ) { st = ST_LOAD; } // load never arrived; force the reload
				break;
			default: break;
		}
	}

	// ---------------------------------------------------------------- bot
	static std::mt19937 botRng;
	static bool botSeeded = false;
	static Uint32 botNextThink[MAXPLAYERS] = {};
	static Entity* botTarget[MAXPLAYERS] = {};
	static Uint32 botTargetUid[MAXPLAYERS] = {};
	static Uint32 botSwingAt[MAXPLAYERS] = {};
	static int botHold[MAXPLAYERS] = {};
	static double botDist[MAXPLAYERS] = {};

	static Entity* nearestHostile(Entity* me, double& dTiles)
	{
		Entity* best = nullptr;
		double bd = 1e9;
		for ( node_t* nd = map.creatures ? map.creatures->first : nullptr; nd; nd = nd->next )
		{
			Entity* m = (Entity*)nd->element;
			if ( !m || m->behavior != &actMonster || m->monsterAllyIndex >= 0 ) { continue; }
			Stat* s = m->getStats();
			if ( !s || s->HP <= 0 ) { continue; }
			double d = sqrt((m->x - me->x) * (m->x - me->x) + (m->y - me->y) * (m->y - me->y));
			if ( d < bd ) { bd = d; best = m; }
		}
		dTiles = bd / 16.0;
		return best;
	}

	// walking brain: follow the game's own path (generatePath) to the down ladder; descent and doors use the
	// same selection state the Use key sets (selectedEntity / inrange), because the crosshair pick lives in the draw path.
	static Entity* pendSel[MAXPLAYERS] = {};
	void botPost(Entity* my)
	{
		int p = my ? my->skill[2] : -1;
		if ( p < 0 || p >= MAXPLAYERS || !pendSel[p] ) { return; }
		selectedEntity[p] = pendSel[p];
		inrange[p] = true;
		pendSel[p] = nullptr;
	}

	static void walkStep(Entity* my, Input& in, int p, Entity* dest = nullptr)
	{
		static std::vector<std::pair<int, int>> wpA[MAXPLAYERS];
		static Uint32 nextPathA[MAXPLAYERS] = {}, lastMoveTickA[MAXPLAYERS] = {};
		static real_t lx = 0, ly = 0, pxA[MAXPLAYERS] = {}, pyA[MAXPLAYERS] = {};
		auto& wp = wpA[p];
		Uint32& nextPath = nextPathA[p];
		Uint32& lastMoveTick = lastMoveTickA[p];
		in.apxSet("Attack", false);
		const bool isLadder = dest == nullptr;
		Entity* lad = dest ? dest : (walkLadderUid ? uidToEntity(walkLadderUid) : nullptr);
		if ( !lad ) { in.apxSet("Move Forward", false); return; }
		double ld = sqrt((lad->x - my->x) * (lad->x - my->x) + (lad->y - my->y) * (lad->y - my->y)) / 16.0;
		if ( isLadder && ld < 1.9 ) // actLadder needs EVERY player within TOUCHRANGE (32 units = 2 tiles): keep closing in after selecting it
		{
			pendSel[p] = lad;
			in.apxSet("Use", false);
			if ( ld < 0.9 ) { in.apxSet("Move Forward", false); return; }
		}
		if ( ticks >= nextPath )
		{
			nextPath = ticks + 25;
			wp.clear();
			list_t* path = generatePath((int)(my->x / 16), (int)(my->y / 16), (int)(lad->x / 16), (int)(lad->y / 16), my, lad, GENERATE_PATH_DEFAULT);
			if ( path )
			{
				for ( node_t* nd = path->first; nd; nd = nd->next )
				{
					pathnode_t* pn = (pathnode_t*)nd->element;
					if ( pn ) { wp.push_back({ pn->x, pn->y }); }
				}
				list_FreeAll(path);
				free(path);
			}
		}
		double tx = lad->x, ty = lad->y; // no path (or close in): head straight at the ladder
		if ( !wp.empty() && ld > 3.0 )
		{
			size_t i = 0;
			while ( i + 1 < wp.size() && hypot(wp[i].first * 16.0 + 8 - my->x, wp[i].second * 16.0 + 8 - my->y) < 9.0 ) { ++i; }
			size_t j = i; // aim at the next path tile itself: cutting corners walks into the wall beside a door
			tx = wp[j].first * 16.0 + 8; ty = wp[j].second * 16.0 + 8;
		}
		static const bool wdbg = getenv("APX_BOT_DEBUG") != nullptr;
		if ( wdbg && ticks % 50 == 0 )
		{
			printlog("[apx-walk] p%d t=%u pos=(%.1f,%.1f) ladder=(%.1f,%.1f) ld=%.1f wp=%zu target=(%.1f,%.1f) yaw=%.2f paused=%d ctl=%d shoot=%d mobile=%d", p, ticks, my->x / 16, my->y / 16, lad->x / 16, lad->y / 16, ld, wp.size(), tx / 16, ty / 16, my->yaw,
				(int)gamePaused, (int)players[p]->bControlEnabled, (int)players[p]->shootmode, (int)my->isMobile());
		}
		double want = atan2(ty - my->y, tx - my->x), diff = want - my->yaw;
		while ( diff > PI ) { diff -= 2 * PI; }
		while ( diff < -PI ) { diff += 2 * PI; }
		const double maxTurn = (4.0 + 8.0 * S.skill) * PI / 180.0;
		my->yaw += fabs(diff) <= maxTurn ? diff : (diff > 0 ? maxTurn : -maxTurn);
		while ( my->yaw >= 2 * PI ) { my->yaw -= 2 * PI; }
		while ( my->yaw < 0 ) { my->yaw += 2 * PI; }
		in.apxSet("Move Forward", fabs(diff) < 40.0 * PI / 180.0);
		// blocked by a door? open the nearest one in front through the Use selection
		real_t& px = pxA[p];
		real_t& py = pyA[p];
		if ( fabs(my->x - px) + fabs(my->y - py) > 3.0 ) { px = my->x; py = my->y; lastMoveTick = ticks; }
		if ( ticks - lastMoveTick > 30 )
		{
			Entity* best = nullptr;
			double bd = 48.0;
			for ( node_t* nd = map.entities->first; nd; nd = nd->next )
			{
				Entity* e = (Entity*)nd->element;
				if ( !e || e->behavior != &actDoor ) { continue; }
				double d = hypot(e->x - my->x, e->y - my->y);
				if ( d < bd ) { bd = d; best = e; }
			}
			static const bool wd2 = getenv("APX_BOT_DEBUG") != nullptr;
			if ( wd2 && ticks % 50 == 0 )
			{
				printlog("[apx-walk] blocked: nearest door %s dist %.1f (tiles %.1f,%.1f) sel=%p inrange=%d", best ? "yes" : "none", best ? bd / 16 : -1.0,
					best ? best->x / 16 : 0.0, best ? best->y / 16 : 0.0, (void*)selectedEntity[p], (int)inrange[p]);
			}
			if ( best ) { pendSel[p] = best; }
		}
		else { in.apxSet("Use", false); }
		(void)lx; (void)ly;
	}

	void botThink(Entity* my)
	{
		if ( !simOn || !my ) { return; }
		int p = my->skill[2];
		static const bool tdbg = getenv("APX_BOT_DEBUG") != nullptr;
		if ( tdbg && ticks % 200 == 0 ) { printlog("[apx-think] p%d party %d ticks %u paused %d", p, S.party, ticks, (int)gamePaused); }
		if ( p < 0 || p >= S.party ) { return; }
		if ( !botSeeded ) { botRng.seed(S.seed ^ 0x9e3779b9u); botSeeded = true; }
		Input& in = Input::inputs[p];
		const double skill = S.skill;
		if ( isDowned(p) )
		{
			// sim death model: a downed bot stops fighting. In walk mode it still follows to the ladder (the ladder needs
			// the whole party within reach), which is the documented difference from a real corpse.
			if ( S.walk ) { walkStep(my, in, p); return; }
			in.apxSet("Move Forward", false); in.apxSet("Attack", false);
			return;
		}
		// reaction delay: retarget only every (1 - skill) * 15 ticks
		if ( ticks >= botNextThink[p] )
		{
			botNextThink[p] = ticks + (Uint32)((1.0 - skill) * 15.0);
			double d;
			botTarget[p] = nearestHostile(my, d);
			botDist[p] = d;
			if ( S.walk && botTarget[p] )
			{
				// walking: only fight what is on us (close, or hunting us from within 12 tiles)
				Entity* hunted = uidToEntity(botTarget[p]->monsterTarget);
				if ( !(hunted == my && d <= 12.0) ) { botTarget[p] = nullptr; } // sleepers and unaware monsters are left alone
				if ( botTarget[p] )
				{
					// a target we cannot hurt (behind an obstacle, out of reach) is dropped for a while
					static Uint32 lastUidA[MAXPLAYERS] = {}, sinceA[MAXPLAYERS] = {};
					static int lastHpA[MAXPLAYERS] = {};
					Uint32& lastUid = lastUidA[p];
					Uint32& since = sinceA[p];
					int& lastHp = lastHpA[p];
					static std::map<Uint32, Uint32> ignoreUntil;
					Uint32 u = botTarget[p]->getUID();
					int hp = botTarget[p]->getStats() ? botTarget[p]->getStats()->HP : 0;
					if ( u != lastUid || hp != lastHp ) { lastUid = u; lastHp = hp; since = ticks; }
					if ( ticks - since > 6 * TICKS_PER_SECOND ) { ignoreUntil[u] = ticks + 30 * TICKS_PER_SECOND; since = ticks; }
					auto it = ignoreUntil.find(u);
					if ( it != ignoreUntil.end() && ticks < it->second ) { botTarget[p] = nullptr; }
				}
			}
			botTargetUid[p] = botTarget[p] ? botTarget[p]->getUID() : 0;
		}
		Entity* t = botTargetUid[p] ? uidToEntity(botTargetUid[p]) : nullptr;
		if ( !t || !t->getStats() || t->getStats()->HP <= 0 )
		{
			botTargetUid[p] = 0;
			if ( S.walk ) { walkStep(my, in, p); return; }
			in.apxSet("Move Forward", false); in.apxSet("Attack", false);
			return;
		}
		double dx = t->x - my->x, dy = t->y - my->y;
		double dist = sqrt(dx * dx + dy * dy) / 16.0;
		if ( dist > 3.0 && S.skill > 0.0 ) { walkStep(my, in, p, t); return; } // far: follow the game's own path to it, not a straight line into a wall
		double want = atan2(dy, dx);
		double err = (1.0 - skill) * 12.0 * PI / 180.0 * (((int)(botRng() % 2001) - 1000) / 1000.0);
		double diff = want + err - my->yaw;
		while ( diff > PI ) { diff -= 2 * PI; }
		while ( diff < -PI ) { diff += 2 * PI; }
		const double maxTurn = (4.0 + 8.0 * skill) * PI / 180.0; // per tick
		if ( fabs(diff) <= maxTurn ) { my->yaw += diff; }
		else { my->yaw += diff > 0 ? maxTurn : -maxTurn; }
		while ( my->yaw >= 2 * PI ) { my->yaw -= 2 * PI; }
		while ( my->yaw < 0 ) { my->yaw += 2 * PI; }
		const double reach = 1.9; // tiles: Entity::attack melee reach is TOUCHRANGE-scale (32 units)
		bool facing = fabs(diff) < 18.0 * PI / 180.0;
		in.apxSet("Move Forward", dist > reach * 0.8);
		in.apxSet("Move Left", false); in.apxSet("Move Right", false); in.apxSet("Move Backward", false);
		// swing: press for one tick, then release so the next press is a fresh toggle
		// press held for 2 ticks (entity behaviours run in list order, the weapon HUD may read before or after us)
		bool start = skill > 0.0 && facing && dist <= reach && ticks >= botSwingAt[p]; // skill 0.0 = control bot: never attacks
		if ( start ) { botSwingAt[p] = ticks + 30; botHold[p] = 2; }
		bool swing = botHold[p] > 0;
		if ( swing ) { --botHold[p]; }
		in.apxSet("Attack", swing);
		static const bool dbg = getenv("APX_BOT_DEBUG") != nullptr;
		if ( dbg && ticks % 100 == 0 )
		{
			printlog("[apx-bot] t=%u dist=%.2f diffdeg=%.1f facing=%d swing=%d shootmode=%d mobile=%d hp=%d", ticks, dist, diff * 180 / PI, facing, swing,
				(int)players[p]->shootmode, (int)my->isMobile(), stats[p]->HP);
		}
	}
}
