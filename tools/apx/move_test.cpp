// tools/apx/move_test.cpp -- offline units for apx::move's pure core (src/apx/move/move_core.hpp). No Barony, seconds.
//   pwsh -NoProfile -File tools\apx\move_test.ps1
#include "../../src/apx/move/move_core.hpp"
#include <cmath>
#include <cstdio>

using namespace apx::move::core;
static int g_fail = 0, g_n = 0;
static void check(bool ok, const char* name, double got, double want)
{
	++g_n; if ( !ok ) { ++g_fail; }
	std::printf("%s %-46s got=%.5f want=%.5f\n", ok ? "PASS" : "FAIL", name, got, want);
}
static bool near(double a, double b, double rel) { return std::fabs(a - b) <= rel * std::max(1e-9, std::fabs(b)); }

int main()
{
	Table T;
	const float base = 14.f;                                  // a mid base speed (Barony getSpeedFactor value)
	const float walk = steadySpeed(T, base, T.walk);          // units per tick
	check(near(walk, 14.0 * 6.75 / 50.0, 1e-6), "walk_steady_units_per_tick", walk, 14.0 * 6.75 / 50.0);
	check(near(steadySpeed(T, base, T.sprint) / walk, 1.5, 1e-6), "sprint_is_1.5x", steadySpeed(T, base, T.sprint) / walk, 1.5);
	check(near(steadySpeed(T, base, T.sneak) / walk, 0.5, 1e-6), "sneak_is_0.5x", steadySpeed(T, base, T.sneak) / walk, 0.5);

	// wish: forward / back / strafe / diagonal, relative to yaw
	Vec2 w = wish(T, 1.f, 0.f, 0.f);
	check(near(w.x, 1.0, 1e-5) && std::fabs(w.y) < 1e-5, "wish_forward_yaw0_is_plus_x", w.x, 1.0);
	w = wish(T, 1.f, 0.f, (float)(3.14159265358979 / 2));
	check(std::fabs(w.x) < 1e-5 && near(w.y, 1.0, 1e-5), "wish_forward_yaw90_is_plus_y", w.y, 1.0);
	w = wish(T, 0.f, 1.f, 0.f);
	check(std::fabs(w.x) < 1e-5 && near(w.y, T.strafe, 1e-5), "wish_right_yaw0_is_plus_y_x0.85", w.y, T.strafe);
	w = wish(T, -1.f, 0.f, 0.f);
	check(near(w.x, -T.backpedal, 1e-5), "wish_backpedal_0.75", w.x, -T.backpedal);
	w = wish(T, 1.f, 1.f, 0.f);
	check(len(w) <= 1.0001f && near(len(w), 1.0, 1e-4), "diagonal_normalised_no_bonus", len(w), 1.0);

	// acceleration: reaches the steady speed in accel_seconds, never overshoots
	Vec2 v; float maxv = 0.f; int reach = -1;
	for ( int i = 1; i <= 20; ++i )
	{
		v = stepLoco(T, v, { 1.f, 0.f }, walk, 1.f, 1.f);
		maxv = std::max(maxv, len(v));
		if ( reach < 0 && near(len(v), walk, 1e-5) ) { reach = i; }
	}
	check(reach == 5, "accel_reaches_walk_in_5_ticks", reach, 5);
	check(maxv <= walk * 1.000001f, "accel_no_overshoot", maxv, walk);
	Vec2 s = v; const float sp = steadySpeed(T, base, T.sprint); int toSprint = 0;
	for ( int i = 1; i <= 30 && !near(len(s), sp, 1e-5); ++i ) { s = stepLoco(T, s, { 1.f, 0.f }, sp, 1.f, 1.f); toSprint = i; }
	check(near(len(s), sp, 1e-5) && toSprint <= 8, "sprint_reached_in_<=8_ticks", toSprint, 8);
	int stop = -1; Vec2 b = v; float brakeDist = 0.f;
	for ( int i = 1; i <= 20; ++i ) { b = stepLoco(T, b, {}, walk, 1.f, 1.f); brakeDist += len(b); if ( stop < 0 && len(b) < 1e-6f ) { stop = i; } }
	check(stop == 4, "brake_stops_in_4_ticks", stop, 4);
	check(brakeDist < walk * 4.0f, "brake_distance_under_4_ticks_of_walk", brakeDist, walk * 4);
	Vec2 r = v; int rev = -1;
	for ( int i = 1; i <= 20; ++i ) { r = stepLoco(T, r, { -1.f, 0.f }, walk, 1.f, 1.f); if ( rev < 0 && near(r.x, -walk, 1e-4) ) { rev = i; } }
	check(rev > 0 && rev <= 8, "reverse_reaches_-walk_in_<=8_ticks", rev, 8);
	Vec2 ice; int iceReach = -1;
	for ( int i = 1; i <= 200; ++i ) { ice = stepLoco(T, ice, { 1.f, 0.f }, walk, T.gripSlippery, 1.f); if ( iceReach < 0 && near(len(ice), walk, 1e-5) ) { iceReach = i; } }
	check(iceReach >= 5 * 7, "slippery_floor_accelerates_slower", iceReach, 5.0 / T.gripSlippery);

	// impulses: distance |v| (n+1)/2 on open floor, summed exactly; stacking adds; cap applies
	for ( int n : { 1, 5, 10, 20, 40 } )
	{
		ImpulseStack st; st.add(T, 3.f, 0.f, n);
		double dist = 0; for ( int i = 0; i < n + 3; ++i ) { dist += st.tick().x; }
		check(near(dist, impulseDistance(3.f, n), 1e-5), "impulse_distance_tabled", dist, impulseDistance(3.f, n));
		check(!st.any(), "impulse_ends", st.any(), 0);
	}
	{
		ImpulseStack st; st.add(T, 2.f, 0.f, 10); st.add(T, 0.f, 4.f, 6);
		double dx = 0, dy = 0; for ( int i = 0; i < 12; ++i ) { Vec2 q = st.tick(); dx += q.x; dy += q.y; }
		check(near(dx, impulseDistance(2.f, 10), 1e-5) && near(dy, impulseDistance(4.f, 6), 1e-5), "impulses_stack_per_axis", dx + dy, impulseDistance(2.f, 10) + impulseDistance(4.f, 6));
		ImpulseStack big; big.add(T, 30.f, 0.f, 4);
		check(near(big.tick().x, T.impulseCap, 1e-6), "impulse_speed_capped_at_7", T.impulseCap, 7.0);
		ImpulseStack full; for ( int i = 0; i < 12; ++i ) full.add(T, 1.f, 0.f, 5);
		check(full.count == 8 && full.dropped == 4, "impulse_stack_limit_8_counts_drops", full.dropped, 4);
	}

	// sprint and stamina: drains 4 per second, stops at 0, stays off until the key is released
	{
		SprintState ss; float stam = 100.f, drained = 0.f, drain = 0.f;
		for ( int i = 0; i < 3 * 50; ++i ) { sprintTick(T, ss, true, true, stam, true, &drain); stam -= drain; drained += drain; }
		check(near(drained, 12.0, 1e-4), "sprint_drains_4_per_second", drained, 12.0);
		SprintState s2; stam = 10.f; int ticks = 0;
		for ( int i = 0; i < 500; ++i ) { sprintTick(T, s2, true, true, stam, true, &drain); stam = std::max(0.f, stam - drain); if ( s2.on ) { ++ticks; } }
		check(near(ticks, 10.0 / 4.0 * 50.0, 0.01) && s2.exhausted && stam == 0.f, "sprint_stops_at_zero_2.5s_from_10", ticks, 125);
		stam = 50.f; sprintTick(T, s2, true, true, stam, true, &drain);
		check(!s2.on, "sprint_stays_off_while_key_held_after_exhaustion", s2.on, 0);
		sprintTick(T, s2, false, true, stam, true, &drain); sprintTick(T, s2, true, true, stam, true, &drain);
		check(s2.on, "sprint_resumes_after_release_and_press", s2.on, 1);
		SprintState s3; stam = 0.5f; sprintTick(T, s3, true, true, stam, true, &drain);
		check(!s3.on, "sprint_needs_min_stamina_to_start", s3.on, 0);
		SprintState s4; sprintTick(T, s4, true, false, 100.f, true, &drain);
		check(!s4.on && drain == 0.f, "sprint_refused_when_legs_disabled", s4.on, 0);
		SprintState s5; sprintTick(T, s5, true, true, 0.f, false, &drain);
		check(s5.on && drain == 0.f, "sprint_free_when_stamina_untracked", s5.on, 1);
	}

	// look: unsmoothed turn, clamp, sensitivity, invert, confusion; smoothed accumulates and decays
	{
		float rx = 0.f; lookAxisX(T, rx, 10.f, 128.f, false, false, true, true, 1.f);
		check(near(rx, 0.1, 1e-5), "look_raw_10px_is_0.1rad", rx, 0.1);
		lookAxisX(T, rx, 1000.f, 128.f, false, false, true, true, 1.f);
		check(near(rx, T.lookSmoothClamp, 1e-6), "look_raw_clamped_0.35", rx, 0.35);
		lookAxisX(T, rx, 1000.f, 128.f, false, false, true, false, 1.f);
		check(near(rx, 10.0, 1e-5), "look_raw_unclamped_when_limit_off", rx, 10.0);
		lookAxisX(T, rx, 10.f, 256.f, false, false, true, true, 1.f);
		check(near(rx, 0.2, 1e-5), "look_sensitivity_scales", rx, 0.2);
		lookAxisX(T, rx, 10.f, 128.f, false, true, true, true, 1.f);
		check(near(rx, -0.1, 1e-5), "look_confused_flips_x", rx, -0.1);
		float ry = 0.f; lookAxisY(T, ry, 10.f, 128.f, false, false, false, true, 1.f);
		check(near(ry, -0.1, 1e-5), "look_y_default_not_reversed", ry, -0.1);
		lookAxisY(T, ry, 10.f, 128.f, false, true, false, true, 1.f);
		check(near(ry, 0.1, 1e-5), "look_y_reverse_flips", ry, 0.1);
		float sx = 0.f; lookAxisX(T, sx, 40.f, 128.f, true, false, true, true, 1.f);
		check(near(sx, 40.0 * 0.006 * 0.5, 1e-5), "look_smooth_accumulates_then_halves", sx, 0.12);
		float idle = 0.f; lookAxisX(T, idle, 0.f, 128.f, false, false, false, true, 1.f);
		check(idle == 0.f, "look_not_mobile_no_turn", idle, 0);
	}

	std::printf("MOVE-UNITS: %s (%d checks, %d failed)\n", g_fail ? "FAIL" : "PASS", g_n, g_fail);
	return g_fail ? 1 : 0;
}
