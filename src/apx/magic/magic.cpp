// apx/magic/magic.cpp -- see magic.hpp. Engine glue around the pure core (magic_core.hpp): data, the cast pipeline, emission, stamps,
// hands, sigil, the cast log, and the gate's instruments.
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
#include "../../light.hpp"
#include "../../engine/audio/sound.hpp"
#include "../../prng.hpp"
#include "../../interface/consolecommand.hpp"
#include "../../apx_combat.hpp"
#include "../proj/proj.hpp"
#include "../move/move.hpp"
#include "../qadriver.hpp"
#include "magic.hpp"
#include "../datadir.hpp" // the one data-folder rule
#include "rapidjson/document.h"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <filesystem>
#include <string>
#include <unordered_map>
#include <vector>

namespace apx
{
namespace magic
{
using namespace core;

namespace
{
	// ================================================================================================ tables and rows
	CostTable g_cost;
	ConcTable g_conc;
	TimeTable g_time;
	std::vector<Rule> g_rules;
	std::vector<std::string> g_ruleWhy;
	float g_backfire = kBackfireChance, g_softStrength = 0.5f;
	proj::ProjDef g_projBase;                 // from magic.json "projectile"
	bool g_loaded = false, g_old = false;
	float g_rollOverride = -1.f;              // gate: a fixed backfire roll

	struct Row
	{
		int id = 0;
		std::string key, name, onHit;
		Picks picks;
		int deliver = 1;                      // 0 native proj, 1 legacy
		int damage = 0;
	};
	std::vector<Row> g_rows;
	std::unordered_map<int, int> g_rowById;

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
	int indexOf(const char* const* names, int n, const char* s)
	{
		if ( !s ) { return -1; }
		if ( !std::strcmp(s, "*") ) { return -2; }
		for ( int i = 0; i < n; ++i ) { if ( !std::strcmp(names[i], s) ) { return i; } }
		return -1;
	}
	template <int N> void fillArr(const rapidjson::Value& o, const char* const (&names)[N], float* dst)
	{
		for ( int i = 0; i < N; ++i ) { dst[i] = num(o, names[i], dst[i]); }
	}
	std::vector<std::string> candidates(const char* file)
	{
		std::vector<std::string> c;
		if ( const char* e = std::getenv("APX_MAGIC_DATA") ) { c.push_back(std::string(e) + "/magic/" + file); }
		c.push_back(apx::dataDir() + "/magic/" + file); // exe folder first, then the dev source (datadir.hpp)
		return c;
	}
	std::string readData(const char* file, std::string* used)
	{
		for ( auto& c : candidates(file) ) { std::string t = readFile(c); if ( !t.empty() ) { if ( used ) { *used = c; } return t; } }
		return std::string();
	}

	void load()
	{
		if ( g_loaded ) { return; }
		g_loaded = true;
		const char* old = std::getenv("APX_MAGIC_OLD");
		g_old = old && old[0] == '1';
		g_projBase = proj::ProjDef();
		std::string used;
		std::string text = readData("magic.json", &used);
		rapidjson::Document doc;
		if ( text.empty() || doc.Parse(text.c_str()).HasParseError() || !doc.IsObject() )
		{
			printlog("[apx-magic] ERROR: data/apx/magic/magic.json missing or unparsable; built-in default tables used");
		}
		else
		{
			const auto& co = sub(doc, "cost");
			fillArr(sub(co, "base"), kVerbNames, g_cost.base); fillArr(sub(co, "area"), kShapeNames, g_cost.area);
			fillArr(sub(co, "affinity"), kElementNames, g_cost.affinity); fillArr(sub(co, "factor"), kModifierNames, g_cost.factor);
			g_cost.nearby = num(co, "nearby", g_cost.nearby); g_cost.nearbyTiles = num(co, "nearby_tiles", g_cost.nearbyTiles);
			const auto& cc = sub(doc, "concentration");
			fillArr(sub(cc, "spend"), kVerbNames, g_conc.spend); fillArr(sub(cc, "spend_modifier"), kModifierNames, g_conc.spendMod);
			g_conc.regenPerSecond = num(cc, "regen_per_second", g_conc.regenPerSecond); g_conc.failBelow = num(cc, "fail_below", g_conc.failBelow);
			g_conc.timeStretch = num(cc, "time_stretch", g_conc.timeStretch);
			const auto& ct = sub(doc, "cast_time_ms");
			fillArr(sub(ct, "verb"), kVerbNames, g_time.ms); fillArr(sub(ct, "modifier_mul"), kModifierNames, g_time.modMul);
			const auto& cd = sub(doc, "contradiction");
			g_backfire = num(cd, "backfire_chance", g_backfire); g_softStrength = num(cd, "soft_strength", g_softStrength);
			if ( cd.IsObject() && cd.HasMember("rules") && cd["rules"].IsArray() )
			{
				g_rules.clear();
				g_ruleWhy.clear();
				for ( auto& r : cd["rules"].GetArray() )
				{
					Rule x = { -1, -1, -1, -1, G_SOFT, "" };
					auto pick = [&](const char* key, const char* const* names, int n) {
						return (r.HasMember(key) && r[key].IsString()) ? indexOf(names, n, r[key].GetString()) : -2;
					};
					x.verb = pick("verb", kVerbNames, V_COUNT); x.shape = pick("shape", kShapeNames, S_COUNT);
					x.element = pick("element", kElementNames, E_COUNT); x.modifier = pick("modifier", kModifierNames, M_COUNT);
					x.verb = x.verb == -2 ? -1 : x.verb; x.shape = x.shape == -2 ? -1 : x.shape;
					x.element = x.element == -2 ? -1 : x.element; x.modifier = x.modifier == -2 ? -1 : x.modifier;
					x.grade = (r.HasMember("grade") && r["grade"].IsString() && !std::strcmp(r["grade"].GetString(), "hard")) ? G_HARD : G_SOFT;
					g_ruleWhy.push_back(r.HasMember("why") && r["why"].IsString() ? r["why"].GetString() : "");
					g_rules.push_back(x);
				}
				for ( size_t i = 0; i < g_rules.size(); ++i ) { g_rules[i].why = g_ruleWhy[i].c_str(); }
			}
			const auto& pj = sub(doc, "projectile");
			g_projBase.speed = num(pj, "speed_units_per_tick", g_projBase.speed); g_projBase.ricochetDeg = num(pj, "ricochet_deg", g_projBase.ricochetDeg);
			g_projBase.maxBounces = (int)num(pj, "max_bounces", (float)g_projBase.maxBounces); g_projBase.bounceKeep = num(pj, "bounce_keep", g_projBase.bounceKeep);
			g_projBase.lifeTicks = (int)num(pj, "life_ticks", (float)g_projBase.lifeTicks); g_projBase.radius = num(pj, "radius_units", g_projBase.radius);
		}
		if ( g_rules.empty() ) { g_rules.assign(kDefaultRules, kDefaultRules + kDefaultRuleCount); }
		// the 223 vanilla rows
		std::string stext = readData("spells.json", nullptr);
		rapidjson::Document sd;
		if ( stext.empty() || sd.Parse(stext.c_str()).HasParseError() || !sd.HasMember("spells") || !sd["spells"].IsArray() )
		{
			printlog("[apx-magic] ERROR: data/apx/magic/spells.json missing or unparsable: vanilla spells have no rows");
		}
		else
		{
			for ( auto& s : sd["spells"].GetArray() )
			{
				Row r;
				r.id = s.HasMember("id") ? s["id"].GetInt() : 0;
				r.key = s.HasMember("key") ? s["key"].GetString() : "";
				r.name = s.HasMember("name") ? s["name"].GetString() : r.key;
				const auto& p = sub(s, "picks");
				auto idx = [&](const char* key, const char* const* names, int n) {
					return (p.HasMember(key) && p[key].IsString()) ? std::max(0, indexOf(names, n, p[key].GetString())) : 0;
				};
				r.picks.verb = idx("verb", kVerbNames, V_COUNT); r.picks.shape = idx("shape", kShapeNames, S_COUNT);
				r.picks.element = idx("element", kElementNames, E_COUNT); r.picks.modifier = idx("modifier", kModifierNames, M_COUNT);
				r.deliver = (s.HasMember("deliver") && s["deliver"].IsString() && !std::strcmp(s["deliver"].GetString(), "proj")) ? 0 : 1;
				r.onHit = (s.HasMember("on_hit") && s["on_hit"].IsString()) ? s["on_hit"].GetString() : "";
				r.damage = s.HasMember("damage") && s["damage"].IsInt() ? s["damage"].GetInt() : 0;
				g_rowById[r.id] = (int)g_rows.size();
				g_rows.push_back(r);
			}
		}
		printlog("[apx-magic] loaded %s: %d spell rows, %d contradiction rules, mana table base[bolt]=%.1f, concentration fail below %.0f%%, %s",
			used.empty() ? "(defaults)" : used.c_str(), (int)g_rows.size(), (int)g_rules.size(), g_cost.base[V_BOLT], g_conc.failBelow,
			g_old ? "APX_MAGIC_OLD=1 (CONTROL: Barony casting)" : "apx::magic casting active");
	}

	// ================================================================================================ state
	enum class Phase : int { Idle = 0, Casting, Release };
	struct Aim
	{
		float cx = 0, cy = 0, yaw = 0;       // the caster
		float tx = 0, ty = 0;                // the aim point
		Uint32 targetUid = 0;
		int wallDir = 0, rf = 0;
		bool haveTarget = false;             // a creature (or tile, for floor spells) was found
		float dist = 0;
	};
	struct Cast
	{
		Phase phase = Phase::Idle;
		int row = -1;
		Picks picks;
		Grade grade = G_NONE;
		float roll = 0.f;
		int ticksLeft = 0, ticksTotal = 0, releaseLeft = 0;
		int mana = 0;
		float concAtStart = 100.f;
		spell_t* spell = nullptr;
		bool book = false, tome = false, nearby = false;
		Aim aim;
		Uint32 startTick = 0;
		Entity* sigil = nullptr;
		Uint32 sigilUid = 0;
		int seq = 0;
	};
	struct PlayerS { Cast cast; Concentration conc; };
	PlayerS g_p[MAXPLAYERS];
	Counters g_c;
	long long g_castSeq = 0;
	std::deque<std::string> g_logRing;
	FILE* g_logFile = nullptr;
	bool g_logTried = false;

	struct Ctx { Picks picks; int row = -1; float strength = 1.f; int chain = 0; int refs = 0; int onHitKind = 0; bool trigger = false; Uint32 owner = 0; int damage = 0; bool free = true; };
	std::vector<Ctx> g_ctx;

	struct Stamp
	{
		Uint32 ownerUid = 0;
		float x = 0, y = 0, yaw = 0, r = 16;
		int shape = S_SPHERE, element = E_FIRE, damage = 0;
		int ticksLeft = 0, period = 0, nextHit = 0;
		bool selfHit = false, root = false, state = true, pull = false, push = false, heal = false;
		float strength = 1.f;
		int row = -1;
	};
	std::vector<Stamp> g_stamps;

	struct Pending { int delay = 0; Uint32 ownerUid = 0; Picks picks; int row = -1; float strength = 1.f; Aim aim; int kind = 0; int count = 0; int every = 1; int echoLeft = 0; };
	std::vector<Pending> g_pending;
	std::string g_outcome = "emit";
	float g_outStrength = 1.f;

	using Clock = std::chrono::steady_clock;
	double nowUs() { return std::chrono::duration<double, std::micro>(Clock::now().time_since_epoch()).count(); }
	std::vector<float> g_frameCost;
	double g_lastPerfLog = 0;

	// ================================================================================================ log
	void openLog()
	{
		if ( g_logTried ) { return; }
		g_logTried = true;
		if ( const char* d = std::getenv("APX_MAGIC_LOG") )
		{
			std::error_code ec;
			std::filesystem::create_directories(d, ec);
			g_logFile = std::fopen((std::string(d) + "/magic.jsonl").c_str(), "ab");
		}
	}
	void jsonl(const std::string& line)
	{
		openLog();
		if ( g_logFile ) { std::fputs(line.c_str(), g_logFile); std::fputc('\n', g_logFile); std::fflush(g_logFile); }
	}
	std::string picksStr(const Picks& p)
	{
		return std::string(kVerbNames[p.verb]) + "/" + kShapeNames[p.shape] + "/" + kElementNames[p.element] + "/" + kModifierNames[p.modifier];
	}

	// ================================================================================================ helpers
	Entity* playerEntity(int p) { return (p >= 0 && p < MAXPLAYERS && players[p]) ? players[p]->entity : nullptr; }

	bool creature(Entity* e) { return e && (e->behavior == &actMonster || e->behavior == &actPlayer) && e->getStats() && e->getStats()->HP > 0; }

	// a matching element source within nearbyTiles: fire near flames and torches, light near any light source
	bool elementNearby(Entity* caster, int element)
	{
		if ( !caster || (element != E_FIRE && element != E_LIGHT) ) { return false; }
		const float lim = g_cost.nearbyTiles * 16.f;
		for ( node_t* n = map.entities->first; n; n = n->next )
		{
			Entity* e = (Entity*)n->element;
			if ( !e || e == caster ) { continue; }
			const bool src = (element == E_FIRE) ? (e->behavior == &actTorch || e->behavior == &actCampfire || e->flags[BURNING] != 0)
				: (e->behavior == &actTorch || e->behavior == &actCampfire || e->behavior == &actMagiclightBall);
			if ( !src ) { continue; }
			if ( std::fabs(e->x - caster->x) <= lim && std::fabs(e->y - caster->y) <= lim
				&& std::hypot(e->x - caster->x, e->y - caster->y) <= lim ) { return true; }
		}
		return false;
	}

	Contradiction grade(const Picks& p) { return contradiction(g_rules.data(), (int)g_rules.size(), p); }

	// deterministic roll in [0,1): a pure function of the cast number, the player and the picks -- logged, never hidden
	float rollFor(long long seq, int player, const Picks& p)
	{
		if ( g_rollOverride >= 0.f ) { return g_rollOverride; }
		unsigned long long h = 1469598103934665603ull;
		auto mix = [&](unsigned long long v) { h ^= v; h *= 1099511628211ull; };
		mix((unsigned long long)seq); mix((unsigned long long)player); mix((unsigned long long)(p.verb * 1000 + p.shape * 100 + p.element * 10 + p.modifier));
		h ^= h >> 29; h *= 0xbf58476d1ce4e5b9ull; h ^= h >> 32;
		return (float)((h >> 11) & 0xFFFFFF) / (float)0x1000000;
	}

	int spriteFor(int element)
	{
		static const int s[E_COUNT] = { 168, 172, 171, 175, 171, 170, 173, 598, 1244, 597 };
		return s[element];
	}
	const char* lightFor(int element)
	{
		static const char* l[E_COUNT] = { "orb_red", "orb_blue", nullptr, nullptr, "magic_green", "magic_white", "magic_white", "orb_purple", "orb_purple", "magic_green" };
		return l[element];
	}
	int kindFor(int element)
	{
		static const int k[E_COUNT] = { 0, 2, 0, 2, 1, 2, 2, 1, 0, 0 };   // apx::combat::Kind
		return k[element];
	}

	// the spell's own damage: mana-proportional for free picks, Barony's number for rows that have one
	int damageFor(Entity* caster, const Row* row, const Picks& p, int mana, float strength)
	{
		int d = 0;
		if ( row && row->id > 0 )
		{
			if ( Stat* cs = caster ? caster->getStats() : nullptr )
			{
				d = getSpellDamageFromID(row->id, caster, cs, nullptr, 0.0, false);
			}
			if ( d <= 0 ) { d = row->damage; }
		}
		if ( d <= 0 ) { d = (int)std::lround(mana * 2.4f); }
		(void)p;
		return std::max(1, (int)std::lround(d * strength));
	}

	Ctx& newCtx(int& idx)
	{
		for ( size_t i = 0; i < g_ctx.size(); ++i ) { if ( g_ctx[i].free ) { idx = (int)i; g_ctx[i] = Ctx(); g_ctx[i].free = false; return g_ctx[i]; } }
		g_ctx.emplace_back();
		idx = (int)g_ctx.size() - 1;
		g_ctx[idx].free = false;
		return g_ctx[idx];
	}

	void say(int player, const char* msg)
	{
		if ( player >= 0 ) { messagePlayer(player, MESSAGE_MISC, "%s", msg); }
	}

	// ================================================================================================ aim
	Entity* crosshairCreature(Entity* caster, float maxDist, float wallDist, float* alongOut)
	{
		Entity* best = nullptr;
		float bestAlong = 1e9f;
		const float c = (float)std::cos(caster->yaw), s = (float)std::sin(caster->yaw);
		for ( node_t* n = map.creatures->first; n; n = n->next )
		{
			Entity* e = (Entity*)n->element;
			if ( !e || e == caster || !creature(e) ) { continue; }
			const float dx = (float)(e->x - caster->x), dy = (float)(e->y - caster->y);
			const float along = dx * c + dy * s, perp = std::fabs(-dx * s + dy * c);
			if ( along <= 0.f || along > maxDist || along > wallDist + 4.f ) { continue; }
			if ( perp > (float)std::max(e->sizex, e->sizey) + 6.f ) { continue; }
			if ( along < bestAlong ) { bestAlong = along; best = e; }
		}
		if ( alongOut ) { *alongOut = bestAlong; }
		return best;
	}

	Aim solveAim(int player, Entity* caster, spell_t* spell, const Picks& p)
	{
		Aim a;
		a.cx = (float)caster->x; a.cy = (float)caster->y; a.yaw = (float)caster->yaw;
		a.tx = a.cx; a.ty = a.cy;
		if ( spell && spell->rangefinder != RANGEFINDER_NONE && player >= 0 )
		{
			// Barony's rangefinder as the aim solver (read-only): it fills the animation manager's target fields
			auto& ca = cast_animation[player];
			auto saved = ca;
			ca.spell = spell; ca.caster = caster->getUID(); ca.player = player; ca.stage = 0; /* ANIM_SPELL_CIRCLE */ ca.targetUid = 0; ca.wallDir = 0;
			ca.setRangeFinderLocation();
			a.rf = (int)ca.rangefinder;
			a.tx = (float)ca.target_x; a.ty = (float)ca.target_y; a.targetUid = ca.targetUid; a.wallDir = ca.wallDir;
			const bool tile = ca.rangefinder == RANGEFINDER_TARGET || ca.rangefinder == RANGEFINDER_TOUCH_FLOOR_TILE || ca.rangefinder == RANGEFINDER_TOUCH_WALL_TILE
				|| ca.rangefinder == RANGEFINDER_TOUCH_INTERACT_TEST;
			a.haveTarget = tile ? true : (ca.targetUid != 0 && uidToEntity(ca.targetUid) != nullptr);
			ca = saved;
			if ( tile && ((a.rf == RANGEFINDER_TOUCH_FLOOR_TILE) || (a.rf == RANGEFINDER_TOUCH_WALL_TILE)) )
			{
				a.tx = a.tx * 16.f + 8.f; a.ty = a.ty * 16.f + 8.f;   // tile indices to units for the stamp geometry
			}
			if ( a.rf == RANGEFINDER_TOUCH || a.rf == RANGEFINDER_TOUCH_INTERACT )
			{
				if ( Entity* t = uidToEntity(a.targetUid) ) { a.tx = (float)t->x; a.ty = (float)t->y; }
			}
			a.dist = std::hypot(a.tx - a.cx, a.ty - a.cy);
			return a;
		}
		// free picks: a ray to the first wall, the first creature on the crosshair within reach
		const float reach = (p.verb == V_MEND || p.verb == V_BIND || p.verb == V_PULL || p.verb == V_LIFT) ? 96.f : 80.f;
		const real_t wall = lineTrace(caster, caster->x, caster->y, caster->yaw, reach, LINETRACE_IGNORE_ENTITIES, false);
		float along = 0.f;
		Entity* t = crosshairCreature(caster, reach, (float)wall, &along);
		if ( t ) { a.tx = (float)t->x; a.ty = (float)t->y; a.targetUid = t->getUID(); a.haveTarget = true; a.dist = along; }
		else
		{
			const float d = std::max(8.f, std::min((float)wall - 6.f, reach));
			a.tx = a.cx + std::cos(a.yaw) * d; a.ty = a.cy + std::sin(a.yaw) * d; a.dist = d;
			a.haveTarget = false;
		}
		return a;
	}

	// ================================================================================================ effects
	bool wet(Entity* e)
	{
		const int tx = (int)(e->x / 16), ty = (int)(e->y / 16);
		if ( tx < 0 || ty < 0 || tx >= (int)map.width || ty >= (int)map.height ) { return false; }
		return swimmingtiles[map.tiles[ty * MAPLAYERS + tx * MAPLAYERS * map.height]];
	}

	void effect(Entity* e, int eff, int ticks, float strength)
	{
		if ( !e || !e->getStats() ) { return; }
		e->setEffect(eff, true, std::max(10, (int)std::lround(ticks * strength)), true);
	}

	// element states (04 section 3.2): fire burns, cold slows and makes brittle, shock arcs through wet bodies, acid corrodes
	void elementState(Entity* owner, Entity* target, int element, float strength, bool allowArc)
	{
		if ( !creature(target) ) { return; }
		Stat* ts = target->getStats();
		switch ( element )
		{
			case E_FIRE: target->SetEntityOnFire(owner); break;
			case E_ICE: effect(target, EFF_SLOW, 150, strength); break;
			case E_LIGHTNING:
				if ( allowArc && wet(target) )
				{
					int arcs = 0;
					for ( node_t* n = map.creatures->first; n && arcs < 3; n = n->next )
					{
						Entity* e = (Entity*)n->element;
						if ( !e || e == target || e == owner || !creature(e) || !wet(e) ) { continue; }
						if ( std::hypot(e->x - target->x, e->y - target->y) > 80.0 ) { continue; }
						proj::ProjDef d = g_projBase; d.element = E_LIGHTNING; d.kind = 2; d.momentum = 4; d.area = 4;
						d.damage = std::max(1, (int)std::lround(6 * strength));
						proj::strike(owner, e, d, (float)e->x, (float)e->y, nullptr, nullptr);
						++arcs;
					}
					if ( arcs ) { log("shock arcs through wet bodies: %d", arcs); }
				}
				break;
			case E_ACID:
			{
				bool hit = false;
				if ( ts )
				{
					Item* pieces[4] = { ts->helmet, ts->breastplate, ts->gloves, ts->shoes };
					const int nums[4] = { 0, 1, 2, 3 };
					for ( int i = 0; i < 4 && !hit; ++i ) { if ( pieces[i] && pieces[i]->status > BROKEN ) { target->degradeArmor(*ts, *pieces[i], nums[i]); hit = true; } }
				}
				if ( !hit ) { effect(target, EFF_POISONED, 100, strength); }
				break;
			}
			case E_STONE: apx::move::impulse(target, (float)std::cos(owner ? owner->yaw : 0.0) * 1.2f, (float)std::sin(owner ? owner->yaw : 0.0) * 1.2f, 8); break;
			case E_IRON: effect(target, EFF_BLEEDING, 120, strength); break;
			case E_LIGHT: effect(target, EFF_BLIND, 80, strength); break;
			case E_SHADOW: effect(target, EFF_WEAKNESS, 150, strength); break;
			case E_VOID: effect(target, EFF_CONFUSED, 100, strength); break;
			default: break;
		}
	}

	void rowState(Entity* owner, Entity* target, const std::string& on, float strength)
	{
		if ( on == "slow" ) { effect(target, EFF_SLOW, 150, strength); }
		else if ( on == "burn" ) { if ( target ) { target->SetEntityOnFire(owner); } }
		else if ( on == "bleed" ) { effect(target, EFF_BLEEDING, 120, strength); }
		else if ( on == "poison" ) { effect(target, EFF_POISONED, 120, strength); }
		else if ( on == "sleep" ) { effect(target, EFF_ASLEEP, 150, strength); }
		else if ( on == "confuse" ) { effect(target, EFF_CONFUSED, 150, strength); }
		else if ( on == "weakness" ) { effect(target, EFF_WEAKNESS, 180, strength); }
		else if ( on == "blind" ) { effect(target, EFF_BLIND, 100, strength); }
		else if ( on == "push" )
		{
			const float yaw = owner ? (float)owner->yaw : 0.f;
			apx::move::impulse(target, std::cos(yaw) * 1.5f, std::sin(yaw) * 1.5f, 8);
		}
		else if ( on == "shock" ) { elementState(owner, target, E_LIGHTNING, strength, true); }
		else if ( on == "corrode" ) { elementState(owner, target, E_ACID, strength, false); }
	}

	// ---- projectile callbacks ----------------------------------------------------------------------------
	void addStamp(const Stamp& s);
	void emitBolt(Entity* owner, const Picks& p, int rowIdx, float strength, const Aim& aim, int chainLeft, bool nested);

	void onProjHit(const proj::HitEvent& ev)
	{
		if ( ev.user2 < 0 || ev.user2 >= (int)g_ctx.size() ) { return; }
		Ctx& c = g_ctx[ev.user2];
		const Row* row = c.row >= 0 ? &g_rows[c.row] : nullptr;
		if ( row && row->deliver == 0 && !row->onHit.empty() ) { rowState(ev.owner, ev.target, row->onHit, c.strength); }
		else { elementState(ev.owner, ev.target, c.picks.element, c.strength, true); }
		if ( c.picks.modifier == M_TRIGGER )
		{
			// trigger-on-hit nests a second spell: a burst of the same element at the point of impact, half the damage
			++g_c.triggers;
			Stamp s; s.ownerUid = c.owner; s.x = ev.x; s.y = ev.y; s.r = 32.f; s.shape = S_SPHERE; s.element = c.picks.element; s.damage = std::max(1, c.damage / 2);
			s.ticksLeft = 1; s.period = 1; s.strength = c.strength; s.row = c.row;
			addStamp(s);
			log("trigger-on-hit: nested burst at (%.0f,%.0f) damage %d", ev.x, ev.y, s.damage);
		}
		if ( c.chain > 0 && ev.target )
		{
			Entity* best = nullptr; double bd = 1e9;
			for ( node_t* n = map.creatures->first; n; n = n->next )
			{
				Entity* e = (Entity*)n->element;
				if ( !e || e == ev.target || e == ev.owner || !creature(e) ) { continue; }
				const double d = std::hypot(e->x - ev.x, e->y - ev.y);
				if ( d < 96.0 && d < bd ) { bd = d; best = e; }
			}
			if ( best )
			{
				++g_c.chains;
				Aim a; a.cx = ev.x; a.cy = ev.y; a.yaw = (float)std::atan2(best->y - ev.y, best->x - ev.x); a.tx = (float)best->x; a.ty = (float)best->y;
				Picks np = c.picks; np.modifier = M_CHAIN; np.shape = S_POINT;
				emitBolt(ev.owner, np, c.row, c.strength * 0.7f, a, c.chain - 1, true);
				log("chain: hop to uid %u, %d left", best->getUID(), c.chain - 1);
			}
		}
	}
	void onProjEnd(const proj::EndEvent& ev)
	{
		if ( ev.user2 < 0 || ev.user2 >= (int)g_ctx.size() ) { return; }
		Ctx& c = g_ctx[ev.user2];
		if ( c.picks.shape == S_SPHERE && c.picks.verb == V_BOLT )
		{
			Stamp s; s.ownerUid = c.owner; s.x = ev.x; s.y = ev.y; s.r = 36.f; s.shape = S_SPHERE; s.element = c.picks.element; s.damage = std::max(1, c.damage / 2);
			s.ticksLeft = 1; s.period = 1; s.strength = c.strength; s.row = c.row;
			addStamp(s);
		}
		else if ( c.picks.modifier == M_TRIGGER && ev.why == 1 )
		{
			++g_c.triggers;
			Stamp s; s.ownerUid = c.owner; s.x = ev.x; s.y = ev.y; s.r = 32.f; s.shape = S_SPHERE; s.element = c.picks.element; s.damage = std::max(1, c.damage / 2);
			s.ticksLeft = 1; s.period = 1; s.strength = c.strength; s.row = c.row;
			addStamp(s);
		}
		if ( --c.refs <= 0 ) { c.free = true; }
	}
	void onProjStep(const proj::StepEvent& ev)
	{
		if ( ev.user2 < 0 || ev.user2 >= (int)g_ctx.size() ) { return; }
		Ctx& c = g_ctx[ev.user2];
		// bolt x trail leaves a hazard behind it
		Stamp s; s.ownerUid = c.owner; s.x = ev.x; s.y = ev.y; s.r = 14.f; s.shape = S_POINT; s.element = c.picks.element; s.damage = std::max(1, c.damage / 4);
		s.ticksLeft = 100; s.period = 10; s.nextHit = 0; s.strength = c.strength; s.row = c.row;
		addStamp(s);
	}

	proj::ProjDef makeDef(const Picks& p, int rowIdx, float strength, int damage, int ctxIdx, Entity* owner)
	{
		proj::ProjDef d = g_projBase;
		d.sprite = spriteFor(p.element);
		d.light = lightFor(p.element);
		d.element = p.element;
		d.kind = kindFor(p.element);
		d.damage = damage;
		d.momentum = 3.f + 0.25f * (float)damage;
		d.area = 4.f;
		d.friendly = false;
		d.onHit = onProjHit; d.onEnd = onProjEnd;
		d.user = (uintptr_t)(rowIdx >= 0 ? &g_rows[rowIdx] : nullptr);
		d.user2 = ctxIdx;
		(void)owner; (void)strength;
		return d;
	}

	// ---- bolt and its shapes -----------------------------------------------------------------------------
	void emitBolt(Entity* owner, const Picks& p, int rowIdx, float strength, const Aim& aim, int chainLeft, bool nested)
	{
		const Row* row = rowIdx >= 0 ? &g_rows[rowIdx] : nullptr;
		const int mana = costMana(g_cost, p, false);
		int dmg = damageFor(owner, row, p, mana, strength);
		if ( nested ) { dmg = std::max(1, (int)std::lround(dmg)); }
		int shots = 1;
		float spread = 0.f, lateral = 0.f;
		int life = g_projBase.lifeTicks, pierce = 0;
		float speed = g_projBase.speed;
		switch ( p.shape )
		{
			case S_POINT: break;
			case S_LINE: pierce = 3; speed = g_projBase.speed * 1.6f; break;
			case S_CONE: shots = 5; spread = 0.7f; life = 30; dmg = std::max(1, (int)std::lround(dmg * 0.55)); break;
			case S_SPHERE: break;
			case S_RING: shots = 8; spread = 6.2831853f; life = 40; dmg = std::max(1, (int)std::lround(dmg * 0.5)); break;
			case S_PLANE: shots = 5; lateral = 8.f; dmg = std::max(1, (int)std::lround(dmg * 0.5)); break;
			case S_TRAIL: break;
			default: break;
		}
		if ( p.modifier == M_ANCHORED ) { speed = 1.0f; }   // an anchored bolt drifts, it does not fly
		int ctxIdx = -1;
		Ctx& c = newCtx(ctxIdx);
		c.picks = p; c.row = rowIdx; c.strength = strength; c.chain = (p.modifier == M_CHAIN) ? (nested ? chainLeft : 2) : 0; c.owner = owner ? owner->getUID() : 0; c.damage = dmg;
		if ( p.modifier == M_CHAIN && nested && chainLeft <= 0 ) { c.chain = 0; }
		const float fwd = nested ? 6.f : 12.f;
		const float perpX = -std::sin(aim.yaw), perpY = std::cos(aim.yaw);
		for ( int i = 0; i < shots; ++i )
		{
			float yaw = aim.yaw, off = 0.f;
			if ( p.shape == S_CONE ) { yaw += (i - (shots - 1) / 2.f) * (spread / (shots - 1)); }
			else if ( p.shape == S_RING ) { yaw = aim.yaw + i * (spread / shots); }
			else if ( p.shape == S_PLANE ) { off = (i - (shots - 1) / 2.f) * lateral; }
			proj::ProjDef d = makeDef(p, rowIdx, strength, dmg, ctxIdx, owner);
			d.speed = speed; d.lifeTicks = life; d.pierceMax = pierce;
			if ( p.shape == S_TRAIL ) { d.onStep = onProjStep; d.trailEvery = 4; }
			const float ox = aim.cx + std::cos(yaw) * (p.shape == S_RING ? 14.f : fwd) + perpX * off;
			const float oy = aim.cy + std::sin(yaw) * (p.shape == S_RING ? 14.f : fwd) + perpY * off;
			++c.refs;
			if ( !proj::spawn(d, owner, proj::Vec3(ox, oy, -1.f), yaw) ) { --c.refs; }
			else { ++g_c.boltsNative; }
		}
		if ( c.refs <= 0 ) { c.free = true; }
	}

	// ================================================================================================ stamps (area shapes on tiles)
	bool inShape(const Stamp& s, float px, float py)
	{
		const float dx = px - s.x, dy = py - s.y, d = std::hypot(dx, dy);
		const float c = std::cos(s.yaw), sn = std::sin(s.yaw);
		const float along = dx * c + dy * sn, perp = -dx * sn + dy * c;
		switch ( s.shape )
		{
			case S_POINT: return d <= std::max(14.f, s.r);
			case S_SPHERE: return d <= s.r;
			case S_RING: return std::fabs(d - s.r) <= 14.f;
			case S_LINE: return along >= 0.f && along <= s.r && std::fabs(perp) <= 12.f;
			case S_CONE: return d <= s.r && d > 0.1f && std::fabs(std::atan2(perp, along)) <= 0.52f;
			case S_PLANE: return std::fabs(along) <= 10.f && std::fabs(perp) <= s.r;
			case S_TRAIL: return d <= 14.f;
			default: return d <= s.r;
		}
	}

	void addStamp(const Stamp& s)
	{
		g_stamps.push_back(s);
		++g_c.areas;
	}

	void stampTick(Stamp& s)
	{
		Entity* owner = uidToEntity(s.ownerUid);
		proj::ProjDef d = g_projBase;
		d.element = s.element; d.kind = kindFor(s.element); d.damage = s.damage; d.momentum = 3.f + 0.25f * (float)s.damage; d.area = 12.f; d.friendly = true;
		const Row* row = s.row >= 0 ? &g_rows[s.row] : nullptr;
		for ( node_t* n = map.creatures->first; n; n = n->next )
		{
			Entity* e = (Entity*)n->element;
			if ( !e || !creature(e) || (e == owner && !s.selfHit) ) { continue; }
			if ( !inShape(s, (float)e->x, (float)e->y) ) { continue; }
			++g_c.stampHits;
			if ( s.heal )
			{
				spell_changeHealth(e, std::max(1, (int)std::lround(s.damage * s.strength)), false, e == owner);
				continue;
			}
			if ( s.damage > 0 ) { proj::strike(owner, e, d, (float)e->x, (float)e->y, nullptr, nullptr); }
			if ( row && row->deliver == 0 && !row->onHit.empty() ) { rowState(owner, e, row->onHit, s.strength); }
			else if ( s.state ) { elementState(owner, e, s.element, s.strength, true); }
			if ( s.root ) { effect(e, EFF_ROOTED, 100, s.strength); }
			if ( s.pull || s.push )
			{
				const float dx = (s.pull ? -1.f : 1.f) * (float)(e->x - s.x), dy = (s.pull ? -1.f : 1.f) * (float)(e->y - s.y);
				const float l = std::max(1.f, std::hypot(dx, dy));
				apx::move::impulse(e, dx / l * 1.4f, dy / l * 1.4f, 10);
			}
		}
	}

	void stampsFrame()
	{
		for ( size_t i = 0; i < g_stamps.size(); )
		{
			Stamp& s = g_stamps[i];
			if ( s.nextHit <= 0 ) { stampTick(s); s.nextHit = std::max(1, s.period); }
			--s.nextHit;
			if ( --s.ticksLeft <= 0 ) { g_stamps[i] = g_stamps.back(); g_stamps.pop_back(); continue; }
			++i;
		}
	}

	void groundSigil(Entity* parent, float x, float y, int lifeTicks, int radiusUnits)
	{
		if ( Entity* fx = createParticleAOEIndicator(parent, x, y, 0.0, (Uint32)std::max(10, lifeTicks), std::max(8, radiusUnits)) ) { (void)fx; }
	}

	float shapeRadius(int shape)
	{
		switch ( shape )
		{
			case S_POINT: return 16.f; case S_LINE: return 80.f; case S_CONE: return 64.f; case S_SPHERE: return 40.f;
			case S_RING: return 48.f; case S_PLANE: return 40.f; case S_TRAIL: return 14.f; default: return 32.f;
		}
	}

	// ================================================================================================ emission by verb
	void doBackfire(Entity* caster, const Picks& p, int mana, float strength, int rowIdx)
	{
		++g_c.backfired;
		proj::ProjDef d = g_projBase; d.element = p.element; d.kind = kindFor(p.element); d.friendly = true;
		d.damage = std::max(1, (int)std::lround(mana * 1.5f * strength));
		d.momentum = 3.f + 0.25f * (float)d.damage; d.area = 12.f;
		int dealt = proj::strike(caster, caster, d, (float)caster->x, (float)caster->y, nullptr, nullptr);
		elementState(caster, caster, p.element, strength, false);
		log("BACKFIRE: %s ran on the caster's cell, %d damage taken (row %d)", picksStr(p).c_str(), dealt, rowIdx >= 0 ? g_rows[rowIdx].id : 0);
	}

	// ---- wall blocks: a wall is real bodies in the way (04b section 7), standing for the stamp's life
	void actApxWall(Entity* my)
	{
		if ( --my->skill[1] <= 0 ) { list_RemoveNode(my->mynode); return; }
		my->z = -2.0 + std::min(0, 0);   // keep upright
	}
	void spawnWallBlocks(Entity* owner, const Stamp& s)
	{
		std::vector<std::pair<float, float>> pts;
		const float c = std::cos(s.yaw), sn = std::sin(s.yaw);
		switch ( s.shape )
		{
			case S_PLANE: for ( int i = -2; i <= 2; ++i ) { pts.push_back({ s.x - sn * i * 16.f, s.y + c * i * 16.f }); } break;
			case S_LINE: for ( int i = 1; i <= 5; ++i ) { pts.push_back({ s.x + c * i * 16.f, s.y + sn * i * 16.f }); } break;
			case S_CONE: for ( int i = -1; i <= 1; ++i ) { pts.push_back({ s.x + c * 48.f - sn * i * 20.f, s.y + sn * 48.f + c * i * 20.f }); } break;
			case S_RING: case S_SPHERE:
				for ( int i = 0; i < 8; ++i ) { const float a = i * 0.785398f; pts.push_back({ s.x + std::cos(a) * s.r * 0.8f, s.y + std::sin(a) * s.r * 0.8f }); }
				break;
			default: pts.push_back({ s.x, s.y }); break;
		}
		int n = 0;
		for ( auto& p : pts )
		{
			const int tx = (int)p.first >> 4, ty = (int)p.second >> 4;
			if ( p.first < 0 || p.second < 0 || tx >= (int)map.width || ty >= (int)map.height ) { continue; }
			if ( map.tiles[OBSTACLELAYER + ty * MAPLAYERS + tx * MAPLAYERS * map.height] || !map.tiles[ty * MAPLAYERS + tx * MAPLAYERS * map.height] ) { continue; }
			bool occupied = false;   // do not entomb a creature standing there
			for ( node_t* nn = map.creatures->first; nn; nn = nn->next )
			{
				Entity* e = (Entity*)nn->element;
				if ( e && std::fabs(e->x - p.first) < 12 && std::fabs(e->y - p.second) < 12 ) { occupied = true; break; }
			}
			if ( occupied ) { continue; }
			Entity* b = newEntity(s.element == E_ICE ? 172 : (s.element == E_FIRE ? 168 : 245), 1, map.entities, nullptr);
			if ( !b ) { continue; }
			b->x = p.first; b->y = p.second; b->z = -2.0;
			b->sizex = 7; b->sizey = 7;
			b->yaw = s.yaw;
			b->flags[PASSABLE] = false; b->flags[UPDATENEEDED] = true;
			b->behavior = &actApxWall;
			b->skill[1] = s.ticksLeft;
			++n;
		}
		(void)owner;
		log("wall: %d blocking bodies for %d ticks", n, s.ticksLeft);
	}

	// a stamp placed at (x, y) for an area verb; verb flags decide what it does to those inside
	void emitArea(Entity* owner, const Picks& p, int rowIdx, float strength, const Aim& aim, int mana)
	{
		const Row* row = rowIdx >= 0 ? &g_rows[rowIdx] : nullptr;
		Stamp s;
		s.ownerUid = owner ? owner->getUID() : 0;
		s.yaw = aim.yaw; s.shape = p.shape; s.r = shapeRadius(p.shape); s.element = p.element; s.strength = strength; s.row = rowIdx;
		const bool anchored = p.modifier == M_ANCHORED;
		s.x = anchored ? aim.cx : aim.tx; s.y = anchored ? aim.cy : aim.ty;
		// line, cone and plane stand at the caster's side of the aim point so they cover the way ahead
		if ( p.shape == S_LINE || p.shape == S_CONE ) { s.x = aim.cx; s.y = aim.cy; }
		s.damage = damageFor(owner, row, p, mana, strength);
		s.ticksLeft = 1; s.period = 1;
		switch ( p.verb )
		{
			case V_BURST: break;
			case V_WALL: s.ticksLeft = 200; s.period = 15; s.damage = std::max(1, s.damage / 4); break;
			case V_PIT: s.ticksLeft = 120; s.period = 20; s.root = true; s.damage = std::max(1, s.damage / 2); break;
			case V_LIFT: s.push = true; s.damage = 0; s.state = false; s.ticksLeft = 10; s.period = 5; break;
			case V_PULL: s.pull = true; s.damage = 0; s.state = false; s.ticksLeft = 10; s.period = 5; break;
			case V_BIND: s.root = true; s.damage = 0; s.state = false; s.ticksLeft = 1; break;
			case V_MEND:
				s.heal = true; s.state = false; s.ticksLeft = 1;
				if ( p.modifier == M_INVERTED ) { s.heal = false; s.damage = std::max(1, s.damage); s.state = true; }
				break;
			default: break;
		}
		if ( p.modifier == M_TIMER ) { s.ticksLeft = std::max(s.ticksLeft, 1); }
		if ( p.verb == V_MEND && !aim.haveTarget && p.shape == S_POINT ) { s.x = aim.cx; s.y = aim.cy; }   // nobody on the crosshair: yourself
		addStamp(s);
		// the same sigil that telegraphed the cast stays as the mark of a standing effect
		if ( p.verb == V_WALL || p.verb == V_PIT ) { groundSigil(nullptr, s.x, s.y, s.ticksLeft, (int)s.r); }
		if ( p.verb == V_WALL ) { spawnWallBlocks(owner, s); }
		log("area %s at (%.0f,%.0f) r=%.0f damage=%d ticks=%d", kVerbNames[p.verb], s.x, s.y, s.r, s.damage, s.ticksLeft);
	}

	void emitSelf(Entity* caster, int player, const Picks& p, int rowIdx, float strength, int mana)
	{
		++g_c.selfs;
		const Row* row = rowIdx >= 0 ? &g_rows[rowIdx] : nullptr;
		(void)row;
		const int e = p.element;
		switch ( p.verb )
		{
			case V_WARD:
			{
				int eff = (e == E_FIRE || e == E_LIGHTNING || e == E_ACID) ? EFF_MAGICRESIST : (e == E_ICE || e == E_STONE || e == E_IRON) ? EFF_FORCE_SHIELD : EFF_MAGICREFLECT;
				effect(caster, eff, 500, strength);
				groundSigil(caster, (float)caster->x, (float)caster->y, 120, 28);
				if ( p.shape == S_RING )
				{
					Stamp s; s.ownerUid = caster->getUID(); s.x = (float)caster->x; s.y = (float)caster->y; s.shape = S_RING; s.r = 40.f; s.element = e; s.damage = std::max(1, mana / 2);
					s.ticksLeft = 150; s.period = 15; s.strength = strength; s.row = rowIdx; addStamp(s);
				}
				break;
			}
			case V_VEIL: effect(caster, EFF_INVISIBLE, 400, strength); break;
			case V_MEND:
				spell_changeHealth(caster, std::max(1, (int)std::lround(mana * 3.f * strength)), false, true);
				break;
			case V_SENSE:
			{
				const int radius = (p.shape == S_POINT) ? 6 : (p.shape == S_SPHERE ? 14 : 10);
				if ( player >= 0 ) { spell_magicMap(player, radius, (int)(caster->x / 16), (int)(caster->y / 16)); }
				if ( e == E_LIFE || e == E_SHADOW ) { effect(caster, EFF_DETECT_ENEMY, 300, strength); }
				break;
			}
			case V_LIFT: effect(caster, EFF_LEVITATING, 400, strength); break;
			default: break;
		}
		log("self %s: element %s duration scaled by %.2f", kVerbNames[p.verb], kElementNames[e], strength);
	}

	// vanilla spells that are not four-pick spells: aim with Barony's rangefinder, price with the formula, hand the effect to Barony's own function
	bool emitLegacy(Entity* caster, int player, spell_t* spell, const Aim& aim, bool book, int rowIdx)
	{
		CastSpellProps_t props;
		CastSpellProps_t* pp = nullptr;
		if ( aim.rf == RANGEFINDER_TARGET || aim.rf == RANGEFINDER_TOUCH_FLOOR_TILE || aim.rf == RANGEFINDER_TOUCH_INTERACT_TEST || aim.rf == RANGEFINDER_TOUCH_WALL_TILE )
		{
			props.caster_x = aim.cx; props.caster_y = aim.cy;
			const bool tileIdx = aim.rf == RANGEFINDER_TOUCH_FLOOR_TILE || aim.rf == RANGEFINDER_TOUCH_WALL_TILE;
			props.target_x = tileIdx ? std::floor(aim.tx / 16.f) : aim.tx;   // the rangefinder reports tile indices for these two; castSpell wants them back
			props.target_y = tileIdx ? std::floor(aim.ty / 16.f) : aim.ty;
			props.wallDir = aim.wallDir;
			pp = &props;
		}
		else if ( aim.rf == RANGEFINDER_TOUCH || aim.rf == RANGEFINDER_TOUCH_INTERACT )
		{
			props.targetUID = aim.targetUid; props.wallDir = aim.wallDir; pp = &props;
		}
		if ( !pp ) { pp = &props; }   // castSpell reads the props unconditionally for some spells (bastion roots/mushroom); Barony's own path always passed one for them
		if ( player >= 0 )
		{
			if ( spell->ID == SPELL_MUSHROOM || spell->ID == SPELL_BASTION_MUSHROOM )
			{
				if ( spellInList(&players[player]->magic.spellList, getSpellFromID(SPELL_SPORES)) ) { props.optionalData = 1; }
				if ( spellInList(&players[player]->magic.spellList, getSpellFromID(SPELL_SPORE_BOMB)) ) { props.optionalData = 2; }
			}
			else if ( spell->ID == SPELL_SHRUB || spell->ID == SPELL_BASTION_ROOTS )
			{
				if ( spellInList(&players[player]->magic.spellList, getSpellFromID(SPELL_THORNS)) ) { props.optionalData = 1; }
				if ( spellInList(&players[player]->magic.spellList, getSpellFromID(SPELL_BLADEVINES)) ) { props.optionalData = 2; }
			}
		}
		Entity* r = castSpell(caster->getUID(), spell, false, true, book, pp);
		++g_c.legacyCalls;
		log("legacy effect: castSpell(spell %d) -> %s", spell->ID, r ? "entity" : "no entity");
		(void)player; (void)rowIdx;
		return r != nullptr;
	}

	void release(int player, Cast& c)
	{
		Entity* caster = playerEntity(player);
		if ( !caster ) { return; }
		++g_c.released;
		const Row* row = c.row >= 0 ? &g_rows[c.row] : nullptr;
		// re-aim at release: the world moved during the cast time
		g_outcome = "emit"; g_outStrength = 1.f;
		Aim aim = solveAim(player, caster, c.spell, c.picks);
		if ( c.spell && c.spell->rangefinder != RANGEFINDER_NONE && !aim.haveTarget )
		{
			g_outcome = "notarget";
			++g_c.failedAim;
			say(player, "The spell finds nothing to take hold of.");
			log("release: no target at release (row %d), mana %d lost", row ? row->id : 0, c.mana);
			playSoundEntity(caster, 163, 128);
			return;
		}
		float strength = 1.f;
		g_outStrength = strength;
		if ( c.grade == G_SOFT ) { strength = g_softStrength; g_outStrength = strength; ++g_c.softHalved; log("contradiction SOFT: strength halved (%s)", picksStr(c.picks).c_str()); }
		if ( c.grade == G_HARD )
		{
			if ( c.roll < g_backfire ) { g_outcome = "backfire"; doBackfire(caster, c.picks, c.mana, strength, c.row); return; }
			g_outcome = "fizzle";
			++g_c.fizzled;
			say(player, "The picks contradict one another; the spell comes apart.");
			log("contradiction HARD: roll %.3f >= %.2f, fizzled (%s)", c.roll, g_backfire, picksStr(c.picks).c_str());
			playSoundEntity(caster, 163, 128);
			return;
		}
		if ( c.picks.modifier != M_SILENT ) { playSoundEntity(caster, c.spell ? spellGetCastSound(c.spell) : 170, 128); }
		const bool native = !row || row->deliver == 0;
		if ( !native )
		{
			emitLegacy(caster, player, c.spell, aim, c.book, c.row);
		}
		else
		{
			const Picks& p = c.picks;
			switch ( p.verb )
			{
				case V_BOLT: emitBolt(caster, p, c.row, strength, aim, 0, false); break;
				case V_BEAM:
				{
					Pending pd; pd.delay = 0; pd.ownerUid = caster->getUID(); pd.picks = p; pd.row = c.row; pd.strength = strength; pd.aim = aim; pd.kind = 1; pd.count = 8; pd.every = 1;
					g_pending.push_back(pd);
					break;
				}
				case V_WALL: case V_PIT: case V_BURST: case V_LIFT: case V_PULL: case V_BIND:
					if ( (p.verb == V_LIFT && !aim.haveTarget && p.shape == S_POINT) ) { emitSelf(caster, player, p, c.row, strength, c.mana); }
					else { emitArea(caster, p, c.row, strength, aim, c.mana); }
					break;
				case V_MEND:
					if ( aim.haveTarget || p.shape != S_POINT || p.modifier == M_INVERTED ) { emitArea(caster, p, c.row, strength, aim, c.mana); }
					else { emitSelf(caster, player, p, c.row, strength, c.mana); }
					break;
				case V_WARD: case V_VEIL: case V_SENSE: emitSelf(caster, player, p, c.row, strength, c.mana); break;
				default: break;
			}
		}
		// timer delays the same spell again; echo repeats it
		if ( c.picks.modifier == M_ECHO )
		{
			++g_c.echoes;
			Pending pd; pd.delay = 15; pd.ownerUid = caster->getUID(); pd.picks = c.picks; pd.row = c.row; pd.strength = strength * 0.6f; pd.aim = aim; pd.kind = 2; pd.echoLeft = 1;
			g_pending.push_back(pd);
		}
		if ( c.row >= 0 ) { log("released %s (%s) mana %d grade %d", row->name.c_str(), picksStr(c.picks).c_str(), c.mana, (int)c.grade); }
		else { log("released free picks %s mana %d grade %d", picksStr(c.picks).c_str(), c.mana, (int)c.grade); }
	}

	void pendingFrame()
	{
		for ( size_t i = 0; i < g_pending.size(); )
		{
			Pending& pd = g_pending[i];
			if ( pd.delay > 0 ) { --pd.delay; ++i; continue; }
			Entity* owner = uidToEntity(pd.ownerUid);
			if ( !owner ) { g_pending[i] = g_pending.back(); g_pending.pop_back(); continue; }
			if ( pd.kind == 1 )
			{
				// beam: a stream of fast piercing bodies, one a tick; physical, not hitscan
				Picks bp = pd.picks; bp.verb = V_BOLT; bp.shape = S_LINE;
				Aim a = pd.aim; a.cx = (float)owner->x; a.cy = (float)owner->y; a.yaw = (float)owner->yaw;
				const Row* row = pd.row >= 0 ? &g_rows[pd.row] : nullptr;
				int ctxIdx = -1;
				Ctx& c = newCtx(ctxIdx);
				const int dmg = std::max(1, (int)std::lround(damageFor(owner, row, pd.picks, costMana(g_cost, pd.picks, false), pd.strength) / 4.0));
				c.picks = bp; c.row = pd.row; c.strength = pd.strength; c.owner = owner->getUID(); c.damage = dmg; c.refs = 1;
				proj::ProjDef d = makeDef(bp, pd.row, pd.strength, dmg, ctxIdx, owner);
				d.speed = 9.f; d.lifeTicks = 24; d.pierceMax = 99; d.radius = 2.f;
				if ( !proj::spawn(d, owner, proj::Vec3(a.cx + std::cos(a.yaw) * 10.f, a.cy + std::sin(a.yaw) * 10.f, -1.f), a.yaw) ) { c.free = true; }
				else { ++g_c.boltsNative; }
				if ( --pd.count <= 0 ) { g_pending[i] = g_pending.back(); g_pending.pop_back(); continue; }
				pd.delay = std::max(0, pd.every - 1);
				++i;
			}
			else
			{
				// echo / timer: the same emission again
				const Picks p = pd.picks;
				const Row* row = pd.row >= 0 ? &g_rows[pd.row] : nullptr;
				const bool native = !row || row->deliver == 0;
				Aim a = pd.aim; a.cx = (float)owner->x; a.cy = (float)owner->y;
				const int mana = costMana(g_cost, p, false);
				if ( native )
				{
					switch ( p.verb )
					{
						case V_BOLT: emitBolt(owner, p, pd.row, pd.strength, a, 0, true); break;
						case V_BURST: case V_WALL: case V_PIT: emitArea(owner, p, pd.row, pd.strength, a, mana); break;
						default: break;
					}
				}
				log("echo repeat of %s at %.2f strength", picksStr(p).c_str(), pd.strength);
				g_pending[i] = g_pending.back(); g_pending.pop_back();
			}
		}
	}
} // namespace

// ==================================================================================================== public
bool active() { load(); return !g_old; }

void log(const char* fmt, ...)
{
	char buf[400];
	va_list ap;
	va_start(ap, fmt);
	vsnprintf(buf, sizeof buf, fmt, ap);
	va_end(ap);
	printlog("[apx-magic] %s", buf);
	g_logRing.push_back(std::string("t") + std::to_string((unsigned)::ticks) + " " + buf);
	if ( g_logRing.size() > 400 ) { g_logRing.pop_front(); }
}
std::string castLogTail(int n)
{
	std::string out;
	const int total = (int)g_logRing.size();
	for ( int i = std::max(0, total - n); i < total; ++i ) { out += g_logRing[i]; out += "\n"; }
	return out;
}

const Counters& counters() { return g_c; }
float concentration(int player) { return (player >= 0 && player < MAXPLAYERS) ? g_p[player].conc.pct : 0.f; }
void setConcentration(int player, float pct) { if ( player >= 0 && player < MAXPLAYERS ) { g_p[player].conc.pct = pct; } }
int costOf(const Picks& p, bool nearbyElement) { load(); return costMana(g_cost, p, nearbyElement); }
const CostTable& costTable() { load(); return g_cost; }
int rowCount() { load(); return (int)g_rows.size(); }
bool rowInfo(int index, int* id, std::string* name, Picks* p, int* deliver)
{
	load();
	if ( index < 0 || index >= (int)g_rows.size() ) { return false; }
	const Row& r = g_rows[index];
	if ( id ) { *id = r.id; } if ( name ) { *name = r.name; } if ( p ) { *p = r.picks; } if ( deliver ) { *deliver = r.deliver; }
	return true;
}
bool casting(int player) { return player >= 0 && player < MAXPLAYERS && g_p[player].cast.phase != Phase::Idle; }

void clearLevel()
{
	g_stamps.clear();
	g_pending.clear();
	proj::clearAll();
	for ( auto& c : g_ctx ) { c.free = true; }
	for ( int i = 0; i < MAXPLAYERS; ++i ) { g_p[i].cast = Cast(); }
}

bool beginCast(int player, const SpellRef& ref)
{
	load();
	if ( g_old || player < 0 || player >= MAXPLAYERS ) { return false; }   // false: the caller runs Barony's path (the CONTROL)
	Entity* caster = playerEntity(player);
	Stat* st = stats[player];
	if ( !caster || !st ) { return true; }
	PlayerS& ps = g_p[player];
	if ( ps.cast.phase != Phase::Idle ) { return true; }                    // already casting; the press is spent
	if ( st->getEffectActive(EFF_PARALYZED) || st->getEffectActive(EFF_ASLEEP) ) { return true; }
	if ( (cast_animation[player].active || cast_animation[player].active_spellbook) ) { return true; }

	Cast c;
	c.spell = ref.spell; c.book = ref.book; c.tome = ref.tome;
	int rowIdx = -1;
	if ( ref.hasPicks ) { c.picks = ref.picks; }
	else if ( ref.spell )
	{
		auto it = g_rowById.find(ref.spell->ID);
		if ( it == g_rowById.end() )
		{
			log("spell %d has no row in spells.json: not castable (data gap)", ref.spell->ID);
			say(player, "That spell has no apx row yet.");
			return true;
		}
		rowIdx = it->second;
		c.picks = g_rows[rowIdx].picks;
	}
	else { return true; }
	c.row = rowIdx;
	++g_c.begun;
	c.seq = (int)(++g_castSeq);

	// swimming without levitation or water walking: Barony's rule, kept
	{
		const int tx = std::min<int>(std::max<int>(0, (int)std::floor(caster->x / 16)), map.width - 1), ty = std::min<int>(std::max<int>(0, (int)std::floor(caster->y / 16)), map.height - 1);
		const int tile = map.tiles[ty * MAPLAYERS + tx * MAPLAYERS * map.height];
		if ( (swimmingtiles[tile] || lavatiles[tile]) && !isLevitating(st) && !(st->shoes && st->shoes->type == IRON_BOOTS_WATERWALKING) )
		{
			say(player, "You cannot cast while swimming.");
			log("cast refused: swimming (row %d)", rowIdx >= 0 ? g_rows[rowIdx].id : 0);
			return true;
		}
	}

	const Contradiction ct = grade(c.picks);
	c.grade = ct.grade;
	c.roll = rollFor(g_castSeq, player, c.picks);
	c.nearby = elementNearby(caster, c.picks.element);
	c.mana = costMana(g_cost, c.picks, c.nearby);
	if ( ref.tome && c.mana > 1 ) { c.mana = std::max(1, c.mana / 2); }   // a tome casts at half cost, as before
	c.concAtStart = ps.conc.pct;

	// aim: a spell that needs a target must have one now (no mana is taken for a cast that cannot begin)
	c.aim = solveAim(player, caster, c.spell, c.picks);
	if ( c.spell && c.spell->rangefinder != RANGEFINDER_NONE && !c.aim.haveTarget )
	{
		++g_c.failedAim;
		say(player, "There is nothing there for the spell to take hold of.");
		log("cast refused: no target (row %d rangefinder %d)", rowIdx >= 0 ? g_rows[rowIdx].id : 0, c.aim.rf);
		for ( node_t* nn = map.creatures->first; nn; nn = nn->next )
		{
			Entity* e = (Entity*)nn->element;
			if ( !e || e == caster ) { continue; }
			const double dd = std::hypot(e->x - caster->x, e->y - caster->y);
			if ( dd < 100.0 ) { log("  near uid %u type %d dist %.1f enemy %d targetable %d state %d yawdiff %.2f", e->getUID(), e->getStats() ? (int)e->getStats()->type : -1, dd, (int)e->checkEnemy(caster), (int)e->monsterIsTargetable(true), (int)e->monsterState, std::atan2(e->y - caster->y, e->x - caster->x) - caster->yaw); }
		}
		return true;
	}
	if ( st->MP < c.mana && !(st->type == VAMPIRE) )
	{
		++g_c.failedMana;
		say(player, "Not enough mana.");
		if ( players[player]->isLocalPlayer() ) { playSound(563, 64); if ( players[player]->magic.noManaProcessedOnTick == 0 ) { players[player]->magic.flashNoMana(); } }
		log("cast refused: mana %d < cost %d (%s)", st->MP, c.mana, picksStr(c.picks).c_str());
		return true;
	}
	if ( !ps.conc.canCast(g_conc) )
	{
		++g_c.failedConc;
		say(player, "You cannot concentrate.");
		if ( players[player]->isLocalPlayer() ) { playSound(563, 64); }
		log("cast refused: concentration %.2f%% under %.0f%% (%s)", ps.conc.pct, g_conc.failBelow, picksStr(c.picks).c_str());
		return true;
	}

	// charge, and begin
	const int mpBefore = st->MP;
	caster->drainMP(c.mana, false);
	g_c.manaSpent += (double)std::max(0, mpBefore - st->MP);
	if ( c.spell && player >= 0 )
	{
		const bool sustained = spell_isChanneled(c.spell);
		if ( sustained ) { players[player]->mechanics.sustainedSpellIncrementMP(mpBefore - st->MP, c.spell->skillID); }
		else { players[player]->mechanics.baseSpellIncrementMP(mpBefore - st->MP, c.spell->skillID); }
	}
	const float ms = castMs(g_time, g_conc, c.picks, ps.conc.pct);
	ps.conc.spend(g_conc, c.picks);
	c.ticksTotal = c.ticksLeft = msToTicks(ms);
	c.phase = Phase::Casting;
	c.startTick = ::ticks;
	// telegraph: a ground sigil at the aim point for area verbs and shapes; the caster's own cell for self verbs
	{
		const bool selfVerb = c.picks.verb == V_WARD || c.picks.verb == V_VEIL || c.picks.verb == V_SENSE || c.picks.verb == V_MEND;
		if ( c.picks.verb != V_BOLT && c.picks.verb != V_BEAM )
		{
			const float sx = selfVerb ? (float)caster->x : c.aim.tx, sy = selfVerb ? (float)caster->y : c.aim.ty;
			groundSigil(selfVerb ? caster : nullptr, sx, sy, c.ticksTotal + 8, (int)(selfVerb ? 28.f : shapeRadius(c.picks.shape)));
		}
	}
	ps.cast = c;
	spellcastAnimationUpdate(player, MONSTER_POSE_MAGIC_WINDUP1, c.ticksTotal);
	log("BEGIN #%d player %d %s row %d (%s) picks=%s mana=%d [base %.1f x area %.2f x affinity %.2f x factor %.2f%s] contradiction=%s%s%s cast=%.0f ms/%d ticks conc %.1f%%->%.1f%%",
		c.seq, player, c.spell ? c.spell->spell_internal_name : "(free picks)", rowIdx >= 0 ? g_rows[rowIdx].id : 0, rowIdx >= 0 ? g_rows[rowIdx].name.c_str() : "-",
		picksStr(c.picks).c_str(), c.mana, g_cost.base[c.picks.verb], g_cost.area[c.picks.shape], g_cost.affinity[c.picks.element], g_cost.factor[c.picks.modifier],
		c.nearby ? " x nearby 0.5" : "", ct.grade == G_NONE ? "none" : (ct.grade == G_SOFT ? "SOFT" : "HARD"), ct.grade != G_NONE ? " - " : "", ct.grade != G_NONE ? ct.why : "",
		ms, c.ticksTotal, c.concAtStart, ps.conc.pct);
	{
		char b[900];
		snprintf(b, sizeof b, "{\"ev\":\"begin\",\"seq\":%d,\"tick\":%u,\"player\":%d,\"id\":%d,\"name\":\"%s\",\"picks\":\"%s\",\"deliver\":\"%s\",\"mana\":%d,\"mp_before\":%d,\"mp_after\":%d,\"nearby\":%s,\"grade\":%d,\"roll\":%.4f,\"cast_ms\":%.1f,\"ticks\":%d,\"conc_before\":%.2f,\"conc_after\":%.2f}",
			c.seq, (unsigned)::ticks, player, rowIdx >= 0 ? g_rows[rowIdx].id : 0, rowIdx >= 0 ? g_rows[rowIdx].name.c_str() : "free", picksStr(c.picks).c_str(),
			(rowIdx < 0 || g_rows[rowIdx].deliver == 0) ? "proj" : "legacy", c.mana, mpBefore, st->MP, c.nearby ? "true" : "false", (int)c.grade, c.roll, ms, c.ticksTotal, c.concAtStart, ps.conc.pct);
		jsonl(b);
	}
	return true;
}

void tick(int player)
{
	if ( g_old || player < 0 || player >= MAXPLAYERS ) { return; }
	PlayerS& ps = g_p[player];
	static Uint32 lastTick[MAXPLAYERS] = {};
	if ( lastTick[player] == ::ticks ) { return; }
	lastTick[player] = ::ticks;
	ps.conc.regen(g_conc, 1.f / (float)TICKS_PER_SECOND);
	Cast& c = ps.cast;
	if ( c.phase == Phase::Idle ) { return; }
	Entity* caster = playerEntity(player);
	Stat* st = stats[player];
	if ( !caster || !st || st->HP <= 0 || st->getEffectActive(EFF_PARALYZED) || st->getEffectActive(EFF_ASLEEP) )
	{
		++g_c.interrupted;
		log("cast #%d interrupted (paralysed, asleep or gone); mana %d lost", c.seq, c.mana);
		jsonl(std::string("{\"ev\":\"interrupted\",\"seq\":") + std::to_string(c.seq) + "}");
		c = Cast();
		return;
	}
	if ( c.phase == Phase::Casting )
	{
		if ( --c.ticksLeft <= 0 )
		{
			release(player, c);
			char b[200];
			snprintf(b, sizeof b, "{\"ev\":\"release\",\"seq\":%d,\"tick\":%u,\"cast_ticks\":%d,\"outcome\":\"%s\",\"strength\":%.2f,\"grade\":%d,\"roll\":%.4f}", c.seq, (unsigned)::ticks, (int)(::ticks - c.startTick), g_outcome.c_str(), g_outStrength, (int)c.grade, c.roll);
			jsonl(b);
			spellcastAnimationUpdate(player, 1, 0);
			c.phase = Phase::Release;
			c.releaseLeft = 8;
		}
	}
	else if ( c.phase == Phase::Release )
	{
		if ( --c.releaseLeft <= 0 ) { c = Cast(); }
	}
}

void frame()
{
	if ( g_old ) { return; }
	static int lastLevel = -1;
	static Uint32 lastSeed = 0;
	if ( ::currentlevel != lastLevel || ::mapseed != lastSeed ) { lastLevel = ::currentlevel; lastSeed = ::mapseed; clearLevel(); }
	const double t0 = nowUs();
	proj::frame();
	pendingFrame();
	stampsFrame();
	if ( !g_stamps.empty() || !g_pending.empty() || proj::live() > 0 )
	{
		if ( g_frameCost.size() < 20000 ) { g_frameCost.push_back((float)(nowUs() - t0)); }
	}
}

bool poseHand(Entity* hand, bool left, int player)
{
	if ( g_old || player < 0 || player >= MAXPLAYERS ) { return false; }
	const Cast& c = g_p[player].cast;
	if ( c.phase == Phase::Idle ) { return false; }
	hand->flags[INVISIBLE] = false;
	hand->flags[INVISIBLE_DITHER] = false;
	const float side = left ? -1.f : 1.f;
	if ( c.phase == Phase::Casting )
	{
		const float t = c.ticksTotal > 0 ? 1.f - (float)c.ticksLeft / (float)c.ticksTotal : 1.f;   // 0..1
		const float k = std::min(1.f, t * 3.f);                                                       // hands rise in the first third
		hand->x += 1.2f * k + 0.12f * std::sin((float)::ticks * 1.3f + (left ? 0.f : 1.7f));
		hand->y += -side * 1.6f * k + 0.1f * std::cos((float)::ticks * 1.1f);
		hand->z -= 2.0f * k;
		hand->pitch += -0.5f * k;
		hand->roll += side * 0.35f * k;
		if ( ::ticks % 5 == 0 )
		{
			if ( Entity* fx = spawnGib(hand, 16) )
			{
				fx->flags[INVISIBLE] = false; fx->flags[SPRITE] = true; fx->flags[NOUPDATE] = true; fx->flags[UPDATENEEDED] = false; fx->flags[OVERDRAW] = true;
				fx->lightBonus = vec4(0.2f, 0.2f, 0.2f, 0.f);
				fx->scalex = 0.25f; fx->scaley = 0.25f; fx->scalez = 0.25f;
				fx->sprite = 16; fx->z += 3.0;
				fx->yaw = ((local_rng.rand() % 6) * 60) * PI / 180.0; fx->pitch = (local_rng.rand() % 360) * PI / 180.0; fx->roll = (local_rng.rand() % 360) * PI / 180.0;
				fx->vel_x = cos(fx->yaw) * .1; fx->vel_y = sin(fx->yaw) * .1; fx->vel_z = -.15; fx->fskill[3] = 0.01; fx->skill[11] = player;
			}
		}
	}
	else
	{
		const float r = (float)c.releaseLeft / 8.f;                 // 1 -> 0: the throw eases back
		hand->x += 3.0f * r;
		hand->y += -side * 0.4f * r;
		hand->z -= 1.0f * r;
		hand->pitch += -0.2f * r;
	}
	return true;
}

// ==================================================================================================== console and gate instruments
namespace
{
	int find(const char* const* names, int n, const char* s) { for ( int i = 0; i < n; ++i ) { if ( !std::strcmp(names[i], s) ) { return i; } } return -1; }

	Entity* gateTarget = nullptr;
	Uint32 gateTargetUid = 0;
	struct Snap
	{
		int mp = 0, hp = 0, ents = 0, tHp = 0, row = 0;
		float x = 0, y = 0;
		std::vector<int> cEff, tEff;
		Counters c;
		proj::Counters pc;
	};
	Snap g_snap;
	std::string g_snapTag;

	std::vector<int> effects(Stat* s) { std::vector<int> v; if ( s ) { for ( int i = 0; i < NUMEFFECTS; ++i ) { v.push_back(s->getEffectActive(i) ? 1 : 0); } } return v; }
	int entCount() { int n = 0; for ( node_t* x = map.entities->first; x; x = x->next ) { ++n; } return n; }
	Snap snap(Entity* tgt)
	{
		Snap s;
		Entity* me = playerEntity(0);
		s.mp = stats[0] ? stats[0]->MP : 0; s.hp = stats[0] ? stats[0]->HP : 0; s.ents = entCount();
		s.x = me ? (float)me->x : 0; s.y = me ? (float)me->y : 0;
		s.cEff = effects(stats[0]);
		if ( tgt && tgt->getStats() ) { s.tHp = tgt->getStats()->HP; s.tEff = effects(tgt->getStats()); }
		s.c = g_c; s.pc = proj::counters();
		return s;
	}
} // namespace

static ConsoleCommand ccmd_apxmagic_select("/apxmagic_select", "apx: select a vanilla spell by id for player 0 (gate set-up)", [](int argc, const char** argv) {
	if ( argc < 2 || !players[0] ) { return; }
	spell_t* sp = getSpellFromID(atoi(argv[1]));
	if ( !sp ) { printlog("[apx-magic] select: no spell %s", argv[1]); return; }
	players[0]->magic.equipSpell(sp);
	printlog("[apx-magic] selected spell %d (%s)", sp->ID, sp->spell_internal_name);
});
static ConsoleCommand ccmd_apxmagic_step("/apxmagic_step", "apx: gate set-up: move player 0 <units> along its yaw if the way is clear", [](int argc, const char** argv) {
	Entity* me = playerEntity(0);
	if ( !me || argc < 2 ) { return; }
	const double u = atof(argv[1]);
	const real_t d = lineTrace(me, me->x, me->y, me->yaw, std::fabs(u) + 8, LINETRACE_IGNORE_ENTITIES, false);
	if ( d >= std::fabs(u) + 8 ) { me->x += std::cos(me->yaw) * u; me->y += std::sin(me->yaw) * u; }
});
static ConsoleCommand ccmd_apxmagic_audit("/apxmagic_audit", "apx: print every engine spell id and internal name (row mapping audit)", [](int, const char**) {
	for ( int i = 0; i < 400; ++i )
	{
		if ( spell_t* sp = getSpellFromID(i) ) { printlog("[apx-magic] audit id=%d engine_id=%d name=%s rf=%d", i, sp->ID, sp->spell_internal_name, (int)sp->rangefinder); }
	}
});
static ConsoleCommand ccmd_apxmagic_pick("/apxmagic_pick", "apx: cast free picks: <verb> <shape> <element> <modifier> (names; the cast goes through the real pipeline)", [](int argc, const char** argv) {
	if ( argc < 5 ) { printlog("[apx-magic] usage: /apxmagic_pick bolt point fire none"); return; }
	SpellRef r; r.hasPicks = true;
	r.picks.verb = find(kVerbNames, V_COUNT, argv[1]); r.picks.shape = find(kShapeNames, S_COUNT, argv[2]);
	r.picks.element = find(kElementNames, E_COUNT, argv[3]); r.picks.modifier = find(kModifierNames, M_COUNT, argv[4]);
	if ( !r.picks.valid() ) { printlog("[apx-magic] bad picks"); return; }
	g_snapTag = "pick"; g_snap = snap(gateTarget && uidToEntity(gateTargetUid) == gateTarget ? gateTarget : nullptr);
	beginCast(0, r);
});
static ConsoleCommand ccmd_apxmagic_conc("/apxmagic_conc", "apx: set player 0's concentration percent (gate set-up)", [](int argc, const char** argv) {
	if ( argc >= 2 ) { setConcentration(0, (float)atof(argv[1])); printlog("[apx-magic] concentration set to %.1f", concentration(0)); }
});
static ConsoleCommand ccmd_apxmagic_roll("/apxmagic_roll", "apx: fix the contradiction backfire roll (0..1; negative = the deterministic hash)", [](int argc, const char** argv) {
	if ( argc >= 2 ) { g_rollOverride = (float)atof(argv[1]); printlog("[apx-magic] backfire roll override %.3f", g_rollOverride); }
});
static ConsoleCommand ccmd_apxmagic_prep("/apxmagic_prep", "apx: gate set-up: full HP/MP/concentration, face the most open way, put a sturdy dummy 4 tiles ahead: [notarget]", [](int argc, const char** argv) {
	Entity* me = playerEntity(0);
	if ( !me || !stats[0] ) { return; }
	stats[0]->HP = stats[0]->MAXHP = std::max(stats[0]->MAXHP, 400); stats[0]->MP = stats[0]->MAXMP = std::max(stats[0]->MAXMP, 400);
	if ( me->effectShapeshift != NOTHING || me->effectPolymorph != NOTHING )
	{
		castSpell(me->getUID(), getSpellFromID(SPELL_REVERT_FORM), false, true, false, nullptr);   // the proper way back from a form
	}
	me->flags[BURNING] = false;
	apx::combat_glue::downed(0) = apx::combat::DownedState();   // a spell that took HP to 0 left the downed clock running
	for ( int i = 0; i < NUMEFFECTS; ++i ) { if ( i != EFF_SHAPESHIFT && i != EFF_POLYMORPH && stats[0]->getEffectActive(i) ) { me->setEffect(i, false, 0, true); } }
	stats[0]->HP = stats[0]->MAXHP = std::max(stats[0]->MAXHP, 400); stats[0]->MP = stats[0]->MAXMP = std::max(stats[0]->MAXMP, 400);   // again: reverting a form restores the old pools
	setConcentration(0, 100.f);
	g_p[0].cast = Cast();
	stats[0]->HP = std::max(stats[0]->HP, 1);
	g_stamps.clear(); g_pending.clear();
	// kill every other creature so the dummy is the only target
	for ( node_t* n = map.creatures->first; n; n = n->next )
	{
		Entity* e = (Entity*)n->element;
		if ( e && e != me && e->behavior == &actMonster && e->getStats() ) { e->getStats()->HP = 0; e->setHP(0); }
	}
	// every prep starts from the same spot (the first one's): teleports and pushes of earlier casts must not change the next one's geometry
	{
		static bool anchored = false;
		static double ax = 0, ay = 0;
		if ( !anchored ) { anchored = true; ax = me->x; ay = me->y; }
		me->x = ax; me->y = ay; me->vel_x = 0; me->vel_y = 0;
	}
	// the most open heading: 16 rays
	double bestYaw = me->yaw, bestD = -1;
	for ( int i = 0; i < 16; ++i )
	{
		const double y = i * PI / 8;
		const real_t d = lineTrace(me, me->x, me->y, y, 160, LINETRACE_IGNORE_ENTITIES, false);
		if ( d > bestD ) { bestD = d; bestYaw = y; }
	}
	me->yaw = bestYaw;
	if ( players[0] ) { cameras[0].ang = bestYaw; }
	gateTarget = nullptr; gateTargetUid = 0;
	if ( argc < 2 || std::strcmp(argv[1], "notarget") )
	{
		const double dist = std::min(28.0, std::max(20.0, bestD - 16.0));   // inside touch reach
		Entity* t = summonMonster(DUMMYBOT, (long)(me->x + std::cos(bestYaw) * dist), (long)(me->y + std::sin(bestYaw) * dist), true);
		if ( t && t->getStats() )
		{
			t->getStats()->HP = t->getStats()->MAXHP = 800;
			t->monsterState = MONSTER_STATE_WAIT;
			gateTarget = t; gateTargetUid = t->getUID();
		}
	}
	printlog("[apx-magic] prep pools: hp %d/%d mp %d/%d type %d shape %d poly %d", stats[0]->HP, stats[0]->MAXHP, stats[0]->MP, stats[0]->MAXMP, (int)stats[0]->type, (int)me->effectShapeshift, (int)me->effectPolymorph);
	g_snapTag = "prep";
	g_snap = snap(gateTarget);
	printlog("[apx-magic] prep: player 0 at (%.0f,%.0f) yaw %.2f open %.0f units, dummy %s", me->x, me->y, bestYaw, bestD, gateTarget ? "placed" : "none");
});
static ConsoleCommand ccmd_apxmagic_report("/apxmagic_report", "apx: gate: write one JSONL row comparing the world with the last prep: <tag>", [](int argc, const char** argv) {
	Entity* me = playerEntity(0);
	if ( !me || !stats[0] ) { return; }
	Entity* t = (gateTarget && uidToEntity(gateTargetUid) == gateTarget) ? gateTarget : nullptr;
	const Snap now = snap(t);
	int cDiff = 0, tDiff = 0;
	for ( size_t i = 0; i < now.cEff.size() && i < g_snap.cEff.size(); ++i ) { cDiff += now.cEff[i] != g_snap.cEff[i]; }
	for ( size_t i = 0; i < now.tEff.size() && i < g_snap.tEff.size(); ++i ) { tDiff += now.tEff[i] != g_snap.tEff[i]; }
	const double moved = std::hypot(now.x - g_snap.x, now.y - g_snap.y);
	const int tHpDelta = t ? now.tHp - g_snap.tHp : 0;
	const long long hits = (now.pc.hits - g_snap.pc.hits), stampHits = now.c.stampHits - g_snap.c.stampHits, legacy = now.c.legacyCalls - g_snap.c.legacyCalls;
	const bool strong = tHpDelta < 0 || tDiff > 0 || cDiff > 0 || now.hp > g_snap.hp || now.hp < g_snap.hp || moved > 2.0 || hits > 0 || stampHits > 0
		|| (now.c.selfs - g_snap.c.selfs) > 0 || (now.pc.spawned - g_snap.pc.spawned) > 0 || (now.c.areas - g_snap.c.areas) > 0;
	const bool weak = now.ents - g_snap.ents > 0 || legacy > 0;
	char b[1200];
	snprintf(b, sizeof b, "{\"ev\":\"report\",\"tag\":\"%s\",\"mp_before\":%d,\"mp_after\":%d,\"mp_delta\":%d,\"hp_delta\":%d,\"target_hp_delta\":%d,\"target_effects_changed\":%d,\"caster_effects_changed\":%d,"
		"\"moved\":%.2f,\"entities_delta\":%d,\"proj_spawned\":%lld,\"proj_hits\":%lld,\"stamps\":%lld,\"stamp_hits\":%lld,\"selfs\":%lld,\"legacy_calls\":%lld,\"observed_strong\":%s,\"observed_weak\":%s,\"conc\":%.2f}",
		argc >= 2 ? argv[1] : "", g_snap.mp, now.mp, g_snap.mp - now.mp, now.hp - g_snap.hp, tHpDelta, tDiff, cDiff, moved, now.ents - g_snap.ents,
		now.pc.spawned - g_snap.pc.spawned, hits, now.c.areas - g_snap.c.areas, stampHits, now.c.selfs - g_snap.c.selfs, legacy, strong ? "true" : "false", weak ? "true" : "false", concentration(0));
	jsonl(b);
	printlog("[apx-magic] report %s", b);
});
static ConsoleCommand ccmd_apxmagic_stats("/apxmagic_stats", "apx: print apx::magic and apx::proj counters and the per-tick cost p50/p99", [](int, const char**) {
	double p50, p99; int n, live;
	proj::perfDump(&p50, &p99, &n, &live);
	std::vector<float> v = g_frameCost; std::sort(v.begin(), v.end());
	const double f50 = v.empty() ? 0 : v[v.size() / 2], f99 = v.empty() ? 0 : v[std::min(v.size() - 1, (size_t)(v.size() * 0.99))];
	const proj::Counters& pc = proj::counters();
	printlog("[apx-magic] stats begun=%lld released=%lld failedMana=%lld failedConc=%lld failedAim=%lld interrupted=%lld fizzled=%lld backfired=%lld softHalved=%lld bolts=%lld areas=%lld selfs=%lld legacy=%lld stampHits=%lld echoes=%lld chains=%lld triggers=%lld mana=%.0f",
		g_c.begun, g_c.released, g_c.failedMana, g_c.failedConc, g_c.failedAim, g_c.interrupted, g_c.fizzled, g_c.backfired, g_c.softHalved, g_c.boltsNative, g_c.areas, g_c.selfs,
		g_c.legacyCalls, g_c.stampHits, g_c.echoes, g_c.chains, g_c.triggers, g_c.manaSpent);
	printlog("[apx-proj] spawned=%lld hits=%lld stops=%lld pierces=%lld fracturePierces=%lld ricochets=%lld wallStops=%lld expired=%lld stubHits=%lld coreHits=%lld substeps=%lld combatCore=%d",
		pc.spawned, pc.hits, pc.stops, pc.pierces, pc.fracturePierces, pc.ricochets, pc.wallStops, pc.expired, pc.stubHits, pc.coreHits, pc.substeps, proj::usingCombatCore() ? 1 : 0);
	printlog("[apx-magic] perf proj_tick p50=%.2f us p99=%.2f us samples=%d maxLive=%d | magic_frame p50=%.2f us p99=%.2f us samples=%d", p50, p99, n, live, f50, f99, (int)v.size());
	char b[800];
	snprintf(b, sizeof b, "{\"ev\":\"stats\",\"proj_p50_us\":%.3f,\"proj_p99_us\":%.3f,\"proj_samples\":%d,\"max_live\":%d,\"frame_p50_us\":%.3f,\"frame_p99_us\":%.3f,\"frame_samples\":%d,"
		"\"spawned\":%lld,\"hits\":%lld,\"stops\":%lld,\"pierces\":%lld,\"fracture_pierces\":%lld,\"ricochets\":%lld,\"wall_stops\":%lld,\"stub_hits\":%lld,\"core_hits\":%lld}",
		p50, p99, n, live, f50, f99, (int)v.size(), pc.spawned, pc.hits, pc.stops, pc.pierces, pc.fracturePierces, pc.ricochets, pc.wallStops, pc.stubHits, pc.coreHits);
	jsonl(b);
});
static ConsoleCommand ccmd_apxmagic_stress("/apxmagic_stress", "apx: gate: keep N projectiles in flight for the cost measurement: <n> <ticks>", [](int argc, const char** argv) {
	Entity* me = playerEntity(0);
	if ( !me ) { return; }
	const int n = argc >= 2 ? atoi(argv[1]) : 20, life = argc >= 3 ? atoi(argv[2]) : 100;
	proj::ProjDef d = g_projBase; d.sprite = 168; d.light = "orb_red"; d.lifeTicks = life; d.speed = 0.25f; d.damage = 0; d.ricochet = false; d.element = E_FIRE;
	for ( int i = 0; i < n; ++i )
	{
		const float yaw = (float)(i * 2 * PI / n);
		proj::spawn(d, me, proj::Vec3((float)me->x + std::cos(yaw) * 24.f, (float)me->y + std::sin(yaw) * 24.f, -1.f), yaw);
	}
	printlog("[apx-magic] stress: %d slow projectiles for %d ticks", n, life);
});
static ConsoleCommand ccmd_apxmagic_shoot("/apxmagic_shoot", "apx: gate: fire one projectile along the player's yaw plus an offset in degrees: <speed> <momentum> <area> <offsetDeg> [pierceMax]", [](int argc, const char** argv) {
	Entity* me = playerEntity(0);
	if ( !me ) { return; }
	proj::ProjDef d = g_projBase; d.sprite = 168; d.light = "orb_red"; d.element = E_FIRE; d.damage = 5; d.lifeTicks = 80;
	d.speed = argc >= 2 ? (float)atof(argv[1]) : 4.f; d.momentum = argc >= 3 ? (float)atof(argv[2]) : 6.f; d.area = argc >= 4 ? (float)atof(argv[3]) : 4.f;
	d.pierceMax = argc >= 6 ? atoi(argv[5]) : 0;
	const float yaw = (float)me->yaw + (argc >= 5 ? (float)atof(argv[4]) : 0.f) * 0.017453292f;
	proj::spawn(d, me, proj::Vec3((float)me->x + std::cos(yaw) * 12.f, (float)me->y + std::sin(yaw) * 12.f, -1.f), yaw);
});
static ConsoleCommand ccmd_apxmagic_log("/apxmagic_log", "apx: print the last cast-log lines to the game log", [](int argc, const char** argv) {
	const std::string t = castLogTail(argc >= 2 ? atoi(argv[1]) : 20);
	printlog("[apx-magic] cast log (tail):\n%s", t.c_str());
});
static ConsoleCommand ccmd_apxmagic_dummy("/apxmagic_dummy", "apx: gate: a sturdy waiting creature <rat|skeleton|kobold|goblin> <tiles ahead> [sideways units]", [](int argc, const char** argv) {
	Entity* me = playerEntity(0);
	if ( !me || argc < 3 ) { return; }
	Monster m = RAT;
	if ( !std::strcmp(argv[1], "skeleton") ) { m = SKELETON; } else if ( !std::strcmp(argv[1], "kobold") ) { m = KOBOLD; } else if ( !std::strcmp(argv[1], "goblin") ) { m = GOBLIN; }
	const double d = atof(argv[2]) * 16.0, side = argc >= 4 ? atof(argv[3]) : 0.0;
	Entity* t = summonMonster(m, (long)(me->x + std::cos(me->yaw) * d - std::sin(me->yaw) * side), (long)(me->y + std::sin(me->yaw) * d + std::cos(me->yaw) * side), true);
	if ( t && t->getStats() ) { t->getStats()->HP = t->getStats()->MAXHP = 800; t->monsterState = MONSTER_STATE_WAIT; if ( !gateTarget ) { gateTarget = t; gateTargetUid = t->getUID(); } }
	printlog("[apx-magic] dummy %s at %.0f tiles: %s", argv[1], d / 16.0, t ? "placed" : "failed");
});
static ConsoleCommand ccmd_apxmagic_wallshot("/apxmagic_wallshot", "apx: gate: fire a projectile at the nearest wall at <deg> to its surface from an open tile beside it", [](int argc, const char** argv) {
	Entity* me = playerEntity(0);
	if ( !me ) { return; }
	const double deg = argc >= 2 ? atof(argv[1]) : 10.0;
	const int sx = (int)(me->x / 16), sy = (int)(me->y / 16);
	auto solid = [](int tx, int ty) { return tx < 0 || ty < 0 || tx >= (int)map.width || ty >= (int)map.height || map.tiles[OBSTACLELAYER + ty * MAPLAYERS + tx * MAPLAYERS * map.height] != 0; };
	auto open = [&](int tx, int ty) { return !solid(tx, ty) && map.tiles[ty * MAPLAYERS + tx * MAPLAYERS * map.height] && !swimmingtiles[map.tiles[ty * MAPLAYERS + tx * MAPLAYERS * map.height]]; };
	for ( int r = 0; r < 30; ++r )
	{
		for ( int dy = -r; dy <= r; ++dy ) for ( int dx = -r; dx <= r; ++dx )
		{
			if ( std::max(std::abs(dx), std::abs(dy)) != r ) { continue; }
			const int tx = sx + dx, ty = sy + dy;
			if ( !open(tx, ty) ) { continue; }
			// a wall to the east (normal along x), with open floor north and south to run along
			const int wx[4] = { 1, -1, 0, 0 }, wy[4] = { 0, 0, 1, -1 };
			for ( int k = 0; k < 4; ++k )
			{
				if ( !solid(tx + wx[k], ty + wy[k]) ) { continue; }
				const int px = -wy[k] != 0 ? 0 : 0; (void)px;
				// along-wall axis is perpendicular to the normal; need two open tiles of run
				const int ax = wy[k] != 0 ? 1 : 0, ay = wx[k] != 0 ? 1 : 0;
				if ( !open(tx - ax, ty - ay) || !open(tx - 2 * ax, ty - 2 * ay) ) { continue; }
				// start at the far end of the run, 11 units off the wall, fly along the run tilted toward the wall by deg
				const double nx = wx[k], ny = wy[k], tax = ax, tay = ay;
				const double stx = tx * 16 + 8 - tax * 24 + nx * 3.0, sty = ty * 16 + 8 - tay * 24 + ny * 3.0;
				const double ang = std::atan2(tay * 1.0 + ny * std::tan(deg * PI / 180.0), tax * 1.0 + nx * std::tan(deg * PI / 180.0));
				proj::ProjDef d = g_projBase; d.sprite = 168; d.light = "orb_red"; d.element = E_FIRE; d.damage = 1; d.lifeTicks = 60; d.speed = 4.f;
				const long long r0 = proj::counters().ricochets, w0 = proj::counters().wallStops;
				(void)r0; (void)w0;
				proj::spawn(d, nullptr, proj::Vec3((float)stx, (float)sty, -1.f), (float)ang);
				printlog("[apx-magic] wallshot deg=%.1f from (%.0f,%.0f) heading %.2f rad, wall normal (%d,%d)", deg, stx, sty, ang, wx[k], wy[k]);
				return;
			}
		}
	}
	printlog("[apx-magic] wallshot: no wall with a run beside it within 30 tiles");
});
static ConsoleCommand ccmd_apxmagic_target("/apxmagic_target", "apx: gate: put a sturdy dummy <tiles> ahead of player 0 along its yaw", [](int argc, const char** argv) {
	Entity* me = playerEntity(0);
	if ( !me ) { return; }
	const double d = (argc >= 2 ? atof(argv[1]) : 4.0) * 16.0;
	Entity* t = summonMonster(RAT, (long)(me->x + std::cos(me->yaw) * d), (long)(me->y + std::sin(me->yaw) * d), true);
	if ( t && t->getStats() ) { t->getStats()->HP = t->getStats()->MAXHP = 800; t->monsterState = MONSTER_STATE_WAIT; gateTarget = t; gateTargetUid = t->getUID(); }
});

} // namespace magic
} // namespace apx
