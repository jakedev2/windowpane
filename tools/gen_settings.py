#!/usr/bin/env python3
"""
Single source of truth for Windowpane's settings.

    python tools/gen_settings.py        (run from the repository root)

Writes:
  mod.json                  - the Geode manifest, including every setting
  src/core/CfgFields.inc    - X-macro list that declares and loads the typed config cache
"""
import json
import os

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

S = []  # (kind, key, field, default, name, description, extra)


def title(name, desc=None):
    S.append(("title", f"t-{len(S)}", None, None, name, desc, {}))


def info(key, desc):
    S.append(("info", key, None, None, key, desc, {}))


def b(key, field, default, name, desc, **kw):
    S.append(("bool", key, field, default, name, desc, kw))


def i(key, field, default, name, desc, lo, hi, **kw):
    S.append(("int", key, field, default, name, desc, dict(min=lo, max=hi, **kw)))


def f(key, field, default, name, desc, lo, hi, **kw):
    S.append(("float", key, field, default, name, desc, dict(min=lo, max=hi, **kw)))


def s(key, field, default, name, desc, one_of=None, **kw):
    ex = dict(kw)
    if one_of:
        ex["one-of"] = one_of
    S.append(("string", key, field, default, name, desc, ex))


def c(key, field, default, name, desc, **kw):
    S.append(("color", key, field, default, name, desc, kw))


def k(key, default, name, desc):
    S.append(("keybind", key, None, default, name, desc, {}))


def btn(key, name, desc, label):
    S.append(("button", key, None, None, name, desc, {"buttons": {key: label}}))


# ------------------------------------------------------------------ General
title("General")
b("enabled", "enabled", True, "Enable mod", "Master switch. When off, nothing is drawn and no hooks do any work.")
b("show-normal", "showNormal", True, "Show in normal attempts", "Show the sidebar and markers in ordinary (non-practice) attempts.")
b("show-practice", "showPractice", True, "Show in practice mode", "Show the sidebar and markers in practice mode.")
b("show-playback", "showPlayback", True, "Show during macro playback", "Show the sidebar and markers when an analyzed macro is loaded.")
k("key-toggle-sidebar", "F6", "Toggle sidebar hotkey", "Shows / hides the counter sidebar during a level.")
k("key-analyze", "F7", "Start / cancel analysis hotkey", "Starts analysing the loaded macro, or cancels a running analysis.")

# ------------------------------------------------------------------ FPS / TPS
title("FPS / TPS")
info("tps-note", "Windows are counted in physics ticks. Display FPS only changes screen redraws.")
b("frame-limiter", "frameLimiter", False, "Apply display FPS", "Sets the game's frame interval to the display FPS below. Off keeps your FPS settings.")
s("display-fps", "displayFps", "60", "Display FPS", "Preset display refresh rate.", ["60", "120", "144", "240", "360", "custom"])
i("display-fps-custom", "displayFpsCustom", 75, "Custom display FPS", "Used when Display FPS is set to custom.", 10, 1000, **{"control": {"slider": False}})

# ------------------------------------------------------------------ Sidebar
title("Sidebar counter")
b("sidebar-enabled", "sidebarEnabled", True, "Show sidebar", "The stacked window counter (highest bucket on top).")
s("sidebar-corner", "sidebarCorner", "Top-left", "Position", "Screen corner the sidebar is anchored to.", ["Top-left", "Top-right", "Bottom-left", "Bottom-right", "Custom"])
f("sidebar-margin-x", "sidebarMarginX", 1.5, "Margin X (% of width)", "Distance from the left/right edge. Default is about 1.5% of the screen width.", 0, 50, **{"control": {"slider": True}})
f("sidebar-margin-y", "sidebarMarginY", 3.0, "Margin Y (% of height)", "Distance from the top/bottom edge.", 0, 50, **{"control": {"slider": True}})
f("sidebar-custom-x", "sidebarCustomX", 2.0, "Custom X (% of width)", "Only used when Position is Custom. Measured from the left edge.", 0, 100, **{"control": {"slider": True}})
f("sidebar-custom-y", "sidebarCustomY", 97.0, "Custom Y (% of height)", "Used when Position is Custom. Distance from the bottom edge to the first row.", 0, 100, **{"control": {"slider": True}})
f("sidebar-scale", "sidebarScale", 1.0, "Scale", "Overall size multiplier.", 0.2, 4.0, **{"control": {"slider": True}})
f("sidebar-text-scale", "sidebarTextScale", 0.42, "Text scale", "Scale of the bigFont text. 0.5 matches the reference screenshot.", 0.1, 2.0, **{"control": {"slider": True}})
f("sidebar-row-spacing", "sidebarRowSpacing", 4.9, "Row spacing (% of height)", "Vertical distance between rows. 7% matches the reference screenshot.", 2, 20, **{"control": {"slider": True}})
s("sidebar-align", "sidebarAlign", "Left", "Alignment", "Left: two left-aligned columns. Right: labels end at the counts. Center: centred labels.", ["Left", "Right", "Center"])
s("sidebar-font", "sidebarFont", "bigFont.fnt", "Font", "Bitmap font used for the rows.", ["bigFont.fnt", "goldFont.fnt", "chatFont.fnt"])
f("sidebar-outline", "sidebarOutline", 0.0, "Extra outline (px)", "Thickens the text outline with extra black copies behind the text.", 0, 4, **{"control": {"slider": True}})
i("sidebar-opacity", "sidebarOpacity", 100, "Opacity (%)", "Opacity of the whole sidebar.", 5, 100, **{"control": {"slider": True}})
b("show-zero-rows", "showZeroRows", True, "Show rows with zero count", "When off, a row is hidden until its count is above zero.")
s("extended-rows", "extendedRows", "Auto", "Extended rows (13-15, 16+)", "Auto: appear only when their count is above 0. Always / Never override that.", ["Auto", "Always", "Never"])
s("count-style", "countStyle", "Count", "Count style", "Count: number. Count / Total: out of the total. Percent: share of analysed inputs.", ["Count", "Count / Total", "Percent"])
b("show-total-row", "showTotalRow", False, "Show total row", "Adds a final row with the total number of inputs counted.")
s("sidebar-anim", "sidebarAnim", "Fade", "Show / hide animation", "How the sidebar appears and disappears.", ["None", "Fade", "Slide"])
b("show-sigma", "showSigma", False, "Show σ/s line", "Adds a difficulty line at the bottom of the sidebar.")
f("sigma-hours", "sigmaHours", 24.0, "Assumed completion hours", "The expected time to complete the level from 0% used by the σ/s model.", 0.01, 100000, **{})

s("timings", "timings", "1,2,3,4,5-6,7-8,9+", "Timings to show", "Window sizes to count, comma separated: 4, 5-6, 16+. Colours come from the bucket rows.", filter="0123456789,-+; ")
b("rank-colours", "rankColours", True, "Colour from easiest to hardest", "Shown timings go from light purple (easiest) to red (hardest), whatever their numbers. Off: row colours.")

title("Bucket editor", "Up to nine rows. Ranges: 5, 5-6 or 16+. The highest range is drawn on top.")
ROWS = [
    ("1", [255, 75, 75], None, None, False),
    ("2", [255, 154, 60], None, None, False),
    ("3", [255, 226, 60], None, None, False),
    ("4", [255, 255, 255], None, None, False),
    ("5-6", [107, 255, 107], None, None, False),
    ("7-8", [91, 184, 255], None, None, False),
    ("9-12", [106, 91, 255], [201, 195, 255], [36, 54, 217], False),
    ("12-15", [255, 107, 214], None, None, True),
    ("16+", [226, 226, 232], None, None, True),
]
for n, (rng, lc, cc, mc, ext) in enumerate(ROWS, 1):
    title(f"Row {n}")
    b(f"row-{n}-visible", None, True, f"Row {n} visible", "Untick to remove this row entirely.")
    s(f"row-{n}-range", None, rng, f"Row {n} range", "Window sizes counted in this row. Blank disables the row.", filter="0123456789,-+; ")
    s(f"row-{n}-label", None, "", f"Row {n} label", "Text before the colon. Blank uses the range text.")
    b(f"row-{n}-extended", None, ext, f"Row {n} is an extended row", "Extended rows follow the Extended rows mode above.")
    c(f"row-{n}-label-color", None, lc, f"Row {n} label colour", "Colour of the label.")
    c(f"row-{n}-count-color", None, cc or lc, f"Row {n} count colour", "Colour of the count.")
    c(f"row-{n}-marker-color", None, mc or lc, f"Row {n} marker number colour", "Colour of the window number next to click markers.")

# ------------------------------------------------------------------ NaN counter
title("Caption under the percentage")
b("nan-enabled", "nanEnabled", True, "Show caption", "Small grey text directly under the vanilla percentage label.")
s("nan-text", "nanText", "NaN Counter", "Text", "Leave blank to hide the text.")
s("nan-font", "nanFont", "bigFont.fnt", "Font", "Bitmap font.", ["bigFont.fnt", "goldFont.fnt", "chatFont.fnt"])
b("nan-smallcaps", "nanSmallCaps", True, "Small caps", "Capital letters are drawn larger than the rest, like the reference.")
f("nan-size", "nanSize", 0.3, "Size", "Text scale.", 0.05, 2.0, **{"control": {"slider": True}})
c("nan-color", "nanColor", [154, 154, 154], "Colour", "Text colour.")
i("nan-opacity", "nanOpacity", 42, "Opacity (%)", "About 35-50% matches the reference.", 0, 100, **{"control": {"slider": True}})
f("nan-offset-x", "nanOffsetX", 0.0, "Offset X", "Horizontal nudge in game units.", -200, 200, **{})
f("nan-offset-y", "nanOffsetY", 0.0, "Offset Y", "Vertical nudge in game units.", -100, 100, **{})

# ------------------------------------------------------------------ Markers
title("Click markers")
b("marker-enabled", "markerEnabled", True, "Show markers", "Shows a ring with the window number where each analysed input happened.")
f("marker-size", "markerSize", 3.0, "Size", "Multiplier on the default size (9% of the screen height).", 0.2, 4.0, **{"control": {"slider": True}})
c("marker-outline-color", "markerOutlineColor", [0, 0, 0], "Outline colour", "Colour of the outlines around ring and centre.")
s("marker-number-pos", "markerNumberPos", "Left", "Number position", "Where the window number sits relative to the ring.", ["Left", "Right", "Above"])
f("marker-number-size", "markerNumberSize", 0.4, "Number size", "Multiplier on the window number size.", 0.2, 4.0, **{"control": {"slider": True}})
f("marker-text-distance", "markerTextDistance", -0.3, "Number distance", "Gap between number and ring, in ring widths. Negative moves it onto the ring.", -0.5, 2.0, **{"control": {"slider": True}})
b("frame-sounds", "frameSounds", True, "Frame sounds", "Plays frame-N.wav for each analysed input during playback (9 and up use frame-9).")
f("frame-sound-volume", "frameSoundVolume", 1.0, "Frame sound volume", "Volume of the frame sounds.", 0.0, 2.0, **{"control": {"slider": True}})
f("marker-lifetime", "markerLifetime", 1.5, "Lifetime (seconds)", "0 keeps a marker until it scrolls off screen.", 0, 60, **{})
b("marker-fade", "markerFade", True, "Fade out", "Fade markers instead of popping them.")
i("marker-filter-below", "markerFilterBelow", 0, "Only windows below N", "0 shows every marker. Otherwise only inputs whose window is smaller than N.", 0, 64, **{})
s("marker-kinds", "markerKinds", "All", "Which inputs", "Presses, releases or both.", ["All", "Presses only", "Releases only"])
i("marker-max", "markerMax", 48, "Max markers on screen", "Pool size. Older markers are recycled first.", 4, 256, **{})

# ------------------------------------------------------------------ Readout
title("Secondary readout")
b("readout-enabled", "readoutEnabled", False, "Show readout", "Small white outlined text in the bottom-left corner: progress % and run time.")
b("readout-time", "readoutTime", True, "Show run time", "Adds the run time under the percentage.")
f("readout-x", "readoutX", 1.5, "X (% of width)", "Distance from the left edge.", 0, 100, **{})
f("readout-y", "readoutY", 8.0, "Y (% of height)", "Distance from the bottom edge to the lower line.", 0, 100, **{})
f("readout-scale", "readoutScale", 0.55, "Scale", "Text scale.", 0.1, 3.0, **{})
c("readout-color", "readoutColor", [255, 255, 255], "Colour", "Text colour.")

# ------------------------------------------------------------------ Numbers
title("Numbers")
i("decimals", "decimals", 0, "Decimals", "Digits after the point for window numbers. CBF mode uses at least 2.", 0, 4, **{})
b("auto-timing", "autoTiming", True, "Fix imported macro timing", "If an imported macro dies early, other frame numberings are tried and the best is used.")
b("exact-replay", "exactReplay", True, "Exact replay from the start", "Replays every trial from the start (macros up to 30 s). Longer ones use snapshots.")
b("record-stitch", "recordStitch", True, "Recording: respawn at practice checkpoints", "On: respawn at your last checkpoint, rebuilt exactly. Off: every death restarts at 0%.")
b("rebuild-hide-screen", "rebuildHideScreen", False, "Hide whole screen during respawn", "On: the whole screen is hidden while a checkpoint respawn is rebuilt (beta 2). Off: only the level objects.")
b("auto-repair", "autoRepair", True, "Repair macros that die", "Moves the clicks nearest to a death by a few ticks until a macro that dies early survives.")
i("repair-max-shift", "repairMaxShift", 12, "Repair: max click move (ticks)", "How far a click may be moved while repairing a macro.", 1, 60, **{})
b("keep-hold", "keepHold", True, "Move the release with the click", "A shifted click moves its release too, so the hold length stays the same.")
s("release-timing", "releaseTiming", "Always", "Release timings", "Auto: only where releases matter. Always: every release. Never: clicks only.", one_of=["Auto", "Always", "Never"])
b("subtick-auto", "subtickAuto", False, "Half-tick measurement (experimental)", "Also tries clicks between ticks when the game counts several steps per tick.")
b("cbf-mode", "cbfMode", False, "CBF mode (sub-tick clicks)", "Measures windows in fractions of a tick. Needs a game build that honours click timestamps.")
i("cbf-subdivisions", "cbfSubdivisions", 4, "CBF steps per tick", "Resolution of the sub-tick search.", 2, 16, **{})

# ------------------------------------------------------------------ Analysis
title("Analysis")
i("analysis-speed", "analysisSpeed", 100, "Speed (x real time)", "Target speed in multiples of real time. Higher is faster if your PC keeps up.", 1, 400, **{"control": {"slider": True}})
b("auto-throttle", "autoThrottle", True, "Auto-throttle", "Slows down when frames get long and ramps back up afterwards.")
i("min-fps", "minFps", 10, "Minimum FPS while analysing", "The analysis never spends more than 1/this per frame.", 5, 60, **{})
i("max-shift", "maxShift", 24, "Max shift each side (ticks)", "How far an input is moved before it counts as 'unbounded'.", 1, 64, **{})
i("max-trials", "maxTrials", 120, "Max trials per input", "Hard cap on re-simulations for one input.", 4, 400, **{})
i("horizon-inputs", "horizonInputs", 2, "Survive next N inputs", "A shifted input must survive this many later inputs, plus the tail below.", 1, 32, **{})
i("horizon-tail", "horizonTail", 14, "Tail after last input (ticks)", "Extra ticks that must be survived.", 0, 240, **{})
i("horizon-min", "horizonMin", 48, "Minimum horizon (ticks)", "Shortest section checked for a trial.", 4, 480, **{})
i("horizon-max", "horizonMax", 2400, "Maximum horizon (ticks)", "Longest section checked for a trial. Also the infinite-loop guard.", 30, 4800, **{})
i("fly-strictness", "flyStrictness", 2, "Ship / wave / swing strictness", "0 = same as other modes. Higher checks a longer stretch after each click there.", 0, 3, **{})
i("converge-ticks", "convergeTicks", 12, "Convergence ticks", "A trial ends early after matching the original run for this many ticks in a row.", 2, 60, **{})
i("rejoin-extra", "rejoinExtra", 30, "Keep checking while off-course (ticks)", "Extra ticks checked past the horizon while a shifted run is still off course. 0 = off.", 0, 2400, **{})
b("verify-boundaries", "verifyBoundaries", True, "Re-check every window edge", "Replays the edge of each window to the end with no early exit. Lowers it if it dies.")
b("recheck-tight", "recheckTight", True, "Re-check tight clicks at the end", "Measures every click with a window of 1-3 ticks once more at the end, strictly. Slower.")
b("fidelity-selftest", "fidelitySelfTest", True, "Snapshot self-test", "Re-simulates 16 ticks twice from each snapshot to check both runs match.")
b("fidelity-longselftest", "fidelityLongSelfTest", True, "Check snapshots over the whole trial", "Checks snapshots over a whole trial. Bad snapshots fall back to replaying from the start.")
b("local-start", "localStart", True, "Start trials just before the click", "When a snapshot does not restore bit-exactly, still start each trial from it (a few ticks before the click) and set the player's position, velocity and rotation to what the first pass had there, instead of replaying from an earlier snapshot or from the start. Much faster deeper into a level.")
b("mute-analysis", "muteAnalysis", True, "Mute audio while analysing", "Recommended: checkpoint loads would otherwise seek the music thousands of times.")

# ------------------------------------------------------------------ Export
title("Export")
b("export-on-finish", "exportOnFinish", True, "Save JSON when analysis finishes", "Writes <macro>.windows.json next to the macro.")
b("screenshot-mode", "screenshotMode", False, "Screenshot mode", "Hides pause and practice buttons and attempt labels, leaving the sidebar and markers.")
btn("btn-save-json", "Save results now", "Writes the JSON for the current results.", "Save JSON")
btn("btn-copy-summary", "Copy summary", "Copies a text summary of the current results to the clipboard.", "Copy")
btn("btn-open-folder", "Macros folder", "Opens the folder Windowpane reads macros from.", "Open")

# ------------------------------------------------------------------ emit
settings = {}
for kind, key, field, default, name, desc, extra in S:
    d = {"type": kind, "name": name}
    if desc:
        d["description"] = desc
    if kind in ("bool", "int", "float", "string"):
        d["default"] = default
    elif kind == "color":
        d["default"] = default
        d["type"] = "color"
    elif kind == "keybind":
        d["default"] = [default]
        d["category"] = "gameplay"
    elif kind == "title":
        pass
    for kk, vv in extra.items():
        d[kk] = vv
    if kind == "info":
        d["description"] = desc
        d.pop("name")
        d["name"] = "Note"
    settings[key] = d

mod = {
    "geode": "5.11.2",
    "gd": {"win": "2.2081", "android": "2.2081", "mac": "2.2081", "ios": "2.2081"},
    "id": "jake.windowpane",
    "name": "Windowpane",
    "version": "v2.5.1",
    "developer": "jake",
    "description": "Frame-window analysis for Geometry Dash macros: per-click timing windows at 240 Hz, a live counter, click markers and a σ/s difficulty rating.",
    "repository": "",
    "tags": ["utility", "enhancement", "offline"],
    "settings": settings,
}
mod.pop("repository")
with open(os.path.join(ROOT, "mod.json"), "w", encoding="utf-8") as fh:
    json.dump(mod, fh, indent=2, ensure_ascii=False)
    fh.write("\n")

TYPES = {"bool": "WP_B", "int": "WP_I", "float": "WP_D", "string": "WP_S", "color": "WP_C"}
with open(os.path.join(ROOT, "src/core/CfgFields.inc"), "w", encoding="utf-8") as fh:
    fh.write("// GENERATED by tools/gen_settings.py - do not edit by hand.\n")
    for kind, key, field, default, name, desc, extra in S:
        if field and kind in TYPES:
            fh.write(f'{TYPES[kind]}({field}, "{key}")\n')
print("settings:", len(settings))
