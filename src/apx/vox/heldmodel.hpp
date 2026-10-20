// apx/vox/heldmodel.hpp -- BX1 (C10) engine glue: runtime voxel models and the composed held weapon.
//
// registerModel: grows Barony's models/polymodels arrays at run time and meshes just the new index (the vanilla
// models stay where models.txt put them; saveModelCache keeps writing only those, baseModelCount()).
// heldModel: an item with parts is drawn as its parts' voxel models stacked tip to hand along z (bx1's frame,
// measured to match Barony's held and first-person weapons), each part with its own colours, resampled to the
// vanilla model's box so the hand and scale stay Barony's. Replaces the palette tint stand-in.
// Knobs: APX_VOX_HELD=0 (the control: vanilla model + tint), APX_PARTS_VIS=0 (vanilla look).
#pragma once

#include <cstdint>

class Item;

namespace apx
{
namespace bx1 { struct VoxModel; }
namespace vox
{
	bool heldEnabled();
	int registerModel(const bx1::VoxModel& m);   // a model index, or -1; the same content returns the same index
	unsigned baseModelCount();                     // nummodels before any runtime model (models.txt)
	int heldModel(const Item* item, bool firstPerson, int vanillaModel); // composed index, or vanillaModel

	// player-built models (the voxel builder): stored once per look hash as <outputdir>/apxlooks/<hash>.vox
	uint32_t storeLook(const bx1::VoxModel& m);      // returns the look hash (0 on failure)
	const bx1::VoxModel* findLook(uint32_t look);     // cached; loads from the store; nullptr when absent
	struct Stats { int registered = 0, composed = 0, cacheHits = 0, partsSkipped = 0, looksUsed = 0, looksMissing = 0; };
	const Stats& stats();
} // namespace vox
} // namespace apx
