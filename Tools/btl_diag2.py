import unreal, time
T0 = time.time()
st = {"step": 0, "next": 30.0}
def log(m): unreal.log("BTLDIAG " + m)
def cmd(c): unreal.SystemLibrary.execute_console_command(None, c)
plan = [
 (60, ["BongThumbLive.Status","obj list class=AnimSequence","obj list class=SkeletalMesh","obj list class=Skeleton","obj list class=Texture2D","obj list class=Material","obj list class=MaterialInstanceConstant"]),
 (100, ["memreport -full"]),
 (130, ["BongThumbLive.Status","obj gc"]),
 (160, ["BongThumbLive.Status","obj list class=AnimSequence","obj list class=SkeletalMesh"]),
 (170, ["BongThumbLive.Set Enabled 0"]),
 (185, ["obj gc"]),
 (215, ["BongThumbLive.Status","obj list class=AnimSequence","obj list class=SkeletalMesh","obj list class=Texture2D"]),
]
def tick(dt):
    t = time.time() - T0
    if st["step"] == 0 and t > 30:
        ar = unreal.AssetRegistryHelpers.get_asset_registry()
        flt = unreal.ARFilter(package_paths=["/Game/SM2_Work"], recursive_paths=True, class_paths=[unreal.TopLevelAssetPath("/Script/Engine","AnimSequence")], recursive_classes=True)
        assets = [a for a in ar.get_assets(flt) if not str(a.package_name).startswith("/Game/SM2_Work/Original")]
        log("anims found %d" % len(assets))
        unreal.EditorAssetLibrary.sync_browser_to_objects([str(a.package_name) + "." + str(a.asset_name) for a in assets][:100])
        st["step"] = 1
        return
    i = st["step"] - 1
    if 0 <= i < len(plan) and t > plan[i][0]:
        log("phase %d at %.0fs" % (i, t))
        for c in plan[i][1]: cmd(c)
        st["step"] += 1
        if st["step"] - 1 >= len(plan):
            cmd("BongThumbLive.Set Enabled 1")
            log("done")
            unreal.unregister_slate_post_tick_callback(st["h"])
st["h"] = unreal.register_slate_post_tick_callback(tick)
log("registered")
