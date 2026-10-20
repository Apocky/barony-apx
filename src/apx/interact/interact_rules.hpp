// apx/interact/interact_rules.hpp -- the pure geometry of the crosshair query (no Barony headers; testable offline).
#pragma once
#include <cmath>

namespace apx
{
namespace interact
{
	struct Aim
	{
		bool ok = false;
		float dist = 0.f, edge = 0.f, forward = 0.f, miss = 0.f;
	};

	// A prop is a circle (cx, cy, radius). The view ray starts at (ox, oy) along yaw. It is aimed at when it is in
	// front, its edge is within `reach`, and the ray passes within radius + slack + cone*forward of its centre.
	inline Aim aimAt(float ox, float oy, float yaw, float cx, float cy, float radius, float reach)
	{
		constexpr float kSlack = 2.0f, kCone = 0.105f; // units; tan(6 degrees)
		Aim a;
		const float dx = cx - ox, dy = cy - oy, c = std::cos(yaw), s = std::sin(yaw);
		a.dist = std::sqrt(dx * dx + dy * dy);
		a.edge = a.dist - radius;
		a.forward = dx * c + dy * s;
		a.miss = std::fabs(-dx * s + dy * c);
		const bool inside = a.dist <= radius + 2.0f;   // standing against it
		a.ok = (a.forward > 0.f || inside) && a.edge <= reach && (inside || a.miss <= radius + kSlack + kCone * a.forward);
		return a;
	}

	// lower is better: nearest edge first, a smaller aim miss next, a prop with a real verb ahead of an inert one
	inline float score(const Aim& a, bool hasVerb) { return a.edge + a.miss * 1.5f - (hasVerb ? 6.f : 0.f); }

	// looking too far up or down (a ceiling, the floor at your feet) does not aim at a floor prop: |forward * tan(pitch)| in units
	inline bool pitchOk(float forward, float pitch) { return std::fabs(forward * std::tan(pitch)) <= 18.0f; }
} // namespace interact
} // namespace apx
