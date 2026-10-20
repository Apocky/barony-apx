// apx/interact/interact.hpp -- D93: apx::interact, what the Use button acts on (spec 04b section 3, PROPS lane).
//
//   Entity* target(int player, Prompt* out)   the crosshair query: a ray from the eye along the view yaw, props within
//                                             their tabled reach (edge distance), colliders included, priority by edge
//                                             distance and aim miss; walls block. nullptr = nothing to use.
//   bool use(int player)                      reads Use from apx::in::take(player); acts on target(); true = a prop was used.
// Props are: furniture (chair, table, bed, bunk bed, podium), dark wall sconces, and the colliders of the props table
// (bone and empty cages, candelabra, iron maiden). Our code draws the prompt (ImGui, apx::ui frame) and Barony's world
// tooltip for these props is suppressed, so there is exactly one prompt and one Use path for them.
// Control: APX_INTERACT_OLD=1 restores Barony's tooltip/selectedEntity path (colliders then have no prompt and no Use).
// Single player and host only; a network client keeps Barony's path. Log lines: "[apx-interact]".
#pragma once
#include <cstdint>

class Entity;

namespace apx
{
namespace interact
{
	enum Kind : int { K_NONE = 0, K_FURNITURE, K_TORCH, K_COLLIDER };

	struct Prompt
	{
		Entity* e = nullptr;
		uint32_t uid = 0;
		int kind = K_NONE;
		char verb[24] = {};
		char text[64] = {};
		float dist = 0.f;    // player centre to prop centre (units)
		float edge = 0.f;    // distance to the prop's edge
		float reach = 0.f;   // the tabled reach for this prop
		float miss = 0.f;    // lateral aim miss (units)
	};

	struct Counters
	{
		uint64_t queries = 0, promptsShown = 0, uses = 0, usesNoTarget = 0, usesOldPath = 0, tooltipsSuppressed = 0;
		uint64_t byKind[4] = {};
	};

	bool enabled();                        // false with APX_INTERACT_OLD=1 or on a network client
	bool isProp(Entity* e);                // one of the props this module owns
	Entity* target(int player, Prompt* out);
	bool use(int player);
	const Counters& counters();

	// hooks
	void afterOldClick(int player, Entity*& selected); // actplayer: drops a prop the old path picked, then runs use()
	bool suppressTooltip(Entity* parent);              // player.cpp: true = no Barony tooltip for this prop
	void colliderLight(Entity* e);                     // actColliderDecoration: a lit candelabra glows, a snuffed one does not
	void draw();                                       // inside the apx::ui ImGui frame: the prompt of every local player
} // namespace interact
} // namespace apx
