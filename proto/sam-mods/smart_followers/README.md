# Smart Followers (S.A.M mod)

Install: already in `Barony\mods\smart_followers`. Enable it in the Mods menu. Needs S.A.M >= 3.1.0 (3.4.0 installed).

Behaviours, in priority order: retreat when badly hurt, teleport back when far or stuck,
drop chases that lead away from the party, protect the leader (and take attackers of the
leader first), engage visible enemies nearest the leader (focus fire), keep a spread
formation behind the leader. All numbers are constants at the top of `main.lua`.

Self-test (API presence, clean empty pass): `barony.exe -samtest=smart_followers` -> 19/19 pass.

In-game checklist (needs a recruited ally; not covered by the self-test):
1. Recruit an ally, walk away: it should trail in a loose arc, not stack on one tile.
2. Let a monster hit you from behind: ally should turn on it without having seen it.
3. Run far down a corridor: ally should rejoin, teleporting only after ~5 s at 30+ tiles or ~6 s stuck.
4. Let the ally drop under 30% HP in a fight: it should fall back to you, then fight again.
5. `sam_log.txt` prints `smart_followers: followers=.. engage=.. protect=..` every 30 s.
   All counters at 0 with an ally present means the mod is not reaching it.
