# BongThumbLive benchmark: real wall-clock frame times, hitches, folder-click -> first live tile.
import unreal, time, json
TMP = "C:/Users/Revaptor_FX/AppData/Local/Temp/"
CFG = json.load(open(TMP + "btl_bench_cfg.json"))
T0 = time.perf_counter()
st = {"plan": None, "i": 0, "t_phase": 0.0, "last": None, "win": [], "win_t": 0.0, "status_until": 0.0, "status_next": 0.0, "hitch_total": 0.0}
def log(m): unreal.log("BTLB " + m)
def cmd(c): unreal.SystemLibrary.execute_console_command(None, c)
def goto(folder):
    try:
        unreal.EditorUtilityLibrary.sync_browser_to_folders([folder])
    except Exception as e:
        log("sync_browser_to_folders failed %s" % e)
def anims_in(folder):
    ar = unreal.AssetRegistryHelpers.get_asset_registry()
    flt = unreal.ARFilter(package_paths=[folder], recursive_paths=False, class_paths=[unreal.TopLevelAssetPath("/Script/Engine", "AnimSequence")], recursive_classes=True)
    return [str(a.package_name) + "." + str(a.asset_name) for a in ar.get_assets(flt)]
# Launch the editor with -ini:EditorSettings:[/Script/UnrealEd.EditorPerformanceSettings]:bThrottleCPUWhenNotForeground=False
# (session only) - otherwise it runs at ~3 fps whenever its window is not in the foreground.
def now(): return time.perf_counter() - T0

def build_plan():
    P = []
    P.append((0, lambda: [cmd("BongThumbLive.Set Mode VisibleInBrowser"), cmd("BongThumbLive.Set MaxFPS 60"), cmd("stat unit")]))
    if CFG.get("clear"):
        P.append((1, lambda: cmd("BongThumbLive.ClearDiskCache")))
    P.append((1, lambda: goto(CFG["empty"])))
    for f in CFG["folders"]:
        P.append((4, lambda f=f: click(f, "gen")))
        P.append((CFG.get("gen_secs", 60), lambda: [cmd("BongThumbLive.Status"), goto(CFG["empty"])]))
    # disk-cache path: memory cache dropped, folder clicked again
    if CFG.get("disk", True):
        P.append((4, lambda: cmd("BongThumbLive.ClearMemoryCache")))
        P.append((3, lambda: click(CFG["folders"][0], "disk")))
        P.append((15, lambda: [cmd("BongThumbLive.Status"), goto(CFG["empty"])]))
    if CFG.get("select_folder"):
        def sel():
            objs = anims_in(CFG["select_folder"])
            log("SELECT %d assets in %s" % (len(objs), CFG["select_folder"]))
            st["status_until"] = now() + CFG.get("status_secs", 10.0)
            unreal.EditorAssetLibrary.sync_browser_to_objects(objs)
            log("SELECT returned")
        P.append((4, sel))
        P.append((CFG.get("gen_secs", 60), lambda: cmd("BongThumbLive.Status")))
    P.append((2, lambda: [cmd("obj list class=SkeletalMesh"), cmd("BongThumbLive.Status")]))
    P.append((2, lambda: [log("DONE hitch_total_ms=%.0f" % st["hitch_total"]), cmd("QUIT_EDITOR") if CFG.get("quit") else None]))
    return P

def click(folder, kind):
    log("CLICK %s %s" % (kind, folder))
    st["status_until"] = now() + CFG.get("status_secs", 12.0)
    st["status_next"] = 0.0
    goto(folder)

def tick(dt):
    t = now()
    if st["last"] is not None:
        ft = (t - st["last"]) * 1000.0
        st["win"].append(ft)
        if ft > 50.0:
            st["hitch_total"] += ft
            log("HITCH %.0f ms at t=%.2f" % (ft, t))
    st["last"] = t
    if t - st["win_t"] >= 1.0 and st["win"]:
        w = st["win"]; avg = sum(w) / len(w)
        log("SEC t=%.0f fps=%.1f avg=%.2f max=%.1f n=%d" % (t, 1000.0 / avg if avg else 0, avg, max(w), len(w)))
        st["win"] = []; st["win_t"] = t
    if t < st["status_until"] and t >= st["status_next"]:
        st["status_next"] = t + CFG.get("status_dt", 0.05)
        cmd("BongThumbLive.Status")
    if st["plan"] is None:
        ar = unreal.AssetRegistryHelpers.get_asset_registry()
        if t < 25.0 or ar.is_loading_assets():
            return
        st["plan"] = build_plan(); st["t_phase"] = t; log("PLAN start (%d steps) cfg=%s" % (len(st["plan"]), json.dumps(CFG)))
        return
    if st["i"] < len(st["plan"]):
        delay, fn = st["plan"][st["i"]]
        if t - st["t_phase"] >= delay:
            st["i"] += 1; st["t_phase"] = t
            fn()
st["h"] = unreal.register_slate_post_tick_callback(tick)
log("registered")
