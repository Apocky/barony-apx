// apx/visshots.hpp -- -apxvisshots: two fixed screenshots for the vision work (BVIS1, D84).
//
// On the test floor, once player 0 has stood for a while, the player is turned toward the longest open line of
// sight ("vis-far.png") and then toward the nearest wall ("vis-near.png"); the frame is shot after the
// lightmap has settled, the two ray lengths are logged as "[apx-vis] ..." and the game quits.
// Needs a visible window (rule 14). Measure the PNGs with 0Labyrinth/tools/eye.py.
#pragma once

namespace apx
{
namespace vis
{
	void arm();
	void armProbe();
	void armVoxShot();            // -apxvoxshot (BX1): the held sword drawn from its parts, one screenshot, then quit
	void armBuilderShot();        // -apxbuildershot (BX1): the voxel builder panel, two screenshots, Build, then quit              // -apxlightprobe: per-tile monster-visible light, then quit
	bool armed();
	void tick(bool ready);        // once per game tick
	const char* takeShot();       // a file name when a frame is wanted now, else nullptr
} // namespace vis
} // namespace apx
