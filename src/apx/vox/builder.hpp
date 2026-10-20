// apx/vox/builder.hpp -- BX1 (C10, D87, 05b step 8): the voxel builder at a bench.
//
// MAKE a component: choosing a Make option (slot + pouch material) opens a build session in that slot's volume
// instead of making it at once; the player builds freely voxel by voxel (bx1::BuildSession); BUILD is accepted only
// when the build is valid (connected, inside the volume and size limit, reaching every mount) and earns the full 20
// shaping points (D87, fixed points; the shape never changes stats). The model is stored under its look hash,
// registered, and the component is made with that look (apxPartsBenchMakeLook) -- what the player then sees in hand.
// Slots without a shape (Edge, Fuller, Graft ...) are made directly as before. Knob APX_VOX_BUILDER=0: no builder.
#pragma once

#include <cstdint>
#include <string>

class Item;

namespace apx
{
namespace bx1 { struct BuildSession; }
namespace vox
{
namespace builder
{
	bool enabled();
	bool open(int player, int makeOptionId);   // false: this slot has no shape, make it directly
	bool active(int player);
	bx1::BuildSession* session(int player);
	Item* build(int player, std::string& why);  // the made component, or nullptr with why (the session stays open)
	void cancel(int player);
	struct Stats { int opened = 0, built = 0, refused = 0; };
	const Stats& stats();
} // namespace builder
} // namespace vox
} // namespace apx
