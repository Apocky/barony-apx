// apx/encounter.cpp -- BF1-3: damage attribution and encounter rows (encounters.jsonl).
//
// Choke point: Entity::setHP (every HP change, including Entity::modHP, passes it) calls apxOnHp with the
// effective delta. Producers (melee, arrows, thrown, magic, DoT/environment, explosions, thorns) open an
// ApxDmgScope so the delta can be attributed. No damage value is changed anywhere.
// HP writes that bypass setHP (direct `->HP =` in a few places) are caught by a per-tick shadow compare and
// counted as `unattributed_hp`.
//
// Encounter: opens when a hostile monster within 12 tiles targets a player (scanned every 10 ticks) or a
// player takes damage with no encounter open; closes when no such monster has existed for 250 ticks, all
// players are dead, or the floor changes.
#include "telemetry.hpp"
#include "../main.hpp"
#include "../game.hpp"
#include "../entity.hpp"
#include "../monster.hpp"
#include "../player.hpp"
#include "../items.hpp"
#include "../stat.hpp"
#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <map>
#include <set>

namespace
{
	const char* kSrcName[SRC_N] = { "unattributed", "melee", "arrow", "thrown", "magic", "dot", "env", "explosion", "thorns" };
	thread_local int tlsKind = 0;

	struct Enc
	{
		bool open = false;
		int id = 0;
		Uint32 tOpen = 0, lastEngaged = 0;
		double wallOpen = 0;
		std::vector<int> pidx;                       // player indices tracked
		std::vector<int> hpStart, hpEnd, dmgIn, healsIn;
		std::map<int, int> pslot;                    // player index -> slot
		long dmgBySrc[SRC_N] = {};
		long dmgOut = 0, hitsTaken = 0, hitsLanded = 0, heals = 0, kills = 0, deaths = 0, consumables = 0, itemsUsed = 0;
		std::set<Uint32> creatures;
		std::map<std::string, int> byType;
		std::map<Uint32, int> hitsOn;                // monster uid -> hits taken
		std::map<Uint32, Uint32> firstSeen;          // monster uid -> tick first engaged
		std::map<std::string, std::vector<double>> ttk;
		std::map<std::string, std::vector<int>> hitsToKill;
		double firstContact = -1;
		int firstHitTick = -1;
		long enemySeconds = 0, unlitSeconds = 0;
		long unattributed = 0;
		long regen = 0;
	} E;

	int gEncId = 0;
	long gUnattributedTotal = 0;
	std::map<int, int> gShadow;                      // player index -> last HP seen through apxOnHp
	bool gDowned[MAXPLAYERS] = {};                   // lethal damage taken (HP would have gone below 1); vanilla = dead, sim buddha = HP 1

	Entity* playerEntity(int p) { return (p >= 0 && p < MAXPLAYERS && players[p]) ? players[p]->entity : nullptr; }

	std::string typeName(Entity* e)
	{
		int t = (int)e->getMonsterTypeFromSprite();
		if ( Stat* s = e->getStats() ) { t = (int)s->type; }
		return (t > 0 && t < NUMMONSTERS) ? monstertypename[t] : "unknown";
	}

	bool isPlayerEnt(Entity* e) { return e && e->behavior == &actPlayer; }

	void openEnc(double firstDist)
	{
		E = Enc();
		E.open = true;
		E.id = ++gEncId;
		E.tOpen = ticks;
		E.lastEngaged = ticks;
		E.wallOpen = apx::wallMs();
		E.firstContact = firstDist;
		for ( int p = 0; p < MAXPLAYERS; ++p )
		{
			if ( playerEntity(p) && stats[p] )
			{
				E.pslot[p] = (int)E.pidx.size();
				E.pidx.push_back(p);
				E.hpStart.push_back(stats[p]->HP);
				E.dmgIn.push_back(0);
				E.healsIn.push_back(0);
				gShadow[p] = stats[p]->HP;
			}
		}
	}

	std::string mapJsonInt(const std::map<std::string, int>& m)
	{
		std::string s = "{";
		bool f = true;
		for ( auto& kv : m ) { if ( !f ) { s += ","; } f = false; s += "\"" + kv.first + "\":" + std::to_string(kv.second); }
		return s + "}";
	}
}

ApxDmgScope::ApxDmgScope(int kind) : prev(tlsKind)
{
	// control knob: APX_BREAK_KIND=<melee|arrow|thrown|magic|dot|env|explosion|thorns> leaves that producer
	// unattributed, so a fight that uses it MUST report unattributed_hp > 0 (APX_BREAK_ARROWS=1 is the arrow case)
	static int breakKind = -1;
	static bool init = false;
	if ( !init )
	{
		init = true;
		const char* b = getenv("APX_BREAK_KIND");
		if ( getenv("APX_BREAK_ARROWS") && getenv("APX_BREAK_ARROWS")[0] == '1' ) { breakKind = SRC_ARROW; }
		for ( int k = 1; b && k < SRC_N; ++k ) { if ( !strcmp(b, kSrcName[k]) ) { breakKind = k; } }
	}
	if ( kind != breakKind ) { tlsKind = kind; }
}
ApxDmgScope::~ApxDmgScope() { tlsKind = prev; }

namespace apx
{
	long unattributedTotal() { return gUnattributedTotal; }
	long encountersClosed = 0;
	Uint32 idleTicks()
	{
		static Uint32 v = 0;
		if ( !v ) { const char* e = getenv("APX_ENC_IDLE_TICKS"); v = e ? (Uint32)atoi(e) : 250; if ( v < 10 ) { v = 250; } }
		return v;
	}
	bool encounterOpen() { return E.open; }
	void noContactRow()
	{
		// the pack never engaged the party within the time limit: still one row, so counts match the scenario
		ApxRow r;
		apx::commonFields(r);
		r.i("enc_id", ++gEncId).i("t_open", ticks).i("t_close", ticks).d("duration_s", 0.0).i("creatures_n", 0).i("creatures_killed", 0)
			.str("outcome", "no_contact").str("close_reason", "no_contact").i("dmg_in_total", 0).i("kills", 0).i("deaths", 0)
			.intList("party_hp_start", {}).intList("party_hp_end", {}).intList("dmg_in_per_player", {}).i("dmg_out_total", 0).i("hits_taken", 0).i("hits_landed", 0)
			.i("heals_total", 0).i("regen_total", 0).raw("dmg_in_by_source", "{\"unattributed\":0,\"melee\":0,\"arrow\":0,\"thrown\":0,\"magic\":0,\"dot\":0,\"env\":0,\"explosion\":0,\"thorns\":0}")
			.raw("hits_to_kill_by_type", "{}").raw("creatures_by_type", "{}").raw("dmg_in_per_player_per_min", "[]").d("first_contact_dist_tiles", -1.0)
			.i("unattributed_hp", 0).i("dropped_rows", (long long)apx::droppedRows).i("bots_stuck", (long long)apx::botsStuck)
			.i("shots_failed", (long long)apx::shotsFailed);
		apx::write("encounters.jsonl", r);
		++encountersClosed;
	}
	long encHitsLanded() { return E.hitsLanded; }
	void clearDowned() { for ( bool& d : gDowned ) { d = false; } }
	bool isDowned(int p) { return p >= 0 && p < MAXPLAYERS && gDowned[p]; }

	void closeEncounter(const char* why)
	{
		if ( !E.open ) { return; }
		E.open = false;
		for ( int k = 0; k < (int)E.pidx.size(); ++k )
		{
			int p = E.pidx[k];
			E.hpEnd.push_back(stats[p] ? stats[p]->HP : 0);
		}
		// second reading of the same quantity: HP at open - HP at close + heals must equal damage in
		long dmgIn = 0, check = 0;
		for ( int k = 0; k < (int)E.pidx.size(); ++k ) { dmgIn += E.dmgIn[k]; check += E.hpStart[k] - E.hpEnd[k] + E.healsIn[k]; }
		long unattr = std::labs(check - dmgIn);
		gUnattributedTotal += unattr;
		bool allDead = true;
		for ( int p : E.pidx ) { if ( !gDowned[p] ) { allDead = false; } }
		int alive = 0;
		for ( Uint32 u : E.creatures ) { if ( Entity* m = uidToEntity(u) ) { if ( m->getStats() && m->getStats()->HP > 0 ) { ++alive; } } }
		const char* outcome = allDead ? "wipe" : (alive > 0 ? "flee" : "win");

		ApxRow r;
		apx::commonFields(r);
		double dur = (ticks - E.tOpen) / (double)TICKS_PER_SECOND;
		r.i("enc_id", E.id).i("t_open", E.tOpen).i("t_close", ticks).d("duration_s", dur)
			.i("creatures_n", (long long)E.creatures.size()).raw("creatures_by_type", mapJsonInt(E.byType))
			.i("creatures_killed", E.kills);
		r.intList("party_hp_start", E.hpStart).intList("party_hp_end", E.hpEnd);
		r.i("dmg_in_total", dmgIn).intList("dmg_in_per_player", E.dmgIn);
		std::string perMin = "[";
		for ( size_t k = 0; k < E.dmgIn.size(); ++k )
		{
			char b[48];
			snprintf(b, sizeof(b), "%s%.3f", k ? "," : "", dur > 0 ? E.dmgIn[k] * 60.0 / dur : 0.0);
			perMin += b;
		}
		r.raw("dmg_in_per_player_per_min", perMin + "]");
		std::string bs = "{";
		for ( int k = 0; k < SRC_N; ++k ) { bs += std::string(k ? "," : "") + "\"" + kSrcName[k] + "\":" + std::to_string(E.dmgBySrc[k]); }
		r.raw("dmg_in_by_source", bs + "}");
		r.i("dmg_out_total", E.dmgOut).i("hits_taken", E.hitsTaken).i("hits_landed", E.hitsLanded).i("kills", E.kills)
			.i("downs", 0).i("deaths", E.deaths).i("heals_total", E.heals).i("regen_total", E.regen).i("consumables_used", E.consumables)
			.i("items_used", E.itemsUsed).i("stamina_spent", 0).str("outcome", outcome).str("close_reason", why)
			.d("first_contact_dist_tiles", E.firstContact)
			.d("time_to_first_hit_s", E.firstHitTick >= 0 ? (E.firstHitTick - (long long)E.tOpen) / (double)TICKS_PER_SECOND : -1.0);
		std::string tk = "{";
		bool f = true;
		for ( auto& kv : E.ttk )
		{
			if ( !f ) { tk += ","; } f = false;
			tk += "\"" + kv.first + "\":[";
			for ( size_t j = 0; j < kv.second.size(); ++j ) { char b[32]; snprintf(b, sizeof(b), "%s%.2f", j ? "," : "", kv.second[j]); tk += b; }
			tk += "]";
		}
		r.raw("ttk_by_tier", tk + "}");
		std::string hk = "{";
		f = true;
		for ( auto& kv : E.hitsToKill )
		{
			if ( !f ) { hk += ","; } f = false;
			hk += "\"" + kv.first + "\":[";
			for ( size_t j = 0; j < kv.second.size(); ++j ) { hk += (j ? "," : "") + std::to_string(kv.second[j]); }
			hk += "]";
		}
		r.raw("hits_to_kill_by_type", hk + "}");
		r.d("unlit_fraction", E.enemySeconds > 0 ? (double)E.unlitSeconds / E.enemySeconds : -1.0).i("unattributed_hp", unattr + E.unattributed); // second-reading mismatch + damage whose source scope was missing
		// director inputs (raw)
		std::vector<int> lv, acs;
		int hpMax = 0, atkSum = 0, armor = 0, followers = 0, potions = 0, food = 0, n = 0;
		for ( int p : E.pidx )
		{
			if ( !stats[p] ) { continue; }
			Entity* pe = playerEntity(p);
			lv.push_back(stats[p]->LVL);
			hpMax = std::max(hpMax, (int)stats[p]->MAXHP);
			acs.push_back(AC(stats[p]));
			if ( pe ) { atkSum += Entity::getAttack(pe, stats[p], true); ++n; }
			Item* slots[] = { stats[p]->helmet, stats[p]->breastplate, stats[p]->gloves, stats[p]->shoes, stats[p]->shield, stats[p]->cloak, stats[p]->mask };
			for ( Item* it : slots ) { if ( it ) { ++armor; } }
			for ( node_t* nd = stats[p]->inventory.first; nd; nd = nd->next )
			{
				Item* it = (Item*)nd->element;
				if ( !it ) { continue; }
				if ( itemCategory(it) == POTION ) { potions += it->count; }
				else if ( itemCategory(it) == FOOD ) { food += it->count; }
			}
			for ( node_t* nd = stats[p]->FOLLOWERS.first; nd; nd = nd->next ) { ++followers; }
		}
		double acMean = 0;
		for ( int a : acs ) { acMean += a; }
		if ( !acs.empty() ) { acMean /= acs.size(); }
		r.intList("player_levels", lv).i("player_hp_max", hpMax).d("player_ac_mean", acMean)
			.d("weapon_atk_mean", n ? (double)atkSum / n : 0.0).i("armor_slots_filled", armor).i("followers_n", followers)
			.i("potions_n", potions).i("food_n", food).d("encounter_wall_ms", apx::wallMs() - E.wallOpen)
			.i("dropped_rows", (long long)apx::droppedRows).i("bots_stuck", (long long)apx::botsStuck)
			.i("shots_failed", (long long)apx::shotsFailed);
		apx::write("encounters.jsonl", r);
		++encountersClosed;
	}

	void onHp(Entity* victim, int delta, bool lethal)
	{
		if ( !enabled || multiplayer == CLIENT || !victim || delta == 0 ) { return; }
		int kind = tlsKind;
		if ( isPlayerEnt(victim) )
		{
			int p = victim->skill[2];
			if ( p < 0 || p >= MAXPLAYERS || !stats[p] ) { return; }
			// only creature-caused damage opens an encounter (poison, hunger, falls and untracked sources do not)
			if ( !E.open && delta < 0 && kind != SRC_DOT && kind != SRC_ENV && kind != SRC_UNKNOWN ) { openEnc(-1.0); }
			if ( delta > 0 && stats[p]->HP > 1 ) { gDowned[p] = false; }
			if ( E.open && E.pslot.count(p) )
			{
				int k = E.pslot[p];
				if ( delta < 0 )
				{
					E.dmgIn[k] += -delta;
					E.dmgBySrc[kind] += -delta;
					E.hitsTaken++;
					if ( E.firstHitTick < 0 ) { E.firstHitTick = (int)ticks; }
					if ( kind == SRC_UNKNOWN ) { E.unattributed += -delta; }
					if ( lethal && !gDowned[p] ) { E.deaths++; gDowned[p] = true; }
				}
				else
				{
					E.heals += delta;
					E.healsIn[k] += delta;
					if ( stats[p]->HP > 1 ) { gDowned[p] = false; }
				}
			}
			gShadow[p] = stats[p]->HP;
		}
		else if ( victim->behavior == &actMonster && E.open && victim->monsterAllyIndex < 0 )
		{
			Stat* s = victim->getStats();
			if ( !s ) { return; }
			if ( delta < 0 )
			{
				E.dmgOut += -delta;
				E.hitsLanded++;
				Uint32 u = victim->getUID();
				int hits = ++E.hitsOn[u];
				if ( s->HP <= 0 )
				{
					E.kills++;
					std::string tn = typeName(victim);
					Uint32 fs = E.firstSeen.count(u) ? E.firstSeen[u] : E.tOpen;
					E.ttk[tn].push_back((ticks - fs) / (double)TICKS_PER_SECOND);
					E.hitsToKill[tn].push_back(hits);
				}
			}
		}
	}

	void onItemUse(bool consumed)
	{
		if ( !enabled || !E.open ) { return; }
		if ( consumed ) { E.consumables++; } else { E.itemsUsed++; }
	}

	void encounterTick()
	{
		if ( !enabled || multiplayer == CLIENT ) { return; }
		// shadow compare: HP changes that did not pass through setHP
		if ( E.open )
		{
			for ( int p : E.pidx )
			{
				if ( stats[p] && gShadow.count(p) && gShadow[p] != stats[p]->HP )
				{
					int d = stats[p]->HP - gShadow[p];
					gShadow[p] = stats[p]->HP;
					if ( d < 0 )
					{
						E.unattributed += -d; // bypassed setHP: counted, not hidden
						gUnattributedTotal += -d;
					} else if ( E.pslot.count(p) )
					{
						// natural regeneration writes HP directly (handleEffects): count it as a heal, and say so
						E.heals += d;
						E.healsIn[E.pslot[p]] += d;
						E.regen += d;
					}
				}
			}
		}
		if ( ticks % 10 != 0 ) { return; }
		// engaged hostiles
		double nearest = -1;
		int engaged = 0;
		std::vector<Entity*> hostiles;
		bool secondTick = (ticks % TICKS_PER_SECOND) == 0;
		if ( map.creatures )
		{
			for ( node_t* nd = map.creatures->first; nd; nd = nd->next )
			{
				Entity* m = (Entity*)nd->element;
				if ( !m || m->behavior != &actMonster || m->monsterAllyIndex >= 0 ) { continue; }
				Stat* s = m->getStats();
				if ( !s || s->HP <= 0 ) { continue; }
				Entity* tgt = uidToEntity(m->monsterTarget);
				if ( !isPlayerEnt(tgt) ) { continue; }
				double d = sqrt((m->x - tgt->x) * (m->x - tgt->x) + (m->y - tgt->y) * (m->y - tgt->y)) / 16.0;
				if ( d > 12.0 ) { continue; }
				++engaged;
				hostiles.push_back(m);
				if ( nearest < 0 || d < nearest ) { nearest = d; }
			}
		}
		bool anyDowned = false;
		for ( int p = 0; p < MAXPLAYERS; ++p ) { if ( gDowned[p] ) { anyDowned = true; } }
		if ( engaged > 0 && !(anyDowned && !E.open) )
		{
			if ( !E.open ) { openEnc(nearest); }
			E.lastEngaged = ticks;
			for ( Entity* m : hostiles )
			{
				if ( E.creatures.insert(m->getUID()).second )
				{
					E.byType[typeName(m)]++;
					E.firstSeen[m->getUID()] = ticks;
				}
				if ( secondTick )
				{
					++E.enemySeconds;
					// light sampling needs the lightmap, which only exists while frames are drawn
					int idx = (int)(m->y / 16) + (int)(m->x / 16) * (int)map.height;
					if ( lightmaps[0].size() == (size_t)(map.width * map.height) && idx >= 0 && idx < (int)lightmaps[0].size() )
					{
						const auto& c = lightmaps[0][idx];
						if ( (c.x + c.y + c.z) / 3.f * 255.f < 32.f ) { ++E.unlitSeconds; }
					}
				}
			}
		}
		if ( E.open )
		{
			bool allDead = true;
			for ( int p : E.pidx ) { if ( !gDowned[p] ) { allDead = false; } }
			if ( allDead && !E.pidx.empty() ) { closeEncounter("all_dead"); }
			else if ( ticks - E.lastEngaged >= idleTicks() ) { closeEncounter("idle"); }
		}
	}
}
