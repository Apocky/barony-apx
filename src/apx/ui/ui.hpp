// apx/ui/ui.hpp -- D92 P0: the one Dear ImGui runtime of the game (spec Apockalypsis
// specs/barony-fork/08b-INVENTORY-WINDOW.md sections 2, 3 and 7).
//
// - ONE ImGui context per process, created on the first game frame (ensureContext). The dev menu
//   (ImGui_t in mod_tools) draws on the same context; ImGui_t::init only shows it.
// - THE INPUT GATE: every SDL event goes through processEvent first. It feeds ImGui and returns true
//   when Barony must not see the event (mouse buttons/wheel while one of our windows owns the mouse,
//   keys while a text field is active or while our window uses that key). Mouse-button and key RELEASES
//   always reach Barony so nothing sticks. No other code reads raw input while our window is open.
// - Look: Barony's own pixel TTFs from the install (PhysFS), integer sizes, no oversampling, nearest
//   filtering; no rounding anywhere; Barony palette.
// - Stable ids: every interactive element is made through idItem(); its screen rect is recorded each
//   frame for the QA driver (`ui <id>`, `uidump`).
#pragma once
#include <SDL.h>
#include <string>
#include <vector>

class Item;
struct ImFont;
struct ImVec2;

namespace apx
{
namespace ui
{
	void ensureContext();              // create the context, fonts, style, backends (idempotent)
	bool contextReady();
	bool processEvent(const SDL_Event& e); // the input gate: true = Barony must not see this event
	bool wantsMouse(int player);       // apx::in's owner gate: pointer over our UI (or a UI drag), for the keyboard-and-mouse player
	bool wantsKeyboard(int player);    // apx::in's owner gate: a UI text field is active
	bool wantMouse();                  // our UI (dev menu or a window) owns the mouse this frame
	bool ownsTextInput();              // a text field of ours turned SDL text input on (Barony must leave it on)
	void frame();                      // once per main-loop pass: new ImGui frame, dev menu, our windows
	void render();                     // once per drawn frame, last (before Barony's cursor)

	// stable ids (section 7)
	struct IdRect { std::string id; float x0 = 0, y0 = 0, x1 = 0, y1 = 0; };
	void recordLastItem(const char* id);           // the rect of the item ImGui just submitted
	void recordRect(const char* id, const ImVec2& a, const ImVec2& b); // an explicit rect (windows)
	bool findRect(const std::string& id, IdRect& out); // last frame's rect of a visible id
	const std::vector<IdRect>& lastRects();

	// look (section 3)
	ImFont* fontBody();
	ImFont* fontSmall();
	ImFont* fontHead();
	int scale();                                   // integer UI scale (1 at 720p, 2 at 1440p)
	void* itemIcon(int player, Item& item, int* w = nullptr, int* h = nullptr); // ImTextureID of Barony's item art

	// D90-style frame cost: CPU ms of frame()+render() while any of our item windows is open
	struct Cost { int frames = 0; double p50 = 0, p99 = 0, max = 0, last = 0, buildP50 = 0, buildP99 = 0, cpuP50 = 0, cpuP99 = 0, cpuMax = 0; }; // cpu* = this thread's CPU time (not wall) // build = frame() (widgets), rest = ImGui render
	Cost costWhileOpen();
	void resetCost();

	// gate counters (diag "ui" scope: shown in uidump and the QA status)
	struct GateStats { unsigned mouseSwallowed = 0, keysSwallowed = 0, textSwallowed = 0, framesOpen = 0; };
	const GateStats& gateStats();
} // namespace ui
} // namespace apx
