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
			for ( int k = 0; k < a.nParts; ++k ) { a.parts[k].pad = 0; }
			const int L = pack(a, buf);
			maxLen = std::max(maxLen, L);
			ok += unpack(buf, L, b) && !memcmp(&a, &b, sizeof(a));
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
	printf("{\"summary\":true,\"failures\":%d}\n", failures);
	return failures ? 1 : 0;
}
