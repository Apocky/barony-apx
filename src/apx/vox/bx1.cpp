// apx/vox/bx1.cpp -- see bx1.hpp.
#include "bx1.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <fstream>

namespace apx
{
namespace bx1
{
	int VoxModel::count() const
	{
		int n = 0;
		for ( uint8_t b : data ) { n += b != 255; }
		return n;
	}

	// ------------------------------------------------------------------ format
	std::vector<uint8_t> toBytes(const VoxModel& v)
	{
		std::vector<uint8_t> out;
		out.reserve(12 + v.data.size() + 768);
		auto put32 = [&](int32_t x) { for ( int k = 0; k < 4; ++k ) { out.push_back((uint8_t)((uint32_t)x >> (8 * k))); } };
		put32(v.sx); put32(v.sy); put32(v.sz);
		out.insert(out.end(), v.data.begin(), v.data.end());
		out.insert(out.end(), v.pal, v.pal + 768);
		return out;
	}

	bool readBytes(const std::vector<uint8_t>& b, VoxModel& out, std::string& err)
	{
		if ( b.size() < 12 ) { err = "short header"; return false; }
		auto get32 = [&](size_t o) { return (int32_t)((uint32_t)b[o] | ((uint32_t)b[o + 1] << 8) | ((uint32_t)b[o + 2] << 16) | ((uint32_t)b[o + 3] << 24)); };
		const int32_t x = get32(0), y = get32(4), z = get32(8);
		if ( x == 542658390 ) { err = "MagicaVoxel file, not Barony's slab format"; return false; }
		if ( x <= 0 || y <= 0 || z <= 0 || x > 1024 || y > 1024 || z > 1024 ) { err = "bad dimensions"; return false; }
		const size_t n = (size_t)x * y * z;
		out.sx = x; out.sy = y; out.sz = z;
		out.truncated = b.size() < 12 + n + 768;
		// a short file is read the way Barony's loadVoxel reads it: the data buffer starts as 0 (memset), the
		// bytes present overwrite it, the palette keeps 0 where the file ends (one Steam model, ceiling_bbrick.vox)
		out.data.assign(n, 0);
		const size_t have = std::min(n, b.size() - 12);
		std::copy(b.begin() + 12, b.begin() + 12 + have, out.data.begin());
		std::fill(out.pal, out.pal + 768, 0);
		if ( b.size() > 12 + n ) { const size_t p = std::min<size_t>(768, b.size() - 12 - n); std::copy(b.begin() + 12 + n, b.begin() + 12 + n + p, out.pal); }
		return true;
	}

	bool read(const std::string& path, VoxModel& out, std::string& err)
	{
		std::ifstream f(path, std::ios::binary);
		if ( !f ) { err = "cannot open " + path; return false; }
		std::vector<uint8_t> b((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
		return readBytes(b, out, err);
	}

	bool write(const std::string& path, const VoxModel& v)
	{
		const auto b = toBytes(v);
		std::ofstream f(path, std::ios::binary);
		if ( !f ) { return false; }
		f.write((const char*)b.data(), (std::streamsize)b.size());
		return (bool)f;
	}

	uint32_t hash(const VoxModel& v)
	{
		uint32_t h = 2166136261u;
		for ( uint8_t c : toBytes(v) ) { h ^= c; h *= 16777619u; }
		return h ? h : 1u; // 0 means "procedural" in the look field
	}

	// ------------------------------------------------------------------ slots
	const char* slotName(Slot s)
	{
		static const char* n[] = { "Blade", "Grip", "Guard", "Pommel", "Haft", "Head" };
		return (int)s >= 0 && s < Slot::NumSlots ? n[(int)s] : "?";
	}
	const char* addonName(Addon a)
	{
		static const char* n[] = { "Horn", "Crystal", "Plate", "Spur" };
		return (int)a >= 0 && a < Addon::NumAddons ? n[(int)a] : "?";
	}

	const SlotRule& slotRule(Slot s)
	{
		// volumes in voxels; mounts are where the part meets the Form's other parts (z = sz-1 faces the hand)
		static const SlotRule rules[] = {
			{ 7, 3, 40, 640, { { 3, 1, 39 } } },               // Blade: tang at the hand end
			{ 4, 4, 14, 192, { { 2, 2, 0 }, { 2, 2, 13 } } },  // Grip: both ends (volume 224)
			{ 15, 4, 3, 120, { { 7, 2, 1 } } },                // Guard: the centre the grip passes through
			{ 5, 5, 5, 100, { { 2, 2, 0 } } },                 // Pommel: the face toward the grip
			{ 4, 4, 48, 600, { { 2, 2, 0 }, { 2, 2, 47 } } },  // Haft: both ends
			{ 15, 5, 14, 700, { { 7, 2, 13 } } },              // Head: the socket on the haft
		};
		return rules[std::min(std::max((int)s, 0), (int)Slot::NumSlots - 1)];
	}

	// ------------------------------------------------------------------ palette and rng
	void paletteRamp(VoxModel& v, Rgb base, Rgb accent)
	{
		std::fill(v.pal, v.pal + 768, 0);
		auto put = [&](int i, double r, double g, double b) {
			v.pal[i * 3 + 0] = (uint8_t)std::min(63.0, std::max(0.0, r / 4.0));
			v.pal[i * 3 + 1] = (uint8_t)std::min(63.0, std::max(0.0, g / 4.0));
			v.pal[i * 3 + 2] = (uint8_t)std::min(63.0, std::max(0.0, b / 4.0));
		};
		put(1, base.r * 0.55, base.g * 0.55, base.b * 0.55);             // dark
		put(2, base.r, base.g, base.b);                                  // base
		put(3, base.r * 1.25 + 20, base.g * 1.25 + 20, base.b * 1.25 + 20); // light
		put(4, base.r * 0.5 + 120, base.g * 0.5 + 120, base.b * 0.5 + 120); // edge / highlight
		put(5, accent.r, accent.g, accent.b);                            // accent (wrap, gem)
		put(6, accent.r * 0.6, accent.g * 0.6, accent.b * 0.6);          // accent dark
	}

	namespace
	{
		struct Rng
		{
			uint64_t s;
			uint64_t next() { uint64_t z = (s += 0x9E3779B97F4A7C15ull); z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull; z = (z ^ (z >> 27)) * 0x94D049BB133111EBull; return z ^ (z >> 31); }
			int range(int lo, int hi) { return lo + (int)(next() % (uint64_t)(hi - lo + 1)); }   // inclusive
			double unit() { return (next() >> 11) * (1.0 / 9007199254740992.0); }
		};
		enum : uint8_t { DARK = 1, BASE = 2, LIGHT = 3, EDGE = 4, ACC = 5, ACCD = 6 };

		void blade(VoxModel& v, Rng& r)
		{
			const SlotRule& R = slotRule(Slot::Blade);
			v.resize(R.sx, R.sy, R.sz);
			const int L = r.range(24, 39);                    // blade length; tang occupies the last 3 cells
			const int w0 = r.range(1, 2);                     // half width at the base (x = 3 +- w0): 3 or 5 wide, like Barony's blades
			const int tip = r.range(0, 2);                    // 0 point, 1 clipped, 2 round
			const double curve = r.unit() < 0.35 ? r.range(1, 2) : 0.0;
			const bool fuller = r.unit() < 0.6;
			const int top = R.sz - 1 - L;                     // first z of the blade
			for ( int z = R.sz - 1; z > top; --z )
			{
				const double t = double(R.sz - 1 - z) / L;    // 0 at the hand, 1 at the tip
				int half = w0;
				if ( z >= R.sz - 3 ) { half = 0; }            // tang
				else if ( tip == 0 && t > 0.7 ) { half = (int)std::lround(w0 * (1.0 - (t - 0.7) / 0.3)); }
				else if ( tip == 1 && t > 0.85 ) { half = std::max(0, w0 - 1); }
				else if ( tip == 2 && t > 0.9 ) { half = std::max(0, w0 - 1); }
				const int off = (int)std::lround(curve * t * t);   // a gentle curve toward +x
				for ( int dx = -half; dx <= half; ++dx )
				{
					const int x = std::min(R.sx - 1, std::max(0, 3 + dx + off));
					const bool edge = std::abs(dx) == half && half > 0;
					// one voxel thick (Barony's blades are flat); a centre ridge or fuller on the face side
					v.set(x, 1, z, edge ? EDGE : (fuller && dx == 0 && t > 0.1 && t < 0.75 ? DARK : BASE));
					if ( dx == 0 && half > 0 && !fuller ) { v.set(x, 0, z, LIGHT); }
				}
				v.set(std::min(R.sx - 1, std::max(0, 3 + off)), 1, z, z >= R.sz - 3 ? DARK : v.get(std::min(R.sx - 1, std::max(0, 3 + off)), 1, z));
			}
			v.set(3, 1, R.sz - 1, DARK); // the tang end is the mount
			// keep the spine connected where the curve shifts columns
			for ( int z = R.sz - 2; z > top; --z ) { const int a = 3 + (int)std::lround(curve * std::pow(double(R.sz - 1 - z) / L, 2)); const int b = 3 + (int)std::lround(curve * std::pow(double(R.sz - 2 - z) / L, 2)); for ( int x = std::min(a, b); x <= std::max(a, b); ++x ) { if ( v.get(x, 1, z) == 255 ) { v.set(x, 1, z, BASE); } } }
		}

		void cylinder(VoxModel& v, int cx, int cy, int z0, int z1, double rad, uint8_t c0, uint8_t c1, int band)
		{
			for ( int z = z0; z <= z1; ++z )
				for ( int x = 0; x < v.sx; ++x )
					for ( int y = 0; y < v.sy; ++y )
					{
						const double dx = x + 0.5 - cx, dy = y + 0.5 - cy;
						if ( dx * dx + dy * dy <= rad * rad ) { v.set(x, y, z, band > 0 && ((z - z0) / band) % 2 ? c1 : c0); }
					}
		}

		void grip(VoxModel& v, Rng& r)
		{
			const SlotRule& R = slotRule(Slot::Grip);
			v.resize(R.sx, R.sy, R.sz);
			const double rad = 1.0 + r.unit() * 0.7;
			const int band = r.range(0, 3);
			cylinder(v, 2, 2, 0, R.sz - 1, rad, ACC, ACCD, band);
			const int flare = r.range(0, 2);                  // 0 none, 1 flared ends, 2 a ring in the middle
			if ( flare == 1 ) { cylinder(v, 2, 2, 0, 1, 1.9, ACCD, ACCD, 0); cylinder(v, 2, 2, R.sz - 2, R.sz - 1, 1.9, ACCD, ACCD, 0); }
			if ( flare == 2 ) { const int m = r.range(4, 9); cylinder(v, 2, 2, m, m, 1.9, DARK, DARK, 0); }
			for ( int z = 0; z < R.sz; ++z ) { if ( v.get(2, 2, z) == 255 ) { v.set(2, 2, z, ACC); } }
		}

		void guard(VoxModel& v, Rng& r)
		{
			const SlotRule& R = slotRule(Slot::Guard);
			v.resize(R.sx, R.sy, R.sz);
			const int half = r.range(5, 7);                    // the guard sets the composed width (Barony's spans the box)
			const bool upturn = r.unit() < 0.5;
			const bool knobs = r.unit() < 0.4;
			const bool thin = r.unit() < 0.3;
			for ( int dx = -half; dx <= half; ++dx )
				for ( int y = 1; y <= 2; ++y )
				{
					const int x = 7 + dx;
					if ( thin && y == 1 && std::abs(dx) > 1 ) { continue; }
					v.set(x, y, 1, std::abs(dx) == half ? LIGHT : BASE);
					if ( upturn && std::abs(dx) >= half - 1 ) { v.set(x, y, 0, LIGHT); }
					if ( knobs && std::abs(dx) == half ) { v.set(x, y, 0, EDGE); v.set(x, y, 2, EDGE); }
				}
			v.set(7, 2, 1, BASE);
			v.set(7, 1, 2, DARK); v.set(7, 2, 2, DARK);    // the collar under the grip
		}

		void pommel(VoxModel& v, Rng& r)
		{
			const SlotRule& R = slotRule(Slot::Pommel);
			v.resize(R.sx, R.sy, R.sz);
			const int kind = r.range(0, 3);                   // 0 sphere, 1 disk, 2 faceted (octahedron), 3 cap with a spike
			const double rad = 1.5 + r.unit() * 1.0;
			const double sq = 0.6 + r.unit() * 0.8;            // disk squash
			for ( int x = 0; x < R.sx; ++x )
				for ( int y = 0; y < R.sy; ++y )
					for ( int z = 0; z < R.sz; ++z )
					{
						const double dx = x - 2.0, dy = y - 2.0, dz = z - 2.0;
						bool in = false;
						if ( kind == 0 ) { in = dx * dx + dy * dy + dz * dz <= rad * rad; }
						else if ( kind == 1 ) { in = dx * dx + (dy * dy) / (sq * sq) + dz * dz * 2.2 <= rad * rad; }
						else if ( kind == 2 ) { in = std::abs(dx) + std::abs(dy) + std::abs(dz) <= rad + 0.6; }
						else { in = z <= 2 && dx * dx + dy * dy <= rad * rad; if ( z > 2 && std::abs(dx) + std::abs(dy) < 1 ) { in = true; } }
						if ( in ) { v.set(x, y, z, z == 0 ? DARK : (dx + dy < 0 ? LIGHT : BASE)); }
					}
			for ( int z = 0; z <= 2; ++z ) { if ( v.get(2, 2, z) == 255 ) { v.set(2, 2, z, BASE); } }
			v.set(2, 2, 0, DARK);
		}

		void haft(VoxModel& v, Rng& r)
		{
			const SlotRule& R = slotRule(Slot::Haft);
			v.resize(R.sx, R.sy, R.sz);
			cylinder(v, 2, 2, 0, R.sz - 1, 1.3 + r.unit() * 0.5, BASE, DARK, r.range(4, 9));
			v.set(2, 2, 0, BASE); v.set(2, 2, R.sz - 1, BASE);
		}

		void head(VoxModel& v, Rng& r)
		{
			const SlotRule& R = slotRule(Slot::Head);
			v.resize(R.sx, R.sy, R.sz);
			const bool axe = r.unit() < 0.5;
			// socket column on the haft line
			for ( int z = 4; z < R.sz; ++z ) { for ( int y = 1; y <= 3; ++y ) { v.set(7, y, z, DARK); v.set(6, y, z, DARK); v.set(8, y, z, DARK); } }
			if ( axe )
			{
				const int reach = r.range(4, 7), spread = r.range(3, 6);
				for ( int dx = 1; dx <= reach; ++dx )
				{
					const int h = 3 + (spread * dx) / reach;          // the bit fans out
					for ( int dz = -h; dz <= h; ++dz )
					{
						const int z = 8 + dz;
						if ( z < 0 || z >= R.sz ) { continue; }
						v.set(8 + dx, 2, z, dx == reach ? EDGE : BASE);
					}
				}
				if ( r.unit() < 0.5 ) { for ( int dx = 1; dx <= 3; ++dx ) { v.set(6 - dx, 2, 8, LIGHT); } } // back spike
			}
			else
			{
				const double rad = 3.0 + r.unit() * 2.0;
				for ( int x = 0; x < R.sx; ++x )
					for ( int y = 0; y < R.sy; ++y )
						for ( int z = 0; z < 12; ++z )
						{
							const double dx = x - 7.0, dy = (y - 2.0) * 1.6, dz = z - 6.0;
							if ( dx * dx + dy * dy + dz * dz <= rad * rad ) { v.set(x, y, z, (x + z) % 3 ? BASE : LIGHT); }
						}
				const int spikes = r.range(0, 4);
				for ( int k = 0; k < spikes; ++k )
				{
					const int dir = k % 2 ? 1 : -1, z = 4 + 2 * (k / 2) + (k % 2);
					const int tipX = 7 + dir * ((int)rad + 1);
					for ( int x = 7; x != tipX + dir; x += dir ) { if ( v.get(x, 2, z) == 255 ) { v.set(x, 2, z, x == tipX ? EDGE : LIGHT); } }
				}
			}
			v.set(7, 2, R.sz - 1, DARK);
		}

		// ---------------------------------------------------------- add-ons (monster parts, D79)
		void addon(VoxModel& v, Addon a, Rng& r)
		{
			if ( a == Addon::Horn )
			{
				const int L = r.range(8, 14);
				v.resize(7, 7, L);
				const double bend = 1.0 + r.unit() * 2.0;
				for ( int z = 0; z < L; ++z )
				{
					const double t = double(z) / (L - 1);                  // 0 tip, 1 root
					const double rad = 0.6 + 2.2 * t;
					const double cx = 3.5 + bend * (1.0 - t) * (1.0 - t), cy = 3.5;
					for ( int x = 0; x < 7; ++x ) for ( int y = 0; y < 7; ++y )
					{
						const double dx = x + 0.5 - cx, dy = y + 0.5 - cy;
						if ( dx * dx + dy * dy <= rad * rad ) { v.set(x, y, z, t < 0.3 ? LIGHT : (t > 0.85 ? DARK : BASE)); }
					}
					const int c0 = std::min(6, std::max(0, (int)cx));
					if ( v.get(c0, 3, z) == 255 ) { v.set(c0, 3, z, BASE); }
					if ( z > 0 ) // bridge to the previous layer's centre so the horn stays one piece
					{
						const double tp = double(z - 1) / (L - 1);
						const int cp = std::min(6, std::max(0, (int)(3.5 + bend * (1.0 - tp) * (1.0 - tp))));
						for ( int x = std::min(c0, cp); x <= std::max(c0, cp); ++x ) { if ( v.get(x, 3, z) == 255 ) { v.set(x, 3, z, BASE); } }
					}
				}
			}
			else if ( a == Addon::Crystal )
			{
				const int H = r.range(7, 13), w = r.range(2, 3);
				v.resize(2 * w + 1, 2 * w + 1, H);
				for ( int z = 0; z < H; ++z )
				{
					const double t = double(z) / (H - 1);
					const int half = (int)std::lround(w * (1.0 - std::abs(2.0 * t - 1.0)));
					for ( int dx = -half; dx <= half; ++dx ) for ( int dy = -half; dy <= half; ++dy )
						if ( std::abs(dx) + std::abs(dy) <= half ) { v.set(w + dx, w + dy, z, std::abs(dx) + std::abs(dy) == half ? EDGE : ACC); }
					v.set(w, w, z, ACC);
				}
			}
			else if ( a == Addon::Plate )
			{
				const int W = r.range(6, 10), H = r.range(5, 9);
				v.resize(W, 2, H);
				for ( int x = 0; x < W; ++x ) for ( int z = 0; z < H; ++z )
				{
					v.set(x, 0, z, (x == 0 || z == 0 || x == W - 1 || z == H - 1) ? DARK : BASE);
					if ( (x == 1 || x == W - 2) && (z == 1 || z == H - 2) ) { v.set(x, 1, z, LIGHT); } // rivets
				}
			}
			else // Spur
			{
				const int L = r.range(4, 9);
				const int base = r.range(1, L - 2);
				const bool hook = r.unit() < 0.4;
				v.resize(4, 3, L);
				for ( int z = 0; z < L; ++z )
				{
					v.set(1, 1, z, z < 2 ? EDGE : BASE);
					if ( z >= base ) { v.set(0, 1, z, DARK); v.set(2, 1, z, DARK); v.set(1, 0, z, DARK); v.set(1, 2, z, DARK); }
				}
				if ( hook ) { v.set(2, 1, 0, EDGE); v.set(3, 1, 0, EDGE); v.set(2, 1, 1, BASE); }
			}
		}
	} // namespace

	VoxModel makeComponent(Slot s, uint64_t seed, Rgb material, Rgb accent)
	{
		VoxModel v;
		Rng r{ seed * 0x100000001B3ull + (uint64_t)s + 1 };
		switch ( s )
		{
			case Slot::Blade: blade(v, r); break;
			case Slot::Grip: grip(v, r); break;
			case Slot::Guard: guard(v, r); break;
			case Slot::Pommel: pommel(v, r); break;
			case Slot::Haft: haft(v, r); break;
			default: head(v, r); break;
		}
		paletteRamp(v, material, accent);
		if ( std::getenv("APX_BX1_BREAK") && std::getenv("APX_BX1_BREAK")[0] == '1' ) // control: a detached voxel
		{
			v.set(0, 0, 0, 255); v.set(v.sx - 1, v.sy - 1, v.sz / 2, BASE);
			if ( v.sx > 2 ) { v.set(v.sx - 2, v.sy - 1, v.sz / 2, 255); }
		}
		return v;
	}

	VoxModel makeAddon(Addon a, uint64_t seed, Rgb material, Rgb accent)
	{
		VoxModel v;
		Rng r{ seed * 0x100000001B3ull + 101 + (uint64_t)a };
		addon(v, a, r);
		paletteRamp(v, material, accent);
		return v;
	}

	// ------------------------------------------------------------------ the builder's rule
	Check check(Slot s, const VoxModel& v)
	{
		Check c;
		const SlotRule& R = slotRule(s);
		c.voxels = v.count();
		if ( v.sx > R.sx || v.sy > R.sy || v.sz > R.sz ) { c.why = "larger than the slot volume"; return c; }
		if ( (size_t)v.sx * v.sy * v.sz != v.data.size() ) { c.why = "malformed"; return c; }
		if ( c.voxels == 0 ) { c.why = "empty"; return c; }
		if ( c.voxels > R.maxVoxels ) { c.why = "over the size limit"; return c; }
		for ( const Cell& m : R.mounts ) { c.mountsMissed += v.get(m.x, m.y, m.z) == 255; }
		// 6-connected pieces
		std::vector<uint8_t> seen(v.data.size(), 0);
		std::vector<Cell> stack;
		for ( int x = 0; x < v.sx; ++x ) for ( int y = 0; y < v.sy; ++y ) for ( int z = 0; z < v.sz; ++z )
		{
			const size_t i = v.index(x, y, z);
			if ( v.data[i] == 255 || seen[i] ) { continue; }
			++c.pieces;
			seen[i] = 1; stack.push_back({ x, y, z });
			while ( !stack.empty() )
			{
				Cell p = stack.back(); stack.pop_back();
				const int d[6][3] = { {1,0,0},{-1,0,0},{0,1,0},{0,-1,0},{0,0,1},{0,0,-1} };
				for ( auto& o : d )
				{
					const int nx = p.x + o[0], ny = p.y + o[1], nz = p.z + o[2];
					if ( !v.inside(nx, ny, nz) ) { continue; }
					const size_t j = v.index(nx, ny, nz);
					if ( v.data[j] != 255 && !seen[j] ) { seen[j] = 1; stack.push_back({ nx, ny, nz }); }
				}
			}
		}
		if ( c.mountsMissed ) { c.why = "does not reach every mount"; return c; }
		if ( c.pieces != 1 ) { c.why = "not one connected piece"; return c; }
		c.ok = true;
		return c;
	}

	int shapingPoints(const Check& c) { return c.ok ? 20 : 0; }

	// ------------------------------------------------------------------ the builder's session
	void BuildSession::begin(Slot s, Rgb material, Rgb accent)
	{
		slot = s;
		const SlotRule& R = slotRule(s);
		model = VoxModel();
		model.resize(R.sx, R.sy, R.sz);
		paletteRamp(model, material, accent);
		layer = R.sy / 2;
		mirror = false;
		colour = 2;
		edits = 0;
	}

	bool BuildSession::add(int x, int z)
	{
		if ( !model.inside(x, layer, z) ) { return false; }
		bool changed = model.get(x, layer, z) != colour;
		model.set(x, layer, z, colour);
		const int mx = model.sx - 1 - x;
		if ( mirror && mx != x ) { changed |= model.get(mx, layer, z) != colour; model.set(mx, layer, z, colour); }
		edits += changed;
		return changed;
	}

	bool BuildSession::remove(int x, int z)
	{
		if ( !model.inside(x, layer, z) ) { return false; }
		bool changed = model.get(x, layer, z) != 255;
		model.set(x, layer, z, 255);
		const int mx = model.sx - 1 - x;
		if ( mirror && mx != x ) { changed |= model.get(mx, layer, z) != 255; model.set(mx, layer, z, 255); }
		edits += changed;
		return changed;
	}

	void BuildSession::clear() { std::fill(model.data.begin(), model.data.end(), 255); ++edits; }

	bool BuildSession::isMount(int x, int y, int z) const
	{
		for ( const Cell& m : slotRule(slot).mounts ) { if ( m.x == x && m.y == y && m.z == z ) { return true; } }
		return false;
	}} // namespace bx1
} // namespace apx
