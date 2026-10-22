// apx/hostility.cpp -- see hostility.hpp. Table: data/apx/combat/hostility.json (src/apx/hostility.hpp for the rules).
#include "../main.hpp"
#include "../game.hpp"
#include "../entity.hpp"
#include "../stat.hpp"
#include "../monster.hpp"
#include "../player.hpp"
#include "../items.hpp"
#include "hostility.hpp"
#include "datadir.hpp" // the one data-folder rule
#include "rapidjson/document.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <set>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>
#include "hollow_enums.inc" // kApxMonsterNames (this unit only)

namespace apx
{
namespace hostility
{
namespace
{
	constexpr int kTps = 50;
	struct Row { std::string cls, reason; };
	struct Data
	{
		bool loaded = false, ok = false;
		int provocationTicks = 30 * kTps, calmTicks = 10 * kTps, shopRadius = 5;
		std::unordered_map<int, Row> pairs; // key a*4096+b, a<=b
		std::unordered_map<int, std::pair<const char*, const char*>> names;
	};
	Data& D() { static Data d; return d; }
	int keyOf(int a, int b) { if ( a > b ) std::swap(a, b); return a * 4096 + b; }
	int typeByName(const char* n)
	{
		for ( const auto& kv : kApxMonsterNames ) if ( !std::strcmp(kv.first, n) && kv.second > 0 && kv.second < NUMMONSTERS ) return kv.second;
		return -1;
	}
	const char* tname(int t) { return (t >= 0 && t < NUMMONSTERS) ? monstertypename[t] : "?"; }
	std::string readFile(const std::string& p)
	{
		std::string s;
		if ( FILE* f = std::fopen(p.c_str(), "rb") ) { char b[4096]; size_t n; while ( (n = std::fread(b, 1, sizeof b, f)) > 0 ) s.append(b, n); std::fclose(f); }
		return s;
	}
	void load()
	{
		Data& d = D();
		if ( d.loaded ) return;
		d.loaded = true;
		std::vector<std::string> cands;
		if ( const char* e = std::getenv("APX_PARTS_DATA") ) cands.push_back(std::string(e) + "/combat/hostility.json");
		cands.push_back(apx::dataDir() + "/combat/hostility.json"); // exe folder first, then the dev source (datadir.hpp)
		std::string text, used;
		for ( auto& c : cands ) { text = readFile(c); if ( !text.empty() ) { used = c; break; } }
		rapidjson::Document doc;
		if ( text.empty() || doc.Parse(text.c_str()).HasParseError() || !doc.IsObject() )
		{
			printlog("[apx-hostility] ERROR: data/apx/combat/hostility.json missing or unparsable (tried %s); every pair is NEUTRAL until fixed", cands.empty() ? "-" : cands[0].c_str());
			return;
		}
		if ( doc.HasMember("provocation_seconds") ) d.provocationTicks = doc["provocation_seconds"].GetInt() * kTps;
		if ( doc.HasMember("calm_seconds") ) d.calmTicks = doc["calm_seconds"].GetInt() * kTps;
		if ( doc.HasMember("shop_radius_tiles") ) d.shopRadius = doc["shop_radius_tiles"].GetInt();
		int bad = 0;
		if ( doc.HasMember("pairs") && doc["pairs"].IsArray() )
			for ( auto& r : doc["pairs"].GetArray() )
			{
				const int a = typeByName(r["a"].GetString()), b = typeByName(r["b"].GetString());
				if ( a < 0 || b < 0 ) { ++bad; printlog("[apx-hostility] ERROR: unknown kind in pair %s-%s", r["a"].GetString(), r["b"].GetString()); continue; }
				d.pairs[keyOf(a, b)] = Row{ r["class"].GetString(), r["reason"].GetString() };
			}
		if ( doc.HasMember("monster_vs_nonmonster") )
		{
			auto& m = doc["monster_vs_nonmonster"];
			std::set<int> exempt;
			for ( auto& n : m["exempt_monsters"].GetArray() ) { const int t = typeByName(n.GetString()); if ( t >= 0 ) exempt.insert(t); }
			for ( auto& n : m["nonmonsters"].GetArray() )
			{
				const int nm = typeByName(n.GetString());
				if ( nm < 0 ) { ++bad; continue; }
				for ( int t = 1; t < NUMMONSTERS; ++t )
					if ( !exempt.count(t) && !d.pairs.count(keyOf(t, nm)) ) d.pairs[keyOf(t, nm)] = Row{ m["class"].GetString(), m["reason"].GetString() };
			}
		}
		d.ok = true;
		printlog("[apx-hostility] table loaded from %s: %d fighting pairs, provocation %d s, calm start %d s after control, %d bad rows. Every other pair is neutral.",
			used.c_str(), (int)d.pairs.size(), d.provocationTicks / kTps, d.calmTicks / kTps, bad);
		if ( FILE* f = std::fopen("hostility_table.txt", "w") )
		{
			std::vector<std::string> rows;
			for ( auto& kv : d.pairs ) { char b[320]; snprintf(b, sizeof b, "%s\t%s\t%s\t%s", tname(kv.first / 4096), tname(kv.first % 4096), kv.second.cls.c_str(), kv.second.reason.c_str()); rows.push_back(b); }
			std::sort(rows.begin(), rows.end());
			for ( auto& r : rows ) std::fprintf(f, "%s\n", r.c_str());
			std::fclose(f);
		}
	}

	struct Counters
	{
		long long calmRefusals = 0, provokedHostile = 0, tableHostile = 0, eventHostile = 0, shopDefence = 0, shopIgnored = 0, neutral = 0, provocations = 0;
		std::map<std::string, long long> byClass; // hostile decisions by reason class
	};
	Counters& C() { static Counters c; return c; }
	std::unordered_map<uint32_t, std::unordered_map<uint32_t, uint32_t>>& provoked() { static std::unordered_map<uint32_t, std::unordered_map<uint32_t, uint32_t>> m; return m; }
	std::set<std::pair<uint32_t, uint32_t>>& events() { static std::set<std::pair<uint32_t, uint32_t>> s; return s; }
	std::unordered_map<uint32_t, std::pair<double, double>>& homes() { static std::unordered_map<uint32_t, std::pair<double, double>> m; return m; }
	bool isProvoked(uint32_t victim, uint32_t hitter)
	{
		auto it = provoked().find(victim);
		if ( it == provoked().end() ) return false;
		auto j = it->second.find(hitter);
		return j != it->second.end() && (int32_t)(j->second - ::ticks) > 0;
	}

	struct State
	{
		uint32_t lastTicks = 0, loadedAt = 0, endedAt = 0;
		bool loaded = false, ended = false;
		int floor = -1;
		// gate
		int seconds = -1;
		bool finished = false, popTaken = false;
		int popStart = -1, playerHits = 0, playerDmg = 0, mmHits = 0, mmExplained = 0, mmUnexplained = 0, deaths = 0, minHP = 1 << 30, startHP = 0, mmBeforeCalmEnd = 0, playerBeforeCalmEnd = 0;
		uint32_t deathTick = 0;
		std::map<std::string, int> mmPairs, mmClasses, playerBy;
	};
	State& S() { static State s; return s; }
	int gateSeconds()
	{
		State& s = S();
		if ( s.seconds < 0 ) { const char* v = std::getenv("APX_HOSTILITY_GATE"); s.seconds = (v && v[0]) ? std::max(0, std::atoi(v)) : 0; }
		return s.seconds;
	}
	bool calmActive() { return !legacy() && !S().ended; }
	bool isMon(Entity* e) { return e && e->behavior == &actMonster; }
	bool explained(const char* cls) { return std::strcmp(cls, "neutral") && std::strcmp(cls, "calm") && std::strcmp(cls, "vanilla"); }

	bool decide(Entity* a, Entity* b, Reason& why, bool& handled, int depth)
	{
		handled = true;
		if ( isMon(a) && b && b->behavior == &actPlayer && calmActive() ) // calm start: no creature targets the player either
		{
			why = { "calm", "calm start: nobody targets anyone yet" };
			++C().calmRefusals;
			return false;
		}
		if ( !isMon(a) || !isMon(b) ) { handled = false; return false; }
		Stat* sa = a->getStats(); Stat* sb = b->getStats();
		if ( !sa || !sb || a == b || depth > 4 ) return false;
		Counters& c = C();
		const bool prov = isProvoked(a->getUID(), b->getUID());
		if ( calmActive() )
		{
			if ( prov ) { why = { "provoked", "hit by it during the calm start" }; ++c.provokedHostile; return true; }
			why = { "calm", "calm start: nobody targets anyone yet" };
			++c.calmRefusals;
			return false;
		}
		if ( prov ) { why = { "provoked", "struck by it in the last 30 s" }; ++c.provokedHostile; ++c.byClass["provoked"]; return true; }
		if ( sb->leader_uid )
		{
			Entity* lb = uidToEntity(sb->leader_uid);
			if ( lb && lb->getStats() )
			{
				if ( lb == a ) { why = { "follower", "its own leader" }; return false; }
				why = { "follower", "fights what the leader fights" };
				return decide(a, lb, why, handled, depth + 1);
			}
		}
		if ( sa->leader_uid )
		{
			Entity* la = uidToEntity(sa->leader_uid);
			if ( la && la->getStats() )
			{
				if ( la == b ) { why = { "follower", "its own leader" }; return false; }
				return decide(la, b, why, handled, depth + 1);
			}
		}
		// shopkeepers defend only the few tiles around where they stood
		if ( sa->type == SHOPKEEPER || sb->type == SHOPKEEPER )
		{
			if ( sa->type == SHOPKEEPER && sb->type == SHOPKEEPER ) return false;
			Entity* shop = sa->type == SHOPKEEPER ? a : b;
			Entity* other = shop == a ? b : a;
			auto it = homes().find(shop->getUID());
			if ( it == homes().end() ) it = homes().emplace(shop->getUID(), std::make_pair((double)shop->x, (double)shop->y)).first;
			if ( std::hypot(other->x - it->second.first, other->y - it->second.second) <= D().shopRadius * 16.0 )
			{ why = { "shop", "defends its shop" }; ++c.shopDefence; ++c.byClass["shop"]; return true; }
			++c.shopIgnored; return false;
		}
		const uint32_t ua = a->getUID(), ub = b->getUID();
		if ( events().count({ std::min(ua, ub), std::max(ua, ub) }) ) { why = { "event", "set by a director event" }; ++c.eventHostile; ++c.byClass["event"]; return true; }
		if ( sa->type > 0 && sa->type < NUMMONSTERS && sb->type > 0 && sb->type < NUMMONSTERS )
		{
			auto it = D().pairs.find(keyOf(sa->type, sb->type));
			if ( it != D().pairs.end() )
			{
				static std::set<int> logged;
				if ( logged.insert(it->first).second ) printlog("[apx-hostility] pair %s-%s hostile (%s): %s", tname(sa->type), tname(sb->type), it->second.cls.c_str(), it->second.reason.c_str());
				static std::set<std::string> keep; // stable storage for the reason pointers
				why = { keep.insert(it->second.cls).first->c_str(), keep.insert(it->second.reason).first->c_str() };
				++c.tableHostile; ++c.byClass[it->second.cls];
				return true;
			}
		}
		++c.neutral;
		why = { "neutral", "no listed reason" };
		return false;
	}
	void gateFinish(const char* why);
}

bool legacy() { static const bool on = [] { const char* v = std::getenv("APX_HOSTILITY_OLD"); return v && v[0] == '1'; }(); return on; }
bool calmOver() { return legacy() || S().ended; }

bool hook(Entity* a, Entity* b, bool* handled)
{
	*handled = false;
	if ( legacy() || multiplayer == CLIENT ) return false;
	load();
	Reason r;
	return decide(a, b, r, *handled, 0);
}

bool hostile(Entity* a, Entity* b, Reason* why)
{
	Reason local;
	Reason& r = why ? *why : local;
	r = Reason();
	if ( !a || !b ) return false;
	if ( legacy() ) { r = { "vanilla", "APX_HOSTILITY_OLD=1: Barony's tables" }; return a->checkEnemy(b); }
	load();
	bool handled = false;
	const bool res = decide(a, b, r, handled, 0);
	if ( handled ) return res;
	r = { "vanilla", "a player is involved: Barony's player rules" };
	return a->checkEnemy(b);
}

void eventSet(uint32_t a, uint32_t b, const char* reason)
{
	if ( !a || !b || a == b ) return;
	if ( events().insert({ std::min(a, b), std::max(a, b) }).second )
		printlog("[apx-hostility] event pair uid=%u uid=%u hostile: %s", a, b, reason ? reason : "director event");
}

void onHit(Entity* attacker, Entity* target, int dmg)
{
	if ( !attacker || !target || attacker == target ) return;
	State& s = S();
	if ( gateSeconds() && s.loaded && !s.finished )
	{
		Stat* as = attacker->getStats();
		if ( target->behavior == &actPlayer )
		{
			++s.playerHits; s.playerDmg += dmg;
			if ( !s.ended ) ++s.playerBeforeCalmEnd;
			s.playerBy[as ? tname(as->type) : "?"]++;
		}
		else if ( isMon(attacker) && isMon(target) )
		{
			Stat* ts = target->getStats();
			Reason r; bool h = false;
			decide(attacker, target, r, h, 0); // why did the attacker count the target as a foe (before this hit provokes)
			++s.mmHits;
			if ( !s.ended ) ++s.mmBeforeCalmEnd;
			(explained(r.cls) ? s.mmExplained : s.mmUnexplained)++;
			s.mmClasses[r.cls]++;
			s.mmPairs[std::string(as ? tname(as->type) : "?") + ">" + (ts ? tname(ts->type) : "?") + ":" + r.cls]++;
		}
	}
	if ( isMon(target) && multiplayer != CLIENT && !legacy() )
	{
		auto& m = provoked()[target->getUID()];
		if ( !isProvoked(target->getUID(), attacker->getUID()) ) ++C().provocations;
		m[attacker->getUID()] = ::ticks + D().provocationTicks;
	}
}

namespace
{
	int population()
	{
		int n = 0;
		for ( node_t* nd = map.creatures ? map.creatures->first : nullptr; nd; nd = nd->next )
		{
			Entity* m = (Entity*)nd->element;
			Stat* ms = m && m->behavior == &actMonster ? m->getStats() : nullptr;
			n += ms && ms->HP > 0 && ms->type != SHOPKEEPER;
		}
		return n;
	}
	void gateFinish(const char* why)
	{
		State& s = S();
		if ( s.finished ) return;
		s.finished = true;
		Entity* p = players[0] ? players[0]->entity : nullptr;
		Stat* ps = p ? p->getStats() : nullptr;
		Counters& c = C();
		std::string pairs, classes, by;
		for ( auto& kv : s.mmPairs ) pairs += (pairs.empty() ? "" : ",") + std::string("\"") + kv.first + "\":" + std::to_string(kv.second);
		for ( auto& kv : s.mmClasses ) classes += (classes.empty() ? "" : ",") + std::string("\"") + kv.first + "\":" + std::to_string(kv.second);
		for ( auto& kv : s.playerBy ) by += (by.empty() ? "" : ",") + std::string("\"") + kv.first + "\":" + std::to_string(kv.second);
		char head[900];
		snprintf(head, sizeof head,
			"{\"why\":\"%s\",\"floor\":%d,\"seconds\":%.1f,\"legacy\":%d,\"calm_ended\":%d,\"calm_end_s\":%.1f,\"player_alive\":%d,\"hp\":%d,\"start_hp\":%d,\"min_hp\":%d,"
			"\"player_hits\":%d,\"player_dmg\":%d,\"player_hits_before_calm_end\":%d,\"mm_hits\":%d,\"mm_explained\":%d,\"mm_unexplained\":%d,\"mm_before_calm_end\":%d,"
			"\"pop_start\":%d,\"pop_end\":%d,\"calm_refusals\":%lld,\"provocations\":%lld,\"table_hostile\":%lld,\"provoked_hostile\":%lld,\"event_hostile\":%lld,\"shop_defence\":%lld,\"shop_ignored\":%lld,\"neutral\":%lld,",
			why, s.floor, (::ticks - s.loadedAt) / (double)kTps, (int)legacy(), (int)s.ended, s.endedAt / (double)kTps, (int)(ps && ps->HP > 0), ps ? ps->HP : 0, s.startHP,
			s.minHP == (1 << 30) ? -1 : s.minHP, s.playerHits, s.playerDmg, s.playerBeforeCalmEnd, s.mmHits, s.mmExplained, s.mmUnexplained, s.mmBeforeCalmEnd,
			s.popStart, population(), c.calmRefusals, c.provocations, c.tableHostile, c.provokedHostile, c.eventHostile, c.shopDefence, c.shopIgnored, c.neutral);
		printlog("[apx-hostility] gate %s\"mm_classes\":{%s},\"mm_pairs\":{%s},\"player_by\":{%s}}", head, classes.c_str(), pairs.c_str(), by.c_str());
		mainloop = 0;
	}
}

void frame()
{
	State& s = S();
	if ( ::ticks < s.lastTicks || s.floor != currentlevel )
	{
		const int sec = s.seconds; const bool fin = s.finished;
		s = State();
		s.seconds = sec; s.finished = fin;
		s.floor = currentlevel;
		provoked().clear(); events().clear(); homes().clear();
		load();
	}
	s.lastTicks = ::ticks;
	const bool playerIn = players[clientnum] && players[clientnum]->entity;
	if ( !s.loaded && playerIn && !fadeout && fadealpha == 0 )
	{
		s.loaded = true; s.loadedAt = ::ticks;
		if ( Stat* ps = players[clientnum]->entity->getStats() ) s.startHP = ps->HP;
		printlog("[apx-hostility] player has control floor=%d tick=%u; calm start for %d s (on=%d)", currentlevel, ::ticks, D().calmTicks / kTps, (int)!legacy());
	}
	if ( s.loaded && !s.ended && ::ticks - s.loadedAt >= (uint32_t)D().calmTicks )
	{
		s.ended = true; s.endedAt = ::ticks - s.loadedAt;
		printlog("[apx-hostility] calm start ended floor=%d tick=%u (%.1f s after control; refused target choices %lld)", currentlevel, ::ticks, s.endedAt / (double)kTps, C().calmRefusals);
	}
	if ( !gateSeconds() || !s.loaded || s.finished ) return;
	if ( s.popStart < 0 && ::ticks - s.loadedAt >= 2 ) s.popStart = population();
	{ // every creature death: who killed it (so a population drop is explained)
		static std::set<uint32_t> dead;
		if ( ::ticks < s.loadedAt ) dead.clear();
		for ( node_t* nd = map.creatures ? map.creatures->first : nullptr; nd; nd = nd->next )
		{
			Entity* m = (Entity*)nd->element;
			Stat* ms = m && m->behavior == &actMonster ? m->getStats() : nullptr;
			if ( !ms || ms->HP > 0 || !dead.insert(m->getUID()).second ) continue;
			Entity* k = ms->killer_uid ? uidToEntity(ms->killer_uid) : nullptr;
			Stat* ks = k ? k->getStats() : nullptr;
			printlog("[apx-hostility] death uid=%u type=%s t=%.1f s killer=%s (killer uid=%u killedby=%d)", m->getUID(), tname(ms->type), (::ticks - s.loadedAt) / (double)kTps,
				ks ? tname(ks->type) : (k ? "non-creature" : "none/gone"), ms->killer_uid, (int)ms->killer);
			++s.deaths;
		}
	}
	Entity* p = players[0] ? players[0]->entity : nullptr;
	Stat* ps = p ? p->getStats() : nullptr;
	if ( ps ) s.minHP = std::min(s.minHP, ps->HP);
	if ( !p || !ps || ps->HP <= 0 )
	{
		if ( !s.deathTick ) s.deathTick = ::ticks ? ::ticks : 1;
		if ( ::ticks - s.deathTick >= (uint32_t)kTps ) gateFinish("player_died");
		return;
	}
	if ( ::ticks - s.loadedAt >= (uint32_t)(gateSeconds() * kTps) ) gateFinish("time");
}
}
}
