# v2.5.3 - much faster analysis, recording uses the game's own practice checkpoints
- Accuracy vs the reference counter (Acheron video, 12 progress points read off it): with the restore fixed, the speed is right (573 s instead of 8,000 s) but our windows were too wide. The reference ends at 158 clicks in rows 1..9-10 and 43 uncounted; we had 127 and 74. Limiting each side to 5 ticks reproduces its shape (151 counted, 50 at 11 ticks, and within +-7 of its running total at every point of the level, against -31 without the limit): the reference never looks past 5 ticks each way, so a click with room on both sides is an 11-tick window and sits outside the 1-10 rows. "Max shift each side" now defaults to 5 (set once for existing installs, change it back to measure wider windows). It also makes the analysis faster.
- Diagnostics: a trial that died now shows where in the trace, in ticks after the click ("-2x@38"). A small number means the click's own jump killed it, a large one means a later input that stayed where it was. 13 clicks still come out at 1-2 ticks where the reference has 3; this shows which of the two it is.
- Fixed (the cause of the 1-frame windows and of most of the analysis time, found with your Diag lines): restoring a snapshot did not put the "already used" flag of orbs, pads and portals back. A normal respawn resets every object before it loads a checkpoint; the bare checkpoint load the snapshots use does not. So once the first pass or an earlier trial had used an orb, a later run from the same snapshot could not use it: the click on it did nothing and the run died ~30 ticks later. Your file shows exactly that: two restores of the same snapshot lasting 121 and 46 ticks, "the unshifted macro dies at tick 408" from every snapshot before the orb click at tick 379, and the same wrong y (168.0477 instead of 169.3572) at tick 380 from two different snapshots. Snapshots now save which objects were used and restore those flags. Expect far fewer rejected groups (so the replays from tick 0 mostly disappear) and far fewer 1-frame windows. These bindings have no named flag member, so the snapshot reads each object's used state through GameObject::hasBeenActivatedByPlayer and, on restore, calls GameObject::resetObject (what a normal respawn does) on the objects that are marked used now but were not at capture time.
- Found in your Acheron results file (402 inputs, 5,705 trials, 29,011,209 ticks = 5,085 ticks per trial, 8,000 s): 168 groups were rejected (the snapshot start did not reproduce the first pass) and after the second rejection local start switches itself off, so about two thirds of the trials replay from tick 0 (about 7,500 ticks each at about 0.2 ms per tick). That, not the search, is where the time goes, and it is also where the 1-frame windows come from. The restore failing is a bug I can only find with the failing field, so:
- New (diagnostics): every input in the results file now has a "mode" (cube, ship, ball, ufo, wave, robot, spider, swing, with "(mini)"), and the warnings list now carries up to 25 "Diag:" lines saying where and why a snapshot self-test, a local start or a group start was rejected (tick, inputs, the field that differs).
- Fixed: holding the button into an attempt after dying while recording made the player jump / change velocity compared with the moment the checkpoint was placed. The checkpoint's button state is now saved with it; if the button was up there and is held after the respawn, it is released right after the respawn (and once more before the first update), so the attempt starts exactly like the checkpoint. A new click after that works as usual.
- Changed: recording no longer rebuilds checkpoints itself. The "exact respawn" (restart the level, swap the checkpoint list, fast-forward the recorded clicks to the checkpoint) is not used any more; it made the player jump between checkpoints and flicker when dying. A death now does what practice mode normally does: the game loads its own last checkpoint, and the recording is cut back to that checkpoint's tick and carries on. Practice mode now works the way zBot does it: the game loads its own checkpoint, and the only thing the mod puts back is the players' vertical velocity, saved when each checkpoint is created and written back right after the respawn. The old hook that captured and restored ~200 player fields on every checkpoint load is gone (it could move / rotate the player on respawn). "Recording stopped: the checkpoint could not be rebuilt exactly" can no longer appear.
- Fixed: "Recording stopped - the checkpoint could not be rebuilt exactly" threw away the whole recording when the replayed run died before the checkpoint (common on hard levels with many deaths). Now the mod falls back to the game's own practice respawn: it loads the last checkpoint, keeps the clicks recorded so far and keeps recording. A small notification says it happened. The alert and the stop only remain for the case where there is no checkpoint to load at all.
- Speed: the work done per frame was capped at (Speed setting) x real time x frame time, which at the old default of 100x is only about 24,000 ticks per second, whatever the PC could do. The default is now 1000x (max 2000x, applied once to existing settings), and each frame may spend 75% of its time budget on the analysis (was 50%). The auto-throttle still backs off if the game's own frames get long.
- Speed: new setting "Fast window search" (on by default). Shifts 1-8 are tried one by one exactly as before, so every window up to 8 ticks is found the same way. Beyond that the shift jumps outwards (+4, +8, +16, ... up to the max shift) until one dies, and the gap is bisected. A window that is wide on both sides used to cost up to 2 x 24 trials; now about 2 x 10. The top bucket (9+) is where most trials were spent. Turn it off to try every shift one by one. It assumes that if a shift above 8 survives, the ones between it and 8 do too.
- Not compiled or run in the sandbox this was written in: check the Geode log lines "[Windowpane] input N ..." (the trace shows the shifts tried) and the trials/s readout, and run tools/compare_reference.py on the new results to confirm the bucket shares did not move.

# v2.5.2 - lower analysis overhead, same window decisions
- Enabled the normal compiler optimiser for the analyzer, stepper, snapshots and game hooks on MSVC, while keeping the old no-whole-program-optimization workaround and the `WP_NOOPT_ALL` fallback.
- Removed recording / playback bookkeeping from Stepper-driven physics updates and command passes. Simulated inputs and physics still use the same game functions; only unrelated live-session work is skipped.
- State convergence now uses an allocation-free visible comparison and computes the expensive hidden-player hash only after the visible state matches. Diagnostic-only state reads no longer calculate that hash.
- The baseline reuses the previous step's post-step state instead of capturing the same state again at the start of the next tick.
- A boundary that already survived to the full check limit (or the level end) is not replayed a second time. Boundaries accepted through convergence early-exit still receive the full-length verification.
- Frame-window search order, shifts, horizons, tolerances, snapshot self-tests, death confirmation and `earlier + later + 1` results are unchanged.

# v2.5.1 - fast trials all the way through the level, reference comparison tool
- Fixed (reported: every click and release came out as 1 frame after the first 2.5.1 build): I could not reproduce it without the game, so this fixes the two ways I found that it can happen and adds a net under both. (1) The player's position / velocity / rotation were forced right after the snapshot restore, but a frame passes before a trial takes its first step; they are now forced again right before that first step, exactly as the local-start check does it. (2) A snapshot start that cannot reproduce the first pass makes every shifted trial die, whatever the shift, which is exactly what "1 frame" looks like. Now a trial that dies before its own shift, or the first death of a shifted trial in a group followed by an unshifted trial that also dies, rejects that start: the group is measured by replaying from the start of the level (slow but exact) and the result says how many groups were rejected. A window of 1 is therefore only reported when the unshifted macro survives from the same start. If the warning "group(s) started from a snapshot that did not reproduce the first pass" appears, send me the results file.
- Includes beta.28 (below): trials start just before the click even when a snapshot is not bit-exact, so trials per second no longer collapse deeper into the level. Measured on your Acheron results file (402 inputs): 56.5M ticks over 6,699 trials = about 8,400 ticks per trial and 4.3 hours, because from input 7 on every trial replayed from the start of the level.
- New: tools/compare_reference.py and tools/reference_acheron.json. The JSON holds the counters read from the Acheron frame-window video (final 1/2/3/4/5-6/7-8/9-10 = 1/2/13/25/48/42/27 of 158 counted clicks, 193 clicks in total by its CPS readout, plus four checkpoints); the script compares any exported results file with it (bucket shares, total-variation distance, share of 1-2 frame windows, where those sit in the level). Run it after every analysis: python tools/compare_reference.py results.json
- Baseline of the beta.27 results on that tool: 10.9% of clicks have a window of 1-2 frames (reference 1.6%), and the 3-8 frame buckets are too small (reference 70% of clicks, results file 48%). The 9+ share matches up to about 35% of the level. The rule itself is the same (the hold moves with the click, every later input stays at its tick); only its length changes, see below.
- Changed (tries to fix the windows): the survival check moves to a middle point, next 2 inputs + 14 ticks, minimum 48 ticks, 30 extra ticks while off course (was 3 / 16 / 60 / 60, applied once to existing settings). The older 2 / 12 / 36 / 0 check was a little too generous in ship, wave and some cube parts, the 3 / 16 / 60 / 60 check made 10.9% of the clicks 1-2 frames (reference 1.6%). Not run in the game yet. To tune without a rebuild: run tools/compare_reference.py on the new results; if the 1-2 share is still high, lower "Survive next N inputs" / the minimum horizon; if the 9+ share is too high or the ship parts look too wide, raise them (all in the mod settings).
- Found while comparing: the reference's counters tick up to about 0.3 s after the click and its macro has about 193 clicks where the zBot file has about 201, so single clicks cannot be matched between the two; only shares can.

# v2.5.0-beta.28 - trials stay fast deeper into the level (start just before the click)
- Fixed: trials per second collapsed the further the analysis got, even with "Exact replay from the start" off. When a group's snapshot failed the exact self-test, every trial of that group was restarted from the last snapshot that had passed (beta.26) or from tick 0, and replayed everything from there up to the click. That distance grows with every failed group, so a trial that should replay ~20-60 ticks replayed thousands. After 3 failures in a row on an imported macro it stayed that way for the rest of the level.
- New: local start (setting "Start trials just before the click", on by default). If a snapshot is not bit-exact, it is no longer thrown away. The snapshot is restored (it sits a few ticks before the click), the player's position, velocity and rotation are set to what the first pass had at that tick (both players in dual), and the unmodified macro is replayed over the whole stretch a trial can run; it must stay alive and within 0.1 of the first pass. If it does, every trial of the group starts from that snapshot. If it does not, the old fallbacks run as before.
- The long exact self-test (two replays of up to 600 ticks per group) is skipped after 3 failures in a row and retried every 8th group; the cheaper local check decides in the meantime.
- Early-exit convergence ignores the hidden player state in local-start groups (it can never match the first pass there), so trials still stop as soon as they are back on the original run.
- The results get one warning with how many inputs / groups were measured this way. Turn the setting off for the strictly exact (slower) fallbacks.
- Not compiled or run in the sandbox this was written in: check the Geode log for "[Windowpane] local start at tick ..." lines and the trials/s readout.

# v2.5.0-beta.27 - build fix
- The import menu's show override is now public, so the build compiles.

# v2.5.0-beta.26 - failing groups restart from the last good snapshot
- When a group's snapshot fails its self-test, its trials now restart from the last snapshot that passed (instead of from the very start of the level), so each trial replays far less on long levels. Falls back to the start only if there is no earlier good snapshot.

# v2.5.0-beta.25 - reverted the earlier-fallback change
- Imported macros go back to 3 failed snapshot tests in a row before switching to replays from the start (beta.21 switched after 1, which made long levels slower, not faster).

# v2.5.0-beta.24 - import macro menu scale actually applies
- The import menu's smaller scale was being reset by the popup's open animation, so it stayed full size. The scale is now applied after the animation.

# v2.5.0-beta.23 - analysis runs as fast on long levels as on short ones
- Speed: the auto-throttle counted the analyzer's own work as frame cost, so long levels (more to update per tick) backed off to the floor and ran at ~10x instead of ~40x. It now only looks at the game's own frame time, and it recovers faster.
- Minimum FPS while analysing defaults to 5 (was 10). Applied once.

# v2.5.0-beta.22 - smaller import macro menu
- The "Which bot recorded this macro?" popup is 15% smaller and is fitted to its real size, so it no longer runs off the screen.

# v2.5.0-beta.21 - ship / wave windows no longer capped at 1-3 frames, faster analysis
- Accuracy: a click in ship / wave / swing used to also check the next clicks with their own tight tolerances, so a 1-frame window was reported whenever a later click was tight. Now at most one extra click is checked (default strictness 0). Applied once to existing settings.
- Speed: after one failed snapshot self-test in an imported macro, the rest of the level is measured with exact replays right away (was after 3 in a row). The long self-test is capped at 600 ticks (was 2400).

# v2.5.0-beta.20 - the analysis no longer stops on a one-off hiccup
- Fixed: the analysis could stop by itself after a quarter to half of the inputs. Every level reset (thousands in a long analysis) and every physics step could end the whole run on a single failure: a reset that took more than 150 frames to start, a reset that did not land on tick 0, one update that did not advance a step. Now the level is only asked to start again every 2nd frame, a failed reset is simply run again, a faulty step re-runs that trial from a fresh start, and the stretch advance of a group is restarted. Only 5 failures in a row end the analysis.
- The results file now has "cancelled" and "error", and the results warnings say why a run ended early (also cancelled by Esc / closing the progress window).

# v2.5.0-beta.19 - fast ship / wave windows no longer too low, much faster analysis
- Accuracy: the survival check after a shifted click was counted in ticks, so in fast sections (2x-4x speed) and in dense ship / wave parts it covered much more of the level and many more later clicks, each using up its own tolerance. That is why windows there came out too low. The tick parts of the check (tail, minimum, extra off-course stretch) are now scaled by the player's speed (only shortened), and Ship / wave / swing strictness only adds clicks to check (no longer stretches the tick parts). Default strictness 1 (was 2), applied once.
- Speed: "Exact replay from the start" is off by default (applied once). Trials restore a self-tested snapshot instead of replaying the whole level from tick 0 plus a level reset each time; a group whose snapshot fails the self-test, or drifts, still falls back to exact replays automatically. Imported macros keep their per-group fallback.
- Speed: "Re-check tight clicks at the end" is off by default (applied once). It replayed every click with a window of 1-3 from the start of the level, which is nearly every click in fast sections. Turn it on again for a slower, stricter pass.
- Existing settings that were changed by hand are overwritten once by the three defaults above.

# v2.5.0-beta.18 - releases measured again, stricter windows
- Release timings are Always again (every release is measured, converted once from Never).
- Windows were still a little high in ship, wave and some cube parts, so the survival check is longer: next 3 inputs + 16 ticks, minimum 60 ticks, 60 extra off-course ticks (was 2 / 12 / 36 / 0), and ship / wave / swing strictness 2 (was 1). Applied once to existing settings.
- Every click is measured on both sides: earlier (- ticks) and later (+ ticks), each side one tick at a time until the first death, and the edge of each side is replayed to the end (Re-check every window edge).

# v2.5.0-beta.17 - clicks only by default
- Compared the mod with the reference counter again (13.9% / 32.6% of the level). Totals: the mod counts about 68 entries at 32% where the reference has about 48 bucketed, and about 15 of the mod's entries are 1-2 frames where the reference has 1. The reference counts clicks; the mod also measured releases (Release timings = Auto, in ship and wave), which are tight and numerous. Release timings now default to Never (clicks only), converted once from Auto; set it back to Auto or Always to measure releases.
- The results get a line with the number of measured clicks and releases and how many of each have a window of 2 or less.

# v2.5.0-beta.16 - colours by rank, 0 frame windows, sounds one step up
- Colours: the shown timings are coloured by their rank, not by their numbers: the easiest (biggest) is light purple, the hardest (smallest) is red, and the ones in between are spread over the same rainbow (purple, blue-purple, blue, cyan, green, yellow, orange, red-orange, red) however many timings you list. Labels, counts and circles use it. Setting "Colour from easiest to hardest" (on); off = the old row colours.
- A window of 0 can be listed in the timings (0, 0-1, ...) and set in the manual editor; the analyzer never produces it.
- Sounds: every click sound moves up by one (window 0 plays the 1 frame sound, 1 plays frame 2, ...; the top sound is shared by the rest).

# v2.5.0-beta.15 - option to hide the whole screen during the respawn rebuild (beta 2 behaviour)
- New setting "Hide whole screen during respawn" (off by default). On = the way beta 2 hid the rebuild (the whole level and UI). Off = only the level objects are hidden. Added to find out whether the practice checkpoint markers disappear because of the objects-only hide.

# v2.5.0-beta.14 - timings box in the GD font, transparent; practice checkpoint markers
- The Timings box in the Options page uses the Geometry Dash font and has no dark background (the row colour shows through).
- Practice checkpoint markers: every marker of an existing checkpoint is shown again each frame in practice mode (outside the respawn rebuild), in case one stayed hidden after the mod rebuilt the level.

# v2.5.0-beta.13 - 12-15 pink, 16+ greyish white, wide timings row
- Row 8 is now 12-15 frames and pink, row 9 (16+) is a greyish white (applied once to settings that still have the old defaults).
- The Timings to show row in the Options page is as wide as the two toggle columns together.

# v2.5.0-beta.12 - ship/wave strictness, fewer dropped inputs, sound priority
- Accuracy: clicks in ship, wave and swing are judged over a longer stretch (new setting "Ship / wave / swing strictness", default 1: 2 more inputs, twice the tail and minimum, 120 extra off-course ticks; 0 = same as other modes, 3 = strictest). The offset stays in those modes, so the shorter check made their windows too large.
- Coverage: there is no timeout in the analysis (the only wall-clock limits are per frame, to keep the game responsive). Inputs were dropped as "unreliable" when a replay from the start differed from the first pass or died before the shifted click. Now: a replay that only drifts slightly keeps measuring (one warning, no input is thrown away), a replay that dies or ends early is run again up to 3 times, and a trial that dies before its own shift is replayed up to 3 times before the input is given up. The results still list "Coverage: X of Y" with the reasons.
- Sounds: click sounds play at the highest FMOD priority so overlapping clicks are not cut by the engine.

# v2.5.0-beta.11 - timings box accepts commas and dashes
- Fixed: the "Timings to show" box (and the bucket row range boxes) in the Geode settings did not accept commas or dashes. They now explicitly allow digits, comma, dash, plus, semicolon and space.
- New: the mod's own Options page has a full-width "Timings to show" field under the toggles (the toggle rows are slightly closer together to make room, both columns stay even). It edits the same setting live.

# v2.5.0-beta.10 - analysis screen 10% bigger
- The analysis screen is 10% larger than in beta.9.

# v2.5.0-beta.9 - windows measured locally (matches the reference counter), coverage, sounds, smaller analysis window
- Accuracy: compared against a reference frame-window counter on the same level. Over the first ~40-50% the mod counted about 40 clicks as 1-3 frames where the reference has about 8, while the reference has more 4-8 frame clicks. The big windows agreed. So windows were collapsing, not inflating: a shifted click had to survive the next 8 clicks, at least 240 ticks and up to 480 more while off-course, with every later click fixed in time, which kills runs for reasons that have nothing to do with the click (worst in ship sections where the offset stays). New defaults: survive the next 2 clicks + 12 ticks, minimum 36 ticks, no extra off-course checking. They are applied once to existing settings.
- Coverage: the final strict pass now also re-measures every input that came out unreliable or hit the trial budget, and the results list how many inputs were measured and why the rest were not ("Coverage: X of Y").
- Sounds: the top timing bucket is now "9+" (it was 9-12, so every window of 13 or more had no count and no sound; a saved old default is converted). The playback no longer treats a lag spike of more than 16 ticks as a restart (it skipped the clicks and sounds inside it, the limit is now 240 ticks), and the per-input sound table is rebuilt on every restart.
- Speed: the shorter check makes every trial much shorter. I did not add skip-ahead searching (testing a far shift first), because a window with a hole in it would then be overestimated.
- The analysis window is scaled down further (about 62% of its beta.4 size).

# v2.5.0-beta.8 - smaller import screen
- The Import macro ("Which bot recorded this macro?") frame is scaled down to about 63% so it fits on the screen.

# v2.5.0-beta.7 - final strict re-check of tight clicks, speed per input
- New: when the analysis is done, every click with a window of 1-3 ticks is measured once more. Each trial of that pass is a genuine new attempt from the start of the level, runs to the end with no early exit, and every death is replayed before it counts. If a window changes, the input gets a note ("final re-check: window was N") and the results get a warning with the count. Setting: Re-check tight clicks at the end (on by default).
- New: each analysed input records the player speed (speed portal state) it happens in; it is in the results file as "speed".

# v2.5.0-beta.6 - death confirmation, faster analysis
- Accuracy: a trial that dies at a shift of 1 or 2 ticks is now replayed once more with the identical shift before it counts. A single wrong death at that size is what turns a real window (e.g. a 6 frame cube jump) into 1 frame. If the repeat survives, the trial counts as survived and the results file gets a warning with the number of such trials (a large number means another mod changes the physics during the analysis).
- Speed: a trial starts the level on the second frame after the reset instead of the fourth, so each exact trial needs fewer rendered frames. The analysis speed setting goes up to 400x (was 100) and now defaults to 100x (was 40x); auto-throttle still backs off when frames get long. A speed you already saved keeps its value, raise it yourself.

# v2.5.0-beta.5 - no level-complete screen from playback, smooth checkpoint respawn
- Fixed: playing the level with an analysed macro loaded showed the level-complete screen (twice) when the macro finished. A finish by the macro is now swallowed in every mode (also practice) and the game calling levelComplete twice for one finish is ignored the second time.
- Fixed: the jitter when dying in practice mode while recording. The restart + fast-forward was visible for a few frames. The level objects and players are now hidden during the rebuild (background and UI stay, no black screen) and shown again at the checkpoint.

# v2.5.0-beta.4 - progress bar no longer flashes 0%
- Fixed: the analysis screen showed 0% for a moment after every input. Exact trials pass through a level-reset phase between trials that had no progress value; it now keeps the real progress, and the percentage never goes backwards once inputs are measured.

# v2.5.0-beta.3 - music follows the checkpoint respawn, no black screen
- Fixed: after a death while recording, the music restarted from the beginning. When the checkpoint rebuild finishes, the song is now set to the time the level is at.
- Removed the black screen with "Rebuilding checkpoint" text during the respawn; the level stays visible.

# v2.5.0-beta.2 - imported macros measured exactly, recording crash, shorter info texts, smaller analysis screen
- Fixed (imported macros only): every frame window came out as 1 frame. Found with a real results file (zBot macro of Acheron, 402 inputs, 282 measured, all 1 frame, 2 trials each). The macro is longer than 30 s, so "Exact replay from the start" does not apply to it (that mode only covers macros up to 30 s) and mid-level snapshots are used. On that setup every snapshot self-test failed ("one run lasted 496 ticks and the other 221"), and the fallback for those inputs restored the start-of-level checkpoint, which is not the same as a new attempt: every shifted trial died, whatever the shift, so every window was 1. For imported macros the fallback now replays every trial from a genuine reset of the level (exactly like exact mode), the replay is checked against the first pass (a trial that dies before its own shift is reported as unreliable instead of as a 1-frame window), and after three failed self-tests in a row the rest of the macro is measured that way straight away (the tests themselves cost two long replays each). Windowpane macros are unchanged.
- Fixed: crash when you record a macro and die in practice mode (access violation inside the game's visibility update, on a deleted node). Cause: the mod could leave the game pointing at deleted practice-checkpoint objects. Practice checkpoints were removed by emptying the checkpoint list directly (Record, and respawn with "respawn at practice checkpoints" off) so their level objects stayed behind; the temporary checkpoint list used by the exact respawn was freed even if the game had put checkpoints into it; and the game's "current checkpoint" pointer could be left on an analysis snapshot that was deleted afterwards. Checkpoints are now removed with the game's own removal functions, the temporary list is emptied the same way (and never freed while it still holds any), and snapshots never become the game's current checkpoint.
- Shorter texts in the blue "i" setting pop-ups (all 27 that were long enough to run off the screen are now one to three lines).
- The analysis screen is scaled down (the card is about 78% of its size and centred, it was as tall as the screen).

# v2.5.0-beta.1 - exact checkpoint respawn while recording
- Why recordings died when replayed or analysed, usually in ship and wave parts: with practice checkpoints, the game respawned you at the checkpoint with a state that is not quite the state the run really had there (the game's checkpoint load does not restore everything). The macro joined from such attempts therefore does not replay the same way from tick 0 and dies exactly where you died while recording - ship and wave need exact timing, and that is where most deaths are.
- Fixed: after a death the level is restarted from the very beginning and your recorded clicks are fast-forwarded with the analyzer's stepper to the checkpoint's tick (the level is hidden, the progress is shown). The attempt then continues from the state a replay from tick 0 produces, so the macro is exact by construction. It takes a few seconds per death on long levels; clicks and audio are not played during it.
- The setting is now "Recording: respawn at practice checkpoints" and is ON by default (it was off since beta.3, which restarted the whole level on every death). Off still records a straight run from 0%.
- If the fast-forward cannot reproduce the run (it dies or ends before the checkpoint) the recording is stopped with a message and nothing is stored.
- Version: this is a bigger change than a beta step, so v2.5.0-beta.1.

# v2.4.2-beta.9 - new loading screen, forced TPS match
- New loading screen for an analysis in the look of the new menu: gradient backdrop with drifting colour orbs, a card with the logo, a big percentage, a gradient progress bar with a moving shine, six live stat chips (speed, trials/s, ETA, inputs, trials, elapsed) and a themed Stop button. It follows the menu's light/dark theme and accent colours.
- The "I set it" popup is gone. Instead the game's real physics tick rate is measured (physics steps per real second in normal play). When you press Record, the level restarts from 0% and the first 0.6 s of play are measured: if the game does not run at the FPS you set (+-10%), the recording is cancelled at once, the game is paused and a message says what it measured and what to change. So you cannot record at a TPS that does not match the FPS.
- Analyze is refused when the macro's recording FPS differs from the FPS that is set; the message names the FPS the macro was recorded at. The FPS button turns red while a loaded macro does not match, and the macro card shows its recording FPS.

# v2.4.2-beta.8 - FPS option (physics tick rate)
- New "FPS: N" button on the main page. Each click goes 240 -> 320 -> 480 -> 720 -> 960 -> 240. It sets the tick rate everything is measured at: the physics step of the analysis, the length of a frame window, the playback and the recording stamps. It is saved.
- Changing it asks you to set the TPS of your other mod (Eclipse's TPS bypass or similar) to the same value; Record and Analyze stay locked (the button turns red) until you press "I set it".
- A recording is stamped with the FPS it was recorded at. Analyzing a macro whose recording FPS differs from the current FPS is refused with "recorded at 480 FPS - change FPS to 480". Playback of such a macro stays off for the same reason.
- Imported macros are no longer squeezed to 240 ticks; they stay in their own tick rate and are analysed at that rate once FPS matches (previously a macro at any other rate was refused).
- The 30 s limit for exact replays is now 30 s at the current FPS.

# v2.4.2-beta.7 - recording starts from 0%
- Pressing Record now restarts the level from 0.00% (any practice checkpoint already placed is removed first), so a recording is always one run from tick 0. The recorder texts now say what a death does with the current "Recording: use practice checkpoints" setting.

# v2.4.2-beta.6 - snapshot check over the whole trial
- Found with a real results file (ORDINARY, 198 measured, 176 of them 1 frame): 112 inputs died on the very first shift, and 75 more passed the early-exit trials but died in the final full-length re-check although the run had already matched the original, hidden state included. A run that matches the original for 12 ticks cannot then die unless the snapshot it started from is not exact in some way the 16-tick self-test cannot see. The analysis also logged a velocity drift (-15 vs -7.5, exactly 2x) and a 0.03 position drift.
- The snapshot self-test now compares the restored run with the continuing run for as long as a trial of that group can run (up to 2400 ticks), not just 16. A snapshot that fails is replaced by replaying every trial from the start of the level for those inputs: exact, but slower. New setting "Check snapshots over the whole trial" (on by default); off gives the old fast, less reliable behaviour.

# v2.4.2-beta.5 - "the replay diverged / died" on a level that survives
- Found with a real results file (ORDINARY, 350 inputs, 0 measured): after the macro was repaired, every input was reported unreliable because "the replay died at tick 182", which is before the first input (tick 334) and does not happen in normal play. Cause: the analysis advanced to each group's save point by loading the start-of-level checkpoint, which is not the same as a new attempt; on some levels the run from that restored state dies or drifts by itself. It now starts that advance with a genuine level reset, exactly like the first pass. (This was already fixed in v2.4.1 and was missing from this build.) The earlier 2x yVelocity drift at tick 403 in the `recording` results looks like the same thing.

# v2.4.2-beta.4 - save-state drift no longer gives wrong windows
- With "Exact replay from the start" off, every input starts from a save state taken during the first pass. When one of them does not restore exactly (the restored run drifts from the first pass), the windows measured from it were wrong without any sign, including the inputs measured before the drift was noticed. The analysis now throws those results away and starts over with exact replays (macros up to 30 s), with a warning saying so. Longer macros still use save states.

# v2.4.2-beta.3 - straight-run recording, icon mix-up
- Found with a real results file: replaying the macro from a fresh level start drifted at tick ~403 (yVelocity exactly 2x) and died at tick 482 although the first pass survived. Windows measured on such a macro (1 frame, wrong, or missing) are artifacts of the macro, not of the clicks. This is what a recording joined from practice-checkpoint attempts does.
- New setting "Recording: use practice checkpoints", default OFF: while recording no checkpoints are used, every death restarts the level and the recording, and the macro is one unbroken run that replays exactly. Re-record the macro with it off. (Ported from v2.4.1; the default is now off.)
- New warning when inputs are unreliable because the replay does not reproduce the first pass, with this advice.
- Fixed: the icon of the last game mode used during an analysis was drawn on top of the current / starting icon on the next attempt. Restoring a save state wrote the game-mode flags without switching the vehicle sprites. Restore now runs the game's own mode switches, and the analysis end clears every vehicle's visuals.

# v2.4.2-beta.2 - back on the v2.4.0 analysis
- The analysis engine is v2.4.0 again (the version whose frame windows were right). The v2.4.1 changes to it (anchor / fast search, exact edge checks, repair passes, releases measured in every mode, record-stitch, xdBot extra-data warning) are NOT in this build; the "every window is 1 frame" result came from that rework.
- Kept from the earlier betas: the crash fix (an internal error during analysis no longer resets the level on top of a half-updated game state; it stops with a readable message and writes the cause to the Geode log as "exception during analysis").
- Kept: a warning when nearly every window measures as exactly 1 frame, and a note + log line for every input with a zero-sided window saying whether it had no room to move (adjacent input) or the shift died.

# v2.3.0 - accuracy
- New: "Other" button on the import screen. Windowpane tries every reader first; if none fits it looks inside the file and guesses: a JSON file with any list of inputs (frame + pressed / released, under names like events, inputs, clicks, actions), or a plain-text file with a frame and a pressed / released column (optional framerate on the first line). The result says what was guessed, and the bot named in a GDR header picks the timing order.
- Fixed (found with a real xdBot macro): xdBot writes "player 1 inputs: 0" in one-player macros, which made every click player 2's, so the macro did nothing in the level. It is now read as player 1.
- Checked against the same level (Acheron) recorded with xdBot (.gdr2), zBot (.gdr, GDR1 msgpack) and Mega Hack (.gdr2): all three load as the identical 402-input macro.
- New: import screen. "Load macro" first asks which bot recorded the macro (xdBot, zBot, Eclipse, Windowpane Macro, Mega Hack, or auto-detect). The choice decides which readers may open the file, so a wrong pick gives a clear message instead of garbage, and a GDR file that names a different bot than the one picked gets a warning.
- New: Mega Hack `.mhr.json` can be imported (best-effort reader, labelled as such). Mega Hack 2.2, xdBot, zBot and Eclipse replays are all GDR files and use the GDR readers.
- Old `.xd` (xdBot), `.cml` (xdBot compressed), `.zbf` (zBot) and binary `.mhr` files cannot be read; instead of "unrecognised format" the message says how to convert them to .gdr.
- Fixed: a Windowpane `.wpmacro` (or Mega Hack JSON) could be mistaken for an empty GDR1 JSON macro.
- The macro-timing correction tries its readings in a different order depending on the type (Eclipse: half-tick readings first) and also tries offsets of +-2 ticks.
- New: macro repair (settings "Repair macros that die" and "Repair: max click move"). If the unmodified macro dies long before it ends, which is what a recording stitched together from practice-mode attempts can do, the clicks nearest to the death are moved by a few ticks, each try replayed from a fresh start, until the macro survives. Of the shifts that work the middle one is used, and the repaired macro replaces the loaded one (so playback uses it too). It runs automatically at the start of Analyze.
- New default look of the click markers: size 3, outline black, number on the left, number size 0.4, number distance -0.3.
- Fixed: macros with half-tick / sub-tick clicks (most Eclipse imports) were measured wrongly. A shifted click lost its own sub-tick part, so "-1" really moved it by -1.5 and "+1" by +0.5. The shift is now applied to the click's true position.
- Fixed: the original run (the reference every trial is compared with) started from a restored checkpoint, while the exact trials start from a genuine reset. It now starts from a genuine reset too.
- Fixed: for macros with sub-tick clicks the original run was recorded in a different click mode than the trials. It is now re-recorded in the same mode.
- Fixed: platformer Left / Right releases were skipped by "Release timings: Auto". They always matter and are now measured.
- New: control trial. Before measuring, the unmodified macro is replayed through exactly the same reset / step / input path as every trial. If it does not survive as long as the first pass, the inputs are reported as unreliable with the reason instead of getting wrong windows. (The old control code existed but was never called.)
- New: early exits compare the hidden player state too (jump buffer, orb / pad flags, dash, slope, platformer state), not just position / velocity / flags, so a run that only looks like the original is not accepted early. The control trial checks that this comparison is stable and switches it off, with a warning, if it is not.
- New: "Re-check every window edge" (default on). The furthest accepted shift on each side is replayed to the very end with no early exit; if it dies, the window is lowered and re-checked. Marked "v" in the trace (for example v-6o v+7o).
- New: "Keep checking while off-course" (default 480 ticks). A run that has not become identical to the original is checked past the normal horizon, because a small leftover offset can kill it later. 0 restores the old behaviour.
- Slower on purpose: roughly 15-40% more trials per input.

# v2.2.1
- Fixed: the round shapes in the menu (pills, circles, the Record icon) had spikes on their corners.
- Record now turns on practice mode by itself, and recording at 0% no longer fails with "Could not work out how far the game advances per update".
- The version next to the title is read from the mod itself (v2.2.1).
- Shorter pop-up messages so they fit on screen.

# v2.2.0
- New menu look, modelled on a modern dark dashboard: rounded cards, gradient pill buttons, a sidebar with six pages (Home, Macro, Record, Analyze, Theme, Options) and buttons in several sizes grouped under small category titles.
- Light and dark mode (sun / half-moon button in the header, or Theme > Mode).
- Theme page: pick the main and the second colour from ten colours each (they blend in buttons, highlights and the menu border), or one of six presets (Neon, Ocean, Sunset, Forest, Candy, Fire). Remembered between sessions. The Edit windows window follows the theme too.
- Options page: twelve quick switches for the most used settings (sidebar counter, markers, sounds, timing fix, exact replay, ...). They are the normal mod settings, so changes show up in Geode's settings too. "All settings" opens the full list.

# v2.1.2
- Fixed: imported macros (Eclipse and other bots) playing fine in the analysis but, in the live level, one click would come late or early and the run died. Half-tick clicks (used when the game counts two progress steps per tick, which is what the automatic timing fix picks for most Eclipse macros) were scheduled by counting command passes per game update instead of per tick. At normal FPS one update runs several ticks, so those clicks landed half a tick early or a whole tick late. They are now counted per tick, exactly like the analysis does.

# v2.1.1
- Fixed: "Save macro" / "Load macro" re-opening the file dialog over and over. The dialog is now opened outside the button click, on its own thread, only one can be open at a time, and a click right after it closed is ignored.
- Fixed: a .wpmacro saved from an imported macro (Eclipse, xdBot, GDR json...) died in the level. The file did not remember that the macro was imported, so the automatic timing fix was skipped on load. It now stores the original frame numbers, the imported flag and the timing correction.
- Fixed: a .wpmacro that contained an analysis could be rejected as "unrecognised format" (the format marker was only looked for in the first 400 bytes).

# v2.1.0
- Sidebar flash: when a click is counted, that row's number turns near-white with a soft glow behind it, stays bright for about 0.2 s and then fades back to its colour over about half a second (timed from your reference video). It works for every row and the total.

# v2.0.2
- The pause-menu button is now a real circle (it came out square) with the white rim, the thin black inner stroke and the green face like the game's own round buttons.

# v2.0.1
- The pause-menu button now sits right below the % button, is a round green button of the same size and shape as the game's own, and shows the Windowpane logo instead of text. The logo is built into the mod.

# v2.0.0
- New look: the whole mod got a custom "2.0" interface. A dark navy window with cyan / magenta accents, flat coloured buttons, cards for the macro, a mini histogram of your timings and the rating, a status pill (REC / PLAYBACK) and a matching pause-menu button and analysis screen.
- New: "Edit windows" - page through the measured clicks of an analysed macro and change the frame window of each one (-1, -0.1, +0.1, +1, reset). The sidebar, circles, sounds and rating update immediately.
- New: "Save macro" replaces "Save JSON". It writes a .wpmacro file with the macro AND its analysis (including your edits); importing that file again restores both, no re-analysis needed.
- Removed: the "Copy summary" button and the test sound when turning Playback on.
- Includes the imported-macro timing fix from 1.1.5.

# v1.1.5
- Imported macros (Eclipse and other bots) that died after a few clicks: their frame numbers do not always match this game's ticks (frames counted per progress step, or off by one). When the unmodified replay dies long before the macro ends, the analysis now tries the other readings automatically (half-tick positions included), keeps the one that survives longest and uses it for the playback too. New setting "Fix imported macro timing".
- Playback understands half-tick clicks (second command pass), so a macro read per progress step plays at the right moment.
- Inputs near the end of a macro are no longer dropped (see 1.1.1) and a retry no longer leaves the audio muted or the level hidden.

# v1.1.4
- Frame sounds now only play for clicks (presses) whose window is one of the "Timings to show". Releases are silent unless "Release timings" is set to Always; windows that are not in the shown list are silent too.

# v1.1.3
- New setting "Timings to show": a comma separated list such as `1,2,3,4,5-6,7-8,9-12` (single numbers, ranges, `16+`). Only these windows are counted in the sidebar, drawn as circles and exported; add or remove entries freely (e.g. `...,9-12,13+`). Colours come from the matching row of the bucket editor (the nearest one for new numbers). Blank = use the bucket editor rows.
- Frame sounds: the nine sounds are now compiled into the mod itself and played straight through FMOD (written once to the mod's save folder), so they no longer depend on how the resources folder is packaged or on the game's effect player. Turning Playback ON still plays a test sound.

# v1.1.2
- Mod id is now `jake.windowpane` (was `jake141017.windowpane`). Delete the old `jake141017.windowpane.geode` from the geode/mods folder so only one copy is installed; settings start from their defaults again.
- Fixed "the unmodified macro does not replay: it dies at tick 482 but the first pass survived until tick 898". Restoring a checkpoint taken at the start of the level is not exact in the long run, so a replay from it drifted and died. In exact mode every trial is now a brand-new attempt: the level is reset exactly like the first pass was and the macro is replayed from tick 0 (no checkpoint at all). The untouched part of each trial is checked against the first pass; if it dies there although the first pass survived, that input is reported as unreliable with the reason.
- Playback OFF now also hides the sidebar counter, the NaN caption and the readout.

# v1.1.1
- Accuracy: new "Exact replay from the start" mode (on by default for macros up to 30 s). Every shifted trial is replayed from the very first tick of the level, so no mid-level snapshot can spoil a result. Before measuring, the unmodified macro is replayed once as a control: if it does not survive exactly as long as the first pass, the results are marked unreliable instead of being wrong.
- Fixed inputs near the end of a recorded run being dropped / given wrong windows: the horizon is now shortened to the last tick the recorded run was alive (and the window gets a note), instead of discarding the input or looking at ticks the recording never covered.
- Every click now lists every shift that was tried ("-1o -2o -3x +1o +2x": o = survived, x = died). It is in the saved JSON ("trace") and in the Geode log, so a result can be checked by hand.
- New slider "Number distance" moves the number of the on-screen circles closer to / farther from the ring (0 = touching, negative = onto the ring).
- Frame sounds: the sound files are searched for in the unpacked mod (several possible folders), the lookup is logged, and turning Playback ON plays a test sound so you can tell immediately whether audio works.

# v1.1.0
- Fixed "Inputs 1-1 could not be measured reliably: the replay diverged from the original run (p1.rotation ...)". The player's rotation is only an animation value (it is not restored exactly by a snapshot and does not affect survival), but it was compared like physics, so correct replays were rejected and nothing was measured. Rotation is no longer compared by the replay check, the snapshot self-test or the convergence check.
- If the snapshot self-test fails somewhere, those inputs are no longer thrown away: they are measured by replaying each trial from the start of the level (slower, but exact), and a warning says so.
- More accurate convergence: a shifted run only counts as "rejoined the original" when it matches to 1e-4 (was 1e-3).
- Clicks are checked tick by tick with their releases moved along (same hold length), releases are optional (Auto / Always / Never), the horizon is long (next 8 inputs, at least 1 s) and half-tick measurement is off unless you enable it (see 1.0.16).
- Version 1.1.0 bundles all fixes since 1.0.0: practice-mode recording with checkpoint stitching and record speed, macro playback with frame sounds, the FWA-style menu and analysis screen, sidebar / marker looks, calibration of how the game counts ticks, anti-cheat spike handling, and the build fixes.

# v1.0.16
- Accuracy: a click could only be shifted later until its own release, so short taps got a truncated (too small) window - the "1 instead of 2". A shifted click now drags its release along (same hold length), so only the next click limits it. This is the new default ("Move the release with the click").
- Each click is checked tick by tick: every shift (1, 2, 3, ... ticks earlier and later) is replayed and must stay alive. Defaults are now stricter: it must survive the next 8 inputs (at least 1 s, up to 10 s), the search reaches 24 ticks, and convergence needs 12 identical ticks.
- Releases are optional: setting "Release timings" = Auto (only where they matter), Always, or Never.
- Half-tick measurement is now an experimental opt-in (off by default); the analysis counts whole ticks.
- Frame sounds: they are loaded from the mod's resources folder by absolute path, and a missing file is reported in the Geode log. Sounds play while the macro plays back after an analysis has measured the clicks.

# v1.0.15
- Build: the compiler crash was in Popups.cpp (the last file printed before the error). On Windows the "Load macro" button now uses the native Windows open-file dialog from its own small source file instead of Geode's coroutine-based file picker, so Popups.cpp no longer instantiates that code. Other platforms keep the Geode picker.

# v1.0.14
- Build: all of Windowpane's own code is compiled without the optimiser / whole-program code generation (cl.exe crashed in "Generating Code..."), and every source file gets its own compiler invocation so a compiler crash names the file.

# v1.0.13
- Build: the compiler crash (cl.exe exit code -1073741819 in "Generating Code...") happened while compiling Analyzer.cpp, Hooks.cpp and PlayerExtra.cpp. These three are now compiled without the optimiser / whole-program code generation, and the build uses one compiler process at a time.

# v1.0.12
- Build fix: CL.exe crashing with exit code -1073741819 during "Generating Code..." is the 32-bit compiler running out of memory. The project now asks Visual Studio for the 64-bit compiler (`-T host=x64`).

# v1.0.11
- The "Debug Assertion Failed / xmemory line 209" crash when an analysis finishes is a build problem, not a logic bug: the mod was compiled in the Debug configuration. Build with `cmake --build build --config RelWithDebInfo`. A Debug build now stops with a clear error message instead of producing a mod that crashes.

# v1.0.10
- Fixed a crash at the end of an analysis (heap corruption, "exception code 80000003"). The half-tick measurement added in 1.0.9 sent clicks with timestamps the game does not expect; it now delivers a "between ticks" click in the second command pass of the tick through the normal input queue, is verified by a probe before use, and falls back to whole ticks if the game does not behave that way. The probe code is also bounds-checked.

# v1.0.9
- Releases are only measured where the release timing matters: dash orbs and the hold-controlled vehicles (ship, wave, swing, robot). In cube / ball / ufo / spider the release gets no window, no ring and no sidebar count, so a jump now has one ring instead of two (the 16+/33 "windows" were these releases).
- More accurate windows: when the game counts more than one progress step per tick (your setup counts 2), inputs are now shifted in those finer steps (sub-tick positions) instead of whole ticks, which is what made a 2-frame window read as 1. If the game ignores sub-tick clicks the analysis falls back to whole ticks and says so.
- Markers: the ring appears when the player reaches the click and fades as they move on (lifetime default 1.5 s); it is bigger, always has the same colour as its number, and the number is sized relative to the ring.
- Removed the ring colour source / ring colour / core colour settings (the ring simply matches the number).

# v1.0.8
- Fixed analysis cancelling with "The game advances more (or less) than one physics step per update": that setup advances the game's progress counter by 2 for every 1/240 s. Windowpane now measures this (at the start of an analysis and when you start a recording), counts ticks as progress / 2, steps the game exactly one tick per update, and saves the factor. Recording, playback, markers and the sidebar use the same tick count.
- Frame sounds: during macro playback every analysed input plays frame-N.wav, N being its window in frames (9 and above play frame-9). Toggle and volume are in the settings.

# v1.0.7
- Analysis now calibrates the stepper first: it tries a few ways of driving the game from the start-of-level snapshot and keeps the one that gives exactly one physics step per update, then puts the level back at tick 0. If nothing works it says so, with the step counts it measured, instead of failing mid-run with "one update advanced 2 physics steps".
- Control menu: the 3x3 buttons no longer overlap.
- Sidebar looks like the reference: "5-6:" labels with the count in the same colour, tighter rows and smaller text.
- Click markers look like the reference: a thick ring in the bucket colour with the number touching its left side (no centre dot).

# v1.0.6
- GD's invisible anti-cheat spike at the start of every level is now ignored while a macro is analysed or played back, so it no longer makes the unmodified run "die at tick 0". Hand-played attempts are unaffected.

# v1.0.5
- Analysis now leaves the pause menu before it simulates (Frame Window Analyzer does the same) and puts you back in it afterwards. A paused level misbehaves on its very first steps, which is the likely reason the macro "died at tick 0".
- Recorded clicks are stamped with the tick of the command pass they are processed in, exactly the tick playback and analysis apply them at, so a macro you record replays on the same tick.
- If the unmodified run still dies, the report now says what killed the player (object id / type), where, and the game state, so it can be diagnosed.
- NaN Counter caption: no longer attached to the percentage text; it sits just below the progress bar, centred on the screen.

# v1.0.4
- Sidebar: label and count are close together again, default row spacing lowered to 5.5%. The "NaN Counter" caption is now centred on the percentage text (it was centred on the label's left edge).
- Playback: the loaded macro plays itself while you play the level (toggle with the Playback button; on by default). A bot finish in normal mode is never recorded as a real completion.
- Practice checkpoints now restore the player state the game normally forgets, so a macro stitched from checkpoints replays like a straight run.
- When an analysis measures nothing (e.g. the unmodified run dies), the reason is shown in a popup and in the menu instead of just "0 trials".
- Control menu is now a 3x3 button grid.

# v1.0.3
- Analysis no longer fails with "one update advanced 2 physics steps at tick 0": the first step of a level may run as two steps; this is now accepted (logged once) and the first-pass records stay consistent. Other multi-step updates still report per-attempt details.

# v1.0.2
- New control menu: wide layout with info and result panels, coloured button grid, and every text scales down to fit its panel. Analysis screen and pause button match Frame Window Analyzer.
- Recording now works in practice mode and pieces the run together from checkpoints: respawning from a checkpoint keeps the inputs before it and drops the rest. Holding a button into an attempt is counted, so it no longer breaks the recording. Beating the level while recording stores the macro.
- Added a recording speed button (1x / 0.5x / 0.25x / 0.1x, practice mode only) and a REC indicator.
- Fixed "one update advanced 2 physics steps at tick 0" when analysing Eclipse macros: the stepper now forces exactly one physics step per update, regardless of other mods' tick-rate changes.

# v1.0.1
- Fixed a crash when opening a level (use-after-free of the NaN caption node in the HUD).
- Added a mod icon; developer is now "jake".

# v1.0.0
- First release: timing-window analysis (240 Hz re-simulation with full snapshots), sidebar, "NaN Counter" caption, world-space click markers, optional readout, GDR1/GDR2 import, JSON export, σ/s rating.
