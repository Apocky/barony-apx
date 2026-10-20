// apx/vox/builder.cpp -- see builder.hpp.
#include "../../main.hpp"
#include "../../items.hpp"
#include "../parts_engine.hpp"
#include "bx1.hpp"
#include "heldmodel.hpp"
#include "builder.hpp"

#include <algorithm>
#include <cstdlib>
#include <cstring>

namespace apx
{
namespace vox
{
namespace builder
{
	namespace
	{
		struct State { bool active = false; int option = 0; bx1::BuildSession s; };
		State g_state[MAXPLAYERS];
		Stats g_stats;

		bool slotFromName(const char* n, bx1::Slot& s)
		{
			for ( int k = 0; k < (int)bx1::Slot::NumSlots; ++k ) { if ( n && !strcmp(n, bx1::slotName((bx1::Slot)k)) ) { s = (bx1::Slot)k; return true; } }
			return false;
		}
	} // namespace

	bool enabled() { static const bool on = !(std::getenv("APX_VOX_BUILDER") && std::getenv("APX_VOX_BUILDER")[0] == '0'); return on; }
	const Stats& stats() { return g_stats; }

	bool open(int player, int optionId)
	{
		if ( !enabled() || player < 0 || player >= MAXPLAYERS ) { return false; }
		bx1::Slot slot;
		if ( !slotFromName(apxPartsSlotName(optionId >> 8), slot) ) { return false; } // no shape: the Make tab makes it directly
		float t[3] = { 0.6f, 0.6f, 0.65f };
		apxPartsMaterialTint(optionId & 0xFF, t);
		auto c = [](float v) { return (uint8_t)std::min(255.f, std::max(0.f, 110.f + v * 480.f)); }; // as heldmodel's material colour
		const bx1::Rgb base{ c(t[0]), c(t[1]), c(t[2]) };
		State& st = g_state[player];
		st.active = true;
		st.option = optionId;
		st.s.begin(slot, base, slot == bx1::Slot::Grip ? bx1::Rgb{ 110, 70, 40 } : bx1::Rgb{ (uint8_t)(base.r * 3 / 4), (uint8_t)(base.g * 3 / 4), (uint8_t)(base.b * 3 / 4) });
		++g_stats.opened;
		printlog("[apx-vox] builder open: player %d, slot %s, material %d, volume %dx%dx%d, limit %d", player, bx1::slotName(slot), optionId & 0xFF,
			st.s.model.sx, st.s.model.sy, st.s.model.sz, bx1::slotRule(slot).maxVoxels);
		return true;
	}

	bool active(int player) { return player >= 0 && player < MAXPLAYERS && g_state[player].active; }
	bx1::BuildSession* session(int player) { return active(player) ? &g_state[player].s : nullptr; }
	void cancel(int player) { if ( active(player) ) { g_state[player].active = false; printlog("[apx-vox] builder cancelled: player %d", player); } }

	Item* build(int player, std::string& why)
	{
		if ( !active(player) ) { why = "no build open"; return nullptr; }
		State& st = g_state[player];
		const bx1::Check c = st.s.validate();
		if ( !c.ok ) { why = c.why; ++g_stats.refused; return nullptr; }
		const uint32_t look = storeLook(st.s.model);
		if ( !look ) { why = "could not save the model"; ++g_stats.refused; return nullptr; }
		registerModel(st.s.model);
		Item* made = apxPartsBenchMakeLook(player, st.option, look, why);
		if ( !made ) { ++g_stats.refused; return nullptr; }
		st.active = false;
		++g_stats.built;
		printlog("[apx-vox] builder built: player %d, slot %s, %d voxels, shaping points %d, look %08x, item uid %u", player, bx1::slotName(st.s.slot),
			c.voxels, bx1::shapingPoints(c), look, made->uid);
		return made;
	}
} // namespace builder
} // namespace vox
} // namespace apx
