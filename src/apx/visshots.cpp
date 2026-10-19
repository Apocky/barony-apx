// apx/visshots.cpp -- see visshots.hpp.
#include "../main.hpp"
#include "../game.hpp"
#include "../entity.hpp"
#include "../player.hpp"
#include "visshots.hpp"

#include <cmath>
#include <string>

namespace apx
{
namespace vis
{
	namespace
	{
		bool g_armed = false;
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

	void tick(bool ready)
	{
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
