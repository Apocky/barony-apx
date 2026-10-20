// tools/apx/bx1_test.cpp -- offline gates for src/apx/vox/bx1 (BX1, C10) and a small CLI.
//
//   bx1_test.exe [models_dir]            gates; prints PASS or FAIL per gate and overall (exit 0 = all pass)
//   bx1_test.exe gen <slot|addon> <seed> <out.vox>   write one generated model (slot: blade grip guard pommel haft head;
//                                        addon: horn crystal plate spur)
// models_dir defaults to the Steam install's models folder (read only; Barony's models are never committed).
// Control: APX_BX1_BREAK=1 makes the generators emit a detached voxel; G-gen must then FAIL.
#include "../../src/apx/vox/bx1.hpp"

#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <set>
#include <string>
#include <vector>

using namespace apx::bx1;
namespace fs = std::filesystem;

static const Rgb kSteel{ 170, 175, 185 }, kLeather{ 110, 70, 40 };

static int pieces(const VoxModel& v)
{
	// connectivity for add-ons, which have no slot rule: reuse check() through a volume-sized rule is overkill
	std::vector<uint8_t> seen(v.data.size(), 0);
	int n = 0;
	for ( int x = 0; x < v.sx; ++x ) for ( int y = 0; y < v.sy; ++y ) for ( int z = 0; z < v.sz; ++z )
	{
		size_t i = v.index(x, y, z);
		if ( v.data[i] == 255 || seen[i] ) { continue; }
		++n;
		std::vector<Cell> st{ { x, y, z } }; seen[i] = 1;
		while ( !st.empty() )
		{
			Cell p = st.back(); st.pop_back();
			const int d[6][3] = { {1,0,0},{-1,0,0},{0,1,0},{0,-1,0},{0,0,1},{0,0,-1} };
			for ( auto& o : d )
			{
				int a = p.x + o[0], b = p.y + o[1], c = p.z + o[2];
				if ( !v.inside(a, b, c) ) { continue; }
				size_t j = v.index(a, b, c);
				if ( v.data[j] != 255 && !seen[j] ) { seen[j] = 1; st.push_back({ a, b, c }); }
			}
		}
	}
	return n;
}

static bool gateRoundTrip(const std::string& dir)
{
	int files = 0, same = 0, bad = 0, shortFiles = 0;
	if ( !fs::exists(dir) ) { printf("G-vox-rt SKIP: %s not found\n", dir.c_str()); return true; }
	for ( auto& e : fs::recursive_directory_iterator(dir) )
	{
		if ( !e.is_regular_file() || e.path().extension() != ".vox" ) { continue; }
		std::ifstream f(e.path(), std::ios::binary);
		std::vector<uint8_t> b((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
		VoxModel v; std::string err;
		++files;
		if ( !readBytes(b, v, err) ) { if ( bad++ < 5 ) { printf("  read fail %s: %s\n", e.path().string().c_str(), err.c_str()); } continue; }
		auto out = toBytes(v);
		// a file may carry trailing bytes past the palette; the model part must match exactly. A short file
		// (read as Barony reads it) must come back with every byte it had, as a prefix.
		if ( v.truncated ) { if ( std::equal(b.begin(), b.end(), out.begin()) ) { ++same; ++shortFiles; printf("  short file read as Barony does: %s (%zu of %zu bytes)\n", e.path().filename().string().c_str(), b.size(), out.size()); } else if ( bad++ < 5 ) { printf("  differs (short) %s\n", e.path().string().c_str()); } }
		else if ( out.size() <= b.size() && std::equal(out.begin(), out.end(), b.begin()) ) { ++same; }
		else if ( bad++ < 5 ) { printf("  differs %s\n", e.path().string().c_str()); }
	}
	const bool ok = files > 0 && same == files;
	printf("G-vox-rt %s: %d of %d .vox files read and written back byte-identical (%d short file(s) as a prefix)\n", ok ? "PASS" : "FAIL", same, files, shortFiles);
	return ok;
}

static bool gateGenerators()
{
	bool ok = true;
	for ( int s = 0; s < (int)Slot::NumSlots; ++s )
	{
		int valid = 0, det = 0; std::set<uint32_t> shapes; int minV = 1 << 30, maxV = 0; std::string firstWhy;
		for ( uint64_t seed = 1; seed <= 1000; ++seed )
		{
			VoxModel a = makeComponent((Slot)s, seed, kSteel, kLeather), b = makeComponent((Slot)s, seed, kSteel, kLeather);
			det += a.data == b.data && std::memcmp(a.pal, b.pal, 768) == 0;
			Check c = check((Slot)s, a);
			valid += c.ok;
			if ( !c.ok && firstWhy.empty() ) { firstWhy = c.why; }
			shapes.insert(hash(a));
			minV = std::min(minV, c.voxels); maxV = std::max(maxV, c.voxels);
		}
		static const size_t minShapes[] = { 100, 8, 8, 8, 8, 50 }; // Blade Grip Guard Pommel Haft Head: small volumes allow few distinct shapes
		const bool sOk = valid == 1000 && det == 1000 && shapes.size() >= minShapes[s];
		ok = ok && sOk;
		printf("G-gen %s %-6s: valid %d/1000, deterministic %d/1000, distinct shapes %zu, voxels %d-%d%s%s\n", sOk ? "PASS" : "FAIL",
			slotName((Slot)s), valid, det, shapes.size(), minV, maxV, firstWhy.empty() ? "" : ", first fail: ", firstWhy.c_str());
	}
	for ( int a = 0; a < (int)Addon::NumAddons; ++a )
	{
		int one = 0; std::set<uint32_t> shapes;
		for ( uint64_t seed = 1; seed <= 1000; ++seed ) { VoxModel v = makeAddon((Addon)a, seed, kSteel, kLeather); one += pieces(v) == 1 && v.count() > 0; shapes.insert(hash(v)); }
		static const size_t minAddon[] = { 50, 10, 10, 8 }; // Horn Crystal Plate Spur
		const bool aOk = one == 1000 && shapes.size() >= minAddon[a];
		ok = ok && aOk;
		printf("G-gen %s %-7s: one piece %d/1000, distinct shapes %zu\n", aOk ? "PASS" : "FAIL", addonName((Addon)a), one, shapes.size());
	}
	return ok;
}

static bool gateBuildRules()
{
	struct Case { const char* name; bool wantOk; std::string wantWhy; VoxModel v; Slot s; };
	std::vector<Case> cases;
	const SlotRule& G = slotRule(Slot::Grip);
	auto grip = [&]() { VoxModel v; v.resize(G.sx, G.sy, G.sz); for ( int z = 0; z < G.sz; ++z ) { v.set(2, 2, z, 2); } return v; };
	cases.push_back({ "straight grip", true, "", grip(), Slot::Grip });
	{ VoxModel v; v.resize(G.sx, G.sy, G.sz); cases.push_back({ "empty", false, "empty", v, Slot::Grip }); }
	{ VoxModel v = grip(); v.set(2, 2, 6, 255); cases.push_back({ "broken in two", false, "not one connected piece", v, Slot::Grip }); }
	{ VoxModel v = grip(); v.set(2, 2, 13, 255); cases.push_back({ "misses the bottom mount", false, "does not reach every mount", v, Slot::Grip }); }
	{ VoxModel v = grip(); v.set(0, 0, 0, 2); cases.push_back({ "a stray voxel", false, "not one connected piece", v, Slot::Grip }); }
	{ VoxModel v; v.resize(G.sx, G.sy, G.sz + 1); for ( int z = 0; z <= G.sz; ++z ) { v.set(2, 2, z, 2); } cases.push_back({ "too long", false, "larger than the slot volume", v, Slot::Grip }); }
	{ VoxModel v; v.resize(G.sx, G.sy, G.sz); for ( auto& b : v.data ) { b = 2; } cases.push_back({ "solid block over the limit", false, "over the size limit", v, Slot::Grip }); }
	bool ok = true;
	for ( auto& c : cases )
	{
		Check r = check(c.s, c.v);
		const bool good = r.ok == c.wantOk && (c.wantOk || r.why == c.wantWhy) && shapingPoints(r) == (c.wantOk ? 20 : 0);
		ok = ok && good;
		printf("  %s %-28s ok=%d why='%s' points=%d\n", good ? "ok  " : "BAD ", c.name, r.ok, r.why.c_str(), shapingPoints(r));
	}
	printf("G-build %s: %zu rule cases\n", ok ? "PASS" : "FAIL", cases.size());
	return ok;
}

static bool gateSession()
{
	bool ok = true;
	auto expect = [&](bool cond, const char* what) { if ( !cond ) { printf("  BAD  %s\n", what); ok = false; } };
	BuildSession b;
	b.begin(Slot::Grip, kSteel, kLeather);
	expect(b.model.count() == 0 && b.layer == 2, "begin: empty grip volume, middle layer");
	expect(!b.validate().ok && b.validate().why == "empty", "empty build is not valid");
	for ( int z = 0; z < b.model.sz; ++z ) { b.add(2, z); }
	Check c = b.validate();
	expect(c.ok && shapingPoints(c) == 20, "a straight grip through both mounts earns 20");
	b.remove(2, 6);
	expect(!b.validate().ok && b.validate().why == "not one connected piece", "removing a middle voxel breaks it");
	b.add(2, 6);
	b.mirror = true;
	b.add(0, 3);
	expect(b.model.get(0, 2, 3) != 255 && b.model.get(3, 2, 3) != 255, "mirror adds both sides");
	b.remove(3, 3);
	expect(b.model.get(0, 2, 3) == 255 && b.model.get(3, 2, 3) == 255, "mirror removes both sides");
	b.layerUp(); b.layerUp(); b.layerUp();
	expect(b.layer == b.model.sy - 1, "layer stops at the top");
	expect(!b.add(99, 0), "outside the volume is refused");
	// a blade drawn by hand: 3 wide on the middle layer from z 12 to the tang
	BuildSession s;
	s.begin(Slot::Blade, kSteel, kLeather);
	for ( int z = 12; z < s.model.sz; ++z ) { s.add(3, z); if ( z < s.model.sz - 3 ) { s.add(2, z); s.add(4, z); } }
	Check cb = s.validate();
	expect(cb.ok, "hand-drawn blade is valid");
	const uint32_t h = hash(s.model);
	VoxModel back; std::string err;
	expect(readBytes(toBytes(s.model), back, err) && hash(back) == h && h != 0, "built model round-trips with the same look hash");
	printf("G-session %s: builder session (grip %d voxels, blade %d voxels, look %08x)\n", ok ? "PASS" : "FAIL", b.model.count(), s.model.count(), h);
	return ok;
}

int main(int argc, char** argv)
{
	if ( argc >= 5 && !std::strcmp(argv[1], "gen") )
	{
		const std::string what = argv[2];
		const uint64_t seed = std::strtoull(argv[3], nullptr, 10);
		VoxModel v; bool found = false;
		for ( int s = 0; s < (int)Slot::NumSlots && !found; ++s ) { std::string n = slotName((Slot)s); for ( auto& ch : n ) { ch = (char)tolower(ch); } if ( n == what ) { v = makeComponent((Slot)s, seed, kSteel, kLeather); found = true; } }
		for ( int a = 0; a < (int)Addon::NumAddons && !found; ++a ) { std::string n = addonName((Addon)a); for ( auto& ch : n ) { ch = (char)tolower(ch); } if ( n == what ) { v = makeAddon((Addon)a, seed, kSteel, kLeather); found = true; } }
		if ( !found ) { fprintf(stderr, "unknown slot or addon '%s'\n", what.c_str()); return 2; }
		if ( !write(argv[4], v) ) { fprintf(stderr, "cannot write %s\n", argv[4]); return 1; }
		printf("%s seed %llu: %dx%dx%d, %d voxels, hash %08x -> %s\n", what.c_str(), (unsigned long long)seed, v.sx, v.sy, v.sz, v.count(), hash(v), argv[4]);
		return 0;
	}
	const std::string dir = argc >= 2 ? argv[1] : "C:/Program Files (x86)/Steam/steamapps/common/Barony/models";
	const bool a = gateRoundTrip(dir), b = gateGenerators(), c = gateBuildRules(), d = gateSession();
	const bool all = a && b && c && d;
	printf("%s: BX1 offline gates\n", all ? "PASS" : "FAIL");
	return all ? 0 : 1;
}
