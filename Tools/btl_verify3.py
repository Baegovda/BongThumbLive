import unreal, time, collections
T0 = time.time()
st = {"step": 0, "frames": [], "lastfps": 0.0}
def log(m): unreal.log("BTLDIAG " + m)
def cmd(c): unreal.SystemLibrary.execute_console_command(None, c)
TMP = "C:/Users/Revaptor_FX/AppData/Local/Temp/"
RUN = open(TMP + "btl_run.txt").read().strip()
plan = [
 (45, ["BongThumbLive.Status"]),
 (75, ["BongThumbLive.Status"]),
 (105, ["BongThumbLive.Status", "obj list class=AnimSequence", "obj list class=SkeletalMesh"]),
 (140, ["BongThumbLive.Status"]),
 (180, ["BongThumbLive.Status", "obj list class=AnimSequence", "obj list class=SkeletalMesh"]),
 (220, ["BongThumbLive.Status"]),
 (260, ["BongThumbLive.Status", "obj list class=AnimSequence", "obj list class=SkeletalMesh", "obj list class=Texture2D"]),
 (265, ["BongThumbLive.CaptureEditorWindow " + TMP + "btl_toolbar_" + RUN + ".png"]),
 (270, ["BongThumbLive.DumpCache " + TMP + "btl_dump_" + RUN]),
 (275, ["BongThumbLive.OpenSettings"]),
 (282, ["BongThumbLive.CaptureEditorWindow " + TMP + "btl_settings_" + RUN + ".png"]),
 (290, ["BongThumbLive.Status"]),
]
def tick(dt):
    t = time.time() - T0
    st["frames"].append(dt)
    if t - st["lastfps"] > 5.0 and st["step"] >= 1:
        f = st["frames"]; st["frames"] = []; st["lastfps"] = t
        if f:
            avg = sum(f) / len(f)
            log("t=%.0fs fps=%.1f avgFrameMs=%.2f maxFrameMs=%.1f" % (t, 1.0 / avg if avg > 0 else 0, avg * 1000, max(f) * 1000))
    if st["step"] == 0 and t > 20:
        st["frames"] = []
        st["step"] = 1
        try:
            ar = unreal.AssetRegistryHelpers.get_asset_registry()
            flt = unreal.ARFilter(package_paths=["/Game/SM2_Work"], recursive_paths=True, class_paths=[unreal.TopLevelAssetPath("/Script/Engine","AnimSequence")], recursive_classes=True)
            assets = [a for a in ar.get_assets(flt) if not str(a.package_name).startswith("/Game/SM2_Work/Original")]
            log("anims found %d" % len(assets))
            cmd("BongThumbLive.Set Mode VisibleInBrowser")
            cmd("BongThumbLive.Set MaxFPS 60")
            try:
                perf = unreal.find_object(None, "/Script/UnrealEd.Default__EditorPerformanceSettings")
                st["perf"] = perf
                st["throttle"] = perf.get_editor_property("throttle_cpu_when_not_foreground")
                perf.set_editor_property("throttle_cpu_when_not_foreground", False)
                log("background throttle off (was %s)" % st["throttle"])
            except Exception as e:
                log("throttle setting not available: %s" % e)
            unreal.EditorAssetLibrary.sync_browser_to_objects([str(a.package_name) + "." + str(a.asset_name) for a in assets][:100])
            log("synced")
        except Exception as e:
            log("setup error: %s" % e)
        return
    i = st["step"] - 1
    if 0 <= i < len(plan) and t > plan[i][0]:
        log("phase %d at %.0fs" % (i, t))
        st["step"] += 1
        for c in plan[i][1]: cmd(c)
        if st["step"] - 1 >= len(plan):
            if "perf" in st:
                st["perf"].set_editor_property("throttle_cpu_when_not_foreground", st["throttle"])
            log("done")
            unreal.unregister_slate_post_tick_callback(st["h"])
st["h"] = unreal.register_slate_post_tick_callback(tick)
log("registered run " + RUN)
