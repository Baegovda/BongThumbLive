# BongThumbLive – HANDOFF (keep this file current)

Last update: 2026-11-16 KST (Asia/Seoul).

## Current environment (2026-11-16) — READ FIRST
The PC moved from UE 5.7 to **UE 5.8**. Every `UE_5.7` / `UE_VFX_57` path in the History sections below is **stale** (those folders no longer exist). Use these instead:
- Engine: `K:\Epic Games\UE_5.8` (editor `Engine\Binaries\Win64\UnrealEditor.exe`, RunUAT `Engine\Build\BatchFiles\RunUAT.bat`).
- **Live plugin is installed in the engine**, not in a project: `K:\Epic Games\UE_5.8\Engine\Plugins\Marketplace\BongThumbLive` (`.uplugin` has `EngineVersion 5.8.0`, `Installed true`). Binaries built 2026-11-15 23:31. On 2026-11-16 its `Source` matched repo `main` (9a69106) except line endings. No `.bakN` files exist there yet. This folder is NOT a git repo.
- Test project: `K:\Unreal Projects\UE_VFX_58\UE_VFX_58.uproject` (enables BongThumbLive; the project has no `Plugins\BongThumbLive`). Disk cache `K:\Unreal Projects\UE_VFX_58\Saved\BongThumbLive\Cache`. Log `K:\Unreal Projects\UE_VFX_58\Saved\Logs\UE_VFX_58.log`.
- Git clone used for commits: `C:\workspace\BongThumbLive` (origin https://github.com/Baegovda/BongThumbLive.git, public). The old `%TEMP%\BongThumbLive_repo` and the Codex `Documents\Codex\...` checkout are gone.
- Repo moved to UE 5.8 on 2026-11-16 (user approved): `.uplugin` `EngineVersion 5.8.0`, `Tools/` paths point at UE 5.8 / UE_VFX_58, the one 5.8 deprecation (`GetObjectsWithPackage` bool overload, Cache.cpp) uses `EGetObjectsFlags`. Clean build: `%TEMP%\BongThumbLive_pkg58b`, 0 warnings, log `%TEMP%\btl_pkg58b_build.log`. **Deployed 2026-11-16 12:5x KST** into the engine copy (dll/pdb/modules + Cache.cpp; previous files kept as `.bak1` = 2026-11-15 build; SHA-256 equal to pkg58b). The user's UE 5.8 editor was closed with CloseMainWindow (clean exit) first. Smoke: UE_VFX_58 launched with `-ExecCmds="BongThumbLive.Status,QUIT_EDITOR"`: DLL loaded from the engine path, renderers/pool hook/toolbar registered, disk cache ready (14 files), Status OK, clean `LogExit`, no BongThumbLive errors (log `%TEMP%\btl_smoke58.log`). No Content Browser was visible in that run, so no tile was rendered.
- Settings are engine-wide since 2026-11-15 (`config=EditorSettings`, PR #1 / c68b614); old per-project values are imported once. The frame cache stays per project.
- PC tools (checked 2026-11-16): gh logged in as Baegovda, Python 3.10 `C:\Program Files\Python310\python.exe`, Node v26.13.0.
- Agent: Claude Code (desktop app) runs directly on the PC. No box, CopyToBox or machine id is needed.
- `Tools/*.ps1|py` hard-code the UE 5.8 / UE_VFX_58 paths (updated 2026-11-16, not yet run on 5.8).

## UE 5.8 regression run + Content Browser fix (2026-11-16)
- **Real 5.8 bug found and fixed:** UE 5.8 wraps parts of the Content Browser (incl. the asset view) in `SInvalidationPanel`, whose `GetChildren()` is empty while it caches. All plugin widget walks used `GetChildren()`, so on 5.8 the plugin found **no asset view** (`visibleViews=0`) and live tiles could not start. All walks now use `GetAllChildren()` (Module.cpp). Verified in the test project: `visibleViews=1`, `tiles on screen=96 live=96`. Not yet checked by eye in UE_VFX_58.
- New debug/test console commands: `BongThumbLive.OpenBrowserTab` (dock Content Browser tab 1; fresh 5.8 layouts only have the Content Drawer), `BongThumbLive.ClearBrowserFilters` (disable type filters, which hid all fixture tiles once), `BongThumbLive.DumpBrowserWidgets` (widget types under SContentBrowser, max 6000 lines). Build.cs now also depends on `EditorWidgets`, `ToolWidgets` (for `SFilterList`).
- SpinnerClip test counts a tile cut by the top or the bottom edge as partial (5.8 row layout put the bottom cut into the label). The runner `Tools/Tests/run_editor_regressions.py` opens the tab, clears filters, scrolls again at +18 s and logs Status before the tests.
- **Result: 7/7 automation tests passed** on UE 5.8 (96 real tile clips, 12 partial). Report `%TEMP%\btl_auto58g\index.json`, log `%TEMP%\btl_auto58g.log`, shot `%TEMP%\btl_shots\cb58c.png`.
- Disposable test project (deleted 2026-11-16; recreate with `Tools/btl_make_testproj.ps1`): `C:\workspace\BTLTest58` (marker `BTL_DISPOSABLE_TEST_PROJECT`, copy of `/Game/Props/PrecisionSniperRifle` from UE_VFX_58). Launch: `"C:\workspace\BTLTest58\BTLTest58.uproject" -d3d11 -unattended -AutoDeclinePackageRecovery -ExecCmds="py C:/workspace/BongThumbLive/Tools/Tests/run_editor_regressions.py" -ReportExportPath=<dir> -TestExit="Automation Test Queue Empty" -ini:EditorSettings:[/Script/UnrealEd.EditorPerformanceSettings]:bThrottleCPUWhenNotForeground=False`.
- Deployed into the engine copy: pkg58i (`%TEMP%\BongThumbLive_pkg58i`). Engine-copy backups: binaries `.bak1` = 2026-11-15 build, `.bak2` = pkg58b; `Private\BongThumbLiveModule.cpp.bak1`, `Private\BongThumbLiveCache.cpp.bak1`, `BongThumbLive.Build.cs.bak1` = 2026-11-15 source.
- Engine-wide config backups before the tests: `%TEMP%\btl_EditorSettings_before_test58.ini`, `%TEMP%\btl_EditorLayout_before_test58.ini`. Afterwards only the recent-projects list differed (left as is; another worker's GravityCheck project also writes it).
- Another worker runs `UnrealEditor-Cmd` on `K:\SM2_Backups\Gravity09Isolated\GravityCheck.uproject` (ForgeLens). It does not load BongThumbLive; do not touch it. Before deploying, check that no running editor has the plugin dll loaded.

## v2.3.5: skip idle gaps of one-shot effects (2026-11-18 ~06:10 KST)
- `Simulate` restarts a Niagara effect after 0.5 s with zero particles (sum of `GetNumParticles` over non-complete emitters) once the loop has shown particles (`bHadParticlesThisLoop`); CVar `BongThumbLive.FXSkipIdle` (1), Status `idleRestarts=`. Survey (UE_VFX_58): DeathEffects_Vol2 Skull/Lightning/Black tiles that were empty at capture now show more often; no regressions in Environment/Base/Slashes; 0 asserts.
- Tried and removed: framing the middle of a travelling effect's path at 1.6x its biggest sample (MagicalSlashes blades are long thin streaks and did not grow; NS_Fire_Torch lost its flame to the frame edge). Union framing kept. DeathEffects_Vol2/StaticMesh effects mostly dissolve a mesh given by a user parameter, so many draw little in a thumbnail scene.
- Suite 8/8. Released v2.3.5, deployed to the engine copy.

## v2.3.4: Substrate crash fix (2026-11-18 ~05:40 KST)
- **Crash:** opening /Game/VFX/Base/Particle (258 Niagara systems) in UE_VFX_58 with live effect tiles crashed the editor: `Assertion failed: CurrentMaxBytesPerPixel <= PlatformSettingsBytesPerPixel` (Substrate.cpp:566, "(28) exceeds (20)"). Project: `r.Substrate=True`, `r.Substrate.ProjectGBufferFormat=1`, `r.Substrate.BytesPerPixel=256`; the platform budget the renderer uses is 20. Saved Niagara thumbnails never drew these materials, so the owner never hit it; v2.3.0+ live tiles did. FXTrace showed every used material at <= 20 bytes (shader maps still compiling); the last render before the assert was NS_BeamEnergyFragments; the 28-byte source was not identified.
- **Fix:** `FSafeSubstrateScope` sets `r.Substrate.AllocationMode 2` (buffer sized from the platform budget, the check cannot fire; oversized materials are clamped like non-editor builds) around the plugin's FX renders (tile render and calibration) and restores it right after; the cvar's render-thread copy updates in command order. CVar `BongThumbLive.FXSafeSubstrate` (1). Verified: same folder, 0 asserts, 27 live scenes framed. Also kept: per-material Substrate budget check (`substrateSkips`), `BongThumbLive.FXWaitForShaders` (default 0: on-demand shaders never report complete, so waiting made nothing live), `BongThumbLive.FXTrace`.
- Interrupted session note: when a test run is cut off, the project plugin copy and the four per-user files are not restored. Check `K:\Unreal Projects\UE_VFX_58\Plugins\BongThumbLive` and restore from the newest %TEMP%\btl_bg_backup_* (files 0_..3_) before anything else.
- Suite 8/8. Released v2.3.4 (tag 6dfc5c7), deployed to the engine copy (binaries .bak9 = v2.3.3 build).

## v2.3.3 (2026-11-18 ~03:30 KST)
- Survey of 7 more effect folders in UE_VFX_58 (Base/Particle, MagicalSlashes, DeathEffects_Vol2, Sandstorm, Signal_Flares, TransformationVFX, DashEffects; shots were in %TEMP%tl_shotsxsurvey*). Found: (1) 2.3.2's particle-position fit made Sandstorm NS_DustParticles_01-08 black (thousands of soft sprites, nearest-particle window between them). Fix: validate the particle window with one render, require >= 0.3% of the 256 px view lit, else keep the pixel fit (`particleRejects=`). (2) Big folders starved: the first-drawn tiles took all samples. Fix: separate per-frame budgets, 3 for effects with < 3 samples, 1 for refinement.
- First frame: `CreatedFrame`; the creation-frame draw uses the stock renderer (no black tile) and skips calibration.
- Rain/dust diagnosis: NS_Hard_Rain/Soft_Rain/Rain_Drop/Dust_Grains/Volume_Fog are CPU emitters with thin low-opacity streaks / tiny grains / faint fog; they are faint by nature (max brightness 95-156, fog 13). Left as is.
- Remaining observations: MagicalSlashes blades travel across the view, so the union of their path stays wide (small blade). One-shot DeathEffects vary by capture moment. VFX/Base/Particle tiles mostly showed saved thumbnails in the 40 s window (234 items).
- Suite 8/8 (BTL_Niagara 529 lit, PS_MuzzleSmoke 306).

## v2.3.2: leaves via particle positions (2026-11-18 ~02:50 KST)
- When the pixel calibration finds nothing (< 12 lit) or a scattered effect (biggest blob < 25%), `CalibrateFraming` uses CPU particle positions (`FNiagaraDataSetAccessor<FNiagaraPosition>` on each CPU emitter's `GetParticleData()`, after `WaitForConcurrentTickDoNotFinalize`; local-space emitters transformed by the component): window around the per-axis median, half-size = Chebyshev distance of the K-th nearest particle (K = n/8 clamped 8..40) x1.15. Once an effect uses this (`bParticleFit`), later samples skip the render. Status: `particleFits=`.
- Verified in UE_VFX_58 (Environment_VFX_Pack): Daisy/Dead/Green/Judas/Sakura leaves now show leaves in their tiles; clouds, snow, fire torch, magic sparks/torch, lightning unchanged. Rain/Dust grains stay faint (GPU or very small). Suite 8/8.

## v2.3.1 (2026-11-18 ~02:00 KST)
- `CoveredRect` is blob-based: lit pixels (>32) are flood-filled into blobs (neighbours within 3 px); the biggest blobs are taken until 80% of lit pixels are inside. If the biggest blob has < 25% of the lit pixels (scattered: snow, rain, leaves), the 25th..75th percentile per axis is framed instead. Result in UE_VFX_58: NS_Fire_Torch (sparks fall 25 m) now shows its flame filling the tile; Magic_Sparks/Magic_Torch/lightning large; clouds/snow/slashes unchanged (good).
- Falling-leaf effects (Daisy/Dead/Green/Judas Leaves): 210 CPU particles each, spread ~15 x 24 m, small and dark; tiles still look empty in the Content Browser (calibration often finds < 12 lit pixels, visibility varied between runs). Not solved; FXLightBoost 3 made no clear difference. Next idea if wanted: frame on particle positions (CPU emitters) instead of pixels.
- New: `BongThumbLive.FXDebug` prints per-emitter sim target/state/particle count; CVar `BongThumbLive.FXLightBoost` (default 1).
- Suite 8/8 (BTL_Niagara 572 lit, PS_MuzzleSmoke 51). Released v2.3.1, deployed to the engine copy.
- Cleanup 2026-11-18: deleted 24 temp packages (1.9 GB; kept %TEMP%\BongThumbLive_pkg231 = v2.3.1), old release staging/launcher backups, FX debug shots, the FX folders copied into BTLTest58 (168 MB). Engine-copy backups now: .bak1 = 2026-11-15 build/source, binaries .bak8 = v2.2 release build, uplugin .bak5 = 2.3.0. The v2.2.1-v2.3.0 binary backups were removed by the cleanup; to roll back to those, use the GitHub release zips (every version is attached there).

## v2.3.0: Niagara/Cascade framing + 5.8 live-effect fixes (2026-11-18 ~01:20 KST)
- Owner request (2026-11-17): Niagara thumbnails framed to fill the square. Released as v2.3.0, deployed to the engine copy (binaries .bak5 = v2.2.2 build; engine .uplugin .bak4 = 2.2.2).
- **Auto-framing** (FXRenderer `CalibrateFraming`): the effect is rendered alone (`ShowOnlyPrimitives`) into a 256 px target; covered pixels (max channel > 32, 1st..99th percentile per axis, >= 12 lit) give a rectangle in the camera right/up plane; the union over up to 40 samples (0.1 s of sim time apart, at most 2 samples per engine frame across all effects) fixes the camera (fill 0.85). First sample: wide bounds camera then a refined zoom; later samples: one render around the current fit (wide fallback if cut). CVars `BongThumbLive.FXAutoFrame` (1), `FXAutoFrameFill` (0.85). Status line: `fxFraming: samples renders empty avg max niagaraCompilePolls directAdvances`. Measured 3-5 ms avg per sample (max ~10 ms, one 60 ms outlier).
- **UE 5.8 live-effect bugs found and fixed** (live FX tiles were empty/static on 5.8 before this):
  1. Niagara compile requests are deferred until `PollForCompilationComplete(true)`; only component activation flushes, so systems never opened stayed static. `IsReadyToSimulate` now flushes (throttled 0.5 s per system).
  2. Preview-world ticks barely advance Niagara (0.8 s system age after 9.7 s). `Simulate` calls `AdvanceSimulation(1, Step)` when the world tick did not advance it (adaptive).
  3. The 5.8 thumbnail sky sphere is the checker backdrop: FX scenes are created with `SetCreateSkySphere(false)` and the tile gets alpha forced to 1 (`DrawClearQuadAlpha`, CVar `BongThumbLive.FXOpaqueBackground`).
  4. `World->SendAllEndOfFrameUpdates()` before each FX draw. A scene still renders nothing in the engine frame that creates it (harmless in the Content Browser); the regression runner warms FX fixtures on earlier frames.
- Regression: `Rendering.AssetTypes` now requires visible particles (max lit >= 20 of 64x64 over 45 draws) and an opaque tile; optional real Cascade fixture PS_MuzzleSmoke. Suite 8/8 (BTL_Niagara 139 lit, PS_MuzzleSmoke 95). BTL_Cascade from the 5.8 factory has no emitters (visibility check skipped).
- Verified in UE_VFX_58 (project-plugin copy, removed afterwards): Fireball, clouds, snow, stylized slashes fill their tiles (`Docs/fx_framing_ue58.png`). Known limits: effects with far-flung sparks (NS_Fire_Torch) stay small; leaf effects (Daisy/Dead/Green leaves) draw nothing in thumbnails both before and after (not investigated).
- New tools: `Tools/btl_with_project_plugin.ps1` (test a package as a project plugin, refuses if an editor runs), `Tools/btl_fx_frame_test.py` (CB before/after shots), console commands `BongThumbLive.DumpThumb <asset> <png> [draws] [size]`, `BongThumbLive.FXDebug`, Status skip reasons `[unsafe shaders asset noRenderer]`. Launcher un-maximises before moving.

## v2.2.2 released (2026-11-16 ~20:10 KST)
- Tag v2.2.2 = 2a91e0f, https://github.com/Baegovda/BongThumbLive/releases/tag/v2.2.2 (zip from %TEMP%\BongThumbLive_pkg222).
- **MaxLoadedAnims** (new cvar `BongThumbLive.MaxLoadedAnims`, was a hard-coded 12): default 24. Cold A/B in the side-monitor test project, 113-anim folder, 2 runs each, avg / max new-tile-to-shown: 12: 1.65, 1.46 / 5.3, 4.3 s; 24: 1.32, 1.38 / 3.8, 4.2 s; 32: 1.25, 1.29 / 3.8, 4.0 s. Peak private memory 1.9-2.0 GB for all (launcher now prints peakPrivateGB). Default-build run: 1.40 / 4.5 s, ownedAnimRoots max 24. Preview-mesh cap 6 unchanged. The owner asked for this (raising the old fixed 12) on 2026-11-16.
- Suite 8/8 on pkg222, run as a **project plugin** copy in BTLTest58 (the owner's own UE 5.8 editor, pid 5808 opened 20:01, held the engine dll; it was not closed). The copy was removed afterwards.
- **Engine deploy done 2026-11-16 21:17 KST** by `Tools/btl_deploy_when_free.ps1` right after the owner closed the editor: engine copy = pkg222 (dll hash verified), .uplugin 2.2.2 (backup .bak3); binaries .bak4 = previous 2.2.1-era build.
- Test editors now open directly on the side monitor: the launcher writes [RootWindow] ScreenPosition/WindowSize/InitiallyMaximized into the project's EditorPerProjectUserSettings.ini for the run (FRootWindowLocation reads it; restored afterwards). The move loop stays as a fallback.
- Live check of 2.2.1 in UE_VFX_58 (side monitor, cold plugin cache): 28 tiles on screen, all live, avg 1.57 s, max 2.73 s, no errors (log %TEMP%tl_live221.log).

## v2.2.1 released (2026-11-16 ~16:00 KST)
- Tag v2.2.1 = a055d19, https://github.com/Baegovda/BongThumbLive/releases/tag/v2.2.1, asset BongThumbLive-v2.2.1-UE5.8-Win64.zip (from %TEMP%\BongThumbLive_pkg221). Content: ReleaseAfterCoarse default 1 (below). Same code as pkg58n (suite 8/8) plus the version bump; deployed to the engine copy (binaries .bak2 = pkg58n, .bak8 = v2.2; uplugin .bak2 = 2.2).

## Cold-load speed, folder-click split, cleanup (2026-11-16 15:00-15:50 KST, after v2.2)
- **ReleaseAfterCoarse default 2 -> 1** (main, after the v2.2 tag; deployed as pkg58n). UE 5.8 cold A/B in the side-monitor test project, 113-anim folder, avg / max new-tile-to-shown: mode 2 4.9-5.3 / ~24 s (4 runs); mode 1 2.00, 1.74, 3.14, 1.64, 1.41 / 4.2-10.4 s (5 runs). Hitches > 100 ms per 145 s: mode 1 1-7, mode 2 2-3. All 113 caches complete at the end in both modes. On 5.7 mode 1 had been rejected for ~150 hitches; on 5.8 it no longer has that cost. Rejected again: `SceneRenderBudgetWaitingMs 12` (3.9 s avg, max ~21 s), `FirstFrameFirst 1` (2.00 / 1.35 s = noise vs mode 1).
- **Folder-click split** (`Tools/btl_trace_split.py` over BTLT traces, cold plugin cache, warm DDC): almost all latency is the plugin's load queue (slot/throughput wait). First screen median: queue 7.0 s (mode 2) -> 1.9-6.0 s (mode 1, median of runs ~2.4 s); engine async load 0.02-0.04 s, anim data ready 0.02-0.03 s, first-frame render 0.04-0.08 s. Warm plugin cache: folder click -> tile shown avg 0.16-0.22 s, max 0.53-0.88 s (3 runs; one run had an unexplained hitch burst while the plugin was idle, not reproduced).
- Cleanup: deleted `%TEMP%\BongThumbLive_pkg58a..l` (3.4 GB), old `%TEMP%\btl_bg_backup_*` (kept 3), engine-copy middle backups (kept `.bak1` = 2026-11-15 state and binaries `.bak8` = v2.2 release build pkg58m), and `C:\workspace\BTLTest58`. Recreate the test project with `Tools/btl_make_testproj.ps1` (copies PrecisionSniperRifle, writes the disposable marker; first launch compiles shaders).
- Launcher note: the editor window is first created at 0,0 on the main monitor and moved within ~0.3 s; it stays at -1920,-1080 until exit (`last main-window rect` line).

## v2.2 released (2026-11-16 15:05 KST)
- Owner said all open questions were mine to decide (on phone, away). Decisions: **released v2.2**; TightFraming stays on; the owner's "Niagara System" type filter on CB tab 1 is left as is (looks intentional for VFX work); test/debug console commands stay in (inert unless typed).
- Release: tag `v2.2` = 1c20a43, https://github.com/Baegovda/BongThumbLive/releases/tag/v2.2, asset `BongThumbLive-v2.2-UE5.8-Win64.zip` (dll + modules + Source + Config + README, no pdb; staged from `%TEMP%\BongThumbLive_pkg58m`). v2.1 stays the UE 5.7 build.
- `.uplugin` Version 4 / VersionName 2.2 (repo and engine copy; engine `.uplugin.bak1` = 2.1).
- New automation test `BongThumbLive.Settings.KoreanTooltips` (34 editable settings have Korean name + tooltip, which the details panel shows on hover). Suite 8/8 on pkg58m (`%TEMP%\btl_auto58m`).
- Deployed: pkg58m (engine binaries `.bak7` = pkg58l).

## Autonomous session (2026-11-16 14:00-14:45 KST, user away)
- Tooltips: all 35 settings have Korean doc-comment tooltips (static check). Hover check still manual.
- Spinner styles verified live on 5.8 (`Tools/btl_spin58_test.py`, `Docs/spinner_styles_ue58.png`): Spin/Pulse/Dots/Bar, colour and speed change at runtime. Dots are real filled dots (commit ce19434 is now verified).
- TightFraming cost: Status line now ends with `framing: tight= skinnedBox calls= avg= max= fallbacks=`. 40k-vertex cap measured 1.12 ms avg / 7.6 ms max per pose (9 poses per anim, game thread); cap lowered to 10k: 0.37 / 2.1 ms. Cold speed A/B (side monitor): on 5.49/5.16 (40k), 5.32/4.76 (10k); off 4.43/6.55 s avg new-tile-to-shown: noise. Logs `%TEMP%\btl_speed_tf*.log`.
- New tools: `Tools/btl_launch_bg.ps1` (side monitor, now also restores EditorSettings.ini), `Tools/btl_speed_bg.ps1` (cold speed run in BTLTest58), `Tools/btl_build_deploy58.ps1` (BuildPlugin with retry while another worker's UBT runs, deploy with .bakN backups; `-NoDeploy`, `-SkipBuild`). `btl_speed_test.py` now opens the CB tab and clears filters.
- Deployed: pkg58l. Engine-copy binaries `.bak4` = pkg58j, `.bak5` = pkg58k (duplicate deploy), `.bak6` = pkg58k. Regression 7/7 on pkg58l (`%TEMP%\btl_auto58k`).
- Readiness report: `Docs/V2.2_READINESS.md` (no release made).
- Other workers build with UE 5.8 on this PC: UBT is single-instance (`ConflictingInstance`); the build script waits and retries.

## Live check in the real UE_VFX_58 (2026-11-16 13:47 KST)
- Tool: `Tools/btl_live58_check.py` (read-only; opens CB tab 1, syncs to `/Game/Props/PrecisionSniperRifle/Character/Animations`, clears type filters, Status + shots at +3/8/15/30 s with pairs 0.4 s apart, quits). Plugin disk cache cleared before the run.
- Result (cold plugin cache): `visibleViews=1`, 63 anim tiles on screen; live 12 at +3 s, 23 at +8 s, 29 at +15 s, 48 at +30 s. newTileToShown avg 12.9 s, max 29.2 s at +30 s (cold). Shot pairs differ by 11-14k px inside the tile grid, so the tiles move. No BongThumbLive errors. Shots `%TEMP%\btl_shots\live58\`, log `%TEMP%\btl_live58c.log`.
- Waiting tiles show the pool's floor/checker render with the yellow spinner (intended).
- **Small framing fixed (same day, pkg58j):** cached tiles showed a tiny rifle in the upper middle. Cause: preview mesh `SK_Arms` (first-person arms) has its root bone at the floor and an A-pose reference 140 cm wide, so component/physics bounds (radius ~78 cm, z 0-165) were far larger than the visible hands+rifle; the engine's own thumbnail has the same problem. Fix (Renderer.cpp `GetSkinnedBox`): framing bounds = box of the CPU-skinned LOD0 vertices from the editor imported model (`CacheRefToLocalMatrices`, at most ~40k vertices), in both the stable (9-sample union) and per-pose paths; falls back to component bounds when no imported model. CVar `BongThumbLive.TightFraming` (1 default, 0 = stock; not in the cache key, clear the cache after changing). Cache `KeyVersion` 2 -> 3 so old framing is regenerated. Live re-check: hands+rifle centred and 2-3x larger (`Docs/framing_ue58_before_after.png`); 50/63 tiles live at +30 s; 7/7 automation tests pass (`%TEMP%\btl_auto58h`). Engine-copy backups: binaries `.bak3` = pkg58i, `Renderer.cpp.bak1`, `Cache.cpp.bak2`.
- **User state:** the user's Content Browser tab 1 has the "Niagara System" type filter on. It is stored engine-wide in `%LOCALAPPDATA%\UnrealEngine\Editor\FilterBar.json` (`ContentBrowserTab1.TypeFilters`). The check backs it up and restores it; it was restored byte-identical. Project `EditorPerProjectUserSettings.ini` (selected path, open NS_Fireball) and engine `EditorLayout.ini` were also restored from `%TEMP%\btl_live58_backup\`.
- A first attempt was invalid: the editor window was minimised during the run (≈3 fps, screenshots failed, `visibleViews=0`). Ask the user not to touch the window during a live check.

## Last finished work: full audit fixes (2026-11-08, UE 5.7 era)
- All nine audit fixes were implemented, tested, deployed to the then-current UE_VFX_57 plugin, and merged to main (PR #1, together with the engine-wide settings change). No release or tag.
- Fixes: common guarded load queue; PID/project-bound watchdog; mesh/skeleton dependency invalidation; job generations/file revisions; byte leases covering worker/GPU lifetimes; legacy config import; sub-1-FPS clip sampling (cache key version 2); bounded header parsing; spinner table/tile clipping.
- Verified then: seven UE automation tests passed (`Source/BongThumbLive/Private/BongThumbLiveRenderTests.cpp`, runner `Tools/Tests/run_editor_regressions.py`, fixtures under `/Game/BTLRegression`); cold/warm cache regressions with 113 files; real project smoke with no errors. UE GetStatData signatures use whole-second UTC ticks; do not compare them to fractional file times. Some assets are still loaded by the editor/pool recovery path; do not claim a universal zero-asset-load startup.
- Windows inspection rule: do not use Computer Use, mouse/keyboard driving, focus, or window repositioning. Read processes and GetWindowRect; use CopyFromScreen/PrintWindow only for screenshots.
- Not re-verified on UE 5.8 yet: automation tests, cold/warm cache, spinner.

## What this is
BongThumbLive is a UE 5.7/5.8 **editor-only** plugin (Win64) that makes Content Browser thumbnails move: animation sequences, montages and composites loop on their preview mesh, materials animate, and Niagara and Cascade effects simulate. Animation frames are cached in GPU atlases and on disk.

## Talking to the user
- Owner/user = **Baegovda** (GitHub name; Korean speaker; Windows PC user account `Revaptor_FX`). Replies to the user: **short Korean, 해요체**, beginner-friendly.
- User quotes so far (Korean): "플러그인 작업은 마저 재개해줘", "BongThumbLive의 지속 개발을 해야한다 내 레포에서 가져와라" (2026-11-16).

## Fresh environment setup
1. Work on the PC **Revaptor-FX** (Windows user `Revaptor_FX`, PowerShell). Identify it by `K:\Epic Games\UE_5.8\Engine\Binaries\Win64\UnrealEditor.exe` and `K:\Unreal Projects\UE_VFX_58\UE_VFX_58.uproject`.
2. GitHub: `gh auth status` should show Baegovda. If logged out, ask Baegovda to run `gh auth login` (never handle tokens). Clone if missing: `gh repo clone Baegovda/BongThumbLive C:\workspace\BongThumbLive`.
3. Tools: UE 5.8 + Visual Studio 2022 C++ toolchain, git, gh, Python 3.10, Node.
4. Test tools are in `Tools/`. Copy them to `%TEMP%` before running (several read their config from there) and fix their engine/project paths first.
   - Helpers: `btl_verify3.py` (timed Status/screenshot/settings run), `btl_diag2.py` (obj list / memreport / GC diag), `btl_rep.ps1` (exact-string replace, keeps BOM), `btl_sm2_diag.py` (read-only stuck-tile diag), `btl_skel_fix.py` (**saves** assets: only when asked, back up first).
   - Unreal MCP client: `Tools/btl_mcp/` (server pinned to `unreal-engine-mcp-server@0.5.30`, which must match the project's McpAutomationBridge version; re-check that version on UE_VFX_58).
5. Then run the Resume checklist.

## Resume checklist (run first)
1. `git -C C:\workspace\BongThumbLive pull --ff-only`, then read this file.
2. Editor state: `Get-Process UnrealEditor*,CrashReportClient*`. Tell editors apart by `Get-CimInstance Win32_Process` CommandLine; other bots or the user may own some. Crashes: `K:\Unreal Projects\UE_VFX_58\Saved\Crashes\<newest>`.
3. Compare the live plugin with the clone: `git diff --no-index --ignore-cr-at-eol "K:\Epic Games\UE_5.8\Engine\Plugins\Marketplace\BongThumbLive\Source" C:\workspace\BongThumbLive\Source`. Differences = uncommitted work in progress.
4. Check `.bakN` files and `Binaries\Win64` timestamps vs. source to know whether the last build was deployed.
5. Continue from "Open items" or an "In progress" section. After each step, update this file, commit and push.
6. Launch the editor (pre-authorized): `"K:\Epic Games\UE_5.8\Engine\Binaries\Win64\UnrealEditor.exe" "K:\Unreal Projects\UE_VFX_58\UE_VFX_58.uproject" -AutoDeclinePackageRecovery [-ExecCmds="py <script>"]`.

## Paths
- PC: **Revaptor-FX** (Windows, 128 GB RAM, 24C/32T).
- Live plugin, clone, project, cache, log: see "Current environment" above.
- Test/diag scripts: `%TEMP%\btl_*.py|ps1` (copies of `Tools/`).
- **Bench runner:** copy `Tools/*` to Temp, edit `Temp\btl_bench_cfg.json`, then `pwsh -File $env:TEMP\btl_run.ps1 -Run <name>`. It starts the watchdog, launches the editor with `-AutoDeclinePackageRecovery` and **`-ini:EditorSettings:[/Script/UnrealEd.EditorPerformanceSettings]:bThrottleCPUWhenNotForeground=False`** (otherwise a foreground window drops the editor to ~3 fps and invalidates the run), waits up to 900 s, copies the log and prints the parsed result.
- Unreal Insights helpers: `btl_trace_long.py`, `btl_trace_agg.py` over CSVs exported with `UnrealInsights.exe -OpenTraceFile=<utrace> -NoUI -AutoQuit -ExecOnAnalysisCompleteCmd="TimingInsights.ExportTimingEvents <csv> -threads=* -columns=ThreadName,TimerName,StartTime,Duration,Depth -startTime=a -endTime=b"`.
- Bench: `btl_bench.py` (`-ExecCmds="py C:/Users/Revaptor_FX/AppData/Local/Temp/btl_bench.py"`), `btl_bench_parse.py <log>`, `btl_watchdog.ps1`.

## Repo / releases
- v2.1 released: tag commit 9b3050e. main is at 9a69106 (PR #1 merged 2026-11-15).
- **Release without asking** (owner, 2026-11-16: "릴리즈하고 담부턴 묻지마"): when a tested improvement is on main, bump VersionName/Version, tag, publish a GitHub release with the Win64 zip (dll + modules + Source + Config + README, no pdb), then report. Releases: v2.1 (UE 5.7), v2.2, v2.2.1, v2.2.2, v2.3.0, v2.3.1, v2.3.2, v2.3.3, v2.3.4, v2.3.5 (UE 5.8).

## Build
- `"K:\Epic Games\UE_5.8\Engine\Build\BatchFiles\RunUAT.bat" BuildPlugin -Plugin="C:\workspace\BongThumbLive\BongThumbLive.uplugin" -Package=C:\Users\Revaptor_FX\AppData\Local\Temp\BongThumbLive_pkgN -TargetPlatforms=Win64 -Rocket`. BuildPlugin builds a HostProject copy, so it works while the editor runs. The repo `.uplugin` EngineVersion should match 5.8 first.
- Deploy: close every UE 5.8 editor (the plugin is engine-wide), back up the engine copy's `Binaries\Win64` dll/pdb/modules as `.bakN`, then copy the package's `Binaries\Win64\*` (and changed `Source`) in.
- Build.bat does not work for the project (KawaiiPhysics plugin location, no C++ target).

## Standing rules
- Back up every changed file or binary as `.bakN` next to it (N = newest + 1). **Rotate while working** (owner, 2026-11-18: "백업 정리해가면서해"): per file keep the oldest backup (2026-11-15 baseline) and the two newest by file time; `Tools/btl_build_deploy58.ps1` does this after every deploy and keeps the two newest %TEMP% build packages; `btl_launch_bg.ps1` keeps the three newest config backups. Delete old screenshots/logs/report dirs in %TEMP% when a task is done. Every released build stays available as a GitHub release zip.
- Never delete user assets. Never touch `/Game/SM2_Work/Original/*` or `/Game/Work/*`. Never save Lightning_01.
- Editor restarts are pre-authorized. Launch with `-AutoDeclinePackageRecovery`. On a crash: close CrashReportClient, read the report, relaunch and resume. Never kill editors you did not start (check CommandLine).
- Full CPU use is OK (work is top priority).
- Keep the plugin's memory caps: 1024 MB GPU frame cache, 6 meshes / 12 anims, MinFreeMemoryGB 8, MaxDiskCacheMB 2048.
- Settings UI text stays Korean and beginner-friendly.
- Every test run starts with an empty plugin disk cache (user request 2026-10-30).
- Runtime tests run in the background from now on (user request 2026-11-16): do not disturb the user's desktop, but never minimise or hide the test editor. A minimised editor runs at ~3 fps, screenshots fail and the plugin skips Content Browsers in non-visible windows (`visibleViews=0`), so the data would be wrong. Unfocused/covered is fine with `bThrottleCPUWhenNotForeground=False`. Use `Tools/btl_launch_bg.ps1` (user chose the top-left 1080p monitor, DISPLAY1 at X=-1920 Y=-1080): it launches with `-nosplash`, moves the editor windows there with SetWindowPos(NOACTIVATE) within ~0.3 s, un-minimises them, and backs up + restores FilterBar.json, EditorLayout.ini and the project's EditorPerProjectUserSettings.ini. Call it in-process: `& Tools\btl_launch_bg.ps1 -Project <uproject> -EditorArgs '<args>' -LogCopy <log>`. Verified 2026-11-16: window 1920x1032 at -1920,-1080, regression suite 7/7 (50 tile clips, 20 partial; report `%TEMP%\btl_auto58j`). Window-size-dependent numbers (tiles on screen, live counts) from earlier 2262x1798 runs are not comparable with side-monitor runs.
- Commits: fast-forward only, never force-push. Releases: see Repo / releases (no need to ask).
- Screenshots: `%TEMP%\btl_shots\`; commit small ones to `Docs/` if useful.
- Update this file after every meaningful step, then commit and push it. The repo copy is the only source of truth.

## Open items
1. Automation tests on UE 5.8: done (7/7, 2026-11-16). Live tiles in the real UE_VFX_58 checked 2026-11-16 (they move; see "Live check" section). Small framing of first-person anim tiles fixed (TightFraming).
2. Done 2026-11-16: repo port + pkg58b deployed (rollback = copy the `.bak1` files back).
3. Tooltips: covered by the KoreanTooltips automation test; a by-eye hover is optional.
4. v2.2 and v2.2.1 released 2026-11-16. Future releases need no confirmation.

---
# History (UE 5.7 era; paths below are stale)

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
12. **Loading spinner on a tile until its first frame** (code done + compiled pkg19, deploy/test pending; see section below).

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

## Unreal MCP (in progress, 2026-10-30 03:40 KST)
- User asked to use MCP for real-time editor work. Project plugin McpAutomationBridge 0.5.30 (ChiR24/Unreal_mcp) listens on 127.0.0.1:8090,8091 inside the editor (log: "Automation bridge listening").
- Matching server `unreal-engine-mcp-server@0.5.30` installed in `%TEMP%\btl_mcp` (not 0.6.0: plugin/server versions must match). `btl_mcp_client.mjs` there spawns it over stdio, lists 23 tools; usage: `node btl_mcp_client.mjs <tool> '<json args>'`. Server itself verified.
- Blocked: UE_VFX_57 editor launched twice (03:21, 03:27) and exited by itself within 1 min (clean LogExit, no crash), so port 8091 was never reachable. Suspect: another Grok bot (BongLodge, project `C:\Users\Revaptor_FX\BongLodge_testproj\BLTest.uproject`, logs runI/runM.log) kills UnrealEditor processes on this PC. Unconfirmed.
- Rules: MCP is read-only for assets (no modify/save/delete tools). Never kill BLTest editors or their crash reporter; tell them apart via `Get-CimInstance Win32_Process` CommandLine. Read-only inside BongLodge_testproj.
## Scrolled-in tiles frozen on first frame (DONE 2026-10-30 04:35 KST, pkg17 deployed)
- User report (03:xx KST): tiles that appear after scrolling stay on their first frame until hovered.
- **Root cause (measured):** the pool keys tile textures by (asset, width, height). SAssetView creates a tile's FAssetThumbnail at 256 when the tile widget is generated first, but at its current thumbnail size (160 in the new-style CB) when the relevancy pass prefetched it (offscreen items = tiles that appear by scrolling; `SAssetView::AddItemToNewThumbnailRelevancyMap`). `FBongThumbCache::Push` always pushed into a fixed 256x256 FAssetThumbnail, i.e. a different pool texture than the 160x160 one those tiles display. Hover worked because the pool then renders the real 160 texture (realtime) via our renderer.
- **Fix (v2.3 + pkg17):** Module `UpdateVisibleTiles` reads each tile's SViewport -> ISlateViewport::GetSize() (`BongCollectViewportSizes`) into `VisibleTileSizes`; passed as `FTickParams::TileSizes`. Push keeps one FPushTarget (thumb + LastPushed + LastRefresh) per displayed size (fallback 256). Status shows `pushTargetSizes=WxH:count`.
- **Measurement** (`Tools/btl_scroll_test.py` + `btl_scroll_run.ps1 -Tag X` + `btl_diff.py X`; pairs of shots 0.3 s apart, count of tile-sized changed blobs, /Game/SM2_Work, 27 anim tiles at bottom): before1 (pkg16) after scroll-to-bottom 10-13 moving tiles (top view 24); after1 (pkg17) 18-24 moving at bottom, 24-27 at top. Status after1: `pushTargetSizes=256x256:45,160x160:24`. Remaining still tiles = near-static idle/aim poses and hologram_planet (stock icon, intended).
- Backups: source `BongThumbLiveCache.h.bak2`, `BongThumbLiveCache.cpp.bak4`, `BongThumbLiveModule.h.bak5`, `BongThumbLiveModule.cpp.bak7`; binaries `.bak14` = pkg16. Shots: PC `%TEMP%\btl_shots\scr_before1_*`, `scr_after1_*`, `diff_*`.
- Next: user confirms by scrolling in their own browser; no release unless asked.
## Cold cache: scrolled-in tiles never load (DONE 2026-10-30 04:50 KST, pkg18 deployed)
- User (04:3x KST): "여전히 안불러옴. 매번 테스트할때마다 캐시 비우고 해" -> **every test run must start with an empty disk cache** (`Tools/btl_cold_run.ps1 -Tag X` deletes the files in `Saved\BongThumbLive\Cache` first; plugin cache only). Test = /Game/Props (CB filter Animation Sequence persists, 413 items), 6 scroll steps of 4 rows, Status + DumpCache each step (`Tools/btl_cold_test.py`); diff `Tools/btl_colddiff.py`.
- The user's own session log (copied to PC `%TEMP%\btl_user_session_0434.log`) showed `visible ... loaded=116 pending=271 inflight=0` for >30 s: auto-load stalled.
- **Root cause (measured, before1 = pkg17 cold):** `ownedAnimRoots=12` all the time -> PumpAutoLoads breaks at the 12 plugin-loaded-animation cap, `pending` 40-47 with `inflight=0`. Slots were held by anims scrolled away (released only after 30 s off screen) and by anims still generating, and generation finished one anim fully before starting the next (FillTargetPath). The visible queue also kept every tile ever shown. Result: `newTileToShown waiting=5..8` 12.6 s after each scroll, max 62.9 s.
- **Fix (pkg18):** (1) Fill: visible tiles with < 12 frames first (fewest first) get a coarse pass of 12 evenly spaced frames (`FillFrames(..., CoarseCount)`), so a new tile moves (choppy) right away and is refined later; attempts/tick 3->6. (2) Off-screen plugin-loaded anims released after 3 s (was 30 s); cap 12 unchanged. (3) PumpAutoLoads drops visible-queue anim entries no longer on screen (re-queued when shown again). Status: `coarseRenders=`.
- **Cold numbers:** before1: pending 40-47, ownedAnimRoots 12, waiting 5-8 tiles at +12.6 s every step, newTileToShown avg 11.5 s max 62.9 s. after1: pending 0, ownedAnimRoots 0, waiting 0 at every status, newTileToShown avg 3.1 s max 10.0 s (max = initial view during cold DDC anim compression; scrolled-in tiles avg ~0.6 s vs ~16 s before).
- Backups: `BongThumbLiveCache.cpp.bak5`, `BongThumbLiveModule.cpp.bak8`, `BongThumbLiveRenderer.cpp.bak7`, `Public/BongThumbLiveRenderer.h.bak3`, `BongThumbLiveModule.h.bak6`, `BongThumbLiveCache.h.bak3` (the two .h backups were copied from the clone = pkg17 content); binaries `.bak15` = pkg17.
- Note: I closed the user's own UE_VFX_57 editor (CloseMainWindow, closed cleanly) to run tests.

## Pending request (STARTED 2026-10-30 on user order, see Loading spinner section)
- 2026-10-30 04:51 KST: Baegovda will test after sleeping (clear the disk cache before every test). Wanted next: a loading/buffering animation on a tile from when it is shown until its first frame is ready (idea: small spinner overlay drawn by the plugin; must work for unloaded anims and any tile size, see pushTargetSizes). Start only when the user is back and asks.

## Bot handover (2026-10-30 14:50 KST)
- Baegovda is handing BongThumbLive to another Grok bot (the one already working on Unreal on this PC) so two bots do not mix up Unreal work. The previous bot stops working on the plugin. State: pkg18 deployed (6441ecc), pending loading-animation request still NOT started, user has not yet manually verified pkg18, tooltip, or other thumbnail sizes. Check running editors by CommandLine before touching any process.

## Loading spinner until first frame (pkg19 DEPLOYED + tested 2026-10-30 16:04-16:13 KST)
- User (pt136u, 2026-10-30): "인수 다받으면 새기능 착수해" = start the pending loading-animation feature now.
- **Design:** one hit-test-invisible `SBongSpinnerOverlay` (SLeafWidget, volatile) added via `SWindow::AddOverlaySlot` to each window that shows a Content Browser (main or floating). Each tick `UpdateSpinners` (Module.cpp, after TickCache) builds a list of draws from the tile's own SViewport paint geometry (any tile size: radius = 11 % of the shorter side, 6-18 px), clipped to the asset view rect. Drawn = 3 antialiased Slate line elements (dark ring, faint white ring, rotating 108° white arc, 1.25 turns/s). No textures, no frame-cache memory, no scene renders.
- **Shown when:** anim tile visible + plugin will animate it (Mode: Visible / Selected+selected / hovered) + cache entry has 0 valid frames + resolver has NOT given up (`UBongThumbLiveRenderer::HasGivenUp`, new set `GBongGaveUpAnims`) + waiting >= 0.3 s (no flicker for cached tiles; fades in 0.25 s). Gone the same tick NumValid > 0 (same moment the tile is announced). Safety cap 90 s (`spinnerTimedOut`). Overlay draws nothing if UpdateSpinners did not run for 0.5 s.
- **Setting:** `bShowLoadingSpinner` (재생 / "로딩 표시 보이기", default on, Korean tooltip).
- **Status line:** `spinner: enabled spinnerTiles spinnerDraws spinnerWaiting spinnerShownTotal spinnerTimedOut overlays`.
- **Backups (made before editing):** `Public/BongThumbLiveSettings.h.bak3`, `Public/BongThumbLiveRenderer.h.bak4`, `Private/BongThumbLiveRenderer.cpp.bak8`, `Private/BongThumbLiveModule.h.bak7`, `Private/BongThumbLiveModule.cpp.bak9`.
- **Build:** pkg19 `%TEMP%\BongThumbLive_pkg19` built with RunUAT BuildPlugin while the editor ran (BuildPlugin builds a HostProject copy; live plugin binaries untouched): `BUILD SUCCESSFUL`, 0 errors. Log `%TEMP%\btl_pkg19_build.log`.
- **Not done (needs the UE_VFX_57 editor closed; PID 33536 was in use by another worker for level work):** back up plugin `Binaries\Win64` dll/pdb/modules as `.bak16` (= pkg18), copy pkg19 `Binaries\Win64\*` in, relaunch editor, `btl_cold_run.ps1` (cold cache) on /Game/Props + /Game/SM2_Work, screenshots at 0.3/1/3 s after scroll at 160 and 256 tile sizes, check spinner on waiting tiles, none on hologram_planet__anim_bak1..5 after give-up, Status spinner line, toggle off works.
- Known limits (unverified): before the resolver gives up (~14 s: 2/4/8 s retries) meshless anims spin; a tile whose pool shows a saved package thumbnail still gets the spinner until the first live frame (intended: "until first frame").

### pkg19 deploy + test (2026-10-30 16:04-16:13 KST)
- Deployed: pkg18 binaries backed up as `.bak16` (dll/pdb/modules) in `Plugins\BongThumbLive\Binaries\Win64`; pkg19 copied in, SHA256 equal to `%TEMP%\BongThumbLive_pkg19`. Rollback = copy `.bak16` back.
- New tools: `Tools/btl_spin_run.ps1 -Tag X [-Folder /Game/...]` (cold: clears plugin disk cache, launches editor, quits) + `Tools/btl_spin_test.py` (shots 0.3/1/3/16/20 s after scroll bottom/top/bottom, Status, then setting off via `show_loading_spinner` + ClearMemoryCache + scroll, setting back on).
- /Game/SM2_Work root and /Game/Props root now show only folders (CB "Animation Sequence" filter no longer persisted): 0 anim tiles, spinner 0 (valid, nothing to show). hologram_planet__anim_bak1..5 no longer exist (deleted in SM2_Work cleanup), so the give-up case could not be tested.
- Run `psr` (/Game/Props/PrecisionSniperRifle/Character/Animations, 113 anims, cold): Status at +9 s after open: spinnerTiles=66 draws=66 shownTotal=78 timedOut=0 overlays=1; after scroll bottom +20 s: spinnerTiles=62 shownTotal=113; top +20 s: 36 (live 31/78); 3rd scroll +20 s: 0 (live 74/74, GPU 705 MB / 1024 cap); setting off: enabled=0 spinnerTiles=0 spinnerDraws=0. pushTargetSizes=256x256:117,160x160:39. newTileToShown n=156 avg 17.3 s max 84.2 s (cold; slow, not a spinner issue; ownedAnimRoots=12 cap reached early). No errors/ensures in log.
- Visual: ring visible on waiting tiles (shot spin_psr_0bottom_01.0), none on tiles with frames. Not checked: exact centring / clipping per tile size, fade-in timing by eye, 160 vs 256 side by side.
## Cold-load speed A/B (IN PROGRESS 2026-10-30 17:00-18:05 KST, pkg21 deployed, all new cvars default 0 = old behaviour)
- pkg20/pkg21 add cvars: `BongThumbLive.TileTrace 1` (BTLT per-tile lines: shown/q/l/r/f), `SceneRenderBudgetWaitingMs`, `ReleaseAfterCoarse 1|2`; plus Status `waiting= earlyReleases=`. Caps unchanged (6/12, MinFreeMemoryGB 8, MaxDiskCacheMB 2048). Binaries `.bak17` = pkg19.
- Tools: `%TEMP%\btl_speed_run.ps1 -Tag X -Cvars "..."` (clears plugin disk cache, launches with -AutoDeclinePackageRecovery + no CPU throttle, runs `btl_speed_test.py`, parses with `btl_speed_parse.py`). Folder /Game/Props/PrecisionSniperRifle/Character/Animations.
- Note: the 16:13 baseline (avg 17.3 s / max 84.2 s) had a cold UE DDC; the plugin-cache-only cold runs now give far lower numbers, and the CB layout changed between runs (visibleAnimTiles 28 -> 18), so only same-layout runs are compared.
- 28-tile layout (n=116): base2 avg 2.62 max 17.39; FirstFrameFirst 2.75/18.33; SceneRenderBudgetWaitingMs 12 2.57/17.29; RecreateRenderState 2 2.59/17.30; AutoLoadBatchSize 8 2.57/17.27 (all = noise); ReleaseAfterCoarse 1 2.05/17.47 but 150 hitches >50 ms, fps<50 for 46 s (rejected).
- 18-tile layout (n=76): base3 1.21/8.57, base4 1.22/8.64; ReleaseAfterCoarse 2: g 1.03/8.60, g2 1.04/8.63 (avg ~15% better, max unchanged). g3 invalid (PC disturbed, fps<50 whole run). Hitch counts in g2/base4 were also high (other load on the PC), not attributable.
- Not done yet: decide default for ReleaseAfterCoarse 2; find what dominates the ~8.6 s max (trace l/r/f columns).
- Trace (scrolled-in tiles, BTLT): time is almost all slot-queue wait (q ~= shown; load l-q ~15 ms, first frame f-r ~0-40 ms). ReleaseAfterCoarse 2: traced avgShown 0.34/0.35 s -> 0.09/0.09 s, worst traced tile 3.09 s -> 0.97 s (base3 vs g2). The ~8.6 s max is from the untraced initial open, not scrolled tiles (cause not yet measured).

## Cold-load speed: ReleaseAfterCoarse default 2 (pkg22 DEPLOYED + measured 2026-10-30 19:41-19:54 KST)
- pkg22 = pkg21 code with `BongThumbLive.ReleaseAfterCoarse` default **2** (free a finished+saved plugin-loaded anim's slot at once while visible tiles wait, instead of after 3 s). Caps unchanged (6/12, MinFreeMemoryGB 8, MaxDiskCacheMB 2048, CacheMemoryMB 4096). Old binaries = `.bak18` (pkg21).
- Cold A/B (plugin disk cache cleared every run, psr folder, 18-tile layout, n=76, interleaved): ON p22a avg 1.87 s / p22b 2.17 s; OFF (cvar 0) 2.69 s / 2.52 s. Scrolled-in traced tiles avgShown: ON 0.36 s (p22b) vs OFF 1.13 / 0.94 s; worst traced tile ON 3.44 s (p22b) vs OFF 21.5 / 20.7 s. Hitches >100 ms: 0-3 per run, fps<50 1-2 s of 145 (no regression). Peak private 1.83-1.86 GB.
- The newTileToShown **max** (15-21 s here, 8.6 s earlier) is dominated by tiles the Content Browser restores during editor startup (VisibleSince starts before the editor is idle); it is not the folder-click latency. Not fixed; a proper folder-click measurement needs the test to start from another folder.
- Logs: `%TEMP%\btl_speed_p22a/p22b/p22off_a/p22off_b.log`. Tools: `Tools/btl_speed_run.ps1`, `btl_speed_test.py`, `btl_speed_parse.py`.
- Rejected earlier (noise): FirstFrameFirst, SceneRenderBudgetWaitingMs 12, RecreateRenderState 2, AutoLoadBatchSize 8; ReleaseAfterCoarse 1 (hitches).

## Spinner colour + style settings (pkg23, 2026-10-30 21:20 KST, user t224u/t225u)
- New settings (재생 category, per-user ini, read every paint so they apply without restart): `LoadingSpinnerColor` (FColor, default 255,210,0 yellow, alpha hidden), `LoadingSpinnerStyle` (EBongSpinnerStyle: Spin default / Pulse / Dots / Bar), `LoadingSpinnerSpeed` (0.1-5, default 1 = 1.25 cycles/s as before). Size/radius unchanged. Caps unchanged.
- Backups: `BongThumbLiveSettings.h.bak4`, `BongThumbLiveModule.cpp.bak12` (pre-change). Edit script `Tools/btl_spin_color_edit.py`.
- Built OK as `%TEMP%\BongThumbLive_pkg23`. Status: NOT yet deployed / live-verified (waiting for editor lock `editor-free`/`bonglodge-free`). Next: close editor gracefully, back up binaries as next .bakN, copy pkg23 Binaries\Win64, relaunch, screenshot yellow spinner + change colour/style in settings.

- pkg24 (21:58 KST): faint back ring now drawn only for the Spin style (backup BongThumbLiveModule.cpp.bak13). Built OK %TEMP%\BongThumbLive_pkg24. NOT deployed / NOT verified live: lock was already taken by 'bonglodge 2026-10-30 21:55:51'. Deploy pkg24 (not pkg23) when the lock is free.

- pkg24 DEPLOYED 22:24 KST (binaries backup .bak19 = pkg22). Live test Tools/btl_spin24_test.py (cold cache, /Game/Props/PrecisionSniperRifle/Character/Animations, settings changed via the settings object at runtime, no restart): yellow default ~950 px, red/blue switch live, Spin/Pulse/Dots/Bar all drawn (shots %TEMP%\btl_shots\spin24b\, montage_styles.png). spinnerShownTotal=33. Caveats: changed via Python set_editor_property, not by clicking the Settings window; speed x3 not visually judged (stills); Dots look like small upright bars. Settings reset to defaults (yellow/Spin/1). Caps unchanged.
