// apx/in/in.hpp -- apx::in, spec 04b section 3 contract.
//
// STUB (MOVE lane, 2026-10-19). The INPUT lane (branch apx/input, phase C0) owns this module: it will replace this file and
// in.cpp with the real latching layer (devices, actions, bindings, owner gate). Until then take() reads Barony's CURRENT input
// (Input::inputs[p], the controller sticks, keystatus for Left Shift) once per sim tick and fills the section-3 TickRecord.
// Only the fields apx::move reads are filled (held bits for Sprint/Sneak, moveX/moveY, lookDX/lookDY); press and release counts
// stay 0. Nothing here consumes another system's press (binary(), not binaryToggle()).
#pragma once
#include <cstdint>
#define APX_IN_STUB 1 // removed when the INPUT lane's header replaces this file

namespace apx
{
namespace in
{
	// Same names and order as the INPUT lane's apx::in (branch apx/input), subset only; their header replaces this one.
	enum Button : int
	{
		B_Attack = 0, B_Block, B_Dodge, B_Kick, B_Use, B_Cast, B_Sneak, B_Sprint, B_Inventory, B_Map, B_Menu,
		B_COUNT
	};

	struct TickRecord
	{
		uint64_t held = 0;                    // bit per Button
		uint16_t pressCount[B_COUNT] = {};    // presses since the last tick (stub: 0)
		uint16_t releaseCount[B_COUNT] = {};
		float moveX = 0.f;                    // +1 = right
		float moveY = 0.f;                    // +1 = forward, -1 = backward
		float lookDX = 0.f, lookDY = 0.f;     // look motion this tick (pixels / analogue units)
		float swingDX = 0.f, swingDY = 0.f;   // look motion during a wind-up (stub: 0)
		bool isHeld(int b) const { return (held >> b) & 1u; }
	};

	const TickRecord& take(int player);       // once per sim tick; the same record until the tick advances
	bool uiOwnsMouse(int player);
	bool uiOwnsKeyboard(int player);
}
}
