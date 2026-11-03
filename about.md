# Windowpane

Measures how many **240 Hz ticks** each input of a macro can be moved before the run dies, and shows the result as a bucketed sidebar plus a marker on every click.

## Use
1. Open a level (normal or practice mode).
2. Pause → **Windowpane** button (or press **F7**) → *Load macro* → pick a `.gdr` file (GDR1 and GDR2 are supported). Player 2 inputs are read.
3. Press **Analyze**. The game is driven by the mod at the speed you set (1–100x); the frame rate is kept above the *Minimum FPS* setting. **Cancel** stops cleanly.
4. When it finishes, play or practice the level: the sidebar counts inputs by window size as you progress and markers appear in the world.

## How a window is measured
For every input the mod restores a full game-state snapshot just before it, replays with the input shifted by -1, -2, … and +1, +2, … ticks, and stops at the first shift that dies. If a snapshot does not restore bit-exactly, the trial still starts from it (a few ticks before the click) with the player's position, velocity and rotation set from the first pass, provided the unmodified macro replays correctly from there. `window = earlier + later + 1`. Before measuring, the unmodified macro is replayed as a control. A trial ends early only when the shifted run is identical to the original again (position, velocity and hidden player state); runs that never rejoin are checked past the normal horizon, and the furthest accepted shift on each side is re-run to the very end to confirm it. Release inputs are measured exactly like presses. With *Click-between-frames* mode on (and a game build that supports click timestamps) windows can have decimals.

## Notes
- Disable noclip, speedhack and TPS-bypass mods while analysing; they change the physics being measured.
- `sigma/s` (optional) is a difficulty estimate, see `core/Sigma.cpp` for the maths.
- Results can be exported as JSON next to the macro.


## Building
```
cmake -B build -A x64 -T host=x64
cmake --build build --config RelWithDebInfo
```
Do not build in the Debug configuration (the default for `cmake --build` without `--config`): Geode mods must use the release runtime.
