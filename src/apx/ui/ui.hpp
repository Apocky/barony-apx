// apx/ui/ui.hpp -- STUB (D93 C0). The D92 inventory window lane owns the real apx::ui and replaces this file with
// the same two signatures; apx::in's owner gate reads them. Until then no apx::ui window exists.
#pragma once

namespace apx
{
namespace ui
{
	inline bool wantsMouse(int /*player*/) { return false; }     // pointer over our UI, or a UI drag in progress
	inline bool wantsKeyboard(int /*player*/) { return false; }  // a UI text field is active
}
}
