// F6: the seeded roll, Item Power and its cap, the save string, the packet blob, salvage.
// Card BF6 sections "Data model" and BF6-1/3/4/6. Integer arithmetic only; deterministic in its inputs.
#include <cctype>
#include "parts_model.hpp"
#include <algorithm>
#include <cstdio>
#include <cstring>

namespace apxparts {

uint64_t fnv1a64(const void* data, size_t n, uint64_t h)
{
	const uint8_t* p = (const uint8_t*)data;
	for ( size_t k = 0; k < n; ++k ) { h ^= p[k]; h *= 1099511628211ULL; }
	return h;
}

uint64_t rollKey(uint32_t realm, uint8_t depth, uint8_t source, uint32_t index)
{
	uint8_t b[10];
	for ( int k = 0; k < 4; ++k ) { b[k] = (uint8_t)(realm >> (8 * k)); }
	b[4] = depth;
	b[5] = source;
	for ( int k = 0; k < 4; ++k ) { b[6 + k] = (uint8_t)(index >> (8 * k)); }
	return fnv1a64(b, sizeof(b));
}

namespace {

struct SplitMix
{
	uint64_t s;
	explicit SplitMix(uint64_t seed) : s(seed) {}
	uint64_t next()
	{
		uint64_t z = (s += 0x9E3779B97F4A7C15ULL);
		z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ULL;
		z = (z ^ (z >> 27)) * 0x94D049BB133111EBULL;
		return z ^ (z >> 31);
	}
	int below(int m) { return m > 0 ? (int)(next() % (uint64_t)m) : 0; }
};

const int ROLE_WEIGHT[5] = { 4, 3, 2, 1, 1 }; // strike, structural, balance, grip, other (ip_params, ASSUMED)

// rarity by quality band (19 section 3): q 1-2 {85,15,0,0,0}, q 9-10 {0,0,5,35,60}, linear between
int rollRarity(SplitMix& r, int q)
{
	static const int lo[5] = { 85, 15, 0, 0, 0 }, hi[5] = { 0, 0, 5, 35, 60 };
	int w[5], t = q <= 2 ? 0 : q >= 9 ? 7 : q - 2, sum = 0;
	for ( int k = 0; k < 5; ++k ) { w[k] = (lo[k] * (7 - t) + hi[k] * t); sum += w[k]; }
	int x = r.below(sum);
	for ( int k = 0; k < 5; ++k ) { if ( x < w[k] ) { return k; } x -= w[k]; }
	return 0;
}

bool isGearFamily(const DB& db, int fam)
{
	const std::string& id = db.families[fam].id;
	return id != "Plain" && id != "Cloth" && id != "Tools";
}

// step 3: materials by depth (tier <= 2 + depth/3) from the type's default list; strike and
// structural parts take the type's main material when the tier allows, else the strongest allowed
int pickMaterial(const DB& db, const TypeForm& tf, Role role, int depth, SplitMix& r, bool gear)
{
	const int maxTier = 2 + depth / 3;
	std::vector<int> ok;
	for ( int m : tf.materials )
	{
		if ( db.materials[m].tier <= maxTier && !(gear && m == db.fleshMaterial) ) { ok.push_back(m); }
	}
	if ( ok.empty() ) // nothing allowed: the lowest tier of the list (never flesh on gear)
	{
		int best = -1;
		for ( int m : tf.materials ) { if ( !(gear && m == db.fleshMaterial) && (best < 0 || db.materials[m].tier < db.materials[best].tier) ) { best = m; } }
		return best >= 0 ? best : 0;
	}
	if ( role == ROLE_STRIKE || role == ROLE_STRUCTURAL )
	{
		int best = ok[0];
		for ( int m : ok ) { if ( db.materials[m].tier > db.materials[best].tier ) { best = m; } }
		return ok[0] == tf.materials[0] ? ok[0] : best;
	}
	if ( role == ROLE_GRIP ) { return ok.back(); }
	return ok[(size_t)r.below((int)ok.size())];
}

} // namespace

int itemPower(const DB& db, const ApxItemData& it)
{
	int ip = 0;
	for ( int k = 0; k < it.nParts && k < APX_MAX_PARTS; ++k )
	{
		const ApxPart& p = it.parts[k];
		const int role = p.slot < db.slots.size() ? db.slots[p.slot].role : ROLE_OTHER;
		const int tier = p.material < db.materials.size() ? db.materials[p.material].tier : 1;
		ip += ROLE_WEIGHT[role] * tier * (10 + 3 * p.rarity + p.quality);
	}
	return ip;
}

void roll(const DB& db, const TypeForm& tfIn, uint32_t realm, int depth, int source, uint32_t index, ApxItemData& out, Counters& c, bool capOn)
{
	++c.rolls;
	TypeForm tf = tfIn;
	if ( tf.family < 0 || tf.family >= (int)db.families.size() ) { ++c.forms_unmapped; tf.family = db.plainFamily; }
	if ( tf.materials.empty() ) { tf.materials.push_back(0); }
	depth = std::max(1, std::min(40, depth));
	const uint64_t key = rollKey(realm, (uint8_t)depth, (uint8_t)source, index);
	const Family& fam = db.families[tf.family];
	const bool gear = isGearFamily(db, tf.family);
	SplitMix head(key);
	// step 7 first: quality gates the optional slots of step 4
	const int quality = std::max(1, std::min(10, 1 + (depth + head.below(4)) / 4));
	std::vector<int> slots(fam.required.begin(), fam.required.end());
	const size_t nRequired = slots.size();
	for ( auto& o : fam.optional ) { if ( o.q <= quality ) { slots.push_back(o.slot); } }
	if ( (int)slots.size() > APX_MAX_PARTS ) { ++c.parts_overflow; slots.resize(APX_MAX_PARTS); }
	const int cap = ipCap(depth);
	for ( int attempt = 0; ; ++attempt )
	{
		SplitMix r(key ^ (0xA24BAED4963EE407ULL * (uint64_t)(attempt + 1)));
		memset(&out, 0, sizeof(out));
		out.form = (uint32_t)tf.family;
		out.rollKey = (uint32_t)key;
		out.nParts = (uint8_t)slots.size();
		int best = 0;
		for ( size_t k = 0; k < slots.size(); ++k )
		{
			ApxPart& p = out.parts[k];
			const Slot& s = db.slots[slots[k]];
			p.slot = (uint8_t)slots[k];
			p.material = (uint8_t)pickMaterial(db, tf, s.role, depth, r, gear);
			if ( gear && p.material == db.fleshMaterial ) { ++c.forbidden; }
			p.quality = (uint8_t)quality;
			p.rarity = (uint8_t)rollRarity(r, quality);
			p.integrity = (uint8_t)(40 + r.below(216));
			p.maker = 0;
			const AffixPool* pool = db.pool(tf.family, slots[k]);
			p.nAffix = pool ? (uint8_t)std::min<int>(p.rarity, std::min(4, pool->count)) : 0;
			// 05b section 4 fill order (G19:66): a prefix, then a suffix, then any; each pick weighted by the
			// affix's rarity weight among the unused entries of the wanted position (any position when none is left)
			bool used[64] = { false };
			for ( int a = 0; a < p.nAffix; ++a )
			{
				const int want = a == 0 ? AFFIX_PREFIX : a == 1 ? AFFIX_SUFFIX : -1;
				auto ok = [&](int j, bool posOnly) {
					if ( used[j] ) { return false; }
					const size_t id = (size_t)(pool->firstId + j);
					return !posOnly || want < 0 || (id < db.affixes.size() && db.affixes[id].position == want);
				};
				bool posOnly = false;
				for ( int j = 0; j < pool->count && j < 64 && !posOnly; ++j ) { posOnly = want >= 0 && ok(j, true); }
				int sum = 0;
				for ( int j = 0; j < pool->count && j < 64; ++j ) { if ( ok(j, posOnly) ) { const size_t id = (size_t)(pool->firstId + j); sum += id < db.affixes.size() ? db.affixes[id].weight : 1; } }
				int x = r.below(sum), pick = 0;
				for ( int j = 0; j < pool->count && j < 64; ++j )
				{
					if ( !ok(j, posOnly) ) { continue; }
					const size_t id = (size_t)(pool->firstId + j);
					x -= id < db.affixes.size() ? db.affixes[id].weight : 1;
					if ( x < 0 ) { pick = j; break; }
				}
				used[pick] = true;
				p.affix[a] = (uint16_t)(pool->firstId + pick);
			}
			if ( p.rarity > out.parts[best].rarity ) { best = (int)k; }
		}
		const int sockets = std::min(4, (int)out.parts[best].rarity);
		const int element = db.materials[out.parts[best].material].element & 7;
		out.flags = (uint8_t)(sockets | (element << 3));
		int ip = itemPower(db, out);
		if ( !capOn || ip <= cap ) { out.power = (uint16_t)std::min(ip, 65535); return; }
		// a reroll cannot help when the required parts alone, at rarity 0, exceed the cap (materials
		// come from the same depth-allowed list): go straight to the counted clamp
		// (every rolled slot stays in a reroll; only the clamp drops optional parts, so the test keeps them)
		ApxItemData floorOnly = out;
		for ( int k = 0; k < floorOnly.nParts; ++k ) { floorOnly.parts[k].rarity = 0; }
		const bool hopeless = itemPower(db, floorOnly) > cap;
		if ( attempt < 8 && !hopeless ) { ++c.ip_rerolls; if ( attempt == 0 ) { ++c.rolls_rerolled; } continue; }
		// still over: drop the highest optional part, then strip rarity, until under (counted)
		++c.ip_clamped;
		while ( ip > cap && out.nParts > nRequired ) { --out.nParts; memset(&out.parts[out.nParts], 0, sizeof(ApxPart)); ip = itemPower(db, out); }
		for ( int k = 0; ip > cap && k < out.nParts; ++k ) { out.parts[k].rarity = 0; out.parts[k].nAffix = 0; memset(out.parts[k].affix, 0, sizeof(out.parts[k].affix)); ip = itemPower(db, out); }
		for ( int k = out.nParts - 1; ip > cap && k >= 0; --k ) { out.parts[k].quality = 1; ip = itemPower(db, out); }
		// last: a part's material falls to the lowest tier of the type's list (never flesh on gear)
		int lowest = -1;
		for ( int m : tf.materials ) { if ( !(gear && m == db.fleshMaterial) && (lowest < 0 || db.materials[m].tier < db.materials[lowest].tier) ) { lowest = m; } }
		for ( int k = out.nParts - 1; ip > cap && k >= 0 && lowest >= 0; --k ) { out.parts[k].material = (uint8_t)lowest; ip = itemPower(db, out); }
		// still over (a steel or aethite haft at depth 1: 3 tier-4 parts exceed 340): the lowest
		// tier of all materials, flesh excepted
		int lowestAll = -1;
		for ( int m = 0; m < (int)db.materials.size(); ++m ) { if ( m != db.fleshMaterial && (lowestAll < 0 || db.materials[m].tier < db.materials[lowestAll].tier) ) { lowestAll = m; } }
		for ( int k = out.nParts - 1; ip > cap && k >= 0 && lowestAll >= 0; --k ) { out.parts[k].material = (uint8_t)lowestAll; ip = itemPower(db, out); }
		out.power = (uint16_t)std::min(ip, 65535);
		return;
	}
}

// ---------------------------------------------------------------- save string v1
std::string toString(const ApxItemData& it)
{
	if ( it.nParts == 0 ) { return ""; }
	char buf[96];
	std::string s;
	snprintf(buf, sizeof(buf), "v1;f=%u;k=%u;p=%u;n=%u;fl=%u;", it.form, it.rollKey, it.power, it.nParts, it.flags);
	s = buf;
	for ( int k = 0; k < it.nParts && k < APX_MAX_PARTS; ++k )
	{
		const ApxPart& p = it.parts[k];
		snprintf(buf, sizeof(buf), "P%u.%u.%u.%u.%u.%u.%u.%u-%u-%u-%u;", p.slot, p.material, p.quality, p.rarity, p.integrity, p.maker,
			p.nAffix, p.affix[0], p.affix[1], p.affix[2], p.affix[3]);
		s += buf;
	}
	char sum[24];
	snprintf(sum, sizeof(sum), "c=%08x;", (uint32_t)(fnv1a64(s.data(), s.size()) & 0xffffffffu));
	return s + sum;
}

namespace {
bool readU(const char*& c, unsigned long long maxv, unsigned long long& v)
{
	if ( *c < '0' || *c > '9' ) { return false; }
	v = 0;
	int digits = 0;
	while ( *c >= '0' && *c <= '9' ) { v = v * 10 + (unsigned)(*c - '0'); ++c; if ( ++digits > 12 ) { return false; } }
	return v <= maxv;
}
bool lit(const char*& c, const char* t) { size_t n = strlen(t); if ( strncmp(c, t, n) ) { return false; } c += n; return true; }
} // namespace

bool fromString(const std::string& s, ApxItemData& out)
{
	ApxItemData it{};
	const size_t semi = s.rfind("c=");
	if ( semi == std::string::npos || s.size() != semi + 11 || s.back() != ';' ) { return false; } // "c=xxxxxxxx;"
	char want[24];
	snprintf(want, sizeof(want), "c=%08x;", (uint32_t)(fnv1a64(s.data(), semi) & 0xffffffffu));
	if ( s.compare(semi, std::string::npos, want) ) { return false; } // checksum: a flipped char never decodes
	const std::string body = s.substr(0, semi);
	const char* c = body.c_str();
	unsigned long long v[8];
	if ( !lit(c, "v1;f=") || !readU(c, 0xffffffffULL, v[0]) || !lit(c, ";k=") || !readU(c, 0xffffffffULL, v[1]) || !lit(c, ";p=") || !readU(c, 65535, v[2])
		|| !lit(c, ";n=") || !readU(c, APX_MAX_PARTS, v[3]) || !lit(c, ";fl=") || !readU(c, 255, v[4]) || !lit(c, ";") ) { return false; }
	it.form = (uint32_t)v[0]; it.rollKey = (uint32_t)v[1]; it.power = (uint16_t)v[2]; it.nParts = (uint8_t)v[3]; it.flags = (uint8_t)v[4];
	for ( int k = 0; k < it.nParts; ++k )
	{
		ApxPart& p = it.parts[k];
		unsigned long long f[11];
		if ( !lit(c, "P") ) { return false; }
		for ( int j = 0; j < 7; ++j ) { if ( !readU(c, 255, f[j]) || !lit(c, j < 6 ? "." : ".") ) { return false; } }
		for ( int j = 0; j < 4; ++j ) { if ( !readU(c, 65535, f[7 + j]) || !lit(c, j < 3 ? "-" : ";") ) { return false; } }
		p.slot = (uint8_t)f[0]; p.material = (uint8_t)f[1]; p.quality = (uint8_t)f[2]; p.rarity = (uint8_t)f[3];
		p.integrity = (uint8_t)f[4]; p.maker = (uint8_t)f[5]; p.nAffix = (uint8_t)f[6];
		if ( p.nAffix > 4 ) { return false; }
		for ( int j = 0; j < 4; ++j ) { p.affix[j] = (uint16_t)f[7 + j]; }
	}
	if ( *c != '\0' ) { return false; }
	out = it;
	return true;
}

// ---------------------------------------------------------------- packet blob
int pack(const ApxItemData& it, uint8_t* o)
{
	const int n = std::min<int>(it.nParts, APX_MAX_PARTS);
	int i = 0;
	o[i++] = 1; // blob version
	for ( int k = 0; k < 4; ++k ) { o[i++] = (uint8_t)(it.form >> (8 * k)); }
	for ( int k = 0; k < 4; ++k ) { o[i++] = (uint8_t)(it.rollKey >> (8 * k)); }
	o[i++] = (uint8_t)it.power; o[i++] = (uint8_t)(it.power >> 8);
	o[i++] = (uint8_t)n;
	o[i++] = it.flags;
	for ( int k = 0; k < n; ++k )
	{
		const ApxPart& p = it.parts[k];
		o[i++] = p.slot; o[i++] = p.material; o[i++] = p.quality; o[i++] = p.rarity; o[i++] = p.integrity; o[i++] = p.nAffix;
		for ( int a = 0; a < 4; ++a ) { o[i++] = (uint8_t)p.affix[a]; o[i++] = (uint8_t)(p.affix[a] >> 8); }
		o[i++] = p.maker; o[i++] = p.pad;
	}
	return i;
}

bool unpack(const uint8_t* in, int len, ApxItemData& out)
{
	constexpr int HEAD = 1 + 8 + 2 + 1 + 1; // version, form, rollKey, power, nParts, flags
	if ( !in || len < HEAD || len > PACK_MAX || in[0] != 1 ) { return false; }
	const int n = in[11];
	if ( n > APX_MAX_PARTS || len != HEAD + 16 * n ) { return false; }
	ApxItemData it{};
	int i = 1;
	for ( int k = 0; k < 4; ++k ) { it.form |= (uint32_t)in[i++] << (8 * k); }
	for ( int k = 0; k < 4; ++k ) { it.rollKey |= (uint32_t)in[i++] << (8 * k); }
	it.power = (uint16_t)(in[i] | (in[i + 1] << 8)); i += 2;
	it.nParts = in[i++];
	it.flags = in[i++];
	for ( int k = 0; k < n; ++k )
	{
		ApxPart& p = it.parts[k];
		p.slot = in[i++]; p.material = in[i++]; p.quality = in[i++]; p.rarity = in[i++]; p.integrity = in[i++]; p.nAffix = in[i++];
		for ( int a = 0; a < 4; ++a ) { p.affix[a] = (uint16_t)(in[i] | (in[i + 1] << 8)); i += 2; }
		p.maker = in[i++]; p.pad = in[i++];
		if ( p.nAffix > 4 ) { return false; }
	}
	out = it;
	return true;
}

// ---------------------------------------------------------------- salvage
int partMassGrams(const DB& db, const ApxPart& p)
{
	const int vol = p.slot < db.slots.size() ? db.slots[p.slot].volume : 0;
	const int dens = p.material < db.materials.size() ? db.materials[p.material].density : 0;
	return (int)((long long)vol * dens / 1000); // cm^3 x kg/m^3 / 1000 = grams
}

SalvageResult salvage(const DB& db, const ApxItemData& it, int keepPart, uint64_t seed, bool symmetric, bool keepAsMaterial)
{
	SalvageResult r;
	const SalvageParams& sp = db.salvage;
	SplitMix rng(seed ^ 0x5A17A6E5ULL);
	const bool comp = isComponent(it);
	r.crafted = (it.flags & FLAG_CRAFTED) && !comp;
	r.looted = (it.flags & FLAG_LOOTED) && !(it.flags & FLAG_CRAFTED) && !comp;
	for ( int k = 0; k < it.nParts; ++k ) { r.affixesIn += it.parts[k].nAffix; }
	if ( r.crafted ) // D86: every part comes back as a component item
	{
		for ( int k = 0; k < it.nParts; ++k ) { r.components.push_back(componentOf(db, it.parts[k], it.rollKey + (uint32_t)k, it.flags)); }
		return r;
	}
	if ( comp ) { keepPart = -1; } // a component salvages to its material
	if ( keepAsMaterial ) { keepPart = -1; }
	const int pct = (r.looted && !symmetric) ? sp.lootedPct : sp.craftedPct;
	if ( r.looted && r.affixesIn > 0 && !symmetric ) { r.discharged = rng.below(100) < sp.dischargePct; }
	if ( r.looted && keepPart < 0 && !keepAsMaterial && it.nParts > 0 ) // quick salvage: maybe one loose part
	{
		int best = 0;
		for ( int k = 1; k < it.nParts; ++k ) { if ( it.parts[k].rarity > it.parts[best].rarity ) { best = k; } }
		const int rar = std::min<int>(5, it.parts[best].rarity);
		if ( rng.below(100) < LOOSE_PCT[rar] ) { keepPart = best; r.loosePart = best; }
	}
	for ( int k = 0; k < it.nParts; ++k )
	{
		const ApxPart& p = it.parts[k];
		if ( k == keepPart ) { continue; }
		const int mass = partMassGrams(db, p);
		r.massIn += mass;
		long long g = (long long)mass * pct / 100;
		if ( p.integrity < sp.lowIntegrityByte ) { g = g * sp.lowIntegrityFactorPct / 100; }
		r.massOut += (int)g;
		r.grams.push_back({ p.material, (int)g });
		if ( !r.discharged ) { for ( int a = 0; a < p.nAffix; ++a ) { if ( rng.below(100) < sp.auraPerAffixPct ) { ++r.auraCrystals; } } }
	}
	if ( keepPart >= 0 && keepPart < it.nParts )
	{
		ApxPart p = it.parts[keepPart];
		if ( r.discharged ) { p.nAffix = 0; memset(p.affix, 0, sizeof(p.affix)); }
		r.components.push_back(componentOf(db, p, it.rollKey + (uint32_t)keepPart, it.flags));
	}
	return r;
}

ApxItemData componentOf(const DB& db, const ApxPart& p, uint32_t key, uint8_t flags)
{
	ApxItemData c{};
	c.form = COMPONENT_FORM;
	c.rollKey = key;
	c.nParts = 1;
	c.flags = (uint8_t)(flags & (FLAG_CRAFTED | FLAG_LOOTED));
	c.parts[0] = p;
	c.power = (uint16_t)itemPower(db, c);
	return c;
}

std::string componentName(const DB& db, const ApxPart& p)
{
	auto cap = [](std::string s) { if ( !s.empty() ) { s[0] = (char)toupper((unsigned char)s[0]); } for ( auto& ch : s ) { if ( ch == '_' ) { ch = ' '; } } return s; };
	// 05b section 4 / content-lane lint: prefix words in affix order + Material + Slot + suffix words; at most
	// 2 prefixes and 2 suffixes shown; a second suffix drops its "of": "of Embers and Bleeding"
	std::string pre, suf;
	int nPre = 0, nSuf = 0;
	for ( int a = 0; a < p.nAffix && a < 4; ++a )
	{
		if ( p.affix[a] >= db.affixes.size() ) { continue; }
		const AffixInfo& ai = db.affixes[p.affix[a]];
		if ( ai.position == AFFIX_PREFIX ) { if ( nPre++ < 2 ) { pre += ai.label + " "; } continue; }
		if ( nSuf++ >= 2 ) { continue; }
		std::string w = ai.label;
		if ( w.compare(0, 3, "of ") != 0 ) { w = "of " + w; }
		suf += nSuf == 1 ? " " + w : " and " + w.substr(3);
	}
	const std::string mat = p.material < db.materials.size() ? cap(db.materials[p.material].id) : "?";
	const std::string slot = p.slot < db.slots.size() ? cap(db.slots[p.slot].id) : "Part";
	return pre + mat + " " + slot + suf;
}

} // namespace apxparts
