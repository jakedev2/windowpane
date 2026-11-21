# Reading a reference frame-window video

Needs ffmpeg on PATH and `pip install numpy pillow`.

1. `python refocr.py <video.mp4> ocr.json` reads every frame's sidebar counters (digit templates are learned from the video itself:
   the frames at 30 s and 73 s of the Acheron video, whose values are known; edit `learn(...)` for another video) and the progress bar.
2. `python refclean.py ocr.json events.json` turns the readings into one event per counted click (frame + bucket row) and checks
   them against the counter values read by eye.
3. `python align.py ocr.json events.json <results.windows.json> aligned.json` maps the progress bar to percent and aligns the
   reference clicks with Windowpane's clicks by position, then prints per-click bucket differences.

The Acheron reference macro is a different recording than the zBot file, so per-click differences are noisy; compare distributions
(tools/compare_reference.py) first.
