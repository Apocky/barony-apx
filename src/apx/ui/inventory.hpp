// apx/ui/inventory.hpp -- D92 P1: the inventory window (spec 08b sections 4-7), built on apx/ui.
// Three columns: Equipment + stats | Bag (search, categories, sort, grid) | Details (Barony's own item
// lines, compare deltas, components, actions); hotbar strip; right-click quick menu; drag and drop;
// keyboard and per-player gamepad focus; damage warning. Every action calls the game's own function
// (activateItemContextMenuOption, dropItem, the hotbar binding); the window never edits item lists.
// It replaces Barony's inventory window, character sheet and item tooltip/context menu: while it is
// open those are not created, not updated and get no input. Chest, shop, spell list and the generic
// item GUIs keep Barony's windows until their phases (P2-P5); then our window is closed (invariant 3).
#pragma once
#include <SDL.h>
#include <string>

namespace apx
{
namespace ui
{
namespace inv
{
	bool replacesVanilla(int player);   // the plain inventory is open: our window, not Barony's
	bool anyOpen();
	bool mouseOwned();                  // the keyboard-and-mouse player has our window open
	bool wantsKey(const SDL_Event& e);  // a key our window uses this frame (Barony must not see it)
	void draw();                        // inside the ImGui frame, every local player
	void hideVanilla(int player);       // disable Barony's inventory/sheet/tooltip frames if they exist
	bool padAction(int player, const std::string& action); // QA `uikey`: up/down/left/right/a/b/x/y/lb/rb
	std::string status();               // one line per open window for the QA status file
	std::string dump();                 // view state for `uidump`
} // namespace inv
} // namespace ui
} // namespace apx
