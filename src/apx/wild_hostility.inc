// apx/wild_hostility.inc -- the explicit hostility table for WILD creatures (no leader on either side), one row per pair with
// its reason (Apocky, 2026-10-19: "Skeletons fighting humans makes sense, monsters fighting non-monsters makes sense, creatures
// fighting obvious enemies or predator-prey relationships are fine, but not inter-entity combat for its own sake").
// A pair is symmetric. Any wild pair NOT listed is neutral (skeleton and rat, goblin and slime, ...) unless one provoked the
// other or one belongs to a director event (apx/wild_enmity.hpp). Players, followers and shopkeepers keep Barony's own rules.
// The table is written to wild_hostility.txt in the run folder at the first floor load. Edit rows here; one reason each.
// Row: { kind A, kind B, class, reason }
// classes: "living" = monsters vs humans and humanoid non-monsters; "enemy" = obvious enemies; "prey" = predator and prey.

// monsters vs the living: humans (wild human NPCs) are prey or foe to these
{ SKELETON, HUMAN, "living", "the undead hate the living" },
{ GHOUL, HUMAN, "living", "ghouls feed on the living" },
{ VAMPIRE, HUMAN, "living", "vampires feed on human blood" },
{ SHADOW, HUMAN, "living", "shadows hunt the living" },
{ LICH, HUMAN, "living", "the undead hate the living" },
{ LICH_ICE, HUMAN, "living", "the undead hate the living" },
{ LICH_FIRE, HUMAN, "living", "the undead hate the living" },
{ GOBLIN, HUMAN, "living", "goblin raiders prey on human travellers" },
{ TROLL, HUMAN, "living", "trolls eat people" },
{ BUGBEAR, HUMAN, "living", "bugbears raid human settlements" },
{ DEMON, HUMAN, "living", "demons hunt mortals" },
{ CREATURE_IMP, HUMAN, "living", "imps torment mortals" },
{ SUCCUBUS, HUMAN, "living", "succubi prey on mortals" },
{ INCUBUS, HUMAN, "living", "incubi prey on mortals" },
{ MINOTAUR, HUMAN, "living", "the minotaur devours those in its maze" },
{ SPIDER, HUMAN, "living", "giant spiders take people as prey" },
// obvious enemies
{ GOBLIN, GNOME, "enemy", "goblins and gnomes are old blood enemies over the mines" },
{ TROLL, GNOME, "enemy", "trolls raid gnome burrows" },
{ GOATMAN, GNOME, "enemy", "goatmen raid gnome burrows" },
{ GOBLIN, KOBOLD, "enemy", "goblins and kobolds fight over the same tunnels" },
{ DRYAD, GOATMAN, "enemy", "goatmen burn the dryads' groves" },
{ MYCONID, SLIME, "enemy", "slimes dissolve myconid colonies" },
// predator and prey
{ SPIDER, RAT, "prey", "spiders prey on rats" },
{ SPIDER, BAT_SMALL, "prey", "spiders web and eat bats" },
{ SPIDER, INSECTOID, "prey", "giant spiders prey on insectoids" },
{ SCORPION, SCARAB, "prey", "scorpions hunt scarabs" },
{ BAT_SMALL, MOTH_SMALL, "prey", "bats eat moths" },
{ COCKATRICE, RAT, "prey", "cockatrices hunt rats" },
{ TROLL, RAT, "prey", "trolls snatch rats to eat" },
