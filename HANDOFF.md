# BongThumbLive – HANDOFF (keep this file current)

Last update: 2026-10-30 02:40 KST (Asia/Seoul).

## What this is
BongThumbLive is a UE 5.7 **editor-only** plugin (Win64) that makes Content Browser thumbnails move: animation sequences, montages and composites loop on their preview mesh, materials animate, and Niagara and Cascade effects simulate. Animation frames are cached in GPU atlases and on disk.

## Talking to the user
- Owner/user = **Baegovda** (GitHub name; Korean speaker; Windows PC user account `Revaptor_FX`). Replies to the user: **short Korean, 해요체**, beginner-friendly.
- User quotes so far (Korean): "플러그인 작업은 마저 재개해줘"

## Fresh environment setup (new bot, new account, empty box, PC not registered)
Do NOT rely on any agent-side files (`/workspace/...`, memory). Everything needed is in this repo and on Baegovda's PC.
1. **Connect the PC.** Ask Baegovda (short Korean 해요체) to register their Windows PC with you, e.g. "작업용 PC를 연결해 주세요 (Revaptor-FX)". The PC is named **Revaptor-FX**, Windows user `Revaptor_FX`. Identify it by these paths existing:
   - `K:\Unreal Projects\UE_VFX_57\UE_VFX_57.uproject` (project)
   - `K:\Unreal Projects\UE_VFX_57\Plugins\BongThumbLive\BongThumbLive.uplugin` (plugin)
   - `C:\Program Files\Epic Games\UE_5.7\Engine\Binaries\Win64\UnrealEditor.exe` (engine)
   The old machine id was `d3fb99ad-25ca-45c1-81af-41e5ca09104a` (valid only on the original bot account). Shell on the PC is PowerShell (pwsh).
2. **GitHub access.** The repo is private (owner Baegovda). The PC already has `gh` 2.95 + git 2.47 logged in and able to push: verify with `gh auth status` on the PC. Clone if missing: `gh repo clone Baegovda/BongThumbLive C:\Users\Revaptor_FX\AppData\Local\Temp\BongThumbLive_repo`. If `gh` is logged out, ask Baegovda to run `gh auth login` on the PC (do not handle tokens yourself). A GitHub connector on your side is optional (read-only convenience).
3. **Required tools on the PC** (all already installed): UE 5.7 at `C:\Program Files\Epic Games\UE_5.7`; RunUAT `C:\Program Files\Epic Games\UE_5.7\Engine\Build\BatchFiles\RunUAT.bat` (needs Visual Studio 2022 C++ toolchain, present); git, gh; Python 3.10 (`C:\Program Files\Python310\python.exe`) for log parsing.
4. **Test tools live in the repo** under `Tools/` (bench script, log parser, watchdog). Copy them to `C:\Users\Revaptor_FX\AppData\Local\Temp\` before running (the bench script reads its config from there).
5. Then run the Resume checklist below.

## Access (for a new bot that only has the repo URL)
- The repo is **private**: https://github.com/Baegovda/BongThumbLive. To read it from the box, use the GitHub connector/tools (cursor-github), or `gh` on the user's PC. The PC's `gh` and git are already authenticated as the user (Revaptor_FX / revaptor@gmail.com) and can push.
- All real work happens on the user's Windows PC. Use Shell targeted at the PC (PowerShell; see Fresh environment setup to find/register it), and use CopyToBox / CopyFromBox to move files.
- If the clone at `C:\Users\Revaptor_FX\AppData\Local\Temp\BongThumbLive_repo` is missing, run `gh repo clone Baegovda/BongThumbLive <dir>` on the PC.

## Resume checklist (run first)
1. `git -C C:\Users\Revaptor_FX\AppData\Local\Temp\BongThumbLive_repo pull --ff-only`, then read this file from the clone (newest copy).
2. Check the editor state: `Get-Process UnrealEditor*,CrashReportClient*`. If a crash reporter is open, close it and read `K:\Unreal Projects\UE_VFX_57\Saved\Crashes\<newest>`.
3. Compare the plugin source with the clone: `git diff --no-index` between `K:\Unreal Projects\UE_VFX_57\Plugins\BongThumbLive\Source` and `<clone>\Source`. Uncommitted plugin edits = work in progress described below.
4. Check the newest backups (`*.bakN`) next to the changed files, and Binaries\Win64 timestamps vs. the source, to know whether the last build was deployed.
5. Read the "In progress" section and continue from the exact step. After each step, update this file, commit and push (ff-only). (Optional: mirror a copy to your own box; never rely on it.)
6. Launch the editor (pre-authorized): `"C:\Program Files\Epic Games\UE_5.7\Engine\Binaries\Win64\UnrealEditor.exe" "K:\Unreal Projects\UE_VFX_57\UE_VFX_57.uproject" -AutoDeclinePackageRecovery [-ExecCmds="py <script>"]`. The log is at `K:\Unreal Projects\UE_VFX_57\Saved\Logs\UE_VFX_57.log`.

## Paths
- User PC: **Revaptor-FX** (original-account machine id `d3fb99ad-25ca-45c1-81af-41e5ca09104a`; Windows, 128 GB RAM, 24C/32T). Run commands with Shell machineId (PowerShell).
- Plugin (live, used by editor): `K:\Unreal Projects\UE_VFX_57\Plugins\BongThumbLive`. This folder is NOT a git repo.
- Git clone used for commits: `C:\Users\Revaptor_FX\AppData\Local\Temp\BongThumbLive_repo` (origin https://github.com/Baegovda/BongThumbLive.git, **private**). Workflow: edit the plugin source, then copy the changed files into the clone, then commit and push (fast-forward only).
- Project: `K:\Unreal Projects\UE_VFX_57\UE_VFX_57.uproject`; engine `C:\Program Files\Epic Games\UE_5.7`.
- Disk cache: `K:\Unreal Projects\UE_VFX_57\Saved\BongThumbLive\Cache`.
- Full plugin backups: `K:\Unreal Projects\UE_VFX_57\PluginBackups\BongThumbLive.bak1..bak7`.
- Test/diag scripts: `C:\Users\Revaptor_FX\AppData\Local\Temp\btl_*.py|ps1`.
  - `btl_verify3.py` = timed run: Status dumps, screenshots, settings page; RUN number in `btl_run.txt`.
  - `btl_watchdog.ps1` = memory watchdog that kills the editor above a limit.
  - `btl_rep.ps1` = safe exact-string replace helper.
- **Bench runner (use this):** copy `Tools/*` to Temp, edit `Temp\btl_bench_cfg.json` (run name, folders), then `pwsh -File $env:TEMP\btl_run.ps1 -Run <name>`. It starts the watchdog (`btl_mem_<name>.csv`), launches the editor with `-AutoDeclinePackageRecovery` and **`-ini:EditorSettings:[/Script/UnrealEd.EditorPerformanceSettings]:bThrottleCPUWhenNotForeground=False`** (otherwise any foreground window, e.g. a Windows Security prompt, drops the editor to ~3 fps and invalidates the run), waits up to 900 s, copies the log to `Temp\btl_<name>.log` and prints the parsed result.
- Unreal Insights helpers in `Tools/`: `btl_trace_long.py` (events longer than N ms) and `btl_trace_agg.py` (aggregate by thread/depth/timer) over CSVs exported with `UnrealInsights.exe -OpenTraceFile=<utrace> -NoUI -AutoQuit -ExecOnAnalysisCompleteCmd="TimingInsights.ExportTimingEvents <csv> -threads=* -columns=ThreadName,TimerName,StartTime,Duration,Depth -startTime=a -endTime=b"`.
- Bench tools in repo `Tools/`: `btl_bench.py` (run with `-ExecCmds="py C:/Users/Revaptor_FX/AppData/Local/Temp/btl_bench.py"`, config `btl_bench_cfg.json` in Temp), `btl_bench_parse.py <log>` (prints folder-click → first tile / first playing, hitch summary, fps<50 seconds), `btl_watchdog.ps1`.
- Original bot's box notes (`/workspace/bongthumb/`) are optional extras only.

## Repo / releases
- v2.1 released: tag commit 9b3050e; README fix 8d9a9ac (main).
- Do NOT publish a new release unless the user asks. Report readiness for v2.2 instead.

## Build
- `"C:\Program Files\Epic Games\UE_5.7\Engine\Build\BatchFiles\RunUAT.bat" BuildPlugin -Plugin="K:\Unreal Projects\UE_VFX_57\Plugins\BongThumbLive\BongThumbLive.uplugin" -Package=C:\Users\Revaptor_FX\AppData\Local\Temp\BongThumbLive_pkgN -TargetPlatforms=Win64 -Rocket`
- Then copy `Binaries\Win64\*` from the package into the plugin. Close the editor first, and back up the old dll/pdb/modules as .bakN.
- Build.bat does not work for this project: the KawaiiPhysics plugin location breaks it, and there is no C++ target.

## Standing rules
- Back up every changed file or binary as `.bakN` next to it. Never overwrite an existing .bakN; use the next free N.
- Never delete user assets. Never touch `/Game/SM2_Work/Original/*` or `/Game/Work/*`. Never save Lightning_01.
- Editor restarts are pre-authorized. Launch with `-AutoDeclinePackageRecovery`. On a crash: close CrashReportClient, read the report (Saved/Crashes), relaunch and resume.
- Full CPU use is OK (work is top priority).
- Keep the plugin's memory caps: 1024 MB GPU frame cache, 6 meshes / 12 anims, MinFreeMemoryGB 8, MaxDiskCacheMB 2048.
- Settings UI text stays Korean and beginner-friendly.
- Commits: fast-forward only, never force-push. No new release.
- Screenshots: save on the PC under `C:\Users\Revaptor_FX\AppData\Local\Temp\btl_shots\` and copy to your box (original bot: `/workspace/bongthumb/`). Also commit small ones to `Docs/` if useful.
- This chat may be cut off at any time. Update this file after every meaningful step, then commit and push it. The repo copy is the only source of truth.

## Architecture (source in Source/BongThumbLive)
- `BongThumbLiveModule.cpp`: core ticker.
  - UpdateVisibleTiles: walks SAssetView widgets, reads tile FTagMetaData and builds VisibleAnimPaths.
  - The live set is only tiles on screen.
  - Folder auto-load and visible auto-load: OnPoolThumbnail queues assets into VisiblePending, then LoadPackageAsync with batch size AutoLoadBatchSize.
  - Memory guard, owned-asset release (BongOwned) and throttled GC.
  - Console commands BongThumbLive.*.
- `BongThumbLiveRenderer.cpp`:
  - Anim thumbnail renderer.
  - Preview-mesh resolver (async mesh loads, at most 2 in flight, give up after 4 failures).
  - FillFrames (offscreen render into cache cells within FrameBudgetMs).
- `BongThumbLiveCache.cpp`:
  - GPU atlas frame cache (LRU 1024 MB).
  - Disk cache (Oodle, async read/write, index file, invalidation by package signature and deps).
  - Push = copy one atlas cell into the pool tile texture per frame.
  - BongOwned namespace = tracking and release of plugin-loaded packages, and the GC trigger.
- `BongThumbLiveFXRenderer.cpp`: Niagara and Cascade preview worlds.
- `BongThumbLiveSettings.h/.cpp` + `SettingsCustomization`: Korean settings (기본/재생/캐시/성능/고급), toolbar gear icon left of the lock.

## Request ledger (all user requests so far)
1. Live animated thumbnails for anim/material/Niagara/Cascade (done, v1-2.0).
2. Fix the memory explosion (tens of GB): caps, release, GC, memory guard (done, 2.1).
3. Persistent disk cache with invalidation and an LRU size cap (done, 2.1).
4. Cached playback of ~80 tiles at 60 fps (done: 86 tiles at ~117 editor fps).
5. Settings icon in the Content Browser left of the lock. Korean, beginner-friendly settings (done).
6. Private GitHub repo plus releases (done: v2.1).
7. **Eliminate the 15-25 s hitch when new anim caches are generated** (in progress).
8. **Fix the tile that gives up because its preview mesh can't be found** (generic fallback mesh, or a clear static fallback) (todo).
9. **Verify disk-cache invalidation after edit/re-save, and the tooltip on screen** (todo).
10. **Folder click: capture must start immediately.** Measure folder-click → first live tile before and after, and report which part of the delay is engine-side (todo).
11. Always keep the memory caps. Settings stay Korean and beginner-friendly.

## Done (this session, 2026-10-29 evening)
- Analysed run4 (fresh generation) and run5 (all cached) logs.
- Baselines base1 (v2.1, cold DDC) and base2 (v2.1, warm DDC); Insights trace of the generation phase.
- **v2.2 source changes (in repo Source/, build pkg8 deployed on the PC):**
  - **Render governor** (Module.cpp): every offscreen anim scene render is bracketed by `BeginSceneRender/NoteSceneRender`; render-thread cost per render is measured with enqueued timestamp commands (EMA). Per-frame allowance = clamp(SceneRenderBudgetMs / msPerRender, 1, MaxSceneRendersPerFrame). CVars `BongThumbLive.SceneRenderBudgetMs` (6) and `BongThumbLive.MaxSceneRendersPerFrame` (16). Stats appended to `BongThumbLive.Status` ("sceneRenders: ...").
  - FillFrames stops when `!CanSceneRender()`; pool Draw shows the nearest cached frame instead of rendering when over budget.
  - Selection load: `GetAsset()` (sync, 17 s FlushAsyncLoading) → `LoadPackageAsync`.
  - Anim readiness: `IsCompressedDataValid()` instead of `HasCompressedDataForPlatform` (that one could block); poll every 0.1 s.
  - GC: incremental (`ForceGarbageCollection(false)`) unless memory pressure forces a full purge.
  - Folder click: `OnContentBrowserPathChanged` opens a 3 s fast-scan window (tile scan every frame, requeue check 0.1 s, probe reset).
  - Preview-mesh resolver (Renderer.cpp): candidates = anim preview mesh, skeleton preview mesh, then every mesh whose Skeleton tag is this skeleton or a compatible one, plus same-folder meshes passing `IsCompatibleForEditor`. Waits on compiling meshes instead of blocking. Failures back off 2/4/8 s; after 4 it logs the reasons once, shows the stock thumbnail, and re-checks every 30 s (success clears gave-up).

## Findings so far
- **Insights trace (decisive):** the remaining ~100 ms frames during generation are the game thread waiting on the render thread (`Frame Sync Time → GameThreadWaitForTask`). v2.1 issued ~58 scene renders per frame (466 `SceneRenderBuilder_Render` in 0.84 s, ~1.7 ms render-thread time each) + UpdatePrimitive/CacheMeshDrawCommands/UnlockStagingBuffer. The game-thread FrameBudgetMs could not see this. `GRenderThreadTime` reads 0 here, hence the own measurement.
- **Main hitch cause in run4/5 (high confidence):**
  - `OnContentBrowserSelectionChanged` calls `FAssetData::GetAsset()` **synchronously** for up to 64 selected unloaded assets (setting bLoadAnimsOnSelection, default true).
  - Each sync load pulls the anim, its skeleton, the preview mesh, materials and textures.
  - The log shows a single 17 s frame (`FlushAsyncLoading(362)`) right after the test selected 100 assets (sync_browser_to_objects). It happens in both run4 and run5.
- Secondary low-fps period (18-32 fps for ~15 s during generation) has these possible causes:
  - Full-purge GC: `GEngine->ForceGarbageCollection(true)` every 15 s in BongOwned::TickGC.
  - Async-load PostLoad on the game thread.
  - The first frame per anim in FillFrames always renders, even past the deadline (`Done > 0` check). Mesh setup + 9-sample stable bounds + RecreateRenderState are expensive.
  - Uncached pool Draw path renders without a budget check when NumValid==0.
- Folder-click delay suspects:
  - Uncached anims are only queued after the pool reports the tile (OnPoolThumbnail), or by the 2 s requeue check in TickCache.
  - AutoLoadBatchSize = 4.
  - Disk loads: NextDiskCheck and MaxConcurrentLoads.
  - SAssetView rescan every 2 s (tiles every 0.2 s).
- Gave-up tile: `/Game/SM2_Work/Tests/sani_before/hologram_planet/localbake/hologram_planet__anim` (skeleton hologram_planet_Skeleton). The mesh `hologram_planet` exists and is loaded, so the mesh's skeleton is probably not the same or compatible. Needs checking.

## In progress (exact step, 2026-10-30 02:25 KST)
Status: fix done and verified (after5); remaining = final report to the user. No editor running.

Task (owner msg pt313u + steering): in /Game/SM2_Work (95 items) about 25 anim tiles stay on the stock running-man icon forever, and tiles scrolled into view do not load/animate until hovered. Fix generically in the plugin. **Do NOT modify any assets** (SM2 asset work belongs to another bot; only report exact paths + short English note).
- **Root cause (both bugs, verified in UE 5.7 AssetThumbnail.cpp):** SAssetThumbnail only drops the class icon after the pool's OnThumbnailRendered for that asset. For unloaded anims with no saved package thumbnail (recently bot-generated __ADD / __ADD_Manny / TitusWalkFire `titus_assault__tacticus_wpn1_*` anims) the pool render fails (poolFailures=50), so our cache pushes frames into a tile the widget never shows; a tile texture created by a failed render has no RHI resource (black). Hover → SetRealTime → pool render, which is why hover "fixed" it.
- **Fixes (source in repo, built as pkg16, deployed):**
  - Module: `AnnounceNeeded` / `PrevVisibleAnims` / `VisibleSince` (+latency stats). Newly visible anim tiles (and a 1 s sweep of all visible) whose frame cache has frames get `Pool->OnThumbnailRendered().Broadcast(AssetData)` (guarded by `bAnnouncing` so our own OnPoolThumbnail ignores it). Pool failures for anims also queue an announce. Status line shows `tilesAnnounced`, `announcePending`, `newTileToShown(n avg max waiting)`.
  - Renderer: `CanVisualizeAsset` returns true for the anim CDO when frame cache is on (so ShouldRender() is true for unloaded anims); `Draw` returns early for a CDO.
  - Cache: Push creates the tile RHI texture (`BongThumbLive.TileTexture`) when the tile has none (counter `tileTexturesCreated`).
  - Cache (pkg16, **the actual fix for black/icon tiles**): `FAssetThumbnail::GetViewportRenderTargetTexture()` returns null while the pool texture has no GPU resource, so Push silently skipped exactly the failed tiles (that is why `tileTexturesCreated` stayed 0). `FAssetThumbnailPool::AccessTexture` is not exported (link error), so instead Push now asks the pool to render such a tile again: asset loaded → `Pool->RefreshThumbnail(Tile.Thumb)` (max 1/s per tile); not loaded → one read-only `LoadPackageAsync` (the pool re-renders tiles of freshly loaded assets; our renderer then serves cached frames). Stats: `tileRefreshes`, `tileLoads`.
  - Module (pkg14): asset-view scan also walks child windows (floating Content Browser tabs are child SWindows). (pkg15): `CaptureEditorWindow` captures the window that hosts the Content Browser.
- **Results:** before (pkg11) 25 stuck icons; pkg12 → 2-4 icons (only the 5 gave-up hologram copies), newTileToShown avg 0.13-0.19 s max 0.40 s, but announced tiles were black → RHI-texture fix. pkg15 run `after4` (02:10-02:13 KST): visibleViews=1, tiles 91, playing 86, gaveUp=5, newTileToShown n=150 avg 1.35 s max 3.08 s, tileTexturesCreated=0. after4 screenshots: ~12 tiles still BLACK (the __ADD / wpn1 ones). **pkg16 run `after5` (02:18-02:21 KST): no black tiles, only the hologram_planet__anim_bak1..5 stock icons left (2 in the top view, 4 at the bottom); tileLoads=27, tileRefreshes=0; scrolled-in tiles all show a frame in the 0.5 s shot and animate between the 0.5/1.5/3/8 s shots without hover; status metric newTileToShown n=130 avg 1.01 s max 3.07 s (5 waiting = gave-up hologram copies).** Box shots: /workspace/bongthumb/sm2/after5/, summary PNGs in /workspace/bongthumb/sm2_*.png.
- **SM2 asset issues (report only, do not touch):** hologram_planet_anim_bak1..5 copies under /Game/SM2_Work/Tests/sani_before/hologram_planet/ use a skeleton with no compatible mesh → plugin gives up (stock icon, intended). Diag dumps: PC `%TEMP%\btl_sm2_diag_<tag>.json` (not yet analysed).
- **Editor layout:** the Content Browser is in a separate maximized floating window in `%LOCALAPPDATA%\UnrealEngine\5.7\Saved\Config\WindowsEditor\EditorLayout.ini` (written 02:01 KST by a test run). Not changed by hand; no backup exists for that file.
- **Third-party crash on editor exit:** "Pure virtual function being called" in McpAutomationBridge (`FMcpBridgeWebSocket::Close` from `UMcpAutomationBridgeSubsystem::Deinitialize`), not BongThumbLive. The crash reporter was closed; no editor is running.
- **Backups made this task:** source `BongThumbLiveModule.cpp.bak4` (pre-task), `.bak5` (pkg13), `.bak6` (pkg14); `BongThumbLiveModule.h.bak4`; `BongThumbLiveRenderer.cpp.bak6`; `BongThumbLiveCache.cpp.bak2`. `BongThumbLiveModule.cpp.bak6` (pkg14), `BongThumbLiveCache.cpp.bak3` (pkg15), `BongThumbLiveCache.h.bak1` (pre-pkg16). Binaries (dll/pdb/modules) `.bak9`=pkg11, `.bak10`=pkg12, `.bak11`=pkg13, `.bak12`=pkg14, `.bak13`=pkg15. **pkg16 deployed.** Builds in `%TEMP%\BongThumbLive_pkg12..16`.
- Test tools: `Tools/btl_sm2_test.py` (read-only editor script; tag in `%TEMP%\btl_sm2_tag.txt`), `Tools/btl_sm2_run.ps1 -Tag X` (launches editor with -AutoDeclinePackageRecovery, waits for exit, copies log to `%TEMP%\btl_sm2_X.log`).

## Earlier state (v2.2 verification, 2026-10-29)
- v2.2 source committed; **pkg11 binaries deployed** in the plugin. Binary backups in Binaries\Win64: .bak3 = v2.1, .bak4 = pkg6, .bak5 = pkg7, .bak6/.bak7 = pkg8, .bak8 = pkg9. Source backups next to each .cpp/.h (.bak1/.bak3 = v2.1; Renderer .bak4/.bak5 = intermediate v2.2).
- Verification done 21:35-21:46 KST (script `Tools/btl_verify_v22.py`, `Tools/btl_holo.py`):
  - **Disk-cache invalidation works:** A_bow_01 (Taunts01) metadata tag added → dirty → live re-render; after save the old `A_bow_01_ee45….btlc` was deleted and `A_bow_01_ca1a….btlc` written; tag removed + saved again → `A_bow_01_5a01….btlc`. Asset content back to the original (only the file timestamp changed).
  - **hologram_planet:** real cause found. The anim uses `/Game/SM2_Work/Tests/sani_before/hologram_planet/localbake/hologram_planet_Skeleton`, but the mesh in the same folder uses `/Game/SM2_Work/Props/hologram_planet/hologram_planet_Skeleton` (a different skeleton with the same name, not marked compatible). No mesh can legally play this anim, so the tile shows the stock thumbnail (clear static fallback) and the log names the exact mismatch. Fix on the user side: re-target the anim to the Props skeleton or add the skeletons as compatible (not done: user asset).
  - **hologram_planet assets: DO NOT TOUCH.** Baegovda said (22:44 KST) this is SM2 asset work handed to another bot. Facts (read-only diag `Tools/btl_skel_diag.py`): anim `/Game/SM2_Work/Tests/sani_before/hologram_planet/localbake/hologram_planet__anim` uses skeleton `.../localbake/hologram_planet_Skeleton`; meshes `.../localbake/hologram_planet` and `/Game/SM2_Work/Props/hologram_planet/hologram_planet` use `/Game/SM2_Work/Props/hologram_planet/hologram_planet_Skeleton`. All 78 anim tracks exist on the mesh (79 bones; mesh-only bone `hologram_planet`), so marking the skeletons compatible would make it play. A test of exactly that (22:43) worked (gaveUp=0) but was **reverted** at 22:46 from `.bak1` (SHA-256 verified identical to the originals; the `.bak1` files stay next to the two skeleton .uasset files). The plugin shows the stock thumbnail and logs the mismatch, which is the intended behaviour.
  - Project scan (39,272 anims, 2,557 skeletal meshes, 954 skeletons): only hologram_planet has this split-skeleton pattern. Also without any mesh: `/Game/Characters/Mesh/SK_Sci_Fi_Cyborg_03_Skeleton` (12 anims, no mesh and no same-name skeleton anywhere; stock thumbnail) and the engine tutorial `TutorialTPP_Skeleton` (2 anims).
  - Windows Firewall prompt for Unreal Insights: gone at 22:40 KST (no window, Insights not running, no firewall rule added).
  - **Settings tooltip:** the on-screen hover could not be captured. Slate shows tooltips only in the foreground app, and Windows refused to bring the editor forward from a background process. The row hover highlight is visible in `btl_shots\v22_tooltip_diskcache*.png`. Needs a manual check by Baegovda.

## Next steps
1. Manual tooltip check by Baegovda (hover a setting on the BongThumbLive settings page).
2. Optional: cold-folder v2.2 run on anim folders never opened before (DDC cold), to compare with base1.
3. Report v2.2 readiness (no release).

## Measurements
All via `btl_run.ps1`, same config (Taunts01 gen 60 s, Minigun gen 60 s, disk re-click, select 50 anims in Taunts02). "Hitches" = frames > 50 ms.

| run | build | DDC | Taunts click→first playing | Taunts hitches (max) | Minigun hitches (max) | Selection of 50 | s with fps<50 | editor mem |
|---|---|---|---|---|---|---|---|---|
| base1 | v2.1 | cold | 2.05 s | 69 (1067 ms) | 60 (2134 ms) | 97 hitches, max 738 ms, ~9 fps ~10 s | 30/238 | ~6.7 GB |
| base2 | v2.1 | warm | 0.41 s | 68 (141 ms) | 22 (126 ms) | 96 hitches, max 222 ms, ~8.5 fps 6 s | 19/242 | 6.5 GB |
| after3 | v2.2 pkg8 | warm | 0.41 s | 1 (58 ms) | 4 (60 ms) | 1 hitch, 101 ms, first playing 0.52 s | 2/243 | 6.9 GB |

- Disk re-click → first playing: base2 0.21 s, after3 0.12 s. Startup frame ~705 ms in all runs (editor, not plugin).
- Cache throughput unchanged: disk writes 79 (base2) vs 77 (after3) in the same time; render-thread cost 1.77 ms/render, allowance ~3 renders/frame.
- Honest note on folder click: with a warm DDC the click → first playing is ~0.4 s in both versions; the 2-4 s in base1 is the engine's DDC/anim compression + async package loading (engine side). Our part (scan/requeue latency) was cut by the fast-scan window; the big win of v2.2 is smoothness (no frame drops) during generation and selection.

## Known risks
- The git clone lives in %TEMP% (could be cleaned by Windows). Re-clone with gh if it is missing.
- The plugin folder is not under git; the source of truth for edits is the plugin folder.

## Bot handover log
- 2026-10-30 02:40 KST: new bot account took over (PC Revaptor-FX registered, machine id d3fb99ad-25ca-45c1-81af-41e5ca09104a). Resume checklist verified: clone clean and in sync with origin/main (d311a2d), plugin Source identical to clone, pkg16 binaries deployed (02:18), no editor or crash reporter running, gh logged in as Baegovda. Remaining: final report of the SM2 tile fix, tooltip manual check, optional cold-DDC run.
