// apx/visshots.cpp -- see visshots.hpp.
#include "../main.hpp"
#include "../game.hpp"
#include "../entity.hpp"
#include "../player.hpp"
#include "visshots.hpp"
#include "vision.hpp"
#include "vox/heldmodel.hpp"
#include "parts_engine.hpp"
#include "../items.hpp"
#include <cstdio>
#include <cstdlib>

#include <cmath>
#include <string>

namespace apx
{
namespace vis
{
	namespace
	{
		bool g_armed = false;
		bool g_probe = false;          // -apxlightprobe
		bool g_voxShot = false;        // -apxvoxshot (BX1)
		int g_voxTicks = 0;
		int g_probeTicks = 0;
		int g_ticks = 0;
		int g_phase = 0;               // 0 settle, 1 far view, 2 near view, 3 done
		int g_mark = 0;
		real_t g_farYaw = 0, g_nearYaw = 0, g_farLen = 0, g_nearLen = 0;
		std::string g_shot;

		bool solid(int x, int y)
		{
			if ( x < 0 || y < 0 || x >= (int)map.width || y >= (int)map.height ) { return true; }
			return map.tiles[OBSTACLELAYER + y * MAPLAYERS + x * MAPLAYERS * map.height] != 0;
		}

		// distance in tiles from (x, y) (world units) along yaw until a wall
		real_t ray(real_t x, real_t y, real_t yaw)
		{
			const real_t step = 0.25;
			real_t d = 0;
			for ( ; d < 512; d += step )
			{
				const real_t tx = x / 16.0 + cos(yaw) * d, ty = y / 16.0 + sin(yaw) * d;
				if ( solid((int)floor(tx), (int)floor(ty)) ) { break; }
			}
			return d;
		}

		void aim(real_t yaw)
		{
			Entity* me = players[0] ? players[0]->entity : nullptr;
			if ( !me ) { return; }
			me->yaw = yaw;
			me->pitch = 0;
		}
	} // namespace

	void arm() { g_armed = true; }
	bool armed() { return g_armed; }
	void armProbe() { g_probe = true; }
	void armVoxShot() { g_voxShot = true; }

	// -apxvoxshot (BX1, C10): equip a steel sword with rolled parts, face the longest open line, shoot the first-person
	// view ("vox-held.png"), log the sprite, quit. APX_VOX_HELD=0 gives the same shot with the tinted vanilla sword.
	static void voxShotTick(bool ready)
	{
		if ( !g_voxShot || !ready ) { return; }
		Entity* me = players[0]->entity;
		++g_voxTicks;
		if ( g_voxTicks == 60 )
		{
			Item* sword = newItem(STEEL_SWORD, SERVICABLE, 0, 1, 0, true, &stats[0]->inventory);
			apxPartsBind(sword, 0, 5);
			equipItem(sword, &stats[0]->weapon, 0, false);
			real_t best = -1, yaw = 0;
			for ( int k = 0; k < 72; ++k ) { const real_t y = k * 2 * PI / 72, d = ray(me->x, me->y, y); if ( d > best ) { best = d; yaw = y; } }
			me->yaw = yaw; me->pitch = 0;
			printlog("[apx-vox] voxshot: sword parts %d, rollKey %u", sword ? (int)sword->apx.nParts : -1, sword ? sword->apx.rollKey : 0u);
		}
		if ( g_voxTicks == 160 ) { g_shot = "vox-held.png"; }
		if ( g_voxTicks == 166 )
		{
			const Entity* hud = players[0]->hud.weapon;
			printlog("[apx-vox] voxshot done: hud weapon sprite %d, base models %u, nummodels %u", hud ? (int)hud->sprite : -1, apx::vox::baseModelCount(), nummodels);
			g_voxShot = false;
			mainloop = 0;
		}
	}

	// -apxlightprobe: the light monsters see (entityLightAt) on every open tile of the test floor, written as one
	// JSON row of per-tile values, then quit. Two launches (APX_VIS_FALLOFF=0 vs on) give the change in how far
	// a monster notices the player: notice distance = min(sight range, light after reductions) in world units.
	static void probeTick(bool ready)
	{
		if ( !g_probe || !ready ) { return; }
		if ( ++g_probeTicks < 100 ) { return; } // lights placed and the floor settled
		g_probe = false;
		// the floor's own lights only: player 0's light goes wherever the player stands (and is the same at the
		// player's own tile under either curve), so it is taken out before sampling; it comes back next tick
		if ( players[0] && players[0]->entity && players[0]->entity->light )
		{
			list_RemoveNode(players[0]->entity->light->node);
			players[0]->entity->light = nullptr;
		}
		const char* d = std::getenv("APX_DIAG_DIR");
		const std::string path = std::string(d && d[0] ? d : ".") + "/lightprobe.json";
		FILE* f = fopen(path.c_str(), "w");
		int open = 0; long long sum = 0; int lit = 0;
		if ( f ) { fprintf(f, "{\"map\":[%u,%u],\"floor\":%d,\"falloff\":%d,\"tiles\":[", map.width, map.height, currentlevel, apx::vision::falloffOn() ? 1 : 0); }
		for ( int x = 0; x < (int)map.width; ++x )
		{
			for ( int y = 0; y < (int)map.height; ++y )
			{
				if ( solid(x, y) || !map.tiles[y * MAPLAYERS + x * MAPLAYERS * map.height] ) { continue; } // walls and pits
				const int L = entityLightAt(x, y);
				if ( f ) { fprintf(f, "%s[%d,%d,%d]", open ? "," : "", x, y, L); }
				++open; sum += L; lit += L > 0;
			}
		}
		if ( f ) { fprintf(f, "]}\n"); fclose(f); }
		printlog("[apx-vis] lightprobe: %d open tiles, mean light %.2f, lit %d, falloff %d, file %s", open, open ? (double)sum / open : 0.0, lit,
			apx::vision::falloffOn() ? 1 : 0, path.c_str());
		mainloop = 0;
	}

	void tick(bool ready)
	{
		probeTick(ready);
		voxShotTick(ready);
		if ( !g_armed || g_phase >= 3 ) { return; }
		if ( !ready ) { g_ticks = 0; return; }
		++g_ticks;
		Entity* me = players[0]->entity;
		const int settle = 60; // lightmap smoothing and the camera bob settle
		if ( g_phase == 0 )
		{
			if ( g_ticks < 100 ) { return; }
			g_farLen = -1; g_nearLen = 1e9;
			for ( int k = 0; k < 72; ++k )
			{
				const real_t yaw = k * 2 * PI / 72;
				const real_t d = ray(me->x, me->y, yaw);
				if ( d > g_farLen ) { g_farLen = d; g_farYaw = yaw; }
				if ( d >= 1.5 && d < g_nearLen ) { g_nearLen = d; g_nearYaw = yaw; }
			}
			if ( g_nearLen > 1e8 ) { g_nearLen = g_farLen; g_nearYaw = g_farYaw + PI; }
			g_phase = 1; g_mark = g_ticks;
		}
		if ( g_phase == 1 )
		{
			aim(g_farYaw);
			if ( g_ticks - g_mark == settle ) { g_shot = "vis-far.png"; }
			if ( g_ticks - g_mark >= settle + 2 ) { g_phase = 2; g_mark = g_ticks; }
			return;
		}
		if ( g_phase == 2 )
		{
			aim(g_nearYaw);
			if ( g_ticks - g_mark == settle ) { g_shot = "vis-near.png"; }
			if ( g_ticks - g_mark >= settle + 5 )
			{
				printlog("[apx-vis] shots done: far %.1f tiles yaw %.3f, near %.1f tiles yaw %.3f, pos %.1f,%.1f, map %ux%u, floor %d",
					g_farLen, g_farYaw, g_nearLen, g_nearYaw, me->x / 16.0, me->y / 16.0, map.width, map.height, currentlevel);
				g_phase = 3;
				mainloop = 0;
			}
		}
	}

	const char* takeShot()
	{
		static std::string out;
		if ( g_shot.empty() ) { return nullptr; }
		out = g_shot;
		g_shot.clear();
		return out.c_str();
	}
} // namespace vis
} // namespace apx
