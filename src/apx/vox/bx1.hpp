// apx/vox/bx1.hpp -- BX1 (C10; D79, D87): Barony's voxel model format, seeded component and add-on models,
// and the validity rule of the voxel builder. Pure logic: no engine headers, unit-tested offline
// (tools/apx/bx1_test.cpp, tools/apx/bx1_test.ps1).
//
// Format (Barony "slab" .vox, NOT MagicaVoxel): int32 sx, sy, sz; sx*sy*sz bytes of palette index (255 = empty)
// at index z + y*sz + x*sy*sz; 768 palette bytes in 0..63 (Barony shifts them left by 2 on load).
//
// Component frame used by the generators: z is the long axis (z = 0 is the end AWAY from the hand, so a blade's
// tip is at z = 0 and its tang at z = sz - 1), x is width, y is thickness. Composition (registry/compose) maps
// this frame onto a Form's held model.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace apx
{
namespace bx1
{
	struct VoxModel
	{
		int sx = 0, sy = 0, sz = 0;
		std::vector<uint8_t> data;          // 255 = empty
		uint8_t pal[768] = {0};             // 6-bit values
		bool truncated = false;             // read from a file shorter than its header says (filled as Barony does)
		void resize(int x, int y, int z) { sx = x; sy = y; sz = z; data.assign((size_t)x * y * z, 255); }
		size_t index(int x, int y, int z) const { return (size_t)z + (size_t)y * sz + (size_t)x * sy * sz; }
		bool inside(int x, int y, int z) const { return x >= 0 && y >= 0 && z >= 0 && x < sx && y < sy && z < sz; }
		uint8_t get(int x, int y, int z) const { return inside(x, y, z) ? data[index(x, y, z)] : 255; }
		void set(int x, int y, int z, uint8_t c) { if ( inside(x, y, z) ) { data[index(x, y, z)] = c; } }
		int count() const;
	};

	bool read(const std::string& path, VoxModel& out, std::string& err);
	bool write(const std::string& path, const VoxModel& v);
	bool readBytes(const std::vector<uint8_t>& bytes, VoxModel& out, std::string& err);
	std::vector<uint8_t> toBytes(const VoxModel& v);
	uint32_t hash(const VoxModel& v);       // FNV-1a over the file bytes: a player-built model's look id

	enum class Slot : int { Blade = 0, Grip, Guard, Pommel, Haft, Head, NumSlots };
	enum class Addon : int { Horn = 0, Crystal, Plate, Spur, NumAddons };
	const char* slotName(Slot s);
	const char* addonName(Addon a);

	struct Cell { int x, y, z; };
	struct SlotRule
	{
		int sx, sy, sz;                     // the build volume (the model may be smaller, never larger)
		int maxVoxels;                      // the size limit
		std::vector<Cell> mounts;           // cells the component must fill: where it meets the Form's other parts
	};
	const SlotRule& slotRule(Slot s);

	// material colour as 0..255 RGB; the generator makes a 4-shade ramp (dark, base, light, edge) at indices 1..4
	struct Rgb { uint8_t r, g, b; };
	void paletteRamp(VoxModel& v, Rgb base, Rgb accent);

	// seeded generators: same (slot, seed, colours) -> same bytes; the result always passes check()
	VoxModel makeComponent(Slot s, uint64_t seed, Rgb material, Rgb accent);
	VoxModel makeAddon(Addon a, uint64_t seed, Rgb material, Rgb accent);

	// the builder's rule (D87): inside the slot volume, at most maxVoxels, every mount cell filled, 6-connected
	struct Check { bool ok = false; std::string why; int voxels = 0; int pieces = 0; int mountsMissed = 0; };
	Check check(Slot s, const VoxModel& v);
	int shapingPoints(const Check& c);       // D87: any valid build earns the full 20, else 0
} // namespace bx1
} // namespace apx
