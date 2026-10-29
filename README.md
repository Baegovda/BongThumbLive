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
