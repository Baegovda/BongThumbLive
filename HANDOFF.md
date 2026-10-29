# BongThumbLive – HANDOFF (keep this file current)

Last update: 2026-10-29 20:55 KST (Asia/Seoul).

## What this is
BongThumbLive is a UE 5.7 **editor-only** plugin (Win64) that makes Content Browser thumbnails move: animation sequences, montages and composites loop on their preview mesh, materials animate, and Niagara and Cascade effects simulate. Animation frames are cached in GPU atlases and on disk.

## Talking to the user
- User = Lalamold (Korean). Replies to the user: **short Korean, 해요체**, beginner-friendly.
- User quotes so far (Korean): "플러그인 작업은 마저 재개해줘"

## Access (for a new bot that only has the repo URL)
- The repo is **private**: https://github.com/Baegovda/BongThumbLive. To read it from the box, use the GitHub connector/tools (cursor-github), or `gh` on the user's PC. The PC's `gh` and git are already authenticated as the user (Revaptor_FX / revaptor@gmail.com) and can push.
- All real work happens on the user's Windows PC. Use Shell with `machineId = d3fb99ad-25ca-45c1-81af-41e5ca09104a` (PowerShell), and use CopyToBox / CopyFromBox to move files.
- If the clone at `C:\Users\Revaptor_FX\AppData\Local\Temp\BongThumbLive_repo` is missing, run `gh repo clone Baegovda/BongThumbLive <dir>` on the PC.

## Resume checklist (run first)
1. `git -C C:\Users\Revaptor_FX\AppData\Local\Temp\BongThumbLive_repo pull --ff-only`, then read this file from the clone (newest copy).
2. Check the editor state: `Get-Process UnrealEditor*,CrashReportClient*`. If a crash reporter is open, close it and read `K:\Unreal Projects\UE_VFX_57\Saved\Crashes\<newest>`.
3. Compare the plugin source with the clone: `git diff --no-index` between `K:\Unreal Projects\UE_VFX_57\Plugins\BongThumbLive\Source` and `<clone>\Source`. Uncommitted plugin edits = work in progress described below.
4. Check the newest backups (`*.bakN`) next to the changed files, and Binaries\Win64 timestamps vs. the source, to know whether the last build was deployed.
5. Read the "In progress" section and continue from the exact step. After each step, update this file, commit and push (ff-only), and mirror it to /workspace/bongthumb/HANDOFF.md.
6. Launch the editor (pre-authorized): `"C:\Program Files\Epic Games\UE_5.7\Engine\Binaries\Win64\UnrealEditor.exe" "K:\Unreal Projects\UE_VFX_57\UE_VFX_57.uproject" -AutoDeclinePackageRecovery [-ExecCmds="py <script>"]`. The log is at `K:\Unreal Projects\UE_VFX_57\Saved\Logs\UE_VFX_57.log`.

## Paths
- User PC: machine id `d3fb99ad-25ca-45c1-81af-41e5ca09104a` (Windows, 128 GB RAM, 24C/32T). Run commands with Shell machineId (PowerShell).
- Plugin (live, used by editor): `K:\Unreal Projects\UE_VFX_57\Plugins\BongThumbLive`. This folder is NOT a git repo.
- Git clone used for commits: `C:\Users\Revaptor_FX\AppData\Local\Temp\BongThumbLive_repo` (origin https://github.com/Baegovda/BongThumbLive.git, **private**). Workflow: edit the plugin source, then copy the changed files into the clone, then commit and push (fast-forward only).
- Project: `K:\Unreal Projects\UE_VFX_57\UE_VFX_57.uproject`; engine `C:\Program Files\Epic Games\UE_5.7`.
- Disk cache: `K:\Unreal Projects\UE_VFX_57\Saved\BongThumbLive\Cache`.
- Full plugin backups: `K:\Unreal Projects\UE_VFX_57\PluginBackups\BongThumbLive.bak1..bak7`.
- Test/diag scripts: `C:\Users\Revaptor_FX\AppData\Local\Temp\btl_*.py|ps1`.
  - `btl_verify3.py` = timed run: Status dumps, screenshots, settings page; RUN number in `btl_run.txt`.
  - `btl_watchdog.ps1` = memory watchdog that kills the editor above a limit.
  - `btl_rep.ps1` = safe exact-string replace helper.
- Box (agent) notes: `/workspace/bongthumb/` (README.md, run logs, screenshots, HANDOFF mirror).

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
- Screenshots go to the box under /workspace/bongthumb/.
- This chat may be cut off at any time. Update this file after every meaningful step, then commit and push it, and mirror it to /workspace/bongthumb/HANDOFF.md.

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

## Findings so far
- **Main hitch cause (high confidence):**
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

## In progress (exact step)
- Step: writing HANDOFF, then building a measurement harness (real wall-clock frame times via perf_counter, hitch list, folder-click → first-live-tile timing), then code changes.
- Files touched so far: none in the plugin yet (only HANDOFF.md / NEW_BOT_PROMPT.md in the repo).
- Source snapshot for reading on the box: /workspace/bongthumb/work/Source.

## Next steps
1. Baseline measurement with the current v2.1 binary. Clear the disk cache for test folders with `BongThumbLive.ClearDiskCache`. Folder switch via `unreal.EditorUtilityLibrary.sync_browser_to_folders`.
2. Fixes:
   - selection load → async queue;
   - GC non-full / incremental;
   - FillFrames hard deadline incl. first frame, split setup across frames;
   - budget check for uncached pool Draws;
   - folder/path-changed kick + immediate visible enqueue + disk prefetch for folder assets;
   - higher in-flight anim loads within the 12 cap;
   - fallback mesh search over all compatible meshes/skeletons + clear static fallback.
3. Build, back up as .bakN, relaunch, measure after, screenshots, commit, push, update HANDOFF.

## Known risks
- The git clone lives in %TEMP% (could be cleaned by Windows). Re-clone with gh if it is missing.
- The plugin folder is not under git; the source of truth for edits is the plugin folder.
