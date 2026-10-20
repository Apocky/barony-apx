// apx/move/move_core.hpp -- the pure part of apx::move (spec 04b section 5, D93): no Barony types, header-only,
// compiled by the engine and by tools/apx/move_test.cpp (offline gate, seconds). Velocities are world units per
// 50 Hz tick (1 tile = 16 units), the same unit Barony's vel_x/vel_y use.
#pragma once
#include <algorithm>
#include <cmath>

namespace apx
{
namespace move
{
namespace core
{
	constexpr int kTps = 50;

	struct Vec2 { float x = 0.f, y = 0.f; };
	inline float len(Vec2 a) { return std::sqrt(a.x * a.x + a.y * a.y); }
	inline float dot(Vec2 a, Vec2 b) { return a.x * b.x + a.y * b.y; }

	// data/apx/move/move.json (defaults here are the same numbers; the engine overlays the file)
	struct Table
	{
		float unitsPerSecPerFactor = 6.75f;   // steady walk speed per point of the stat-based base speed
		float maxSpeedPerTick = 5.0f;         // total clamp (vanilla magnitudeMax)
		float walk = 1.0f, sprint = 1.5f, sneak = 0.5f, block = 0.5f;
		float swimHuman = 0.75f, swimOther = 0.625f, swimSkeleton = 0.3125f, levitate = 1.0f, crawl = 1.0f;
		float backpedal = 0.75f, strafe = 0.85f;
		float accelSeconds = 0.10f, brakeSeconds = 0.08f, turnSeconds = 0.06f, knockedControl = 0.25f;
		float gripNormal = 1.0f, gripSlippery = 0.12f, gripGrease = 0.12f, gripMagicGrease = 0.2f, gripCleatsFloor = 0.6f, slowTile = 0.25f;
		float sprintStaminaPerSecond = 4.0f, sprintMinStamina = 1.0f;
		float impulseCap = 7.0f; int impulseStack = 8; float knockedMinSpeed = 0.15f;
		float legacyDecay = 0.75f;
		int bisectSteps = 6; float nudgeMax = 3.0f, nudgeStep = 0.5f;
		float lookSmoothDecay = 0.5f, lookSmoothClamp = 0.35f, lookSmoothGain = 0.006f, lookRawGain = 0.01f;
	};

	enum class State : int { Walk = 0, Sprint, Sneak, Swim, Levitate, Crawl, Knocked, COUNT };
	inline const char* stateName(State s)
	{
		static const char* n[] = { "walk", "sprint", "sneak", "swim", "levitate", "crawl", "knocked" };
		return n[(int)s];
	}

	// Steady forward speed (units per tick) on plain floor: Barony's base speed (getSpeedFactor) x the table's speed x the state's multiplier.
	inline float stateMult(const Table& t, State s, float swimMult)
	{
		switch ( s )
		{
			case State::Sprint: return t.sprint;
			case State::Sneak: return t.sneak;
			case State::Swim: return swimMult;
			case State::Levitate: return t.levitate;
			case State::Crawl: return t.crawl;
			default: return t.walk;
		}
	}
	inline float steadySpeed(const Table& t, float base, float mult) { return base * t.unitsPerSecPerFactor / kTps * mult; }

	// The wanted direction in world space, length <= 1. forward/side are the input axes (+forward, +right) already
	// corrected for confusion; yaw is the view yaw (Barony: forward = cos(yaw), sin(yaw); right = yaw + 90 degrees).
	// Returns the magnitude multiplier through *dirMult (backpedal / strafe shaping, 1 for straight forward).
	inline Vec2 wish(const Table& t, float forward, float side, float yaw, float lateralMult = 1.f, float backpedalMult = -1.f)
	{
		const float back = backpedalMult >= 0.f ? backpedalMult : t.backpedal;
		Vec2 l; // local frame: x = forward, y = right
		l.x = forward >= 0.f ? forward : forward * back;
		l.y = side * t.strafe * lateralMult;
		const float n = len(l);
		if ( n > 1.f ) { l.x /= n; l.y /= n; }
		const float cy = std::cos(yaw), sy = std::sin(yaw);
		Vec2 w;
		w.x = l.x * cy - l.y * sy;      // forward = (cos, sin); right = (cos(yaw+90), sin(yaw+90)) = (-sin, cos)
		w.y = l.x * sy + l.y * cy;
		return w;
	}

	// One tick of locomotion toward wish * target speed. `base` is the steady forward speed of this state (units/tick).
	// grip scales both acceleration and braking (ice, grease); control scales them (knocked: little steering).
	inline Vec2 stepLoco(const Table& t, Vec2 v, Vec2 w, float steady, float grip, float control)
	{
		const Vec2 target = { w.x * steady, w.y * steady };
		const float tl = len(target), sp = len(v);
		const float ref = std::max(steady, 0.05f);
		float rate;
		if ( tl < 1e-5f ) { rate = ref / (t.brakeSeconds * kTps); }
		else if ( sp > 1e-4f && dot(v, target) < 0.f ) { rate = ref / (t.turnSeconds * kTps); }
		else if ( tl >= sp ) { rate = ref / (t.accelSeconds * kTps); }
		else { rate = ref / (t.brakeSeconds * kTps); }
		rate *= std::max(0.f, grip) * std::max(0.f, control);
		const Vec2 d = { target.x - v.x, target.y - v.y };
		const float dl = len(d);
		if ( dl <= rate || dl < 1e-7f ) { return target; }
		return { v.x + d.x / dl * rate, v.y + d.y / dl * rate };
	}

	// Impulses: velocity (vx, vy) per tick, falling linearly to zero over n ticks. Distance on open floor |v| * (n + 1) / 2.
	struct Impulse { float vx = 0.f, vy = 0.f; int n = 0, k = 0; };
	struct ImpulseStack
	{
		static constexpr int kMax = 8;
		Impulse a[kMax];
		int count = 0;
		int dropped = 0;
		bool add(const Table& t, float vx, float vy, int ticks)
		{
			if ( ticks < 1 ) { return false; }
			const float l = std::sqrt(vx * vx + vy * vy);
			if ( l > t.impulseCap ) { vx *= t.impulseCap / l; vy *= t.impulseCap / l; }
			if ( count >= std::min(kMax, t.impulseStack) ) { ++dropped; return false; }
			a[count++] = { vx, vy, ticks, 0 };
			return true;
		}
		// the summed velocity for this tick; advances every record, removing the finished ones
		Vec2 tick()
		{
			Vec2 s;
			for ( int i = 0; i < count; )
			{
				Impulse& r = a[i];
				const float f = 1.f - (float)r.k / (float)r.n;
				s.x += r.vx * f; s.y += r.vy * f;
				if ( ++r.k >= r.n ) { a[i] = a[--count]; } else { ++i; }
			}
			return s;
		}
		bool any() const { return count > 0; }
		void clear() { count = 0; }
	};
	inline float impulseDistance(float speed, int ticks) { return speed * (float)(ticks + 1) * 0.5f; }

	// Legacy writers (knockback, dash, strafe kick, player push, wind): vanilla geometric decay.
	struct Legacy
	{
		Vec2 v;
		void kick(Vec2 k) { v.x += k.x; v.y += k.y; }
		Vec2 tick(float decay) { v.x *= decay; v.y *= decay; if ( len(v) < 0.005f ) { v = {}; } return v; }
	};

	inline Vec2 clampSpeed(const Table& t, Vec2 v)
	{
		const float l = len(v);
		return l > t.maxSpeedPerTick ? Vec2{ v.x * t.maxSpeedPerTick / l, v.y * t.maxSpeedPerTick / l } : v;
	}

	// Sprint gate (one tick). Returns the state of the key after this tick; *drain gets the stamina to spend.
	struct SprintState { bool exhausted = false; bool on = false; };
	inline void sprintTick(const Table& t, SprintState& s, bool keyHeld, bool allowed, float stamina, bool staminaTracked, float* drain)
	{
		*drain = 0.f;
		if ( !keyHeld ) { s.exhausted = false; s.on = false; return; }
		if ( s.exhausted || !allowed ) { s.on = false; return; }
		if ( staminaTracked && stamina <= 0.f ) { s.exhausted = true; s.on = false; return; }
		if ( !s.on && staminaTracked && stamina < t.sprintMinStamina ) { s.on = false; return; }
		s.on = true;
		if ( staminaTracked ) { *drain = t.sprintStaminaPerSecond / kTps; }
	}

	// Mouse look (the shootmode part of Barony's camera update, as our own functions; the caller keeps the yaw wrap, the
	// pitch clamp and the post-use decay). sens is Barony's mouse speed (128 = neutral); smooth accumulates and the caller
	// halves it each tick; raw sets the turn directly, clamped; confused flips X (and smoothed Y). The raw confused Y is not
	// flipped (as shipped by Barony). Barony stores pitch inverted: pitch -= rotY.
	inline void lookAxisX(const Table& t, float& rotX, float dx, float sens, bool smooth, bool confused, bool mobile, bool limit, float dt)
	{
		const float sgn = confused ? -1.f : 1.f, k = sens / 128.f;
		if ( smooth )
		{
			if ( mobile ) { rotX += sgn * dx * t.lookSmoothGain * k; }
			if ( limit ) { rotX = std::min(std::max(-t.lookSmoothClamp, rotX), t.lookSmoothClamp); }
			rotX *= std::pow(t.lookSmoothDecay, dt);
		}
		else if ( mobile )
		{
			rotX = sgn * dx * t.lookRawGain * k;
			if ( limit ) { rotX = std::min(std::max(-t.lookSmoothClamp, sgn * dx * t.lookRawGain * k), t.lookSmoothClamp); }
		}
		else { rotX = 0.f; }
	}
	inline void lookAxisY(const Table& t, float& rotY, float dy, float sens, bool smooth, bool reverse, bool confused, bool mobile, float dt)
	{
		const float sgn = confused ? -1.f : 1.f, k = sens / 128.f, inv = reverse ? 1.f : -1.f;
		if ( smooth )
		{
			if ( mobile ) { rotY += sgn * dy * t.lookSmoothGain * k * inv; }
			rotY = std::min(std::max(-t.lookSmoothClamp, rotY), t.lookSmoothClamp);
			rotY *= std::pow(t.lookSmoothDecay, dt);
		}
		else if ( mobile ) { rotY = std::min(std::max(-t.lookSmoothClamp, dy * t.lookRawGain * k * inv), t.lookSmoothClamp); }
		else { rotY = 0.f; }
	}
} // namespace core
} // namespace move
} // namespace apx
