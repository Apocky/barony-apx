// apx/vox/builder_ui.cpp -- the voxel builder's panel (BX1, C10, D87): drawn over the screen while a build is open.
//
// A grid of the current layer (x across, z along the part; z = 0, the tip, at the top), one button per cell: a click
// toggles the voxel (mirrored across x when Mirror is on). Filled cells show their colour; cells filled on other layers
// show faintly; empty mount cells are outlined in gold. Buttons: Layer -/+, Mirror, Colour, Clear, Build, Cancel.
// The status line names the slot, layer, voxel count against the limit and why the build is not valid yet, or that a
// valid build earns the full 20 shaping points. Build calls builder::build (the component is made with the look).
#include "../../main.hpp"
#include "../../game.hpp"
#include "../../player.hpp"
#include "../../net.hpp"
#include "../../interface/interface.hpp"
#include "../../ui/Frame.hpp"
#include "../../ui/Button.hpp"
#include "../../ui/Field.hpp"
#include "../../apx_bench_api.hpp"
#include "../parts_engine.hpp"
#include "bx1.hpp"
#include "builder.hpp"
#include "builder_ui.hpp"

#include <cstdio>
#include <cstdlib>
#include <string>

namespace apx
{
namespace vox
{
namespace builderui
{
	namespace
	{
		const char* kName = "apx_vox_builder";
		const char* kFont = "fonts/pixel_maz.ttf#32#2";
		std::string g_msg[MAXPLAYERS];

		int ownerOf(Button& b) { const int p = b.getOwner(); return p >= 0 && p < MAXPLAYERS ? p : 0; }

		void cellClick(Button& b)
		{
			const int p = ownerOf(b);
			bx1::BuildSession* s = builder::session(p);
			int x = -1, z = -1;
			if ( !s || sscanf(b.getName(), "cell %d %d", &x, &z) != 2 ) { return; }
			if ( s->model.get(x, s->layer, z) != 255 ) { s->remove(x, z); } else { s->add(x, z); }
		}
		void layerDown(Button& b) { if ( auto* s = builder::session(ownerOf(b)) ) { s->layerDown(); } }
		void layerUp(Button& b) { if ( auto* s = builder::session(ownerOf(b)) ) { s->layerUp(); } }
		void mirror(Button& b) { if ( auto* s = builder::session(ownerOf(b)) ) { s->mirror = !s->mirror; } }
		void colour(Button& b) { if ( auto* s = builder::session(ownerOf(b)) ) { s->colour = (uint8_t)(s->colour % 6 + 1); } }
		void clearAll(Button& b) { if ( auto* s = builder::session(ownerOf(b)) ) { s->clear(); } }
		void cancel(Button& b) { const int p = ownerOf(b); builder::cancel(p); g_msg[p] = "Cancelled."; }
		void buildIt(Button& b)
		{
			const int p = ownerOf(b);
			std::string why;
			Item* made = builder::build(p, why);
			g_msg[p] = made ? "Built and made the component." : "Cannot build: " + why;
			if ( made ) { messagePlayer(p, MESSAGE_INVENTORY, "%s", g_msg[p].c_str()); }
		}

		Uint32 paletteColour(const bx1::VoxModel& m, uint8_t c, int alpha)
		{
			return makeColor(m.pal[c * 3] << 2, m.pal[c * 3 + 1] << 2, m.pal[c * 3 + 2] << 2, alpha);
		}

		Button* addButton(Frame* f, int player, const char* name, const char* text, SDL_Rect r, void (*cb)(Button&))
		{
			Button* b = f->addButton(name);
			b->setOwner(player);
			b->setSize(r);
			b->setText(text);
			b->setFont(kFont);
			b->setColor(makeColor(70, 60, 50, 255));
			b->setHighlightColor(makeColor(110, 95, 75, 255));
			b->setTextColor(makeColor(240, 230, 210, 255));
			b->setBorder(1);
			b->setCallback(cb);
			return b;
		}
	} // namespace

	void tick()
	{
		if ( !apx_bench::makeOpenHook && builder::enabled() ) { apx_bench::makeOpenHook = &builder::open; }
		if ( !gui ) { return; }
		for ( int p = 0; p < MAXPLAYERS; ++p )
		{
			if ( !players[p] || !players[p]->isLocalPlayer() ) { continue; }
			char fname[48];
			snprintf(fname, sizeof fname, "%s_%d", kName, p);
			Frame* f = gui->findFrame(fname);
			bx1::BuildSession* s = builder::session(p);
			if ( !s ) { if ( f ) { f->removeSelf(); } continue; }

			const int sx = s->model.sx, sz = s->model.sz;
			const int cell = std::max(8, std::min(16, 560 / std::max(sz, 1)));
			const int gridW = sx * cell, gridH = sz * cell;
			const int W = std::max(gridW + 40, 660), H = gridH + 150;
			if ( !f )
			{
				f = gui->addFrame(fname);
				f->setOwner(p);
				f->setSize(SDL_Rect{ (Frame::virtualScreenX - W) / 2, std::max(10, (Frame::virtualScreenY - H) / 2), W, H });
				f->setActualSize(SDL_Rect{ 0, 0, W, H });
				f->setColor(makeColor(18, 14, 10, 235));
				f->setBorder(2);
				Field* title = f->addField("title", 256);
				title->setFont(kFont); title->setSize(SDL_Rect{ 12, 6, W - 24, 28 }); title->setColor(makeColor(250, 220, 150, 255));
				Field* status = f->addField("status", 256);
				status->setFont(kFont); status->setSize(SDL_Rect{ 12, H - 74, W - 24, 28 }); status->setColor(makeColor(220, 220, 220, 255));
				Field* msg = f->addField("msg", 256);
				msg->setFont(kFont); msg->setSize(SDL_Rect{ 12, H - 46, W - 24, 28 }); msg->setColor(makeColor(200, 200, 160, 255));
				const int gx = (W - gridW) / 2, gy = 40;
				for ( int x = 0; x < sx; ++x ) for ( int z = 0; z < sz; ++z )
				{
					char n[32]; snprintf(n, sizeof n, "cell %d %d", x, z);
					Button* b = f->addButton(n);
					b->setOwner(p);
					b->setSize(SDL_Rect{ gx + x * cell, gy + z * cell, cell - 1, cell - 1 });
					b->setBorder(1);
					b->setCallback(&cellClick);
				}
				const int by = gy + gridH + 8, bw = (W - 24) / 7;
				const struct { const char* n; const char* t; void (*cb)(Button&); } btns[] = {
					{ "layer down", "Layer -", &layerDown }, { "layer up", "Layer +", &layerUp }, { "mirror", "Mirror", &mirror },
					{ "colour", "Colour", &colour }, { "clear", "Clear", &clearAll }, { "build", "Build", &buildIt }, { "cancel", "Cancel", &cancel },
				};
				for ( int k = 0; k < 7; ++k ) { addButton(f, p, btns[k].n, btns[k].t, SDL_Rect{ 12 + k * bw, by, bw - 4, 26 }, btns[k].cb); }
				g_msg[p].clear();
			}

			// refresh every tick from the session
			const bx1::Check c = s->validate();
			char line[256];
			snprintf(line, sizeof line, "Make a %s -- build it voxel by voxel (free build, D87)", bx1::slotName(s->slot));
			if ( Field* t = f->findField("title") ) { t->setText(line); }
			snprintf(line, sizeof line, "Layer %d/%d  %s  voxels %d/%d  %s", s->layer + 1, s->model.sy, s->mirror ? "mirror" : "", c.voxels,
				bx1::slotRule(s->slot).maxVoxels, c.ok ? "VALID: Build earns the full 20 shaping points" : ("not yet: " + c.why).c_str());
			if ( Field* t = f->findField("status") ) { t->setText(line); }
			if ( Field* t = f->findField("msg") ) { t->setText(g_msg[p].c_str()); }
			for ( int x = 0; x < sx; ++x ) for ( int z = 0; z < sz; ++z )
			{
				char n[32]; snprintf(n, sizeof n, "cell %d %d", x, z);
				Button* b = f->findButton(n);
				if ( !b ) { continue; }
				const uint8_t here = s->model.get(x, s->layer, z);
				bool other = false;
				for ( int y = 0; y < s->model.sy && !other; ++y ) { other = y != s->layer && s->model.get(x, y, z) != 255; }
				const bool mount = s->isMount(x, s->layer, z);
				Uint32 col = here != 255 ? paletteColour(s->model, here, 255) : (mount ? makeColor(120, 92, 24, 255) : (other ? makeColor(70, 66, 60, 255) : makeColor(34, 30, 26, 255))); // empty mount cells: dim gold
				b->setColor(col);
				b->setHighlightColor(here != 255 ? col : makeColor(90, 84, 70, 255));
				b->setBorderColor(here == 255 && s->isMount(x, s->layer, z) ? makeColor(255, 200, 60, 255) : makeColor(10, 8, 6, 255));
			}
			if ( Button* b = f->findButton("build") ) { b->setColor(c.ok ? makeColor(60, 110, 50, 255) : makeColor(60, 50, 45, 255)); }
		}
	}
} // namespace builderui
} // namespace vox
} // namespace apx
