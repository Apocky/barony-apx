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
			p.integrity = (uint8_t)(40 + r.below(216)); p.foundDepth = (uint8_t)depth; // 05b s3 biography
			p.maker = 0;
			const AffixPool* pool = db.pool(tf.family, slots[k]);
			p.nAffix = pool ? (uint8_t)std::min<int>(p.rarity, std::min(APX_MAX_AFFIX, pool->count)) : 0;
			// 05b section 4 fill order (G19:66): a prefix, then a suffix, then any; each pick weighted by the
			// affix's rarity weight among the unused entries of the wanted position (any position when none is left)
			bool used[64] = { false };
			for ( int a = 0; a < p.nAffix; ++a )
			{
				const int want = a == 0 ? AFFIX_PREFIX : a == 1 ? AFFIX_SUFFIX : -1;
				auto ok = [&](int j, bool posOnly) {
					if ( used[j] ) { return false; }
					const size_t id = (size_t)(pool->firstId + j);
					ApxPart sofar = p; sofar.nAffix = (uint8_t)a;
					if ( affixConflicts(db, sofar, (uint16_t)id) ) { return false; }
					return !posOnly || want < 0 || (id < db.affixes.size() && db.affixes[id].position == want);
				};
				bool posOnly = false;
				for ( int j = 0; j < pool->count && j < 64 && !posOnly; ++j ) { posOnly = want >= 0 && ok(j, true); }
				int sum = 0;
				for ( int j = 0; j < pool->count && j < 64; ++j ) { if ( ok(j, posOnly) ) { const size_t id = (size_t)(pool->firstId + j); sum += id < db.affixes.size() ? db.affixes[id].weight : 1; } }
				if ( sum <= 0 ) { p.nAffix = (uint8_t)a; ++c.affix_starved; break; } // every entry used or in conflict: fewer affixes, counted (never a forced pick)
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
				p.mag[a] = (uint8_t)rollMagnitude(db, p.affix[a], p.rarity, r.next());
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

// ---------------------------------------------------------------- save string (v1 base; v2 = extension tokens)
// A part is "P<slot>.<material>.<quality>.<rarity>.<integrity>.<maker>.<nAffix>.<a0>-<a1>-<a2>-<a3>" then, in
// this order and only when set, ":l<look hex8>" (BX1), ":x<a4>" (the 5th, Unique affix), ":m<m0>.<m1>.<m2>.<m3>.<m4>"
// (affix magnitudes), ":b<kills>.<floors>.<found depth>" (biography), then ";". A string with no extension
// token anywhere is "v1;..." and byte-identical to the format before them; any token makes it "v2;...".
namespace {
bool partHasExt(const ApxPart& p)
{
	bool m = false;
	for ( int a = 0; a < APX_MAX_AFFIX; ++a ) { m = m || p.mag[a] != 0; }
	return p.look || p.nAffix > 4 || p.affix[4] || m || p.kills || p.floors || p.foundDepth;
}
}

std::string toString(const ApxItemData& it)
{
	if ( it.nParts == 0 ) { return ""; }
	char buf[128];
	std::string s;
	bool ext = false;
	for ( int k = 0; k < it.nParts && k < APX_MAX_PARTS; ++k ) { ext = ext || partHasExt(it.parts[k]); }
	snprintf(buf, sizeof(buf), "%s;f=%u;k=%u;p=%u;n=%u;fl=%u;", ext ? "v2" : "v1", it.form, it.rollKey, it.power, it.nParts, it.flags);
	s = buf;
	for ( int k = 0; k < it.nParts && k < APX_MAX_PARTS; ++k )
	{
		const ApxPart& p = it.parts[k];
		snprintf(buf, sizeof(buf), "P%u.%u.%u.%u.%u.%u.%u.%u-%u-%u-%u", p.slot, p.material, p.quality, p.rarity, p.integrity, p.maker,
			p.nAffix, p.affix[0], p.affix[1], p.affix[2], p.affix[3]);
		s += buf;
		if ( p.look ) { snprintf(buf, sizeof(buf), ":l%08x", p.look); s += buf; }
		if ( p.nAffix > 4 || p.affix[4] ) { snprintf(buf, sizeof(buf), ":x%u", p.affix[4]); s += buf; }
		bool m = false;
		for ( int a = 0; a < APX_MAX_AFFIX; ++a ) { m = m || p.mag[a] != 0; }
		if ( m ) { snprintf(buf, sizeof(buf), ":m%u.%u.%u.%u.%u", p.mag[0], p.mag[1], p.mag[2], p.mag[3], p.mag[4]); s += buf; }
		if ( p.kills || p.floors || p.foundDepth ) { snprintf(buf, sizeof(buf), ":b%u.%u.%u", p.kills, p.floors, p.foundDepth); s += buf; }
		s += ";";
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
	const bool v2 = lit(c, "v2;f=");
	if ( (!v2 && !lit(c, "v1;f=")) || !readU(c, 0xffffffffULL, v[0]) || !lit(c, ";k=") || !readU(c, 0xffffffffULL, v[1]) || !lit(c, ";p=") || !readU(c, 65535, v[2])
		|| !lit(c, ";n=") || !readU(c, APX_MAX_PARTS, v[3]) || !lit(c, ";fl=") || !readU(c, 255, v[4]) || !lit(c, ";") ) { return false; }
	it.form = (uint32_t)v[0]; it.rollKey = (uint32_t)v[1]; it.power = (uint16_t)v[2]; it.nParts = (uint8_t)v[3]; it.flags = (uint8_t)v[4];
	for ( int k = 0; k < it.nParts; ++k )
	{
		ApxPart& p = it.parts[k];
		unsigned long long f[11];
		if ( !lit(c, "P") ) { return false; }
		for ( int j = 0; j < 7; ++j ) { if ( !readU(c, 255, f[j]) || !lit(c, ".") ) { return false; } }
		for ( int j = 0; j < 4; ++j ) { if ( !readU(c, 65535, f[7 + j]) || (j < 3 && !lit(c, "-")) ) { return false; } }
		p.slot = (uint8_t)f[0]; p.material = (uint8_t)f[1]; p.quality = (uint8_t)f[2]; p.rarity = (uint8_t)f[3];
		p.integrity = (uint8_t)f[4]; p.maker = (uint8_t)f[5]; p.nAffix = (uint8_t)f[6];
		for ( int j = 0; j < 4; ++j ) { p.affix[j] = (uint16_t)f[7 + j]; }
		if ( v2 && lit(c, ":l") ) // BX1 look: exactly 8 lower-case hex digits, never 0
		{
			uint32_t lv = 0;
			for ( int h = 0; h < 8; ++h, ++c ) { const char ch = *c; if ( ch >= '0' && ch <= '9' ) { lv = lv << 4 | (uint32_t)(ch - '0'); } else if ( ch >= 'a' && ch <= 'f' ) { lv = lv << 4 | (uint32_t)(ch - 'a' + 10); } else { return false; } }
			if ( lv == 0 ) { return false; }
			p.look = lv;
		}
		if ( v2 && lit(c, ":x") ) { unsigned long long x; if ( !readU(c, 65535, x) ) { return false; } p.affix[4] = (uint16_t)x; }
		if ( v2 && lit(c, ":m") )
		{
			for ( int a = 0; a < APX_MAX_AFFIX; ++a ) { unsigned long long m; if ( !readU(c, 255, m) || (a < APX_MAX_AFFIX - 1 && !lit(c, ".")) ) { return false; } p.mag[a] = (uint8_t)m; }
		}
		if ( v2 && lit(c, ":b") )
		{
			unsigned long long b0, b1, b2;
			if ( !readU(c, 65535, b0) || !lit(c, ".") || !readU(c, 255, b1) || !lit(c, ".") || !readU(c, 255, b2) ) { return false; }
			p.kills = (uint16_t)b0; p.floors = (uint8_t)b1; p.foundDepth = (uint8_t)b2;
		}
		if ( !lit(c, ";") ) { return false; }
		if ( p.nAffix > APX_MAX_AFFIX ) { return false; }
	}
	if ( *c != '\0' ) { return false; }
	out = it;
	return true;
}

// ---------------------------------------------------------------- packet blob
// v3 (written): 32-byte rows = slot material quality rarity integrity nAffix, affix[5] u16 LE, mag[5], maker,
// pad, look u32 LE, kills u16 LE, floors, foundDepth, pad2. Read: v1 (16-byte rows: 4 affixes, maker, pad),
// v2 (v1 + look), v3.
int pack(const ApxItemData& it, uint8_t* o)
{
	const int n = std::min<int>(it.nParts, APX_MAX_PARTS);
	int i = 0;
	o[i++] = 3;
	for ( int k = 0; k < 4; ++k ) { o[i++] = (uint8_t)(it.form >> (8 * k)); }
	for ( int k = 0; k < 4; ++k ) { o[i++] = (uint8_t)(it.rollKey >> (8 * k)); }
	o[i++] = (uint8_t)it.power; o[i++] = (uint8_t)(it.power >> 8);
	o[i++] = (uint8_t)n;
	o[i++] = it.flags;
	for ( int k = 0; k < n; ++k )
	{
		const ApxPart& p = it.parts[k];
		o[i++] = p.slot; o[i++] = p.material; o[i++] = p.quality; o[i++] = p.rarity; o[i++] = p.integrity; o[i++] = p.nAffix;
		for ( int a = 0; a < APX_MAX_AFFIX; ++a ) { o[i++] = (uint8_t)p.affix[a]; o[i++] = (uint8_t)(p.affix[a] >> 8); }
		for ( int a = 0; a < APX_MAX_AFFIX; ++a ) { o[i++] = p.mag[a]; }
		o[i++] = p.maker; o[i++] = p.pad;
		for ( int b = 0; b < 4; ++b ) { o[i++] = (uint8_t)(p.look >> (8 * b)); }
		o[i++] = (uint8_t)p.kills; o[i++] = (uint8_t)(p.kills >> 8);
		o[i++] = p.floors; o[i++] = p.foundDepth; o[i++] = p.pad2;
	}
	return i;
}

bool unpack(const uint8_t* in, int len, ApxItemData& out)
{
	constexpr int HEAD = 1 + 8 + 2 + 1 + 1; // version, form, rollKey, power, nParts, flags
	if ( !in || len < HEAD || len > PACK_MAX || in[0] < 1 || in[0] > 3 ) { return false; }
	const int ver = in[0];
	const int n = in[11];
	const int row = ver == 1 ? PACK_ROW_V1 : ver == 2 ? PACK_ROW_V2 : PACK_ROW;
	if ( n > APX_MAX_PARTS || len != HEAD + row * n ) { return false; }
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
		const int na = ver == 3 ? APX_MAX_AFFIX : 4;
		for ( int a = 0; a < na; ++a ) { p.affix[a] = (uint16_t)(in[i] | (in[i + 1] << 8)); i += 2; }
		if ( ver == 3 ) { for ( int a = 0; a < APX_MAX_AFFIX; ++a ) { p.mag[a] = in[i++]; } }
		p.maker = in[i++]; p.pad = in[i++];
		if ( ver >= 2 ) { for ( int b = 0; b < 4; ++b ) { p.look |= (uint32_t)in[i++] << (8 * b); } }
		if ( ver == 3 ) { p.kills = (uint16_t)(in[i] | (in[i + 1] << 8)); i += 2; p.floors = in[i++]; p.foundDepth = in[i++]; p.pad2 = in[i++]; }
		if ( p.nAffix > (ver == 3 ? APX_MAX_AFFIX : 4) ) { return false; }
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
		if ( !r.discharged ) { for ( int a = 0; a < p.nAffix; ++a ) { if ( rng.below(100) < sp.auraPerAffixPct ) { ++r.auraCrystals; r.auraAffixes.push_back(p.affix[a]); r.auraMags.push_back(p.mag[a]); } } }
	}
	if ( keepPart >= 0 && keepPart < it.nParts )
	{
		ApxPart p = it.parts[keepPart];
		if ( r.discharged ) { p.nAffix = 0; memset(p.affix, 0, sizeof(p.affix)); memset(p.mag, 0, sizeof(p.mag)); }
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

std::string componentName(const DB& db, const ApxPart& p, bool known)
{
	auto cap = [](std::string s) { if ( !s.empty() ) { s[0] = (char)toupper((unsigned char)s[0]); } for ( auto& ch : s ) { if ( ch == '_' ) { ch = ' '; } } return s; };
	// 05b section 4 / content-lane lint: prefix words in affix order + Material + Slot + suffix words; at most
	// 2 prefixes and 2 suffixes shown; a second suffix drops its "of": "of Embers and Bleeding"
	if ( p.rarity >= 3 && known ) { ApxItemData t{}; t.nParts = 1; t.parts[0] = p; return properName(db, t); } // Epic and up: a proper name (D87)
	std::string pre, suf;
	int nPre = 0, nSuf = 0;
	for ( int a = 0; a < p.nAffix && a < APX_MAX_AFFIX && known; ++a )
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

namespace apxparts {

bool assemble(const DB& db, int family, const std::vector<ApxPart>& parts, uint32_t key, int ipCapValue, ApxItemData& out, std::string& why)
{
	if ( family < 0 || family >= (int)db.families.size() ) { why = "no such Form"; return false; }
	const Family& f = db.families[(size_t)family];
	if ( parts.size() > (size_t)APX_MAX_PARTS ) { why = "too many components"; return false; }
	int minQ = 100;
	for ( int req : f.required )
	{
		int n = 0;
		for ( auto& p : parts ) { if ( p.slot == req ) { ++n; minQ = std::min<int>(minQ, p.quality); } }
		if ( n != 1 ) { why = std::string(n ? "two components in slot " : "missing the required slot ") + db.slots[(size_t)req].id; return false; }
	}
	for ( auto& p : parts )
	{
		bool fits = false;
		for ( int req : f.required ) { fits = fits || p.slot == req; }
		for ( auto& o : f.optional )
		{
			if ( p.slot != o.slot ) { continue; }
			if ( minQ < o.q ) { why = db.slots[(size_t)o.slot].id + " opens at quality " + std::to_string(o.q) + " (required parts reach " + std::to_string(minQ) + ")"; return false; }
			fits = true;
		}
		if ( !fits ) { why = "a " + (p.slot < db.slots.size() ? db.slots[p.slot].id : std::string("?")) + " does not mount on " + f.id; return false; }
		int same = 0;
		for ( auto& q : parts ) { same += q.slot == p.slot; }
		if ( same > 1 ) { why = "two components in slot " + db.slots[p.slot].id; return false; }
	}
	ApxItemData a{};
	a.form = (uint32_t)family;
	a.rollKey = key;
	a.flags = FLAG_CRAFTED;
	// required slots first, in the Form's order (the main part is parts[0], as a roll makes it)
	for ( int req : f.required ) { for ( auto& p : parts ) { if ( p.slot == req ) { a.parts[a.nParts++] = p; } } }
	for ( auto& p : parts ) { bool req = false; for ( int r : f.required ) { req = req || p.slot == r; } if ( !req ) { a.parts[a.nParts++] = p; } }
	refreshDerived(db, a); // sockets, element, power from the components (C7)
	if ( a.power > ipCapValue ) { why = "Item Power " + std::to_string(a.power) + " over this depth's cap " + std::to_string(ipCapValue); return false; }
	out = a;
	return true;
}

bool swapAllowed(const DB& db, const ApxItemData& it, int k)
{
	if ( isComponent(it) || k < 0 || k >= it.nParts ) { return false; }
	const bool looted = (it.flags & FLAG_LOOTED) && !(it.flags & FLAG_CRAFTED);
	return !looted || (it.parts[k].slot < db.slots.size() && db.slots[it.parts[k].slot].quick);
}

bool swapPart(const DB& db, ApxItemData& it, int k, const ApxPart& c, int ipCapValue, ApxPart& old, std::string& why)
{
	if ( !swapAllowed(db, it, k) ) { why = isComponent(it) ? "a component is not an assembly" : "a looted item swaps only its quick slots; break it for the rest"; return false; }
	if ( c.slot != it.parts[k].slot ) { why = "the component does not fit that slot"; return false; }
	ApxItemData t = it;
	t.parts[k] = c;
	refreshDerived(db, t);
	if ( t.power > ipCapValue ) { why = "Item Power " + std::to_string(t.power) + " over this depth's cap " + std::to_string(ipCapValue); return false; }
	old = it.parts[k];
	it = t;
	return true;
}

int makeCostGrams(const DB& db, int slot, int material)
{
	ApxPart p{};
	p.slot = (uint8_t)slot; p.material = (uint8_t)material;
	return std::max(1, partMassGrams(db, p));
}

ApxPart makePart(const DB& db, int slot, int material, int stationPts, int toolPts, uint32_t look, int* quality100)
{
	ApxPart p{};
	p.slot = (uint8_t)slot; p.material = (uint8_t)material;
	const int tier = material >= 0 && material < (int)db.materials.size() ? db.materials[(size_t)material].tier : 1;
	const int q = std::min(100, 30 * std::min(9, tier) / 9 + std::max(0, std::min(30, stationPts)) + 20 + std::max(0, std::min(20, toolPts)));
	if ( quality100 ) { *quality100 = q; }
	p.quality = (uint8_t)std::max(1, q / 10);
	p.rarity = q >= 70 ? 2 : q >= 50 ? 1 : 0;
	p.nAffix = 0; // affix slots start empty; only INFUSE adds an affix (05b invariant 7)
	p.integrity = 255;
	p.look = look;
	return p;
}

} // namespace apxparts

namespace apxparts {

namespace {
int bestPart(const ApxItemData& it)
{
	int b = 0;
	for ( int k = 1; k < it.nParts; ++k ) { if ( it.parts[k].rarity > it.parts[b].rarity ) { b = k; } }
	return b;
}
}

std::string properName(const DB& db, const ApxItemData& it)
{
	static const char* const OWNER[] = { "Vey", "Orm", "Sabine", "Kael", "Idris", "Maren", "Tovah", "Ulric", "Nessa", "Brannoc", "Yselde", "Corvin", "Halde", "Rook", "Ilse", "Dunmar" };
	static const char* const FIRST[] = { "Ember", "Frost", "Grave", "Storm", "Ash", "Iron", "Moon", "Blood", "Thorn", "Gloom", "Star", "Wyrm", "Hollow", "Bright", "Raven", "Salt" };
	static const char* const SECOND[] = { "Tongue", "Fang", "Song", "Ward", "Bite", "Whisper", "Brand", "Heart", "Crown", "Wake", "Thirst", "Mark", "Veil", "Root", "Edge", "Call" };
	uint64_t h = fnv1a64(&it.rollKey, sizeof(it.rollKey), fnv1a64("propername", 10));
	for ( int k = 0; k < it.nParts; ++k ) { h = fnv1a64(&it.parts[k], sizeof(ApxPart), h); }
	std::string n = std::string(OWNER[h % 16]) + "'s " + FIRST[(h >> 8) % 16] + "-" + SECOND[(h >> 16) % 16];
	(void)db;
	return n;
}

std::string assemblyName(const DB& db, const ApxItemData& it, const std::string& formName, bool known)
{
	if ( it.nParts == 0 || isComponent(it) || !known ) { return formName; }
	const ApxPart& b = it.parts[bestPart(it)];
	if ( b.rarity >= 3 ) { return properName(db, it); }
	std::string pre, suf;
	for ( int a = 0; a < b.nAffix && a < APX_MAX_AFFIX; ++a )
	{
		if ( b.affix[a] >= db.affixes.size() ) { continue; }
		const AffixInfo& ai = db.affixes[b.affix[a]];
		if ( ai.position == AFFIX_PREFIX && pre.empty() ) { pre = ai.label + " "; }
		else if ( ai.position == AFFIX_SUFFIX && suf.empty() ) { suf = " " + (ai.label.compare(0, 3, "of ") ? "of " + ai.label : ai.label); }
	}
	return pre + formName + suf;
}

} // namespace apxparts

namespace apxparts {

bool stripParts(const DB& db, ApxItemData& it, std::vector<ApxPart>& removed, bool& consumed, std::string& why)
{
	removed.clear();
	consumed = false;
	if ( it.nParts == 0 || isComponent(it) ) { why = "not an assembly"; return false; }
	const bool looted = (it.flags & FLAG_LOOTED) && !(it.flags & FLAG_CRAFTED);
	if ( !looted )
	{
		for ( int k = 0; k < it.nParts; ++k ) { removed.push_back(it.parts[k]); }
		consumed = true;
		return true;
	}
	const Family* f = it.form < db.families.size() ? &db.families[it.form] : nullptr;
	ApxItemData t = it;
	t.nParts = 0;
	for ( int k = 0; k < it.nParts; ++k )
	{
		const ApxPart& p = it.parts[k];
		bool required = false;
		if ( f ) { for ( int r : f->required ) { required = required || r == p.slot; } }
		const bool quick = p.slot < db.slots.size() && db.slots[p.slot].quick;
		if ( quick && !required ) { removed.push_back(p); }
		else { t.parts[t.nParts++] = p; }
	}
	for ( int k = t.nParts; k < APX_MAX_PARTS; ++k ) { memset(&t.parts[k], 0, sizeof(ApxPart)); }
	if ( removed.empty() ) { why = "a looted item strips only its optional quick slots; it has none: break it instead"; return false; }
	refreshDerived(db, t);
	it = t;
	return true;
}

} // namespace apxparts

namespace apxparts {

int affixSlots(const ApxPart& p) { return std::min(APX_MAX_AFFIX, (int)p.rarity); } // Unique 5 (05b s3)

bool affixConflicts(const DB& db, const ApxPart& p, uint16_t affix)
{
	if ( affix >= db.affixes.size() ) { return false; }
	for ( int a = 0; a < p.nAffix && a < APX_MAX_AFFIX; ++a )
	{
		const uint16_t o = p.affix[a];
		for ( int c : db.affixes[affix].conflicts ) { if ( c == o ) { return true; } }
		if ( o < db.affixes.size() ) { for ( int c : db.affixes[o].conflicts ) { if ( c == affix ) { return true; } } }
	}
	return false;
}

int rollMagnitude(const DB& db, uint16_t affix, int rarity, uint64_t r)
{
	if ( affix >= db.affixes.size() ) { return 0; }
	const AffixInfo& ai = db.affixes[affix];
	const int t = std::max(1, std::min(5, rarity));
	const int lo = ai.magLo[t], hi = std::max(lo, ai.magHi[t]);
	return std::min(255, lo + (int)(r % (uint64_t)(hi - lo + 1)));
}

bool affixFitsSlot(const DB& db, int slot, uint16_t affix)
{
	for ( int f = -1; f < (int)db.families.size(); ++f )
	{
		const AffixPool* pl = db.pool(f, slot);
		if ( pl && affix >= pl->firstId && affix < pl->firstId + pl->count ) { return true; }
	}
	return false;
}

bool infuse(const DB& db, ApxPart& p, uint16_t affix, std::string& why, uint8_t mag)
{
	if ( affix >= db.affixes.size() ) { why = "no such affix"; return false; }
	if ( p.nAffix >= affixSlots(p) ) { why = "no free affix slot (a part has as many as its rarity)"; return false; }
	for ( int a = 0; a < p.nAffix; ++a ) { if ( p.affix[a] == affix ) { why = "that affix is already on the part"; return false; } }
	if ( affixConflicts(db, p, affix ) ) { why = "that affix conflicts with one already on the part"; return false; }
	if ( !affixFitsSlot(db, p.slot, affix) ) { why = "that affix does not take on a " + (p.slot < db.slots.size() ? db.slots[p.slot].id : std::string("?")); return false; }
	p.mag[p.nAffix] = mag ? mag : (uint8_t)std::min(255, db.affixes[affix].magLo[std::max(1, std::min(5, (int)p.rarity))]);
	p.affix[p.nAffix++] = affix;
	return true;
}

} // namespace apxparts

namespace apxparts {

void refreshDerived(const DB& db, ApxItemData& it)
{
	if ( it.nParts == 0 ) { return; }
	int best = 0;
	for ( int k = 1; k < it.nParts; ++k ) { if ( it.parts[k].rarity > it.parts[best].rarity ) { best = k; } }
	const int sockets = isComponent(it) ? 0 : std::min(4, (int)it.parts[best].rarity);
	const int element = it.parts[best].material < db.materials.size() ? (db.materials[it.parts[best].material].element & 7) : 0;
	it.flags = (uint8_t)((it.flags & (FLAG_CRAFTED | FLAG_LOOTED)) | sockets | (element << 3));
	it.power = (uint16_t)itemPower(db, it);
}

int graftSlot(const DB& db) { return db.slot("Graft"); }

bool extractGraft(const DB& db, const ApxItemData& it, ApxPart& g, std::string& why)
{
	if ( it.nParts == 0 || isComponent(it) ) { why = "extract a graft from an assembly"; return false; }
	int best = 0, q = 0;
	for ( int k = 0; k < it.nParts; ++k ) { if ( it.parts[k].rarity > it.parts[best].rarity ) { best = k; } q += it.parts[k].quality; }
	if ( it.parts[best].rarity < 2 ) { why = "only a Rare or better item yields a graft"; return false; }
	const int gs = graftSlot(db);
	if ( gs < 0 ) { why = "no Graft slot in forms.json"; return false; }
	g = ApxPart{};
	g.slot = (uint8_t)gs;
	g.material = it.parts[0].material;
	g.quality = (uint8_t)std::max(1, q / it.nParts);
	g.rarity = it.parts[best].rarity;
	g.integrity = 255;
	g.maker = it.parts[0].maker;
	g.nAffix = 0; // a graft carries a maker's gimmick, not affixes (invariant 7: affixes only by roll or infuse)
	return true;
}

} // namespace apxparts
