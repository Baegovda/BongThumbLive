> **For AI agents: read [HANDOFF.md](HANDOFF.md) first.** It has a Fresh environment setup (connect Baegovda's PC "Revaptor-FX", GitHub access, tools), local paths, build/test/push steps, standing rules, the current step and a Resume checklist. Do not rely on any agent-side files; this repo is the source of truth. Reply to the user in short Korean (해요체).

# BongThumbLive 2.1 (Unreal Engine 5.7 editor plugin)

Live, looping Content Browser thumbnails:

| Asset type | What moves |
|---|---|
| Animation Sequence / Montage / Composite | the clip plays in a loop on its preview mesh |
| Material / Material Instance | Time / Panner / Sine driven nodes animate |
| Niagara System, Cascade Particle System | the effect is simulated in a small preview world |

## What's new in 2.1
- **Memory leak fix.** In earlier versions the editor could grow to tens of GB. The visible-thumbnail auto-load loaded every animation on screen. Each of those loads pulled in a full preview mesh with its materials and textures. Editor assets are kept with `RF_Standalone`, so nothing was ever garbage-collected. The held-mesh list had no cap, and the 256 MB frame cache was too small, so frames were re-rendered forever. 2.1 adds these limits:
  - a hard frame-cache budget (default 1024 MB, LRU eviction);
  - a cap on held preview meshes (default 6) and on in-flight loads;
  - release + GC of every package the plugin itself loaded, once its frames are cached or it has been off screen for a while;
  - a memory guard that pauses loading and rendering and frees everything when RAM runs low.
- **Persistent disk cache.** Rendered frames are saved to `Saved/BongThumbLive/Cache` (one Oodle-compressed file per asset + settings key, written asynchronously).
  - After an editor restart, cached animations play **without loading the animation or its mesh**.
  - An entry is invalidated automatically when the asset, its mesh or its skeleton changes. Deleted and renamed assets are cleaned up.
  - Total size is capped (default 2048 MB) with LRU, where "recently used" means "last shown in a visible tile".
- **GPU playback.** Cached frames live in GPU atlases, uploaded once. Each frame only copies one atlas cell (128x128 by default) into the tile, with no re-render and no readback. Measured: 86 animated tiles at ~117 editor fps (8.5 ms frames), with about 700 MB of GPU frames. The frame budget is spent only on generating new frames.
- **Content Browser button.** A settings icon left of the lock icon opens the plugin settings.
- **Korean settings UI.** Categories are 기본 (Basic), 재생 (Playback), 캐시 (Cache), 성능 (Performance) and 고급 (Advanced), with one-line tooltips.
- The cache section of the settings page shows the cache folder, an "open folder" button and a "clear cache" (캐시 비우기) button.

## Install
Copy the `BongThumbLive` folder into `<Project>/Plugins/` and restart the editor. The release zip contains prebuilt Win64 binaries for UE 5.7. To build from source instead, use `RunUAT BuildPlugin`.

## Settings
**Project Settings > Plugins > Bong Thumb Live**, or the gear icon in the Content Browser. The settings are saved per user.

## Console commands
| Command | Purpose |
|---|---|
| `BongThumbLive.Status` | state, memory/disk cache stats (entries, MB, hits/misses), process memory |
| `BongThumbLive.Set <Name> <Value>` | change a setting (e.g. `Mode VisibleInBrowser`, `MaxFPS 60`, `CacheMemoryMB 1024`, `MaxDiskCacheMB 2048`) |
| `BongThumbLive.ClearDiskCache` | delete all cached frames on disk and in memory |
| `BongThumbLive.ClearMemoryCache` | drop the in-memory (GPU) cache only |
| `BongThumbLive.DumpCache <dir>` | debug: write a few cached frames as PNG |
| `BongThumbLive.RecreateRenderState 0/1` | 1 (default) = exact pose for every cached frame |

## Notes
- Win64 / editor only. Tested with D3D11 on UE 5.7.
- Filling the cache for a large folder costs editor frame time once (a few seconds per screen of new animations). After that, playback is essentially free.
- The disk cache is safe to delete at any time.

## Audit fixes (2026-11-08, unreleased)
- Selection, visible auto-load and missing-tile recovery share the memory guard and async batch limit. Plugin-loaded animation roots remain capped at 12; preview meshes remain capped at 6 by default.
- Saving or editing a preview mesh/skeleton invalidates dependent frames immediately. Package signatures are also rechecked while entries are in memory.
- Cache jobs carry generation stamps. Clear, rename and removal discard obsolete work; file revisions prevent delayed deletes/index writes from overwriting newer results.
- The frame-cache budget now includes live atlas references and reserved readback, compression, decoding and upload buffers, including those still held after an entry is evicted. Preview assets and engine/driver overhead are handled separately by the RAM guard.
- Legacy AnimThumbLive settings migrate before the new section is saved. Clips with an effective cache rate below 1 FPS sample the entire loop. Invalid cache geometry and oversized string/array prefixes are rejected before allocation.
- Loading overlays clip to the tile table and tile bounds, including partially scrolled rows.
- The benchmark watchdog requires an exact PID, start time, executable and project. It never selects an arbitrary editor or terminates unrelated crash reporters.
- The optional MCP helper has a lockfile; use `npm ci` in `Tools/btl_mcp`.

## Regression tests
Run only in a **disposable UE 5.7 project**, with PythonScriptPlugin and EditorScriptingUtilities enabled and copied animation fixtures (the default runner folder is listed in the script). Create an empty `BTL_DISPOSABLE_TEST_PROJECT` marker in that project directory. The runner creates three rendering fixtures under `/Game/BTLRegression` and 256 unsaved UI tiles; it must not be run in a working art project.

Launch the disposable project with `-d3d11 -unattended -AutoDeclinePackageRecovery -ExecCmds="py <repo>/Tools/Tests/run_editor_regressions.py" -ReportExportPath=<report-dir> -TestExit="Automation Test Queue Empty" -ini:EditorSettings:[/Script/UnrealEd.EditorPerformanceSettings]:bThrottleCPUWhenNotForeground=False`.

The `BongThumbLive.*` automation suite covers sampling, bounded cache parsing, concurrent memory reservations, stale async publication and dependency invalidation, loading guards, Material/Niagara/Cascade rendering, and clipping of real partially visible thumbnail widgets. Read the exported `index.json` failure count; TestExit alone is not a passing result.

`Tools/Tests/test_watchdog.ps1` validates the identity guards against a running disposable editor without terminating it. Supply `-EditorProcessId`, `-ProjectPath`, `-EditorExecutable` and an existing `-OutputDirectory`.
