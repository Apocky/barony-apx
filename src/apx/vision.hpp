// apx/vision.hpp -- BVIS1 (D84): see more and further without lifting gamma.
//
// Three parts, each with a knob (0 = the old behaviour, the control); -apxvanilla / APX_VANILLA=1 turns all off:
//   far      APX_VIS_FAR=0      far clip plane covers the whole floor (was 4000 units = 125 tiles); the fortress
//                               maps' mist starts at 4x its old distance (a tint, no longer a wall)
//   falloff  APX_VIS_FALLOFF=0  light falls off on a perceptual curve with a faint tail past the old radius
//                               (was linear to a hard zero at the radius)
//   bounce   APX_VIS_BOUNCE=0   a little light spills from lit tiles onto their neighbourhood so shapes read;
//                               unlit areas far from any light stay dark
#pragma once

namespace apx
{
namespace vision
{
	bool farOn();
	bool falloffOn();
	bool bounceOn();
	void setVanilla();                  // -apxvanilla

	float clipFar();                    // the far clip plane in world units for the current map
	float fogDistance(float vanillaDistance);

	// falloff: light left at distance^2 d2 (tiles^2) from a light whose old curve was
	// 1 - min(d2^exp / radius, 1); returns 0..1; reach() is the extent in tiles to loop over
	float falloff(float d2, float radius, float exp);
	int reach(int radius, float exp);

	float bounceStrength();             // share of the neighbourhood's light added to a tile (0 when off)
	int bounceRadius();                 // tiles
} // namespace vision
} // namespace apx
