// apx/vox/heldmodel.cpp -- see heldmodel.hpp.
#include "../../main.hpp"
#include "../../init.hpp"
#include "../../items.hpp"
#include "../parts_engine.hpp"
#include "bx1.hpp"
#include "heldmodel.hpp"

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <string>
#include <unordered_map>
#include <vector>

namespace apx
{
namespace vox
{
	namespace
	{
		Stats g_stats;
		unsigned g_base = 0;
		std::unordered_map<uint32_t, int> g_byHash;           // content hash -> model index
		std::unordered_map<std::string, int> g_composed;      // (vanilla model, parts) -> model index
		constexpr unsigned kMaxRuntimeModels = 512;

		bool envOff(const char* k) { const char* v = std::getenv(k); return v && v[0] == '0'; }

		// part slot name (data/apx/forms.json) -> bx1 slot and its place from tip (0) to hand (5)
		bool slotOf(const char* name, bx1::Slot& s, int& order)
		{
			static const struct { const char* n; bx1::Slot s; int o; } m[] = {
				{ "Head", bx1::Slot::Head, 0 }, { "Blade", bx1::Slot::Blade, 1 }, { "Guard", bx1::Slot::Guard, 2 },
				{ "Haft", bx1::Slot::Haft, 3 }, { "Grip", bx1::Slot::Grip, 4 }, { "Pommel", bx1::Slot::Pommel, 5 },
			};
			for ( auto& e : m ) { if ( name && !strcmp(name, e.n) ) { s = e.s; order = e.o; return true; } }
			return false;
		}

		bx1::Rgb rgb(const float t[3])
		{
			// materials.json "tint" is a small additive light bonus (0..0.36: steel 0.04,0.08,0.16), not a colour:
			// the voxel colour is a neutral grey shifted by it (iron grey, steel blue-grey, bronze, gold, tan wood)
			auto c = [](float v) { return (uint8_t)std::min(255.f, std::max(0.f, 110.f + v * 480.f)); };
			return { c(t[0]), c(t[1]), c(t[2]) };
		}
	} // namespace

	bool heldEnabled() { static const bool on = !envOff("APX_VOX_HELD") && !envOff("APX_PARTS_VIS"); return on; }
	unsigned baseModelCount() { return g_base ? g_base : nummodels; }
	const Stats& stats() { return g_stats; }

	int registerModel(const bx1::VoxModel& m)
	{
		if ( !g_base ) { g_base = nummodels; }
		const uint32_t h = bx1::hash(m);
		auto it = g_byHash.find(h);
		if ( it != g_byHash.end() ) { ++g_stats.cacheHits; return it->second; }
		if ( nummodels - g_base >= kMaxRuntimeModels || m.data.empty() ) { return -1; }

		voxel_t* v = (voxel_t*)malloc(sizeof(voxel_t));
		if ( !v ) { return -1; }
		v->sizex = m.sx; v->sizey = m.sy; v->sizez = m.sz;
		v->data = (Uint8*)malloc(m.data.size());
		if ( !v->data ) { free(v); return -1; }
		memcpy(v->data, m.data.data(), m.data.size());
		for ( int i = 0; i < 256; ++i ) { for ( int c = 0; c < 3; ++c ) { v->palette[i][c] = (Uint8)(m.pal[i * 3 + c] << 2); } } // 6-bit, as loadVoxel

		const Uint32 idx = nummodels;
		voxel_t** nm = (voxel_t**)realloc(models, sizeof(voxel_t*) * (idx + 1));
		polymodel_t* np = (polymodel_t*)realloc(polymodels, sizeof(polymodel_t) * (idx + 1));
		if ( !nm || !np ) { if ( nm ) { models = nm; } if ( np ) { polymodels = np; } free(v->data); free(v); return -1; }
		models = nm; polymodels = np;
		memset(&polymodels[idx], 0, sizeof(polymodel_t));
		models[idx] = v;
		nummodels = idx + 1;
		generatePolyModels((int)idx, (int)idx + 1, true);
		generateVBOs((int)idx, (int)idx + 1);
		g_byHash[h] = (int)idx;
		++g_stats.registered;
		return (int)idx;
	}

	int heldModel(const Item* item, bool firstPerson, int vanillaModel)
	{
		if ( !item || !heldEnabled() || item->apx.nParts == 0 || vanillaModel < 0 || (Uint32)vanillaModel >= nummodels || !models[vanillaModel] ) { return vanillaModel; }
		const voxel_t* van = models[vanillaModel];

		// the cache key: the vanilla box this view uses and what the parts are
		std::string key = std::to_string(vanillaModel) + (firstPerson ? "f" : "t") + ":" + std::to_string(item->apx.rollKey);
		for ( int i = 0; i < item->apx.nParts && i < APX_MAX_PARTS; ++i ) { key += "," + std::to_string(item->apx.parts[i].slot) + "/" + std::to_string(item->apx.parts[i].material); }
		auto hit = g_composed.find(key);
		if ( hit != g_composed.end() ) { return hit->second >= 0 ? hit->second : vanillaModel; }

		// one generated model per modelled part (look 0: procedural from the item's roll key, D79)
		struct Piece { bx1::VoxModel m; int order; };
		std::vector<Piece> pieces;
		for ( int i = 0; i < item->apx.nParts && i < APX_MAX_PARTS; ++i )
		{
			const ApxPart& p = item->apx.parts[i];
			bx1::Slot s; int order;
			if ( !slotOf(apxPartsSlotName(p.slot), s, order) ) { ++g_stats.partsSkipped; continue; } // Edge, Fuller, Graft...: details, not shapes
			float t[3] = { 0.6f, 0.6f, 0.65f };
			apxPartsMaterialTint(p.material, t);
			const bx1::Rgb base = rgb(t);
			const bx1::Rgb accent = s == bx1::Slot::Grip ? bx1::Rgb{ 110, 70, 40 } : bx1::Rgb{ (uint8_t)(base.r * 3 / 4), (uint8_t)(base.g * 3 / 4), (uint8_t)(base.b * 3 / 4) };
			pieces.push_back({ bx1::makeComponent(s, (uint64_t)item->apx.rollKey * 1315423911ull + (uint64_t)i * 2654435761ull, base, accent), order });
		}
		if ( pieces.empty() ) { g_composed[key] = -1; return vanillaModel; }
		std::stable_sort(pieces.begin(), pieces.end(), [](const Piece& a, const Piece& b) { return a.order < b.order; });

		// stack tip to hand along z, centred in x and y; each piece gets its own 6 palette entries
		// each piece contributes only its occupied z range (generators leave empty layers at the tip)
		std::vector<std::pair<int, int>> zr(pieces.size());
		int X = 0, Y = 0, Z = 0;
		for ( size_t k = 0; k < pieces.size(); ++k )
		{
			const bx1::VoxModel& m = pieces[k].m;
			int lo = m.sz, hi = -1;
			for ( int z = 0; z < m.sz; ++z ) { bool any = false; for ( int x = 0; x < m.sx && !any; ++x ) for ( int y = 0; y < m.sy && !any; ++y ) { any = m.get(x, y, z) != 255; } if ( any ) { lo = std::min(lo, z); hi = z; } }
			if ( hi < lo ) { lo = 0; hi = -1; }
			zr[k] = { lo, hi };
			X = std::max(X, m.sx); Y = std::max(Y, m.sy); Z += hi - lo + 1;
		}
		if ( Z <= 0 ) { g_composed[key] = -1; return vanillaModel; }
		bx1::VoxModel full; full.resize(X, Y, Z);
		int z0 = 0;
		for ( size_t k = 0; k < pieces.size() && k < 40; ++k )
		{
			const bx1::VoxModel& m = pieces[k].m;
			const int base = 1 + (int)k * 6;
			for ( int c = 1; c <= 6; ++c ) { for ( int ch = 0; ch < 3; ++ch ) { full.pal[(base + c - 1) * 3 + ch] = m.pal[c * 3 + ch]; } }
			const int ox = (X - m.sx) / 2, oy = (Y - m.sy) / 2;
			for ( int x = 0; x < m.sx; ++x ) for ( int y = 0; y < m.sy; ++y ) for ( int z = zr[k].first; z <= zr[k].second; ++z )
			{
				const uint8_t c = m.get(x, y, z);
				if ( c != 255 && c >= 1 && c <= 6 ) { full.set(ox + x, oy + y, z0 + z - zr[k].first, (uint8_t)(base + c - 1)); }
			}
			z0 += zr[k].second - zr[k].first + 1;
		}

		// resample into the vanilla model's box: a target cell is filled when enough of its source cells are,
		// with the most common colour
		bx1::VoxModel out; out.resize(van->sizex, van->sizey, van->sizez);
		std::memcpy(out.pal, full.pal, 768);
		std::vector<int> hist(256);
		for ( int x = 0; x < out.sx; ++x ) for ( int y = 0; y < out.sy; ++y ) for ( int z = 0; z < out.sz; ++z )
		{
			const int x0 = x * X / out.sx, x1 = std::max(x0 + 1, (x + 1) * X / out.sx);
			const int y0 = y * Y / out.sy, y1 = std::max(y0 + 1, (y + 1) * Y / out.sy);
			const int zs = z * Z / out.sz, ze = std::max(zs + 1, (z + 1) * Z / out.sz);
			int filled = 0, cells = 0;
			std::fill(hist.begin(), hist.end(), 0);
			for ( int a = x0; a < x1; ++a ) for ( int b = y0; b < y1; ++b ) for ( int c = zs; c < ze; ++c )
			{
				++cells;
				const uint8_t col = full.get(a, b, c);
				if ( col != 255 ) { ++filled; ++hist[col]; }
			}
			if ( filled * 3 >= cells && filled > 0 ) { out.set(x, y, z, (uint8_t)(std::max_element(hist.begin(), hist.end()) - hist.begin())); }
		}
		if ( const char* dump = std::getenv("APX_VOX_DUMP") ) // inspection: write the stacked and the resampled model
		{
			bx1::write(std::string(dump) + "/composed-" + key.substr(0, key.find(':')) + "-full.vox", full);
			bx1::write(std::string(dump) + "/composed-" + key.substr(0, key.find(':')) + ".vox", out);
		}
		const int idx = registerModel(out);
		g_composed[key] = idx;
		if ( idx < 0 ) { return vanillaModel; }
		++g_stats.composed;
		return idx;
	}
} // namespace vox
} // namespace apx
