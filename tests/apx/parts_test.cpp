#include <algorithm>
// F6 offline gates (card BF6-1, -3, -4, -6; tier T1, no Barony launch). One JSON line per gate;
// exit 0 = every gate passed. Usage: parts_test <data/apx dir> <tests/apx/itemtypes.txt>
#include "../../src/apx/parts_model.hpp"
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <string>
#include <vector>

using namespace apxparts;

static int failures = 0;
static void gate(const char* name, bool pass, const std::string& detail)
{
	printf("{\"gate\":\"%s\",\"pass\":%s,%s}\n", name, pass ? "true" : "false", detail.c_str());
	fflush(stdout);
	if ( !pass ) { ++failures; }
}
static std::string kv(const char* k, double v) { char b[96]; snprintf(b, sizeof(b), "\"%s\":%.6g", k, v); return b; }
static std::string join(std::initializer_list<std::string> l) { std::string s; for ( auto& x : l ) { s += (s.empty() ? "" : ",") + x; } return s; }

struct Rng { uint64_t s; explicit Rng(uint64_t x) : s(x) {} uint64_t next() { s ^= s << 13; s ^= s >> 7; s ^= s << 17; return s; } int below(int m) { return (int)(next() % (uint64_t)m); } };

int main(int argc, char** argv)
{
	const std::string data = argc > 1 ? argv[1] : "data/apx";
	const std::string typesPath = argc > 2 ? argv[2] : "tests/apx/itemtypes.txt";
	DB db;
	const std::string err = loadDB(data, db);
	gate("data_load", err.empty(), "\"err\":\"" + err + "\"");
	if ( !err.empty() ) { return 1; }

	// coverage: every vanilla ItemType resolves to a family
	std::vector<std::string> types;
	{
		std::ifstream in(typesPath);
		std::string line;
		while ( std::getline(in, line) ) { if ( !line.empty() && line.back() == '\r' ) { line.pop_back(); } if ( !line.empty() ) { types.push_back(line); } }
	}
	int unmapped = 0;
	std::vector<const TypeForm*> gearForms; // the families bound in the engine (not Plain)
	for ( auto& t : types )
	{
		const TypeForm* tf = db.typeForm(t);
		if ( !tf ) { ++unmapped; continue; }
		if ( tf->family != db.plainFamily ) { gearForms.push_back(tf); }
	}
	gate("coverage", unmapped == 0 && types.size() >= 500, join({ kv("types", (double)types.size()), kv("forms_unmapped", unmapped), kv("gear_types", (double)gearForms.size()) }));

	// S1 determinism: 10^6 rolls, each twice, over realm 1..10, depth 1..40, 5 sources, 25,000 indices
	const int N = 1000000;
	auto t0 = std::chrono::steady_clock::now();
	Counters c1, c2;
	uint64_t h1 = 1469598103934665603ULL, h2 = h1;
	long long overCap = 0, maxOver = -1000000, partsHist[APX_MAX_PARTS + 1] = { 0 };
	for ( int n = 0; n < N; ++n )
	{
		const uint32_t realm = 1 + n % 10, index = (uint32_t)(n / 2000) * 50 + n % 50;
		const int depth = 1 + (n / 10) % 40, source = (n / 400) % 5;
		const TypeForm& tf = *gearForms[(size_t)n % gearForms.size()];
		ApxItemData a, b;
		roll(db, tf, realm, depth, source, index, a, c1);
		roll(db, tf, realm, depth, source, index, b, c2);
		h1 = fnv1a64(&a, sizeof(a), h1);
		h2 = fnv1a64(&b, sizeof(b), h2);
		const long long over = (long long)itemPower(db, a) - ipCap(depth);
		overCap += over > 0;
		maxOver = std::max(maxOver, over);
		++partsHist[a.nParts];
	}
	const double s1 = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
	gate("S1_determinism", h1 == h2 && c1.rolls == N, join({ kv("rolls", N), kv("seconds", s1), kv("hash_equal", h1 == h2), kv("forbidden", (double)c1.forbidden) }));
	{
		ApxItemData a, b;
		Counters c;
		const TypeForm& tf = *gearForms[0];
		roll(db, tf, 3, 7, 1, 4242, a, c);
		roll(db, tf, 3, 7, 1, 4243, b, c);
		gate("S1_ctrl_index", memcmp(&a, &b, sizeof(a)) != 0, "\"note\":\"one changed index must change the item\"");
	}
	std::string hist;
	for ( int k = 0; k <= APX_MAX_PARTS; ++k ) { hist += (k ? "," : "") + std::to_string(partsHist[k]); }
	gate("S2_cap", overCap == 0 && c1.ip_rerolls * 20 < N && c1.forms_unmapped == 0 && c1.parts_overflow == 0, join({ kv("rolls", N), kv("over_cap", (double)overCap), kv("max_over", (double)maxOver),
		kv("ip_rerolls", (double)c1.ip_rerolls), kv("ip_reroll_share", (double)c1.ip_rerolls / N), kv("rolls_rerolled_share", (double)c1.rolls_rerolled / N), kv("ip_clamped", (double)c1.ip_clamped),
		kv("parts_overflow", (double)c1.parts_overflow), "\"parts_hist\":[" + hist + "]" }));
	{
		Counters c;
		long long over = 0;
		for ( int n = 0; n < N; ++n )
		{
			ApxItemData a;
			const int depth = 1 + (n / 10) % 40;
			roll(db, *gearForms[(size_t)n % gearForms.size()], 1 + n % 10, depth, (n / 400) % 5, (uint32_t)n, a, c, false);
			over += itemPower(db, a) > ipCap(depth);
		}
		gate("S2_ctrl_cap_off", over > 0, join({ kv("over_cap", (double)over), "\"note\":\"APX_IP_CAP=0 must exceed the cap, else S2 proves nothing\"" }));
	}

	// random items for the codecs (all fields random, 0..12 parts)
	Rng rr(12345);
	auto randomItem = [&](ApxItemData& it) {
		memset(&it, 0, sizeof(it));
		it.form = (uint32_t)rr.next(); it.rollKey = (uint32_t)rr.next(); it.power = (uint16_t)rr.next(); it.flags = (uint8_t)rr.next();
		it.nParts = (uint8_t)rr.below(APX_MAX_PARTS + 1);
		for ( int k = 0; k < it.nParts; ++k )
		{
			ApxPart& p = it.parts[k];
			p.slot = (uint8_t)rr.next(); p.material = (uint8_t)rr.next(); p.quality = (uint8_t)rr.next(); p.rarity = (uint8_t)rr.next();
			p.integrity = (uint8_t)rr.next(); p.maker = (uint8_t)rr.next(); p.nAffix = (uint8_t)rr.below(5);
			for ( int a = 0; a < 4; ++a ) { p.affix[a] = (uint16_t)rr.next(); }
			p.look = rr.below(3) ? 0u : ((uint32_t)rr.next() | 1u); // BX1 looks: saved (v2 strings) and packed (v2 blobs)
		}
	};
	{
		int ok = 0, nonEmpty = 0;
		for ( int n = 0; n < 100000; ++n )
		{
			ApxItemData a, b{};
			randomItem(a);
			if ( a.nParts == 0 ) { ok += toString(a).empty(); continue; }
			++nonEmpty;
			ok += fromString(toString(a), b) && !memcmp(&a, &b, sizeof(a));
		}
		gate("io_roundtrip", ok == 100000, join({ kv("items", 100000), kv("equal", ok), kv("with_parts", nonEmpty) }));
		int accepted = 0, tried = 0, crashes = 0;
		for ( int n = 0; n < 1000; ++n )
		{
			ApxItemData a, b{};
			do { randomItem(a); } while ( a.nParts == 0 );
			std::string s = toString(a);
			const size_t pos = (size_t)rr.below((int)s.size());
			char ch;
			do { ch = (char)(33 + rr.below(94)); } while ( ch == s[pos] );
			s[pos] = ch;
			++tried;
			if ( fromString(s, b) && !memcmp(&a, &b, sizeof(a)) ) { ++accepted; }
		}
		long long truncAccepted = 0, truncs = 0;
		for ( int n = 0; n < 1000; ++n )
		{
			ApxItemData a, b{};
			do { randomItem(a); } while ( a.nParts == 0 );
			const std::string s = toString(a);
			for ( size_t L = 0; L < s.size(); ++L ) { ++truncs; truncAccepted += fromString(s.substr(0, L), b); }
		}
		gate("io_reject", accepted == 0 && truncAccepted == 0, join({ kv("corrupted", tried), kv("accepted_as_original", accepted), kv("truncations", (double)truncs),
			kv("truncations_accepted", (double)truncAccepted), kv("crashes", crashes) }));
	}
	{
		int ok = 0, maxLen = 0;
		uint8_t buf[PACK_MAX + 8];
		for ( int n = 0; n < 100000; ++n )
		{
			ApxItemData a, b{};
			randomItem(a);
			for ( int k = 0; k < a.nParts; ++k ) { a.parts[k].look = n % 3 ? 0u : 0x9E3779B9u * (uint32_t)(n + 1) | 1u; /* BX1 looks ride the blob */ }
			const int L = pack(a, buf);
			maxLen = std::max(maxLen, L);
			ok += unpack(buf, L, b) && !memcmp(&a, &b, sizeof(a));
		}
		{
			// v1 blobs (16-byte rows, before BX1) still unpack, look 0
			int v1ok = 0;
			for ( int n = 0; n < 1000; ++n )
			{
				ApxItemData a, b{};
				do { randomItem(a); } while ( a.nParts == 0 );
				for ( int k = 0; k < a.nParts; ++k ) { a.parts[k].look = 0; }
				uint8_t v2b[PACK_MAX + 8], v1b[PACK_MAX + 8];
				const int L = pack(a, v2b);
				int j = 0;
				for ( int i = 0; i < 13; ++i ) { v1b[j++] = v2b[i]; }
				v1b[0] = 1;
				for ( int k = 0; k < a.nParts; ++k ) { for ( int i = 0; i < PACK_ROW_V1; ++i ) { v1b[j++] = v2b[13 + k * PACK_ROW + i]; } }
				v1ok += L == 13 + PACK_ROW * a.nParts && unpack(v1b, j, b) && !memcmp(&a, &b, sizeof(a));
			}
			gate("pack_v1_compat", v1ok == 1000, kv("v1_blobs_read", v1ok));
		}
		gate("pack_roundtrip", ok == 100000 && maxLen <= PACK_MAX, join({ kv("items", 100000), kv("equal", ok), kv("max_len", maxLen), kv("pack_max", PACK_MAX) }));
		long long shortAccepted = 0, shorts = 0;
		for ( int n = 0; n < 2000; ++n )
		{
			ApxItemData a, b{};
			randomItem(a);
			const int L = pack(a, buf);
			for ( int s = 0; s < L; ++s ) { ++shorts; shortAccepted += unpack(buf, s, b); }
			shortAccepted += unpack(buf, L + 1, b) ? 1 : 0; // oversized by one
		}
		gate("pack_short", shortAccepted == 0, join({ kv("short_or_long_buffers", (double)shorts), kv("accepted", (double)shortAccepted) }));
	}
	{
		ApxItemData a{}, b{}, v{};
		Counters c;
		roll(db, *gearForms[1], 1, 5, 0, 1, a, c);
		ApxItemData copy;
		memcpy(&copy, &a, sizeof(a)); // Barony memcpy's Item (stat.cpp); the POD must survive it
		gate("bind_roundtrip", !memcmp(&copy, &a, sizeof(a)) && a.nParts > 0, kv("parts", a.nParts));
		gate("stack_rule", !mayStack(a, b) && !mayStack(b, a) && mayStack(v, b), "\"note\":\"items with parts never stack; vanilla items still may\"");
	}

	// S4 salvage over seeded items (D76 stop rule: 95% half-width under 0.5 points or 10^5 items)
	long long craftedN = 0, craftedOk = 0, keptN = 0, keptOk = 0; // D86 component hand-over
	auto salvageRun = [&](bool symmetric, double& lootedPct, double& craftedPct, double& dischargePct, double& auraPct, double& hwLooted, int& lowHalvedOk, int& lowParts, int& items) {
		double sumL = 0, sumL2 = 0, sumC = 0; long long nL = 0, nC = 0, withAff = 0, disch = 0, affixes = 0, auras = 0;
		lowHalvedOk = 0; lowParts = 0; craftedN = craftedOk = keptN = keptOk = 0;
		Counters c;
		for ( items = 0; items < 100000; ++items )
		{
			ApxItemData it;
			const int depth = 1 + items % 40;
			roll(db, *gearForms[(size_t)items % gearForms.size()], 7, depth, items % 5, (uint32_t)items, it, c);
			if ( it.nParts < 2 ) { continue; }
			// thirds: looted (60 %), crafted (every part a component), neither (shop/start gear: 100 %)
			it.flags = (uint8_t)((it.flags & 0x3f) | (items % 3 == 0 ? FLAG_LOOTED : items % 3 == 1 ? FLAG_CRAFTED : 0));
			const int keep = items % it.nParts;
			SalvageResult r = salvage(db, it, keep, rollKey(7, (uint8_t)depth, 9, (uint32_t)items), symmetric);
			if ( it.flags & FLAG_CRAFTED )
			{
				++craftedN;
				bool ok = r.crafted && (int)r.components.size() == it.nParts && r.grams.empty() && r.auraCrystals == 0;
				for ( size_t k = 0; ok && k < r.components.size(); ++k ) { ok = isComponent(r.components[k]) && !memcmp(&r.components[k].parts[0], &it.parts[k], sizeof(ApxPart)); }
				craftedOk += ok;
				continue;
			}
			{
				++keptN;
				const ApxItemData* c = r.components.size() == 1 ? &r.components[0] : nullptr;
				keptOk += c && isComponent(*c) && c->parts[0].slot == it.parts[keep].slot && c->parts[0].material == it.parts[keep].material
					&& c->parts[0].nAffix == (r.discharged ? 0 : it.parts[keep].nAffix) && (int)r.grams.size() == it.nParts - 1;
			}
			// the integrity-adjusted base: what the rule says each part gives at 100 percent
			long long base = 0;
			for ( int k = 0; k < it.nParts; ++k )
			{
				if ( k == keep ) { continue; }
				const int m = partMassGrams(db, it.parts[k]);
				const bool low = it.parts[k].integrity < db.salvage.lowIntegrityByte;
				base += low ? (long long)m * db.salvage.lowIntegrityFactorPct / 100 : m;
				if ( low ) { ++lowParts; const long long g = r.grams[(size_t)(k < keep ? k : k - 1)].second; const long long full = (long long)m * (r.looted && !symmetric ? db.salvage.lootedPct : db.salvage.craftedPct) / 100; lowHalvedOk += g == full * db.salvage.lowIntegrityFactorPct / 100; }
			}
			if ( base <= 0 ) { continue; }
			const double ratio = 100.0 * r.massOut / (double)base;
			if ( r.looted ) { sumL += ratio; sumL2 += ratio * ratio; ++nL; if ( r.affixesIn > 0 ) { ++withAff; disch += r.discharged; } }
			else { sumC += ratio; ++nC; }
			if ( !r.discharged ) { affixes += r.affixesIn - (keep < it.nParts ? it.parts[keep].nAffix : 0); auras += r.auraCrystals; }
			if ( nL > 1000 && nC > 1000 ) // stop rule
			{
				const double mean = sumL / nL, var = sumL2 / nL - mean * mean, hw = 1.96 * std::sqrt(std::max(0.0, var) / nL);
				if ( hw < 0.05 && withAff > 40000 && affixes > 40000 ) { break; }
			}
		}
		lootedPct = nL ? sumL / nL : 0; craftedPct = nC ? sumC / nC : 0;
		dischargePct = withAff ? 100.0 * disch / withAff : 0; auraPct = affixes ? 100.0 * auras / affixes : 0;
		const double var = nL ? sumL2 / nL - lootedPct * lootedPct : 0;
		hwLooted = nL ? 1.96 * std::sqrt(std::max(0.0, var) / nL) : 0;
	};
	{
		double lp, cp, dp, ap, hw; int lowOk, lowN, items;
		salvageRun(false, lp, cp, dp, ap, hw, lowOk, lowN, items);
		const bool pass = std::fabs(lp - db.salvage.lootedPct) <= 1.0 && std::fabs(cp - db.salvage.craftedPct) <= 1.0
			&& std::fabs(dp - db.salvage.dischargePct) <= 1.0 && std::fabs(ap - db.salvage.auraPerAffixPct) <= 1.0 && lowOk == lowN && lowN > 0;
		gate("D86_salvage_components", craftedN > 0 && craftedOk == craftedN && keptN > 0 && keptOk == keptN,
			join({ kv("crafted_items", (int)craftedN), kv("crafted_every_part_a_component", (int)craftedOk), kv("other_items", (int)keptN), kv("chosen_part_a_component", (int)keptOk) }));
		gate("S4_salvage", pass, join({ kv("items", items), kv("looted_return_pct", lp), kv("looted_half_width", hw), kv("crafted_return_pct", cp),
			kv("discharge_pct", dp), kv("aura_per_affix_pct", ap), kv("low_integrity_parts", lowN), kv("low_integrity_halved", lowOk) }));
		double lp2, cp2, dp2, ap2, hw2;
		salvageRun(true, lp2, cp2, dp2, ap2, hw2, lowOk, lowN, items);
		const bool asym = std::fabs(lp2 - db.salvage.lootedPct) <= 1.0 && std::fabs(cp2 - db.salvage.craftedPct) <= 1.0;
		gate("S4_ctrl_symmetric", !asym, join({ kv("looted_return_pct", lp2), kv("crafted_return_pct", cp2), "\"note\":\"APX_SALVAGE_SYMMETRIC=1 must fail the asymmetry test\"" }));
	}
	{
		// D86 names: prefix/suffix from the affix table; a component salvages back to its material
		ApxItemData it; Counters c;
		std::string name; int found = 0;
		for ( uint32_t i = 0; i < 2000 && !found; ++i )
		{
			roll(db, *gearForms[i % gearForms.size()], 7, 30, 0, i, it, c);
			for ( int k = 0; k < it.nParts; ++k ) { if ( it.parts[k].nAffix >= 2 ) { name = componentName(db, it.parts[k]); found = k + 1; break; } }
		}
		const std::string mat = found ? db.materials[it.parts[found - 1].material].id : "";
		ApxItemData comp = found ? componentOf(db, it.parts[found - 1], 1, FLAG_LOOTED) : ApxItemData{};
		SalvageResult back = salvage(db, comp, 0, 5);
		const bool ok = found && name.size() > mat.size() && (name.find(" of ") != std::string::npos || name.find(' ') < name.size()) && isComponent(comp)
			&& back.components.empty() && back.grams.size() == 1 && back.massOut == partMassGrams(db, comp.parts[0]) * db.salvage.craftedPct / 100 * (comp.parts[0].integrity < db.salvage.lowIntegrityByte ? db.salvage.lowIntegrityFactorPct : 100) / 100;
		gate("D86_component_name", ok, join({ "\"name\":\"" + name + "\"", kv("affix_defaults", db.affixDefaults), kv("back_to_material_g", back.massOut) }));
	}
	{
		// LS44 quick salvage (05b 5.5): the top-rarity part drops loose at LOOSE_PCT[rarity]
		long long n[6] = {}, loose[6] = {};
		Counters c; ApxItemData it;
		for ( uint32_t i = 0; i < 200000; ++i )
		{
			roll(db, *gearForms[i % gearForms.size()], 11, 1 + i % 40, 0, i, it, c);
			if ( it.nParts == 0 ) { continue; }
			it.flags = (uint8_t)((it.flags & 0x3f) | FLAG_LOOTED);
			int top = 0;
			for ( int k = 0; k < it.nParts; ++k ) { top = std::max<int>(top, it.parts[k].rarity); }
			const SalvageResult r = salvage(db, it, -1, rollKey(11, 3, 9, i));
			++n[std::min(5, top)];
			loose[std::min(5, top)] += r.components.size() == 1 && r.loosePart >= 0 && r.components[0].parts[0].rarity == top;
		}
		bool ok = true;
		std::string rows;
		for ( int k = 0; k < 6; ++k )
		{
			const double pct = n[k] ? 100.0 * loose[k] / n[k] : -1;
			const double se = n[k] ? 100.0 * std::sqrt(LOOSE_PCT[k] / 100.0 * (1 - LOOSE_PCT[k] / 100.0) / n[k]) : 0; // 3 standard errors, at least 0.5 points
			if ( n[k] >= 500 && std::fabs(pct - LOOSE_PCT[k]) > std::max(0.5, 3 * se) ) { ok = false; }
			rows += (k ? "," : "") + std::to_string((int)(pct * 10) / 10.0).substr(0, 5);
		}
		gate("LS44_quick_salvage_loose", ok && n[0] + n[1] > 1000, "\"loose_pct_by_rarity\":[" + rows + "],\"n\":[" + std::to_string(n[0]) + "," + std::to_string(n[1]) + "," + std::to_string(n[2]) + "," + std::to_string(n[3]) + "," + std::to_string(n[4]) + "," + std::to_string(n[5]) + "],\"want\":[0,10,25,50,100,100]");
		// LS44 control: APX_BREAK_KEEP_MATERIAL=1 melts the chosen part, so break-keep must fail
		long long keepOk = 0, tried = 0;
		for ( uint32_t i = 0; i < 2000; ++i )
		{
			roll(db, *gearForms[i % gearForms.size()], 11, 20, 0, i, it, c);
			if ( it.nParts < 2 ) { continue; }
			it.flags = (uint8_t)((it.flags & 0x3f) | FLAG_LOOTED);
			++tried;
			keepOk += salvage(db, it, 0, 9, false, true).components.size() == 1;
		}
		gate("LS44_ctrl_keep_as_material", tried > 0 && keepOk == 0, kv("chosen_part_kept", (int)keepOk) + "," + kv("tried", (int)tried) + ",\"note\":\"the control must lose the chosen component\"");
	}
	{
		// C-name over 1,000 rolled components + fill order and weights (a test copy of the table: alternate
		// prefix/suffix, entry 0 of each pool weight 97, the rest 1)
		DB t = db;
		for ( auto& pl : t.pools ) { for ( int j = 0; j < pl.count; ++j ) { AffixInfo& a = t.affixes[(size_t)(pl.firstId + j)]; a.position = j % 2 ? AFFIX_SUFFIX : AFFIX_PREFIX; a.weight = j == 0 ? 97 : 1; a.label = j % 2 ? "of W" + std::to_string(j) : "P" + std::to_string(j); } }
		Counters c; ApxItemData it;
		int names = 0, bad = 0, commonOk = 0, commons = 0, order = 0, orderN = 0, proper = 0, properOk = 0; long long first0 = 0, firstN = 0; std::string properEx;
		std::string example;
		for ( uint32_t i = 0; names < 1000 || firstN < 20000; ++i )
		{
			roll(t, *gearForms[i % gearForms.size()], 13, 1 + i % 40, 0, i, it, c);
			for ( int k = 0; k < it.nParts; ++k )
			{
				const ApxPart& q = it.parts[k];
				const std::string nm = componentName(t, q);
				if ( names < 1000 )
				{
					++names;
					bool b = nm.empty() || nm.find("  ") != std::string::npos || nm.front() == ' ' || nm.back() == ' ';
					std::string prev, w;
					for ( size_t z = 0; z <= nm.size(); ++z ) { if ( z == nm.size() || nm[z] == ' ' ) { if ( !w.empty() && w == prev && w != "of" ) { b = true; } prev = w; w.clear(); } else { w += nm[z]; } }
					bad += b;
					if ( q.nAffix == 0 ) { ++commons; commonOk += nm.find(" of ") == std::string::npos && std::count(nm.begin(), nm.end(), ' ') <= 3; }
					if ( q.nAffix >= 2 && q.rarity < 3 && example.empty() ) { example = nm; }
					if ( q.rarity >= 3 ) { ++proper; properOk += nm.find("'s ") != std::string::npos && nm.find('-') != std::string::npos; if ( properEx.empty() ) { properEx = nm; } }
				}
				if ( q.nAffix >= 2 )
				{
					++orderN;
					order += t.affixes[q.affix[0]].position == AFFIX_PREFIX && t.affixes[q.affix[1]].position == AFFIX_SUFFIX;
				}
				if ( q.nAffix >= 1 )
				{
					const AffixPool* pl = nullptr;
					for ( auto& x : t.pools ) { if ( q.affix[0] >= x.firstId && q.affix[0] < x.firstId + x.count ) { pl = &x; } }
					if ( pl ) { int prefixes = 0, wsum = 0; for ( int j = 0; j < pl->count; j += 2 ) { ++prefixes; wsum += j == 0 ? 97 : 1; } (void)prefixes;
						++firstN; first0 += q.affix[0] == pl->firstId; }
				}
			}
		}
		// entry 0 is a prefix with weight 97 among the pool's prefixes (the rest weight 1): expected share 97 / (97 + prefixes - 1)
		double expect = 0; { const AffixPool& pl = t.pools[0]; int pre = (pl.count + 1) / 2; expect = 97.0 / (97 + pre - 1); }
		const double share = firstN ? (double)first0 / firstN : 0;
		gate("C_name", bad == 0 && commons > 0 && commonOk == commons && proper > 0 && properOk == proper, kv("names", names) + "," + kv("bad", bad) + "," + kv("commons_plain", commonOk) + "/" + std::to_string(commons) + ",\"example\":\"" + example + "\"," + kv("epic_up_proper", properOk) + "/" + std::to_string(proper) + ",\"proper_example\":\"" + properEx + "\"");
		gate("C2_fill_order_prefix_then_suffix", orderN > 1000 && order == orderN, kv("components", orderN) + "," + kv("ordered", order));
		gate("C2_weight", firstN > 0 && std::fabs(share - expect) < 0.03, "\"first_pick_share\":" + std::to_string(share) + ",\"approx_expected\":" + std::to_string(expect) + ",\"note\":\"pools differ in size; tolerance 0.03\"");
	}
	{
		// parts by slot, from rolls (the components a player could hold)
		std::vector<std::vector<ApxPart>> bySlot(db.slots.size());
		Counters c; ApxItemData it;
		for ( uint32_t i = 0; i < 20000; ++i )
		{
			roll(db, *gearForms[i % gearForms.size()], 17, 1 + i % 40, 0, i, it, c);
			for ( int k = 0; k < it.nParts; ++k ) { if ( bySlot[it.parts[k].slot].size() < 400 ) { bySlot[it.parts[k].slot].push_back(it.parts[k]); } }
		}
		for ( size_t sl = 0; sl < db.slots.size(); ++sl ) { if ( bySlot[sl].empty() ) { ApxPart q{}; q.slot = (uint8_t)sl; q.material = 0; q.quality = 5; q.integrity = 255; bySlot[sl].push_back(q); } }
		// LS41: every Form 1,000 times; deterministic; mismatches rejected; nothing dropped; IP cap holds
		uint64_t st = 12345;
		auto rnd = [&](int m) { st = st * 6364136223846793005ULL + 1442695040888963407ULL; return m > 0 ? (int)((st >> 33) % (uint64_t)m) : 0; };
		long long built = 0, okBuilt = 0, gated = 0, wrongAccepted = 0, wrongTried = 0, dropped = 0, overCap = 0, capRejects = 0, nondet = 0;
		for ( size_t fam = 0; fam < db.families.size(); ++fam )
		{
			const Family& f = db.families[fam];
			for ( int n = 0; n < 1000; ++n )
			{
				std::vector<ApxPart> parts;
				for ( int req : f.required ) { parts.push_back(bySlot[(size_t)req][(size_t)rnd((int)bySlot[(size_t)req].size())]); }
				for ( auto& o : f.optional ) { if ( rnd(2) ) { parts.push_back(bySlot[(size_t)o.slot][(size_t)rnd((int)bySlot[(size_t)o.slot].size())]); } }
				const int cap = ipCap(1 + rnd(40));
				ApxItemData a, b; std::string why, why2;
				const bool ok = assemble(db, (int)fam, parts, (uint32_t)n, cap, a, why);
				const bool ok2 = assemble(db, (int)fam, parts, (uint32_t)n, cap, b, why2);
				++built;
				nondet += ok != ok2 || (ok && memcmp(&a, &b, sizeof(a)));
				if ( ok ) { ++okBuilt; dropped += a.nParts != (int)parts.size(); overCap += a.power > cap; }
				else if ( why.find("opens at quality") != std::string::npos ) { ++gated; }
				else if ( why.find("over this depth") != std::string::npos ) { ++capRejects; }
				// a component from a slot this Form does not have must be refused
				for ( size_t sl = 0; sl < db.slots.size(); ++sl )
				{
					bool onForm = false;
					for ( int r : f.required ) { onForm = onForm || (size_t)r == sl; }
					for ( auto& o : f.optional ) { onForm = onForm || (size_t)o.slot == sl; }
					if ( onForm ) { continue; }
					std::vector<ApxPart> bad = parts; bad.push_back(bySlot[sl][0]);
					ApxItemData x; std::string w;
					++wrongTried; wrongAccepted += assemble(db, (int)fam, bad, 1, 1 << 30, x, w);
					break;
				}
			}
		}
		gate("LS41_assemble", okBuilt > 0 && wrongAccepted == 0 && dropped == 0 && overCap == 0 && nondet == 0,
			join({ kv("tries", (int)built), kv("assembled", (int)okBuilt), kv("refused_quality_gate", (int)gated), kv("refused_ip_cap", (int)capRejects),
				kv("wrong_slot_accepted", (int)wrongAccepted) + "/" + std::to_string(wrongTried), kv("parts_dropped", (int)dropped), kv("over_cap", (int)overCap), kv("nondeterministic", (int)nondet) }));
		// LS42: swap on crafted (every slot) and looted (quick slots only); affixes move with the component
		long long crTried = 0, crOk = 0, lootQuick = 0, lootQuickOk = 0, lootOther = 0, lootOtherRefused = 0, moved = 0, movedOk = 0;
		for ( uint32_t i = 0; i < 20000; ++i )
		{
			roll(db, *gearForms[i % gearForms.size()], 19, 1 + i % 40, 0, i, it, c);
			if ( it.nParts == 0 ) { continue; }
			const bool looted = i % 2;
			it.flags = (uint8_t)((it.flags & 0x3f) | (looted ? FLAG_LOOTED : FLAG_CRAFTED));
			const int k = (int)(i % it.nParts);
			const auto& pool = bySlot[it.parts[k].slot];
			const ApxPart comp = pool[(size_t)(i % pool.size())];
			ApxItemData t = it; ApxPart old{}; std::string why;
			const bool ok = swapPart(db, t, k, comp, 1 << 30, old, why);
			const bool quick = db.slots[it.parts[k].slot].quick;
			if ( !looted ) { ++crTried; crOk += ok; }
			else if ( quick ) { ++lootQuick; lootQuickOk += ok; }
			else { ++lootOther; lootOtherRefused += !ok; }
			if ( ok ) { ++moved; movedOk += !memcmp(&old, &it.parts[k], sizeof(ApxPart)) && !memcmp(&t.parts[k], &comp, sizeof(ApxPart)); }
		}
		gate("LS42_swap", crTried > 0 && crOk == crTried && lootOther > 0 && lootOtherRefused == lootOther && lootQuickOk == lootQuick && moved > 0 && movedOk == moved,
			join({ kv("crafted", (int)crOk) + "/" + std::to_string(crTried), kv("looted_quick", (int)lootQuickOk) + "/" + std::to_string(lootQuick),
				kv("looted_other_refused", (int)lootOtherRefused) + "/" + std::to_string(lootOther), kv("swaps_intact", (int)movedOk) + "/" + std::to_string(moved) }));
		// C5 make: cost = the part's mass; quality formula; Rare only at Masterwork (70+); no affixes
		int makes = 0, makeOk = 0, rare = 0;
		for ( size_t sl = 0; sl < db.slots.size(); ++sl )
		{
			for ( size_t m = 0; m < db.materials.size(); ++m )
			{
				for ( int stp = 0; stp <= 30; stp += 15 ) { for ( int tool = 0; tool <= 20; tool += 10 )
				{
					int q = 0;
					const ApxPart mp = makePart(db, (int)sl, (int)m, stp, tool, 0xC0FFEE07u, &q);
					const int want = 30 * std::min(9, db.materials[m].tier) / 9 + stp + 20 + tool;
					const int wantR = want >= 70 ? 2 : want >= 50 ? 1 : 0;
					++makes;
					rare += mp.rarity == 2;
					makeOk += q == want && mp.rarity == wantR && mp.nAffix == 0 && mp.look == 0xC0FFEE07u && mp.integrity == 255 && mp.quality == std::max(1, want / 10)
						&& makeCostGrams(db, (int)sl, (int)m) == std::max(1, partMassGrams(db, mp));
				} }
			}
		}
		ApxItemData mi = componentOf(db, makePart(db, 0, 0, 30, 20, 0xC0FFEE07u), 3, FLAG_CRAFTED), mo{};
		const std::string ms = toString(mi);
		const bool modelSaved = fromString(ms, mo) && !memcmp(&mi, &mo, sizeof(mi)) && mo.parts[0].look == 0xC0FFEE07u;
		gate("C5_make", makes > 0 && makeOk == makes && rare > 0 && modelSaved, join({ kv("makes", makes), kv("ok", makeOk), kv("rare_at_masterwork", rare), kv("model_id_saved", modelSaved ? 1 : 0), "\"save_string\":\"" + ms + "\"" }));
	}
	{
		// assembly names (05b section 4): best component; prefix + form + suffix up to Rare, proper name from Epic
		Counters c; ApxItemData it; int plain = 0, plainOk = 0, proper = 0, properOk = 0; std::string ex1, ex2;
		for ( uint32_t i = 0; i < 5000; ++i )
		{
			roll(db, *gearForms[i % gearForms.size()], 23, 1 + i % 40, 0, i, it, c);
			if ( it.nParts == 0 ) { continue; }
			int b = 0; for ( int k = 1; k < it.nParts; ++k ) { if ( it.parts[k].rarity > it.parts[b].rarity ) { b = k; } }
			const std::string n = assemblyName(db, it, "longsword");
			if ( it.parts[b].rarity >= 3 ) { ++proper; properOk += n.find("'s ") != std::string::npos && n.find("longsword") == std::string::npos; if ( ex2.empty() ) { ex2 = n; } }
			else { ++plain; properOk += 0; plainOk += n.find("longsword") != std::string::npos && (it.parts[b].nAffix == 0 ? n == "longsword" : n.size() > 9); if ( ex1.empty() && it.parts[b].nAffix ) { ex1 = n; } }
		}
		// unidentified (05b section 4): no affix words, no proper name
		int hid = 0, hidOk = 0;
		for ( uint32_t i = 0; i < 3000; ++i )
		{
			roll(db, *gearForms[i % gearForms.size()], 29, 1 + i % 40, 0, i, it, c);
			for ( int k = 0; k < it.nParts; ++k )
			{
				const ApxPart& q = it.parts[k];
				const std::string want = componentName(db, ApxPart{ q.slot, q.material, 0, 0, 0, 0, { 0, 0, 0, 0 }, 0, 0, 0 });
				++hid; hidOk += componentName(db, q, false) == want && want.find(" of ") == std::string::npos;
			}
			hidOk += 0;
			if ( it.nParts ) { ++hid; hidOk += assemblyName(db, it, "longsword", false) == "longsword"; }
		}
		gate("C_name_unidentified", hid > 0 && hidOk == hid, kv("names_hidden", hidOk) + "/" + std::to_string(hid));
		// STRIP (05b 5.4, D87): crafted gives every part and the item is gone; looted gives only optional quick slots
		int crN = 0, crOk = 0, ltN = 0, ltOk = 0, ltRefused = 0;
		for ( uint32_t i = 0; i < 20000; ++i )
		{
			roll(db, *gearForms[i % gearForms.size()], 31, 1 + i % 40, 0, i, it, c);
			if ( it.nParts == 0 ) { continue; }
			const bool looted = i % 2;
			it.flags = (uint8_t)((it.flags & 0x3f) | (looted ? FLAG_LOOTED : FLAG_CRAFTED));
			ApxItemData t = it; std::vector<ApxPart> rem; bool consumed = false; std::string why;
			const bool ok = stripParts(db, t, rem, consumed, why);
			if ( !looted ) { ++crN; crOk += ok && consumed && (int)rem.size() == it.nParts; continue; }
			int quickOpt = 0;
			const Family& f = db.families[it.form];
			for ( int k = 0; k < it.nParts; ++k )
			{
				bool req = false; for ( int r : f.required ) { req = req || r == it.parts[k].slot; }
				quickOpt += db.slots[it.parts[k].slot].quick && !req;
			}
			++ltN;
			if ( !quickOpt ) { ltRefused += !ok; ltOk += !ok; }
			else { ltOk += ok && !consumed && (int)rem.size() == quickOpt && t.nParts == it.nParts - quickOpt; }
		}
		gate("C4_strip", crN > 0 && crOk == crN && ltN > 0 && ltOk == ltN, join({ kv("crafted_all_parts", crOk) + "/" + std::to_string(crN), kv("looted_ok", ltOk) + "/" + std::to_string(ltN), kv("looted_refused_no_quick", ltRefused) }));
		gate("C_assembly_name", plain > 0 && plainOk == plain && proper > 0 && properOk == proper, join({ kv("plain_ok", plainOk) + "/" + std::to_string(plain), kv("proper_ok", properOk) + "/" + std::to_string(proper), "\"examples\":[\"" + ex1 + "\",\"" + ex2 + "\"]" }));
	}
	{
		// C6 infuse (05b 5.9): free slots = rarity; no duplicate; only an affix the slot can roll; aura crystals hold their affix
		int tried = 0, ok = 0, full = 0, fullRefused = 0, dup = 0, dupRefused = 0, foreign = 0, foreignRefused = 0;
		for ( size_t sl = 0; sl < db.slots.size(); ++sl )
		{
			for ( int rar = 0; rar <= 5; ++rar )
			{
				ApxPart p{}; p.slot = (uint8_t)sl; p.rarity = (uint8_t)rar;
				std::vector<uint16_t> fit, other;
				for ( size_t a = 0; a < db.affixes.size(); ++a ) { (affixFitsSlot(db, (int)sl, (uint16_t)a) ? fit : other).push_back((uint16_t)a); }
				std::string why;
				for ( size_t k = 0; k < fit.size() && k < 6; ++k )
				{
					const bool room = p.nAffix < std::min(4, rar);
					++tried;
					const bool r = infuse(db, p, fit[k], why);
					if ( room ) { ok += r; } else { ++full; fullRefused += !r; }
				}
				if ( p.nAffix > 0 ) { ++dup; ApxPart q = p; if ( q.nAffix < std::min(4, rar) ) { dupRefused += !infuse(db, q, p.affix[0], why); } else { dupRefused += 1; } }
				if ( !other.empty() ) { ++foreign; ApxPart q{}; q.slot = (uint8_t)sl; q.rarity = 4; foreignRefused += !infuse(db, q, other[0], why); }
			}
		}
		// salvage: crystals carry the affixes of the parts that became material
		Counters c; ApxItemData it; long long crystals = 0, crystalsOk = 0;
		for ( uint32_t i = 0; i < 20000; ++i )
		{
			roll(db, *gearForms[i % gearForms.size()], 37, 1 + i % 40, 0, i, it, c);
			if ( it.nParts < 2 ) { continue; }
			it.flags = (uint8_t)((it.flags & 0x3f) | FLAG_LOOTED);
			const SalvageResult r = salvage(db, it, 0, i);
			crystals += r.auraCrystals;
			for ( uint16_t a : r.auraAffixes ) { bool from = false; for ( int k = 1; k < it.nParts; ++k ) { for ( int j = 0; j < it.parts[k].nAffix; ++j ) { from = from || it.parts[k].affix[j] == a; } } crystalsOk += from; }
			crystalsOk += (long long)r.auraAffixes.size() == r.auraCrystals ? 0 : -1000000;
		}
		gate("C6_infuse", tried > 0 && ok > 0 && fullRefused == full && dupRefused == dup && foreignRefused == foreign && crystals > 0 && crystalsOk == crystals,
			join({ kv("infused", ok), kv("full_refused", fullRefused) + "/" + std::to_string(full), kv("dup_refused", dupRefused) + "/" + std::to_string(dup),
				kv("foreign_refused", foreignRefused) + "/" + std::to_string(foreign), kv("crystals_hold_their_affix", (int)crystalsOk) + "/" + std::to_string(crystals) }));
	}
	{
		// C7: grafts from Rare+ only (no affixes, Graft slot, best rarity, maker kept); derived sockets follow the components
		Counters c; ApxItemData it; int rare = 0, rareOk = 0, low = 0, lowRefused = 0, sock = 0, sockOk = 0, mount = 0, mountOk = 0;
		const int gs = graftSlot(db);
		for ( uint32_t i = 0; i < 20000; ++i )
		{
			roll(db, *gearForms[i % gearForms.size()], 41, 1 + i % 40, 0, i, it, c);
			if ( it.nParts == 0 ) { continue; }
			int best = 0; for ( int k = 1; k < it.nParts; ++k ) { if ( it.parts[k].rarity > it.parts[best].rarity ) { best = k; } }
			ApxPart g; std::string why;
			const bool ok = extractGraft(db, it, g, why);
			if ( it.parts[best].rarity >= 2 ) { ++rare; rareOk += ok && g.slot == gs && g.nAffix == 0 && g.rarity == it.parts[best].rarity && g.maker == it.parts[0].maker && g.material == it.parts[0].material; }
			else { ++low; lowRefused += !ok; }
			// a refreshed assembly's sockets equal min(4, best rarity)
			ApxItemData t = it; t.flags &= (FLAG_CRAFTED | FLAG_LOOTED); refreshDerived(db, t);
			++sock; sockOk += (t.flags & 7) == std::min(4, (int)it.parts[best].rarity) && t.power == itemPower(db, it);
			// the graft mounts in a Form's Graft slot when the required parts reach its gate
			if ( ok && (int)it.form < (int)db.families.size() )
			{
				const Family& f = db.families[it.form];
				int gate = -1; for ( auto& o : f.optional ) { if ( o.slot == gs ) { gate = o.q; } }
				if ( gate < 0 ) { continue; }
				std::vector<ApxPart> parts;
				for ( int r : f.required ) { for ( int k = 0; k < it.nParts; ++k ) { if ( it.parts[k].slot == r ) { ApxPart q = it.parts[k]; q.quality = 10; parts.push_back(q); break; } } }
				if ( parts.size() != f.required.size() ) { continue; }
				parts.push_back(g);
				ApxItemData a; std::string w;
				++mount; mountOk += assemble(db, (int)it.form, parts, 1, 1 << 30, a, w) && (a.flags & 7) == std::min(4, std::max<int>(g.rarity, 0) > 0 ? (int)std::max<uint8_t>(g.rarity, a.parts[0].rarity) : 0) ? 1 : 0;
			}
		}
		gate("C7_graft_sockets", rare > 0 && rareOk == rare && low > 0 && lowRefused == low && sockOk == sock && mount > 0 && mountOk == mount,
			join({ kv("grafts", rareOk) + "/" + std::to_string(rare), kv("below_rare_refused", lowRefused) + "/" + std::to_string(low), kv("sockets_follow", sockOk) + "/" + std::to_string(sock), kv("graft_mounted", mountOk) + "/" + std::to_string(mount) }));
	}
	printf("{\"summary\":true,\"failures\":%d}\n", failures);
	return failures ? 1 : 0;
}
