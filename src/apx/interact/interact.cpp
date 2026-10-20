// apx/interact/interact.cpp -- apx::interact (see interact.hpp). Barony glue; the geometry is interact_rules.hpp.
#include "interact.hpp"
#include "interact_rules.hpp"
#include "../in/in.hpp"
#include "../ui/ui.hpp"
#include "../props.hpp"
#include "../../imgui/imgui.h"
#include <SDL.h>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <unordered_map>
#include <unordered_set>

namespace apx
{
namespace interact
{
	namespace
	{
		Counters g_c;
		struct Cached { Uint32 tick = 0xFFFFFFFFu; Prompt p; };
		Cached g_cache[MAXPLAYERS];
		Uint32 g_lastLogged[MAXPLAYERS] = {};
		std::unordered_set<Uint32> g_snuffed, g_searched;
		struct Cls { Uint32 uid = 0; const props::PropRow* row = nullptr; };
		std::unordered_map<Entity*, Cls> g_cls; // collider -> its props row (validated by uid; a new level reuses pointers rarely)

		// the use row of a collider (cached: rowFor walks the table and names)
		const props::PropRow* colliderRow(Entity* e)
		{
			auto it = g_cls.find(e);
			if ( it != g_cls.end() && it->second.uid == e->getUID() ) { return it->second.row; }
			const props::PropRow* r = props::rowFor(e, true);
			g_cls[e] = { e->getUID(), r };
			return r;
		}

		float radiusOf(Entity* e)
		{
			float r = (float)std::max(e->sizex, e->sizey);
			if ( e->behavior == &actColliderDecoration ) { r = std::max(r, (float)std::max(e->colliderSizeX, e->colliderSizeY)); }
			return std::max(r, 3.f);
		}

		float wrapPitch(float v)
		{
			v = std::fmod(v, 2.f * (float)PI);
			if ( v > (float)PI ) { v -= 2.f * (float)PI; }
			if ( v < -(float)PI ) { v += 2.f * (float)PI; }
			return v;
		}

		bool envOld()
		{
			static const char* v = std::getenv("APX_INTERACT_OLD");
			return v && v[0] == '1';
		}

		const char* keyLabel()
		{
			static char buf[40];
			in::Raw d, u;
			if ( in::resolveAction("Use", &d, &u) )
			{
				if ( d.type == in::R_MouseDown ) { snprintf(buf, sizeof buf, "%s", d.code == 1 ? "LMB" : d.code == 2 ? "MMB" : d.code == 3 ? "RMB" : "Mouse"); return buf; }
				if ( d.type == in::R_KeyDown ) { snprintf(buf, sizeof buf, "%s", SDL_GetScancodeName((SDL_Scancode)d.code)); return buf; }
			}
			return "Use";
		}

		bool vanillaTooltipShowing(int pn)
		{
			return players[pn] && players[pn]->worldUI.bTooltipInView;
		}

		void fillPrompt(Prompt& p, Entity* e, int kind, const char* verb, const char* text, const Aim& a, float reach)
		{
			p.e = e; p.uid = e->getUID(); p.kind = kind;
			snprintf(p.verb, sizeof p.verb, "%s", verb);
			snprintf(p.text, sizeof p.text, "%s", text);
			p.dist = a.dist; p.edge = a.edge; p.reach = reach; p.miss = a.miss;
		}

		// the query itself, uncached
		Entity* query(int pn, Prompt* out)
		{
			if ( out ) { *out = Prompt(); }
			if ( pn < 0 || pn >= MAXPLAYERS || !players[pn] || !players[pn]->entity || !map.entities ) { return nullptr; }
			Entity* me = players[pn]->entity;
			const float ox = (float)me->x, oy = (float)me->y, yaw = (float)me->yaw;
			const float pitch = wrapPitch((float)cameras[pn].vang);
			Entity* best = nullptr;
			Prompt bp;
			float bestScore = 1e9f;
			for ( node_t* n = map.entities->first; n; n = n->next )
			{
				Entity* e = (Entity*)n->element;
				if ( !e || e->flags[INVISIBLE] ) { continue; }
				int kind = K_NONE;
				const props::PropRow* row = nullptr;
				const char* verb = "";
				std::string label;
				float reach = 28.f;
				if ( e->behavior == &actFurniture )
				{
					row = props::rowFor(e, true);
					if ( !row ) { continue; }
					kind = K_FURNITURE;
				}
				else if ( e->behavior == &actTorch )
				{
					if ( !props::torchDark(e) ) { continue; }
					kind = K_TORCH;
				}
				else if ( e->behavior == &actColliderDecoration )
				{
					row = colliderRow(e);
					if ( !row ) { continue; }
					kind = K_COLLIDER;
				}
				else { continue; }
				// cheap reject before the geometry: a prop beyond ~48 units cannot be in reach
				if ( std::fabs((float)e->x - ox) > 48.f || std::fabs((float)e->y - oy) > 48.f ) { continue; }
				if ( row ) { reach = (float)row->reach; verb = row->verb.c_str(); }
				else { verb = "relight"; }
				const char* lab = props::labelFor(e, pn);
				if ( kind == K_COLLIDER && row && row->verb == "snuff" ) { lab = g_snuffed.count(e->getUID()) ? "Relight the candles" : "Snuff the candles"; }
				if ( kind == K_COLLIDER && row && row->verb == "search" && g_searched.count(e->getUID()) ) { lab = "Searched"; }
				if ( !lab ) { continue; }
				const Aim a = aimAt(ox, oy, yaw, (float)e->x, (float)e->y, radiusOf(e), reach);
				if ( !a.ok || !pitchOk(a.forward, pitch) ) { continue; }
				// walls block: the trace stops at a wall (entities ignored); it must reach this prop's edge
				const real_t ang = std::atan2(e->y - oy, e->x - ox);
				const real_t tr = lineTrace(me, ox, oy, ang, std::max(1.f, a.dist), LINETRACE_IGNORE_ENTITIES, false);
				if ( tr + 2.5 < a.edge ) { continue; }
				const float sc = score(a, true);
				if ( sc < bestScore )
				{
					bestScore = sc;
					best = e;
					fillPrompt(bp, e, kind, verb, lab, a, reach);
				}
			}
			if ( best && out ) { *out = bp; }
			return best;
		}
	} // namespace

	bool enabled() { return !envOld() && multiplayer != CLIENT; }
	const Counters& counters() { return g_c; }

	bool isProp(Entity* e)
	{
		if ( !e || !enabled() ) { return false; }
		if ( e->behavior == &actFurniture ) { return props::rowFor(e, true) != nullptr; }
		if ( e->behavior == &actTorch ) { return props::torchDark(e); }
		if ( e->behavior == &actColliderDecoration ) { return colliderRow(e) != nullptr; }
		return false;
	}

	Entity* target(int pn, Prompt* out)
	{
		if ( !enabled() || pn < 0 || pn >= MAXPLAYERS ) { if ( out ) { *out = Prompt(); } return nullptr; }
		Cached& c = g_cache[pn];
		if ( c.tick != ::ticks )
		{
			c.tick = ::ticks;
			++g_c.queries;
			Entity* e = query(pn, &c.p);
			c.p.e = e;
			if ( c.p.uid != g_lastLogged[pn] )
			{
				g_lastLogged[pn] = c.p.uid;
				if ( e )
				{
					++g_c.promptsShown;
					printlog("[apx-interact] prompt player=%d uid=%u kind=%d verb=%s text='%s' dist=%.1f edge=%.1f reach=%.0f miss=%.1f",
						pn, c.p.uid, c.p.kind, c.p.verb, c.p.text, c.p.dist, c.p.edge, c.p.reach, c.p.miss);
				}
				else { printlog("[apx-interact] prompt player=%d cleared", pn); }
			}
		}
		if ( out ) { *out = c.p; }
		return c.p.e;
	}

	namespace
	{
		bool act(int pn, Entity* e, const Prompt& p)
		{
			Entity* me = players[pn]->entity;
			if ( p.kind == K_FURNITURE )
			{
				if ( props::onUse(e, pn) ) { return true; }
				const int ft = e->furnitureType;
				messagePlayer(pn, MESSAGE_INTERACTION, Language::get(ft == FURNITURE_CHAIR ? 476 : ft == FURNITURE_BED ? 2493 : ft == FURNITURE_PODIUM ? 2496 : 477));
				return true;
			}
			if ( p.kind == K_TORCH ) { return props::torchUse(e, pn); }
			if ( p.kind == K_COLLIDER )
			{
				const props::PropRow* r = colliderRow(e);
				if ( !r ) { return false; }
				if ( r->verb == "snuff" )
				{
					const bool wasSnuffed = g_snuffed.count(e->getUID()) > 0;
					if ( wasSnuffed ) { g_snuffed.erase(e->getUID()); } else { g_snuffed.insert(e->getUID()); }
					props::C().snuffs++;
					messagePlayer(pn, MESSAGE_INTERACTION, wasSnuffed ? "You relight the candles." : "You snuff the candles.");
					printlog("[apx-interact] %s candelabra uid=%u player=%d", wasSnuffed ? "relit" : "snuffed", e->getUID(), pn);
					return true;
				}
				if ( r->verb == "search" )
				{
					if ( g_searched.count(e->getUID()) ) { messagePlayer(pn, MESSAGE_INTERACTION, "You already searched it. Nothing more."); return true; }
					g_searched.insert(e->getUID());
					props::C().searches++;
					const int before = props::C().scrap + props::C().trinkets;
					props::giveOutcome(*r, e, false);
					const bool found = props::C().scrap + props::C().trinkets > before;
					messagePlayer(pn, MESSAGE_INTERACTION, found ? "You search the iron maiden and find something." : "You search the iron maiden. Only rust and old blood.");
					printlog("[apx-interact] searched iron maiden uid=%u player=%d found=%d", e->getUID(), pn, found ? 1 : 0);
					return true;
				}
				if ( r->verb == "break" )
				{
					props::C().rattles++;
					messagePlayer(pn, MESSAGE_INTERACTION, "The cage rattles on its chain. Hit it to break it open.");
					printlog("[apx-interact] rattled cage uid=%u player=%d", e->getUID(), pn);
					return true;
				}
			}
			(void)me;
			return false;
		}
	} // namespace

	bool use(int pn)
	{
		if ( !enabled() || pn < 0 || pn >= MAXPLAYERS || !players[pn] || !players[pn]->entity ) { return false; }
		if ( !players[pn]->shootmode || !players[pn]->bControlEnabled || gamePaused || players[pn]->usingCommand() ) { return false; }
		if ( in::oldPath() || !in::loaded() ) { ++g_c.usesOldPath; return false; }
		const in::TickRecord& rec = in::take(pn);
		if ( !rec.pressCount[in::B_Use] ) { return false; }
		Prompt p;
		Entity* e = target(pn, &p);
		if ( !e ) { ++g_c.usesNoTarget; return false; }
		const bool ok = act(pn, e, p);
		if ( ok )
		{
			++g_c.uses;
			++g_c.byKind[p.kind & 3];
			printlog("[apx-interact] use player=%d uid=%u kind=%d verb=%s edge=%.1f reach=%.0f", pn, p.uid, p.kind, p.verb, p.edge, p.reach);
		}
		return ok;
	}

	void afterOldClick(int pn, Entity*& selected)
	{
		if ( !enabled() ) { return; }
		if ( selected && isProp(selected) ) { selected = nullptr; } // the old path's pick of a prop is ours now
		if ( !selected ) { use(pn); }
	}

	bool suppressTooltip(Entity* parent)
	{
		if ( !isProp(parent) ) { return false; }
		++g_c.tooltipsSuppressed;
		return true;
	}

	void colliderLight(Entity* e)
	{
		if ( !enabled() || !e ) { return; }
		const props::PropRow* r = colliderRow(e);
		if ( r && r->verb == "snuff" && !g_snuffed.count(e->getUID()) ) { e->light = addLight(e->x / 16, e->y / 16, "torch_wall"); }
	}

	void draw()
	{
		if ( !enabled() || !ui::contextReady() ) { return; }
		for ( int pn = 0; pn < MAXPLAYERS; ++pn )
		{
			if ( !players[pn] || !players[pn]->isLocalPlayer() || !players[pn]->entity ) { continue; }
			if ( !players[pn]->shootmode || gamePaused || in::uiOwnsMouse(pn) || vanillaTooltipShowing(pn) ) { continue; }
			Prompt p;
			if ( !target(pn, &p) ) { continue; }
			char line[120];
			snprintf(line, sizeof line, "[%s]  %s", keyLabel(), p.text);
			ImFont* f = ui::fontBody();
			const float fs = f->FontSize;
			const ImVec2 sz = f->CalcTextSizeA(fs, 4096.f, 0.f, line);
			const float cx = (float)players[pn]->camera_midx(), cy = (float)players[pn]->camera_midy() + (float)players[pn]->camera_height() * 0.16f;
			const float pad = fs * 0.5f;
			const ImVec2 a(cx - sz.x * 0.5f - pad, cy - pad * 0.6f), b(cx + sz.x * 0.5f + pad, cy + sz.y + pad * 0.6f);
			ImDrawList* dl = ImGui::GetForegroundDrawList();
			dl->AddRectFilled(a, b, IM_COL32(24, 17, 12, 215));
			dl->AddRect(a, b, IM_COL32(204, 158, 64, 255));
			dl->AddText(f, fs, ImVec2(cx - sz.x * 0.5f, cy), IM_COL32(237, 219, 178, 255), line);
			ui::recordRect("interact.prompt", a, b);
		}
	}
} // namespace interact
} // namespace apx
