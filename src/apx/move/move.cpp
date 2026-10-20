// apx/move/move.cpp -- see move.hpp. Engine glue + collision + the gate's instruments around the pure core (move_core.hpp).
#include "../../main.hpp"
#include "../../game.hpp"
#include "../../stat.hpp"
#include "../../entity.hpp"
#include "../../items.hpp"
#include "../../magic/magic.hpp"
#include "../../monster.hpp"
#include "../../net.hpp"
#include "../../collision.hpp"
#include "../../player.hpp"
#include "../../mod_tools.hpp"
#include "../../interface/consolecommand.hpp"
#include "../../apx_stamina.hpp"
#include "../../apx_combat.hpp"
#include "../in/in.hpp"
#include "move.hpp"
#ifdef APX_IN_STUB
#define APX_IN_STUB_FLAG 1
#else
#define APX_IN_STUB_FLAG 0
#endif
#include "rapidjson/document.h"
#include <algorithm>
#include <chrono>
#include <cstdarg>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <unordered_map>
#include <vector>

bool playerAllowedMovement(const int playernum); // actplayer.cpp

namespace apx
{
namespace move
{
using core::Vec2;
using core::len;

namespace
{
	struct PS
	{
		Vec2 loco, lastOut, prevPos;
		bool haveOut = false, havePrev = false;
		core::ImpulseStack imp;
		core::Legacy leg;
		core::SprintState spr;
		State st = State::Walk;
		float scale[(int)Source::COUNT] = { 1.f, 1.f, 1.f, 1.f, 1.f, 1.f };
		Uint32 lastStepTick = (Uint32)-1, prevTick = 0;
		double costUs = 0.0;           // step + collide this tick
		// counters (D90)
		int sprintStarts = 0, exhaustions = 0;
		double sprintDrained = 0.0;
		long long tickCount = 0, stuckTicks = 0, blockedTicks = 0;
		// impulse test hook
		bool testPending = false; Vec2 testStart; float testSpeed = 0.f; int testTicks = 0; float testRun = 0.f; Uint32 testBegin = 0;
		bool lastSprintOn = false;
	};
	PS g_ps[MAXPLAYERS];
	std::unordered_map<Uint32, core::ImpulseStack> g_world;   // creatures and anything not a local player
	core::Table g_table;
	bool g_loaded = false, g_old = false, g_logOn = false;
	FILE* g_log = nullptr;
	std::vector<float> g_cost;                                 // us per player tick, rolling
	int g_costNext = 0;

	using Clock = std::chrono::steady_clock;
	double nowUs() { return std::chrono::duration<double, std::micro>(Clock::now().time_since_epoch()).count(); }

	std::string readFile(const std::string& p)
	{
		std::string s;
		if ( FILE* f = std::fopen(p.c_str(), "rb") ) { char b[4096]; size_t n; while ( (n = std::fread(b, 1, sizeof b, f)) > 0 ) s.append(b, n); std::fclose(f); }
		return s;
	}
	float num(const rapidjson::Value& o, const char* key, float def)
	{
		if ( o.IsObject() && o.HasMember(key) && o[key].IsNumber() ) { return (float)o[key].GetDouble(); }
		return def;
	}
	const rapidjson::Value& sub(const rapidjson::Value& o, const char* key)
	{
		static const rapidjson::Value nul;
		return (o.IsObject() && o.HasMember(key) && o[key].IsObject()) ? o[key] : nul;
	}

	void load()
	{
		if ( g_loaded ) { return; }
		g_loaded = true;
		const char* old = std::getenv("APX_MOVE_OLD");
		g_old = old && old[0] == '1';
		std::vector<std::string> cands;
		if ( const char* e = std::getenv("APX_MOVE_DATA") ) { cands.push_back(std::string(e) + "/move/move.json"); }
#ifdef APX_PARTS_DATA_DIR
		cands.push_back(std::string(APX_PARTS_DATA_DIR) + "/move/move.json");
#endif
		cands.push_back("data/apx/move/move.json");
		std::string text, used;
		for ( auto& c : cands ) { text = readFile(c); if ( !text.empty() ) { used = c; break; } }
		rapidjson::Document doc;
		core::Table& t = g_table;
		if ( text.empty() || doc.Parse(text.c_str()).HasParseError() || !doc.IsObject() )
		{
			printlog("[apx-move] ERROR: data/apx/move/move.json missing or unparsable (tried %s); built-in defaults used", cands.empty() ? "-" : cands[0].c_str());
		}
		else
		{
			t.unitsPerSecPerFactor = num(doc, "units_per_second_per_factor", t.unitsPerSecPerFactor);
			t.maxSpeedPerTick = num(doc, "max_speed_units_per_tick", t.maxSpeedPerTick);
			const auto& ss = sub(doc, "state_speed");
			t.walk = num(ss, "walk", t.walk); t.sprint = num(ss, "sprint", t.sprint); t.sneak = num(ss, "sneak", t.sneak); t.block = num(ss, "block", t.block);
			t.swimHuman = num(ss, "swim_human", t.swimHuman); t.swimOther = num(ss, "swim_other", t.swimOther); t.swimSkeleton = num(ss, "swim_skeleton", t.swimSkeleton);
			t.levitate = num(ss, "levitate", t.levitate); t.crawl = num(ss, "crawl", t.crawl);
			const auto& dr = sub(doc, "direction");
			t.backpedal = num(dr, "backpedal", t.backpedal); t.strafe = num(dr, "strafe", t.strafe);
			const auto& cu = sub(doc, "curves");
			t.accelSeconds = num(cu, "accel_seconds", t.accelSeconds); t.brakeSeconds = num(cu, "brake_seconds", t.brakeSeconds);
			t.turnSeconds = num(cu, "turn_seconds", t.turnSeconds); t.knockedControl = num(cu, "knocked_control", t.knockedControl);
			const auto& gr = sub(doc, "grip");
			t.gripNormal = num(gr, "normal", t.gripNormal); t.gripSlippery = num(gr, "slippery", t.gripSlippery); t.gripGrease = num(gr, "grease", t.gripGrease);
			t.gripMagicGrease = num(gr, "magic_grease", t.gripMagicGrease); t.gripCleatsFloor = num(gr, "cleats_floor", t.gripCleatsFloor);
			t.slowTile = num(gr, "slow_tile_speed", t.slowTile);
			const auto& sp = sub(doc, "sprint");
			t.sprint = num(sp, "boost", t.sprint); t.sprintStaminaPerSecond = num(sp, "stamina_per_second", t.sprintStaminaPerSecond);
			t.sprintMinStamina = num(sp, "min_stamina_to_start", t.sprintMinStamina);
			const auto& im = sub(doc, "impulse");
			t.impulseCap = num(im, "cap_units_per_tick", t.impulseCap); t.impulseStack = (int)num(im, "max_stack", (float)t.impulseStack);
			t.knockedMinSpeed = num(im, "knocked_min_speed", t.knockedMinSpeed);
			t.legacyDecay = num(sub(doc, "legacy"), "decay_per_tick", t.legacyDecay);
			const auto& co = sub(doc, "collision");
			t.bisectSteps = (int)num(co, "bisect_steps", (float)t.bisectSteps); t.nudgeMax = num(co, "nudge_max_units", t.nudgeMax); t.nudgeStep = num(co, "nudge_step_units", t.nudgeStep);
			const auto& lk = sub(doc, "look");
			t.lookSmoothDecay = num(lk, "smooth_decay_per_tick", t.lookSmoothDecay); t.lookSmoothClamp = num(lk, "smooth_clamp", t.lookSmoothClamp);
			t.lookSmoothGain = num(lk, "smooth_gain", t.lookSmoothGain); t.lookRawGain = num(lk, "raw_gain", t.lookRawGain);
		}
		if ( t.bisectSteps < 1 ) { t.bisectSteps = 1; }
		if ( t.impulseStack > core::ImpulseStack::kMax ) { t.impulseStack = core::ImpulseStack::kMax; }
		printlog("[apx-move] %s table=%s walk=%.1f u/s per factor sprint=%.2fx sneak=%.2fx backpedal=%.2fx strafe=%.2fx accel=%.2fs brake=%.2fs sprint_drain=%.1f/s in_stub=%d",
			g_old ? "OLD (Barony movement, CONTROL)" : "NEW", used.empty() ? "defaults" : used.c_str(), t.unitsPerSecPerFactor, t.sprint, t.sneak, t.backpedal, t.strafe,
			t.accelSeconds, t.brakeSeconds, t.sprintStaminaPerSecond, APX_IN_STUB_FLAG);
		if ( const char* dir = std::getenv("APX_MOVE_LOG") )
		{
			std::string path = std::string(dir) + "/move.jsonl";
			g_log = std::fopen(path.c_str(), "w");
			g_logOn = g_log != nullptr;
			printlog("[apx-move] row log %s (%s)", path.c_str(), g_logOn ? "open" : "FAILED to open");
			if ( g_log ) { std::fprintf(g_log, "{\"ev\":\"start\",\"mode\":\"%s\",\"tps\":50}\n", g_old ? "old" : "new"); std::fflush(g_log); }
		}
		g_cost.assign(65536, 0.f);
	}

	inline int pnOf(Entity* e) { return (e && e->behavior == &actPlayer) ? (int)e->skill[2] : -1; }
	inline bool localPlayer(int pn) { return pn >= 0 && pn < MAXPLAYERS && players[pn] && players[pn]->isLocalPlayer(); }

	// everything the step (and the row log) needs, read from the game
	struct Ctx
	{
		bool ok = false, allow = false, pacified = false, rooted = false, levit = false, swim = false, downed = false, legs = false;
		bool defending = false, sneaking = false, sprintKey = false, sprintWanted = false, noclipOn = false, confused = false;
		float base = 0.f;          // Barony's stat-based base speed (getSpeedFactor) x sources x slow tile
		float baseRaw = 0.f;       // getSpeedFactor alone
		float swimMult = 1.f, defendMult = 1.f, grip = 1.f;
		Vec2 wishV; float wishLen = 0.f; float forward = 0.f, side = 0.f;
	};

	void buildCtx(int pn, Ctx& c, bool perTickSideEffects)
	{
		Entity* my = players[pn]->entity;
		Stat* st = stats[pn];
		Player& pl = *players[pn];
		auto& mv = pl.movement;
		const core::Table& T = g_table;
		const in::TickRecord& in = in::take(pn);
		c.ok = true;
		c.pacified = st->getEffectActive(EFF_PACIFY) > 0;
		c.rooted = st->getEffectActive(EFF_ROOTED) > 0;
		c.confused = st->getEffectActive(EFF_CONFUSED) > 0;
		c.noclipOn = noclip;
		c.allow = my->isMobile() && playerAllowedMovement(pn);
		if ( c.rooted ) { c.allow = false; }
		if ( !c.allow && c.pacified && !c.rooted )
		{
			if ( !st->getEffectActive(EFF_PARALYZED) && !st->getEffectActive(EFF_STUNNED) && !st->getEffectActive(EFF_ASLEEP) ) { c.allow = true; }
		}
		if ( gamePaused && !c.pacified ) { c.allow = false; }
		c.levit = isLevitating(st);
		c.swim = mv.isPlayerSwimming();
		c.downed = apx::combat_glue::isDowned(pn);
		c.legs = apx::combat_glue::legsDisabled(pn);
		c.defending = st->defending;
		c.sneaking = in.isHeld(in::B_Sneak) || st->sneaking == 1;
		c.sprintKey = in.isHeld(in::B_Sprint);

		// the base speed: Barony's getSpeedFactor (DEX, encumbrance, effects, legs and downed scale)
		const int weight = mv.getCharacterModifiedWeight();
		const real_t wr = mv.getWeightRatio(weight, statGetSTR(st, my));
		const real_t sf = mv.getSpeedFactor(wr, statGetDEX(st, my));
		if ( perTickSideEffects && ticks % TICKS_PER_SECOND == 0 )
		{
			Compendium_t::Events_t::eventUpdateCodex(pn, Compendium_t::CPDM_CLASS_WGT_MAX, "wgt", mv.getCharacterWeight());
			if ( sf >= mv.getMaximumSpeed() ) { Compendium_t::Events_t::eventUpdateCodex(pn, Compendium_t::CPDM_CLASS_WGT_MAX_MOVE_100, "wgt", mv.getCharacterWeight()); }
		}
		c.baseRaw = (float)sf;
		float base = (float)sf;
		for ( int i = 0; i < (int)Source::COUNT; ++i ) { base *= g_ps[pn].scale[i]; }
		const int tx = (int)(my->x / 16), ty = (int)(my->y / 16);
		if ( map.tileHasAttribute(tx, ty, 0, map_t::TILE_ATTRIBUTE_SLOW) && !c.levit ) { base *= T.slowTile; }
		c.base = base;

		// ground grip
		float grip = T.gripNormal;
		if ( map.tileHasAttribute(tx, ty, 0, map_t::TILE_ATTRIBUTE_SLIPPERY) ) { grip = T.gripSlippery; }
		else if ( map.tileHasAttribute(tx, ty, 0, map_t::TILE_ATTRIBUTE_GREASE) ) { grip = T.gripGrease; }
		else if ( st->getEffectActive(EFF_MAGIC_GREASE) ) { grip = T.gripMagicGrease; }
		if ( st->shoes && st->shoes->type == CLEAT_BOOTS && my->effectShapeshift == NOTHING ) { grip = std::max(grip, T.gripCleatsFloor); }
		c.grip = grip;

		// water: swimming slows unless the amulet breathes it
		const bool breathes = st->amulet && st->amulet->type == AMULET_WATERBREATHING;
		c.swimMult = 1.f;
		if ( c.swim && !breathes )
		{
			c.swimMult = (st->type == HUMAN || st->type == RAT) ? T.swimHuman : T.swimOther;
			if ( st->type == SKELETON ) { c.swimMult = T.swimSkeleton; }
		}

		// block and sneak: x0.5 (Barony's defend penalty), foci shields scale it by the spell skill, gremlins sneak at 2/3
		if ( c.defending || c.sneaking )
		{
			float m = (c.sneaking) ? T.sneak : T.block;
			if ( c.sneaking && !c.defending && st->type == GREMLIN ) { m = 1.f / 1.5f; }
			if ( c.defending && !c.sneaking && st->shield && itemTypeIsFoci(st->shield->type) )
			{
				if ( int spellID = getSpellIDFromFoci(st->shield->type) )
				{
					if ( auto spell = getSpellFromID(spellID) )
					{
						const float modifier = std::min(100, st->getModifiedProficiency(spell->skillID)) / 100.f;
						float penalty = 1.f / m - 1.f;
						penalty *= std::max(0.f, 1.f - modifier);
						m = 1.f / (1.f + penalty);
					}
				}
			}
			c.defendMult = m;
		}

		// the wanted direction
		float forward = 0.f, side = 0.f;
		float backMult = T.backpedal, latMult = 1.f;
		if ( st->helmet && st->helmet->type == HAT_BANDANA )
		{
			const int b = st->helmet->beatitude;
			const float bonus = 0.125f * (1 + std::abs(b));
			backMult = std::min(1.0f, T.backpedal + bonus);
			if ( b >= 0 || shouldInvertEquipmentBeatitude(st) ) { latMult = std::min(1.5f, 1.f + bonus); }
			else { latMult = 0.f; }
		}
		if ( st->getEffectActive(EFF_DASH) ) { backMult = 1.25f; }
		if ( c.pacified ) { forward = -0.1f; side = 0.f; }
		else { forward = in.moveY; side = in.moveX; }
		if ( c.confused ) { forward = -forward; side = -side; }
		if ( !c.allow ) { forward = 0.f; side = 0.f; }
		if ( c.noclipOn && c.sprintKey ) { forward *= 0.5f; side *= 0.5f; }
		c.forward = forward; c.side = side;
		c.wishV = core::wish(T, forward, side, (float)my->yaw, latMult, backMult);
		c.wishLen = len(c.wishV);
		c.sprintWanted = c.sprintKey && c.allow && c.wishLen > 0.f && forward >= 0.f && !c.legs && !c.downed && !c.sneaking && !c.defending
			&& !c.swim && !c.pacified && !c.noclipOn;
	}

	State resolveState(const PS& S, const Ctx& c, bool sprintOn, bool knocked)
	{
		if ( knocked ) { return State::Knocked; }
		if ( c.downed ) { return State::Crawl; }
		if ( c.swim ) { return State::Swim; }
		if ( sprintOn ) { return State::Sprint; }
		if ( c.sneaking || c.defending ) { return State::Sneak; }
		if ( c.levit ) { return State::Levitate; }
		(void)S;
		return State::Walk;
	}

	float multOf(const core::Table& T, State s, const Ctx& c)
	{
		if ( s == State::Sneak ) { return c.defendMult; }
		return core::stateMult(T, s, c.swimMult);
	}

	void logRow(const char* fmt, ...)
	{
		if ( !g_log ) { return; }
		va_list ap; va_start(ap, fmt); std::vfprintf(g_log, fmt, ap); va_end(ap);
	}
} // namespace

bool active() { load(); return !g_old; }
const core::Table& table() { load(); return g_table; }
bool lookActive() { return active(); }
void lookX(float& rotX, float dx, float sens, bool smooth, bool confused, bool mobile, bool limit, float dt)
{
	core::lookAxisX(table(), rotX, dx, sens, smooth, confused, mobile, limit, dt);
}
void lookY(float& rotY, float dy, float sens, bool smooth, bool reverse, bool confused, bool mobile, float dt)
{
	core::lookAxisY(table(), rotY, dy, sens, smooth, reverse, confused, mobile, dt);
}

namespace
{
}
PS& g_ps_test(int pn) { return g_ps[pn]; }

State state(int player)
{
	return (player >= 0 && player < MAXPLAYERS) ? g_ps[player].st : State::Walk;
}

void speedScale(Entity* e, Source s, float k)
{
	const int pn = pnOf(e);
	if ( pn < 0 || pn >= MAXPLAYERS ) { return; }
	g_ps[pn].scale[(int)s] = k;
}

void impulse(Entity* e, float vx, float vy, int ticks_)
{
	load();
	if ( !e ) { return; }
	const int pn = pnOf(e);
	const bool toPlayer = localPlayer(pn);
	core::ImpulseStack& st = toPlayer ? g_ps[pn].imp : g_world[e->getUID()];
	const bool ok = st.add(g_table, vx, vy, ticks_);
	printlog("[apx-move] impulse %s uid=%u v=(%.2f,%.2f) ticks=%d %s dist_tabled=%.1f", toPlayer ? "player" : "entity", e->getUID(), vx, vy, ticks_,
		ok ? "added" : "REFUSED (stack full or ticks<1)", core::impulseDistance(std::sqrt(vx * vx + vy * vy), ticks_));
}

void step(int pn)
{
	load();
	if ( g_old || pn < 0 || pn >= MAXPLAYERS || !players[pn] || !players[pn]->entity || !stats[pn] ) { return; }
	PS& S = g_ps[pn];
	if ( S.lastStepTick == ticks ) { return; }
	S.lastStepTick = ticks;
	const double t0 = nowUs();
	Entity* my = players[pn]->entity;
	const core::Table& T = g_table;

	// someone else wrote my velocity since I did (knockback elsewhere, dash, door steering, wind): keep it, decaying as vanilla did
	Vec2 vel = { (float)my->vel_x, (float)my->vel_y };
	if ( S.haveOut )
	{
		if ( len(vel) < 1e-6f && len(S.lastOut) > 0.05f ) { S.loco = {}; S.imp.clear(); S.leg.v = {}; } // stopped from outside (teleport, reset)
		else
		{
			const Vec2 ext = { vel.x - S.lastOut.x, vel.y - S.lastOut.y };
			if ( len(ext) > 1e-4f ) { S.leg.kick(ext); }
		}
	}

	Ctx c;
	buildCtx(pn, c, true);
	Stat* st = stats[pn];

	// legacy writers: Barony's knockback and dash fields, the strafe kick, players pushing each other apart
	if ( st->getEffectActive(EFF_DASH) || st->getEffectActive(EFF_KNOCKBACK) )
	{
		S.leg.kick({ (float)(my->monsterKnockbackVelocity * std::cos(my->monsterKnockbackTangentDir)), (float)(my->monsterKnockbackVelocity * std::sin(my->monsterKnockbackTangentDir)) });
		float rate = 0.95f;
		if ( c.grip >= 1.f ) { rate = 0.8f; }
		if ( !st->getEffectActive(EFF_DASH) && st->shoes && st->shoes->type == CLEAT_BOOTS && my->effectShapeshift == NOTHING ) { rate *= 0.5f; }
		my->monsterKnockbackVelocity *= rate;
	}
	else { my->monsterKnockbackVelocity = 0.f; my->monsterKnockbackTangentDir = 0.f; }
	if ( std::fabs(my->playerStrafeVelocity) > 0.1 )
	{
		S.leg.kick({ (float)(my->playerStrafeVelocity * std::cos(my->playerStrafeDir)), (float)(my->playerStrafeVelocity * std::sin(my->playerStrafeDir)) });
		my->playerStrafeVelocity *= 0.95;
	}
	else { my->playerStrafeDir = 0.0f; my->playerStrafeVelocity = 0.0f; }
	if ( !c.rooted )
	{
		for ( int i = 0; i < MAXPLAYERS; ++i )
		{
			if ( i == pn || !players[i] || !players[i]->entity ) { continue; }
			if ( entityInsideEntity(my, players[i]->entity) )
			{
				const double tg = std::atan2(my->y - players[i]->entity->y, my->x - players[i]->entity->x);
				S.leg.kick({ (float)(std::cos(tg) * 0.075), (float)(std::sin(tg) * 0.075) });
			}
		}
	}

	// impulses; knocked while a meaningful one is in flight
	const Vec2 iv = S.imp.tick();
	const bool knocked = len(iv) >= T.knockedMinSpeed || st->getEffectActive(EFF_KNOCKBACK);

	// sprint: hold, 1.5x, 4 stamina per s, off at 0 until the key is released
	float drain = 0.f;
	const bool tracked = apx::staminaEnabled();
	core::sprintTick(T, S.spr, c.sprintKey, c.sprintWanted, (float)apx::get(pn).value, tracked, &drain);
	if ( S.spr.on && drain > 0.f )
	{
		const real_t got = apx::spend(pn, drain, 2);
		S.sprintDrained += got;
		if ( apx::get(pn).value <= 0.0 ) { S.spr.on = false; S.spr.exhausted = true; }
	}
	if ( S.spr.on && !S.lastSprintOn ) { ++S.sprintStarts; printlog("[apx-move] sprint start p=%d stamina=%.1f", pn, (float)apx::get(pn).value); logRow("{\"ev\":\"sprint_start\",\"t\":%u,\"stam\":%.2f}\n", (unsigned)ticks, (float)apx::get(pn).value); }
	if ( !S.spr.on && S.lastSprintOn )
	{
		if ( S.spr.exhausted ) { ++S.exhaustions; }
		printlog("[apx-move] sprint stop p=%d stamina=%.1f exhausted=%d total_drained=%.2f", pn, (float)apx::get(pn).value, S.spr.exhausted ? 1 : 0, S.sprintDrained);
		logRow("{\"ev\":\"sprint_stop\",\"t\":%u,\"stam\":%.2f,\"exhausted\":%d}\n", (unsigned)ticks, (float)apx::get(pn).value, S.spr.exhausted ? 1 : 0);
	}
	S.lastSprintOn = S.spr.on;

	S.st = resolveState(S, c, S.spr.on, knocked);
	const float steady = core::steadySpeed(T, c.base, multOf(T, S.st == State::Knocked ? resolveState(S, c, S.spr.on, false) : S.st, c));
	const float control = knocked ? T.knockedControl : 1.f;
	S.loco = core::stepLoco(T, S.loco, c.wishV, steady, c.grip, control);

	const float decay = 1.f - (1.f - T.legacyDecay) * c.grip;
	const Vec2 lv = S.leg.tick(decay);
	Vec2 out = { S.loco.x + iv.x + lv.x, S.loco.y + iv.y + lv.y };
	out = core::clampSpeed(T, out);
	my->vel_x = out.x; my->vel_y = out.y;

	// wind (adds to vel); keep it as a legacy kick so it decays instead of vanishing or doubling
	my->processEntityWind();
	const Vec2 afterWind = { (float)my->vel_x, (float)my->vel_y };
	const Vec2 w = { afterWind.x - out.x, afterWind.y - out.y };
	if ( len(w) > 1e-6f ) { S.leg.kick(w); }
	S.lastOut = afterWind; S.haveOut = true;
	S.costUs += nowUs() - t0;
}

double collide(Entity* my, double vx, double vy)
{
	load();
	const double t0 = nowUs();
	const core::Table& T = g_table;
	hit.entity = nullptr;
	real_t x = my->x, y = my->y;
	double moved = 0.0;
	if ( barony_clear(x + vx, y + vy, my) )
	{
		my->x = x + vx; my->y = y + vy; hit.side = 0;
		moved = std::sqrt(vx * vx + vy * vy);
	}
	else
	{
		const hit_t first = hit;
		// into contact along the whole move, so a wall is met flush instead of a step short
		double lo = 0.0, hi = 1.0;
		for ( int i = 0; i < T.bisectSteps; ++i )
		{
			const double mid = (lo + hi) * 0.5;
			if ( barony_clear(x + vx * mid, y + vy * mid, my) ) { lo = mid; } else { hi = mid; }
		}
		x += vx * lo; y += vy * lo;
		const double rx = vx * (1.0 - lo), ry = vy * (1.0 - lo);
		bool slid = false, slidX = false, slidY = false;
		auto tryMove = [&](double dx, double dy) -> bool {
			if ( std::fabs(dx) + std::fabs(dy) < 1e-9 ) { return false; }
			if ( barony_clear(x + dx, y + dy, my) ) { x += dx; y += dy; return true; }
			return false;
		};
		// slide along the free axis (full tangent component: the part already travelled counts)
		if ( std::fabs(rx) >= std::fabs(ry) )
		{
			if ( tryMove(rx, 0.0) ) { slid = slidX = true; }
			if ( tryMove(0.0, ry) ) { slid = slidY = true; }
		}
		else
		{
			if ( tryMove(0.0, ry) ) { slid = slidY = true; }
			if ( tryMove(rx, 0.0) ) { slid = slidX = true; }
		}
		// both axes blocked and the move was real: a corner. Shift sideways to the nearest free line and carry on (no snag)
		if ( !slid && std::fabs(rx) + std::fabs(ry) > 0.02 )
		{
			const bool xDominant = std::fabs(rx) >= std::fabs(ry);
			for ( double off = T.nudgeStep; off <= T.nudgeMax + 1e-9 && !slid; off += T.nudgeStep )
			{
				for ( int sgn = -1; sgn <= 1 && !slid; sgn += 2 )
				{
					const double px = xDominant ? 0.0 : sgn * off, py = xDominant ? sgn * off : 0.0;
					if ( !barony_clear(x + px, y + py, my) ) { continue; }
					if ( barony_clear(x + px + rx, y + py + ry, my) ) { x += px + rx; y += py + ry; slid = true; }
					else if ( xDominant ? barony_clear(x + px + rx, y + py, my) : barony_clear(x + px, y + py + ry, my) )
					{
						x += px + (xDominant ? rx : 0.0); y += py + (xDominant ? 0.0 : ry); slid = true;
					}
				}
			}
		}
		const double dx = x - my->x, dy = y - my->y;
		my->x = x; my->y = y;
		moved = std::sqrt(dx * dx + dy * dy);
		hit = first;                       // what stood in the way of the intended move (callers bump creatures, doors, ...)
		hit.side = (slidX && !slidY) ? VERTICAL : ((slidY && !slidX) ? HORIZONTAL : 0);
	}
	const int pn = pnOf(my);
	if ( pn >= 0 && pn < MAXPLAYERS ) { g_ps[pn].costUs += nowUs() - t0; }
	return moved;
}

void frame()
{
	load();
	if ( !g_world.empty() && multiplayer != CLIENT )
	{
		for ( auto it = g_world.begin(); it != g_world.end(); )
		{
			Entity* e = uidToEntity(it->first);
			if ( !e ) { it = g_world.erase(it); continue; }
			const Vec2 v = it->second.tick();
			if ( len(v) > 0.f ) { collide(e, v.x, v.y); }
			if ( !it->second.any() ) { it = g_world.erase(it); } else { ++it; }
		}
	}
	static Uint32 lastSummary = 0;
	if ( ticks - lastSummary >= 500 && g_costNext > 0 )
	{
		lastSummary = ticks;
		std::vector<float> v(g_cost.begin(), g_cost.begin() + std::min<int>(g_costNext, (int)g_cost.size()));
		std::sort(v.begin(), v.end());
		const size_t n = v.size();
		printlog("[apx-move] cost n=%zu p50=%.1fus p99=%.1fus max=%.1fus", n, v[n / 2], v[std::min(n - 1, (size_t)(n * 0.99))], v[n - 1]);
	}
}

// ---------------------------------------------------------------------------------------------------------------------
// The gate's instrument: one JSONL row per tick for player 0 (any mode), written after collision.
void logTick(int pn)
{
	load();
	if ( pn < 0 || pn >= MAXPLAYERS || !players[pn] || !players[pn]->entity || !stats[pn] || !localPlayer(pn) ) { return; }
	PS& S = g_ps[pn];
	Entity* my = players[pn]->entity;
	const Vec2 pos = { (float)my->x, (float)my->y };
	const double cost = S.costUs;
	S.costUs = 0.0;
	if ( !g_old && cost > 0.0 )
	{
		g_cost[g_costNext % (int)g_cost.size()] = (float)cost;
		++g_costNext;
	}
	const bool contiguous = S.havePrev && ticks == S.prevTick + 1;
	const Vec2 prev = S.prevPos;
	S.prevPos = pos; S.prevTick = ticks; S.havePrev = true;
	if ( !g_logOn || !contiguous ) { return; }

	Ctx c;
	buildCtx(pn, c, false);
	const bool sprintOn = g_old ? c.sprintWanted : S.spr.on;
	const float dispX = pos.x - prev.x, dispY = pos.y - prev.y;
	const float disp = std::sqrt(dispX * dispX + dispY * dispY);
	const bool knocked = g_old ? (stats[pn]->getEffectActive(EFF_KNOCKBACK) > 0) : (S.st == State::Knocked);
	const State st = g_old ? resolveState(S, c, sprintOn, knocked) : S.st;
	const float steady = core::steadySpeed(g_table, c.base, multOf(g_table, st == State::Knocked ? resolveState(S, c, sprintOn, false) : st, c));
	const float expect = steady * c.wishLen;

	// stuck: input wants motion, the body did not move, and a step of 0.5 unit in a direction the player wants IS free
	bool stuck = false, blocked = false;
	if ( c.wishLen > 0.f && !knocked && disp < 0.25f * expect )
	{
		blocked = true;
		const hit_t keep = hit;
		const float wl = c.wishLen;
		for ( int i = 0; i < 16 && !stuck; ++i )
		{
			const float a = (float)i * (float)(PI / 8.0);
			const Vec2 d = { std::cos(a), std::sin(a) };
			if ( core::dot(d, c.wishV) / wl > 0.1f && barony_clear(my->x + 0.5f * d.x, my->y + 0.5f * d.y, my) ) { stuck = true; }
		}
		hit = keep;
	}
	else if ( c.wishLen > 0.f && !knocked && disp < 0.97f * expect ) { blocked = true; }
	++S.tickCount; if ( stuck ) { ++S.stuckTicks; } if ( blocked ) { ++S.blockedTicks; }

	logRow("{\"t\":%u,\"x\":%.3f,\"y\":%.3f,\"yaw\":%.4f,\"vx\":%.4f,\"vy\":%.4f,\"disp\":%.4f,\"st\":\"%s\",\"wl\":%.3f,\"fw\":%.2f,\"sd\":%.2f,\"base\":%.3f,\"exp\":%.4f,"
		"\"sprint_key\":%d,\"stam\":%.3f,\"blk\":%d,\"stk\":%d,\"cost_us\":%.2f,\"level\":%d,\"mode\":\"%s\"}\n",
		(unsigned)ticks, pos.x, pos.y, (float)my->yaw, (float)my->vel_x, (float)my->vel_y, disp, core::stateName(st), c.wishLen, c.forward, c.side, c.base, expect,
		c.sprintKey ? 1 : 0, apx::staminaEnabled() ? (float)apx::get(pn).value : -1.f, blocked ? 1 : 0, stuck ? 1 : 0, cost, (int)currentlevel, g_old ? "old" : "new");

	if ( S.testPending && !S.imp.any() )
	{
		S.testPending = false;
		const float got = std::sqrt((pos.x - S.testStart.x) * (pos.x - S.testStart.x) + (pos.y - S.testStart.y) * (pos.y - S.testStart.y));
		const float tabled = core::impulseDistance(S.testSpeed, S.testTicks);
		printlog("[apx-move] impulse_test speed=%.2f ticks=%d tabled=%.2f got=%.2f open_run=%.1f err=%.2f%%", S.testSpeed, S.testTicks, tabled, got, S.testRun, tabled > 0 ? 100.f * (got - tabled) / tabled : 0.f);
		logRow("{\"ev\":\"impulse_test\",\"speed\":%.3f,\"ticks\":%d,\"tabled\":%.3f,\"got\":%.3f,\"open_run\":%.1f,\"t0\":%u,\"t1\":%u}\n", S.testSpeed, S.testTicks, tabled, got, S.testRun, (unsigned)S.testBegin, (unsigned)ticks);
	}
	if ( g_log && (ticks % 50) == 0 ) { std::fflush(g_log); }
}

} // namespace move
} // namespace apx

// ---------------------------------------------------------------------------------------------------------------------
// Test hooks for the gate (console, allowed in QA launches): set-up only; every walk itself is real input.
namespace
{
	// length of the clear run (units) from the player along a direction
	float runLength(Entity* my, double ang, float maxUnits = 480.f)
	{
		const hit_t keep = hit;
		float run = 0.f;
		for ( float d = 2.f; d <= maxUnits; d += 2.f )
		{
			if ( !barony_clear(my->x + std::cos(ang) * d, my->y + std::sin(ang) * d, my) ) { break; }
			run = d;
		}
		hit = keep;
		return run;
	}
}

static ConsoleCommand ccmd_apxmove_face("/apxmove_face", "apx: set the player's facing for a gate route: open | wall | <degrees>", [](int argc, const char** argv) {
	if ( !players[0] || !players[0]->entity ) { return; }
	Entity* my = players[0]->entity;
	const std::string a = argc > 1 ? argv[1] : "open";
	double yaw = my->yaw;
	float run = 0.f;
	if ( a == "open" )
	{
		for ( int i = 0; i < 32; ++i )
		{
			const double ang = i * (PI / 16.0);
			const float r = runLength(my, ang);
			if ( r > run + 1e-3f ) { run = r; yaw = ang; }
		}
	}
	else if ( a == "wall" )
	{
		// a wall at least 3 tiles off with room to slide: face it 40 degrees off the normal toward the longer side
		float best = 1e9f; double bestAng = 0.0;
		for ( int i = 0; i < 4; ++i )
		{
			const double ang = i * (PI / 2.0);
			const float r = runLength(my, ang, 320.f);
			if ( r >= 48.f && r < best ) { best = r; bestAng = ang; }
		}
		const float left = runLength(my, bestAng - PI / 2.0), right = runLength(my, bestAng + PI / 2.0);
		const double off = (right >= left ? 1.0 : -1.0) * (40.0 * PI / 180.0);
		yaw = bestAng + off; run = best;
	}
	else { yaw = std::atof(a.c_str()) * PI / 180.0; run = runLength(my, yaw); }
	while ( yaw < 0 ) { yaw += 2 * PI; }
	while ( yaw >= 2 * PI ) { yaw -= 2 * PI; }
	my->yaw = yaw;
	printlog("[apx-move] face %s yaw=%.4f rad run_ahead=%.1f units x=%.1f y=%.1f", a.c_str(), yaw, run, my->x, my->y);
});

static ConsoleCommand ccmd_apxmove_stamina("/apxmove_stamina", "apx: set player 0's stamina (gate set-up)", [](int argc, const char** argv) {
	if ( argc < 2 ) { return; }
	apx::get(0).value = std::atof(argv[1]);
	printlog("[apx-move] stamina set to %.1f", (float)apx::get(0).value);
});

static ConsoleCommand ccmd_apxmove_impulse("/apxmove_impulse", "apx: apply an impulse to player 0 along the most open direction: <speed units/tick> <ticks>", [](int argc, const char** argv) {
	if ( argc < 3 || !players[0] || !players[0]->entity ) { return; }
	Entity* my = players[0]->entity;
	const float speed = (float)std::atof(argv[1]);
	const int n = std::atoi(argv[2]);
	double bestAng = 0.0; float run = 0.f;
	for ( int i = 0; i < 32; ++i )
	{
		const double ang = i * (PI / 16.0);
		const float r = runLength(my, ang);
		if ( r > run + 1e-3f ) { run = r; bestAng = ang; }
	}
	auto& S = apx::move::g_ps_test(0);
	S.testPending = true; S.testStart = { (float)my->x, (float)my->y }; S.testSpeed = speed; S.testTicks = n; S.testRun = run; S.testBegin = ticks;
	apx::move::impulse(my, speed * (float)std::cos(bestAng), speed * (float)std::sin(bestAng), n);
});
