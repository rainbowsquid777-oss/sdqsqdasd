Layout Placer v1.1

Manual placement (editor playtest): Q/E choose the object, R places it at the player (+ X offset setting).

Gimmicks:
1. In the editor select the objects of your gimmick. Put the object that should sit exactly where the player was at the first click (e.g. the orb) as the LEFTMOST object.
2. Set "New gimmick: clicks" (1-10) and "New gimmick: gamemode" in the mod settings, then press G. Repeat for as many gimmicks as you like. H clears all gimmicks.
3. Playtest. When your last N clicks happen within the click window in the right gamemode, a matching gimmick (the one with the most clicks wins, ties are random) is pasted at the spot of your first click. It appears once you have passed it. Chance, minimum gap and optional spacing variation are in the settings.

Build: push this folder to GitHub, run the "Build" action, download the .geode artifact.
Teleport portals are placed unlinked; link them in the editor afterwards.
