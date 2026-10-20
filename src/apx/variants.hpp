// apx/variants.hpp -- creature variant tree (card BV1): the node table (generated from data/apx/variants/*.json by
// tools/apx/gen_variants.py) and the PURE rolling and loot functions. No Barony header here: tests/apx/variants_test.cpp
// covers it offline. The roll uses its own hash stream (FNV-1a 64 + splitmix64, the BF6 style) and never the engine RNG,
// so floor hashes and every other random stream stay exactly as they were.
#pragma once
#include <cstdint>
#include <cstring>
#include <vector>

namespace apx
{
namespace variants
{
	struct VLoot { const char* id; double p; };
	struct VNode
	{
		const char* id; int parent; int level;      // level 0 base, 1 type, 2 subtype, 3 rare
		double rarity; int dmin, dmax;
		double hp, dmg, speed, poise;
		const char* material; int tierShift; int maxGroup;
		const char* traits;                          // ';' separated "name:params"
		const char* tint; double scale; const char* light;
		int lootFirst, lootCount;                    // the node's RESOLVED loot table (parent's extended)
		const char* monster;                         // base nodes: the monster type name
	};
#include "variants_table.inc"
	inline int nodeCount() { return static_cast<int>(sizeof(V_NODES) / sizeof(V_NODES[0])); }
	inline const VNode& node(int i) { return V_NODES[i]; }
	inline const VLoot& lootRow(const VNode& n, int k) { return V_LOOT[n.lootFirst + k]; }

	inline int find(const char* id)
	{
		for ( int i = 0; i < nodeCount(); ++i ) { if ( std::strcmp(V_NODES[i].id, id) == 0 ) { return i; } }
		return -1;
	}

	inline uint64_t fnv1a64(const void* data, size_t n, uint64_t h = 1469598103934665603ULL)
	{
		const unsigned char* p = static_cast<const unsigned char*>(data);
		for ( size_t i = 0; i < n; ++i ) { h ^= p[i]; h *= 1099511628211ULL; }
		return h;
	}
	inline uint64_t splitmix64(uint64_t& s)
	{
		uint64_t z = (s += 0x9E3779B97F4A7C15ULL);
		z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ULL;
		z = (z ^ (z >> 27)) * 0x94D049BB133111EBULL;
		return z ^ (z >> 31);
	}
	inline double u01(uint64_t& s) { return (splitmix64(s) >> 11) * (1.0 / 9007199254740992.0); }

	// the stream for one spawn: floor seed + spawn index + base id (the same key on host and clients, every launch)
	inline uint64_t spawnStream(uint64_t floorSeed, uint32_t spawnIndex, const char* baseId)
	{
		uint64_t h = fnv1a64(&floorSeed, sizeof(floorSeed));
		h = fnv1a64(&spawnIndex, sizeof(spawnIndex), h);
		h = fnv1a64(baseId, std::strlen(baseId), h);
		return h;
	}

	inline int rootOf(int i) { while ( V_NODES[i].parent >= 0 ) { i = V_NODES[i].parent; } return i; }
	inline bool eligible(const VNode& n, int depth) { return depth >= n.dmin && depth <= n.dmax; }

	// Walk down the tree from the base: at each level draw among the depth-eligible children by rarity; the rest of the mass
	// stays with the current node. flat = the control: one draw over ALL non-base nodes of the base, ignoring the tree.
	inline int roll(uint64_t floorSeed, uint32_t spawnIndex, const char* baseId, int depth, bool flat = false)
	{
		int cur = find(baseId);
		if ( cur < 0 ) { return -1; }
		uint64_t s = spawnStream(floorSeed, spawnIndex, baseId);
		if ( flat )
		{
			// control: one draw over every eligible non-base node of this base, each weighted by its own rarity, the base by 1
			double tot = 1.0;
			for ( int i = 0; i < nodeCount(); ++i ) { if ( V_NODES[i].level > 0 && rootOf(i) == cur && eligible(V_NODES[i], depth) ) { tot += V_NODES[i].rarity; } }
			const double u = u01(s) * tot;
			double acc = 1.0;
			if ( u < acc ) { return cur; }
			for ( int i = 0; i < nodeCount(); ++i )
			{
				if ( V_NODES[i].level == 0 || rootOf(i) != cur || !eligible(V_NODES[i], depth) ) { continue; }
				acc += V_NODES[i].rarity;
				if ( u < acc ) { return i; }
			}
			return cur;
		}
		for ( ;; )
		{
			const double u = u01(s);
			double acc = 0.0;
			int next = -1;
			for ( int i = cur + 1; i < nodeCount(); ++i )
			{
				const VNode& n = V_NODES[i];
				if ( n.parent != cur || !eligible(n, depth) ) { continue; }
				acc += n.rarity;
				if ( u < acc ) { next = i; break; }
			}
			if ( next < 0 ) { return cur; }
			cur = next;
		}
	}

	// analytic probability that a spawn of this base ends on node i at this depth (the unit test compares rolls to it)
	inline double probability(int i, int depth)
	{
		if ( i < 0 || !eligible(V_NODES[i], depth) && V_NODES[i].level > 0 ) { return 0.0; }
		double p = 1.0;
		int chain[4], n = 0;
		for ( int a = i; a >= 0 && n < 4; a = V_NODES[a].parent ) { chain[n++] = a; }
		for ( int k = n - 1; k >= 1; --k ) { if ( !eligible(V_NODES[chain[k - 1]], depth) ) { return 0.0; } p *= V_NODES[chain[k - 1]].rarity; }
		double stay = 1.0;
		for ( int c = 0; c < nodeCount(); ++c ) { if ( V_NODES[c].parent == i && eligible(V_NODES[c], depth) ) { stay -= V_NODES[c].rarity; } }
		return p * stay;
	}

	// independent draw of the node's resolved loot table: each row drops with its own chance
	inline std::vector<const char*> drops(int nodeIdx, uint64_t& stream)
	{
		std::vector<const char*> out;
		const VNode& n = V_NODES[nodeIdx];
		for ( int k = 0; k < n.lootCount; ++k ) { const VLoot& r = lootRow(n, k); if ( u01(stream) < r.p ) { out.push_back(r.id); } }
		return out;
	}
}
}
