// apx/vision.cpp -- see vision.hpp.
#include "../main.hpp"
#include "vision.hpp"

#include <algorithm>
#include <cmath>
#include <cstdlib>

namespace apx
{
namespace vision
{
	namespace
	{
		bool g_vanilla = []() { const char* v = std::getenv("APX_VANILLA"); return v && v[0] == '1'; }();
		bool knob(const char* name)
		{
			const char* v = std::getenv(name);
			return !(v && v[0] == '0');
		}
		constexpr float kTail = 1.6f;   // the curve reaches zero at kTail x the old radius
		constexpr float kKnee = 1.9f;   // 1 / (1 + kKnee (d/R)^2): about the old linear curve's level at R/2
	} // namespace

	void setVanilla() { g_vanilla = true; }
	bool farOn() { static const bool on = knob("APX_VIS_FAR"); return on && !g_vanilla; }
	bool falloffOn() { static const bool on = knob("APX_VIS_FALLOFF"); return on && !g_vanilla; }
	bool bounceOn() { static const bool on = knob("APX_VIS_BOUNCE"); return on && !g_vanilla; }

	float clipFar()
	{
		const float vanilla = 4000.f;
		if ( !farOn() || map.width == 0 || map.height == 0 ) { return vanilla; }
		const float diag = std::sqrt((float)map.width * map.width + (float)map.height * map.height) * 32.f;
		return std::max(vanilla, diag * 1.05f);
	}

	float fogDistance(float vanillaDistance)
	{
		return farOn() && vanillaDistance > 0.f ? vanillaDistance * 4.f : vanillaDistance;
	}

	// the distance (tiles) where the old curve reached zero: d2^exp == radius
	static float oldReach(float radius, float exp)
	{
		if ( exp <= 0.f ) { return radius; }
		return std::pow(std::max(radius, 0.f), 1.f / (2.f * exp));
	}

	float falloff(float d2, float radius, float exp)
	{
		const float R = std::max(oldReach(radius, exp), 0.5f);
		const float t = d2 / (R * R);                       // (d/R)^2
		const float w = std::max(0.f, 1.f - t / (kTail * kTail));
		return w * w / (1.f + kKnee * t);                   // windowed inverse square with a knee
	}

	int reach(int radius, float exp)
	{
		return std::max(radius, (int)std::ceil(kTail * oldReach((float)radius, exp)));
	}

	float bounceStrength()
	{
		if ( !bounceOn() ) { return 0.f; }
		static const float s = []() { const char* v = std::getenv("APX_VIS_BOUNCE_K"); return v ? (float)std::atof(v) : 0.18f; }();
		return s;
	}

	int bounceRadius() { return 3; }
} // namespace vision
} // namespace apx
