> **For AI agents: read [HANDOFF.md](HANDOFF.md) first.** It has a Fresh environment setup (connect Baegovda's PC "Revaptor-FX", GitHub access, tools), local paths, build/test/push steps, standing rules, the current step and a Resume checklist. Do not rely on any agent-side files; this repo is the source of truth. Reply to the user in short Korean (해요체).

# BongThumbLive 2.3.3 (Unreal Engine 5.8 editor plugin)

Live, looping Content Browser thumbnails:

| Asset type | What moves |
|---|---|
| Animation Sequence / Montage / Composite | the clip plays in a loop on its preview mesh |
| Material / Material Instance | Time / Panner / Sine driven nodes animate |
| Niagara System, Cascade Particle System | the effect is simulated in a small preview world |

## What's new in 2.3.3
- **Dense soft effects stay visible.** The particle-position framing from 2.3.2 is now checked with one render before use; if the view would show almost nothing (thousands of soft sand-dust sprites), the pixel framing is kept. Sand dust tiles that went black in 2.3.2 are large again.
- **Big effect folders get framed quickly.** Effects without a fit yet get their own calibration budget (3 per frame; refinement 1 per frame), so in a folder of 150 effects the tiles drawn first no longer take all samples.
- **No black first frame.** A new effect scene renders nothing in the frame that creates it; that one draw now shows the stock thumbnail.

## What's new in 2.3.2
- **Falling leaves and other scattered effects show up.** When an effect draws too few pixels to measure, or only scattered specks (leaves, petals, dust spread over many metres), the camera is fitted to the particle positions of its CPU emitters: around the middle of the particles, wide enough to hold the nearest eighth of them (8-40). Leaf effects that looked empty now show their leaves in the tile.

## What's new in 2.3.1
- **Effects with far-flung sparks fill the tile too.** Auto-framing groups the drawn pixels into blobs and frames the biggest blobs until 80% of the effect is inside, so a few sparks far from the flame no longer make it tiny (a torch whose sparks fall 25 m now shows its flame large). Evenly scattered effects (snow, rain, falling leaves) frame their densest middle half.
- `BongThumbLive.FXDebug` lists every emitter (CPU/GPU, state, particle count); `BongThumbLive.FXLightBoost` brightens lit particles in effect tiles (default 1 = unchanged).

## What's new in 2.3.0
- **Effect tiles fill the square.** Niagara and Cascade thumbnails are framed on what the effect actually draws, not on its bounds (Niagara bounds are often hand-set and far too large). The plugin renders the effect alone into a small image a few dozen times during its first seconds and fits the camera to the covered pixels (outlying sparks ignored), so the effect fills about 85% of the tile. `BongThumbLive.FXAutoFrame 0` restores the old framing; `BongThumbLive.FXAutoFrameFill` sets the share.
- **Live effects work again on UE 5.8.** Four 5.8 problems made live effect tiles empty or frozen; all are fixed:
  - Niagara systems never opened in the session stayed on their static thumbnail: their deferred compile request was never started. The plugin now starts it.
  - Ticking the preview world barely advanced Niagara (0.8 s of effect time per 9.7 s), so almost no particles existed. The plugin now advances the system directly when needed.
  - The 5.8 thumbnail sky sphere (the checker backdrop) hid or washed out effects. Effect tiles now have no sky sphere and an opaque black background (`BongThumbLive.FXOpaqueBackground`).
  - Particles were not sent to the render thread before a draw in the same frame.
- The regression suite now checks that effect thumbnails actually show particles.

## What's new in 2.2.2
- **More load slots.** The plugin may hold 24 animations at once while building caches (was 12; console variable `BongThumbLive.MaxLoadedAnims`). UE 5.8, 113-animation folder, empty cache: tiles appear after about 1.35 s on average (was about 1.55 s), the slowest after about 4 s (was 4.3-5.3 s). Peak editor memory was the same (1.9-2.0 GB). The preview-mesh cap is unchanged.

## What's new in 2.2.1
- **Faster first look at a new folder.** While tiles still wait, an animation's load slot is freed as soon as its first coarse frames exist (`BongThumbLive.ReleaseAfterCoarse 1`). UE 5.8, 113-animation folder with an empty cache: tiles appear after 1.4-3 s on average (was about 5 s), the slowest after 4-10 s (was about 24 s). The memory caps are unchanged, and every cache still finishes.

## What's new in 2.2
- **UE 5.8.** Built for 5.8. The 5.8 Content Browser hides its asset view inside an `SInvalidationPanel`; 2.2 finds it, so tiles animate on 5.8.
- **Better framing.** Animation tiles are framed on the skinned mesh itself, not on the engine bounds. Meshes such as first-person arms (root at the floor, wide reference pose) were tiny; they now fill the tile. `BongThumbLive.TightFraming 0` restores the old framing. Old caches are rebuilt once.
- **Smoother cache generation.** A render-thread budget for offscreen renders, asynchronous loads on selection and incremental GC remove the long hitches while new frames are made.
- **No stuck tiles.** Black tiles, stock icons and scrolled-in tiles that waited for a mouse hover now animate. Cached frames are pushed into every tile size the browser shows.
- **Loading indicator.** A tile shows a spinner until its first frame. Colour, style (Spin, Pulse, Dots, Bar) and speed are settings.
- **Settings for the whole engine.** Settings are shared by every project on the same engine. Values from older builds are imported once.
- The audit fixes listed below.

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
Copy the `BongThumbLive` folder into `<Project>/Plugins/` (one project) or `<Engine>/Plugins/Marketplace/` (every project on that engine) and restart the editor. The source targets UE 5.8 (`EngineVersion 5.8.0`). The v2.2.x and v2.3.x release zips contain prebuilt Win64 binaries for UE 5.8; v2.1 is the last UE 5.7 build. To build from source, use `RunUAT BuildPlugin`.

## Settings
**Project Settings > Plugins > Bong Thumb Live**, or the gear icon in the Content Browser. The settings are saved per user for the whole engine (`EditorSettings`), so every project on the same engine shares them. Values saved per project by older builds are imported once. The frame cache stays per project.

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
- Win64 / editor only. Tested with D3D11 on UE 5.7. On UE 5.8 the automation suite passes (7/7, 2026-11-16).
- Filling the cache for a large folder costs editor frame time once (a few seconds per screen of new animations). After that, playback is essentially free.
- The disk cache is safe to delete at any time.

## Audit fixes (2026-11-08, in 2.2)
- Selection, visible auto-load and missing-tile recovery share the memory guard and async batch limit. Plugin-loaded animation roots remain capped at 12; preview meshes remain capped at 6 by default.
- Saving or editing a preview mesh/skeleton invalidates dependent frames immediately. Package signatures are also rechecked while entries are in memory.
- Cache jobs carry generation stamps. Clear, rename and removal discard obsolete work; file revisions prevent delayed deletes/index writes from overwriting newer results.
- The frame-cache budget now includes live atlas references and reserved readback, compression, decoding and upload buffers, including those still held after an entry is evicted. Preview assets and engine/driver overhead are handled separately by the RAM guard.
- Legacy AnimThumbLive settings migrate before the new section is saved. Clips with an effective cache rate below 1 FPS sample the entire loop. Invalid cache geometry and oversized string/array prefixes are rejected before allocation.
- Loading overlays clip to the tile table and tile bounds, including partially scrolled rows.
- The benchmark watchdog requires an exact PID, start time, executable and project. It never selects an arbitrary editor or terminates unrelated crash reporters.
- The optional MCP helper has a lockfile; use `npm ci` in `Tools/btl_mcp`.

## Regression tests
Run only in a **disposable UE 5.8 project**, with PythonScriptPlugin and EditorScriptingUtilities enabled and copied animation fixtures (the default runner folder is listed in the script). Create an empty `BTL_DISPOSABLE_TEST_PROJECT` marker in that project directory. The runner creates three rendering fixtures under `/Game/BTLRegression` and 256 unsaved UI tiles; it must not be run in a working art project.

Launch the disposable project with `-d3d11 -unattended -AutoDeclinePackageRecovery -ExecCmds="py <repo>/Tools/Tests/run_editor_regressions.py" -ReportExportPath=<report-dir> -TestExit="Automation Test Queue Empty" -ini:EditorSettings:[/Script/UnrealEd.EditorPerformanceSettings]:bThrottleCPUWhenNotForeground=False`.

The `BongThumbLive.*` automation suite covers sampling, bounded cache parsing, concurrent memory reservations, stale async publication and dependency invalidation, loading guards, Material/Niagara/Cascade rendering, and clipping of real partially visible thumbnail widgets. Read the exported `index.json` failure count; TestExit alone is not a passing result.

`Tools/Tests/test_watchdog.ps1` validates the identity guards against a running disposable editor without terminating it. Supply `-EditorProcessId`, `-ProjectPath`, `-EditorExecutable` and an existing `-OutputDirectory`.
