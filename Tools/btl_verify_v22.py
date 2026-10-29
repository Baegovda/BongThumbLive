# v2.2 verification: hologram_planet tile, disk-cache invalidation after edit + re-save, settings page.
import unreal, time, glob, os
T0 = time.time()
TMP = "C:/Users/Revaptor_FX/AppData/Local/Temp/"
SHOT = TMP + "btl_shots/"
os.makedirs(SHOT, exist_ok=True)
CACHE = "K:/Unreal Projects/UE_VFX_57/Saved/BongThumbLive/Cache"
HOLO = "/Game/SM2_Work/Tests/sani_before/hologram_planet/localbake"
TAUNT = "/Game/Animations/HugeAnimBundle/TauntMultiPack/MegaTauntsPack01/Animations/Taunts"
st = {"i": 0, "anim": None}
def log(m): unreal.log("BTLV " + m)
def cmd(c): unreal.SystemLibrary.execute_console_command(None, c)
def files(name):
    return sorted(os.path.basename(f) + " " + time.strftime("%H:%M:%S", time.localtime(os.path.getmtime(f))) for f in glob.glob(CACHE + "/**/" + name + "_*.btlc", recursive=True))
def setup():
    cmd("BongThumbLive.Set Mode VisibleInBrowser"); cmd("BongThumbLive.Set MaxFPS 60")
    unreal.EditorAssetLibrary.sync_browser_to_objects  # exists check
    unreal.EditorUtilityLibrary.sync_browser_to_folders([HOLO])
def taunts():
    ar = unreal.AssetRegistryHelpers.get_asset_registry()
    a = sorted([x for x in ar.get_assets_by_path(TAUNT, recursive=False) if str(x.asset_class_path.asset_name) == "AnimSequence"], key=lambda x: str(x.asset_name))
    st["anim"] = str(a[0].package_name) + "." + str(a[0].asset_name); st["name"] = str(a[0].asset_name)
    log("test anim " + st["anim"] + " files before: " + repr(files(st["name"])))
    unreal.EditorUtilityLibrary.sync_browser_to_folders([TAUNT])
def edit():
    o = unreal.EditorAssetLibrary.load_asset(st["anim"]); st["obj"] = o
    unreal.EditorAssetLibrary.set_metadata_tag(o, "BTLInvalidationTest", "1")
    o.modify()
    log("edited (metadata tag)")
def save1():
    log("save ok=%s" % unreal.EditorAssetLibrary.save_loaded_asset(st["obj"], False))
def files_log(tag): log(tag + " files: " + repr(files(st["name"])))
def revert():
    unreal.EditorAssetLibrary.remove_metadata_tag(st["obj"], "BTLInvalidationTest")
    log("reverted tag, save ok=%s" % unreal.EditorAssetLibrary.save_loaded_asset(st["obj"], False))
plan = [
 (20, lambda: setup()),
 (40, lambda: (cmd("BongThumbLive.Status"), cmd("BongThumbLive.CaptureEditorWindow " + SHOT + "v22_hologram.png"))),
 (45, lambda: taunts()),
 (65, lambda: (cmd("BongThumbLive.Status"), cmd("BongThumbLive.CaptureEditorWindow " + SHOT + "v22_taunts_before_edit.png"))),
 (68, lambda: edit()),
 (72, lambda: (cmd("BongThumbLive.Status"), files_log("dirty"))),
 (74, lambda: save1()),
 (95, lambda: (cmd("BongThumbLive.Status"), files_log("after save"), cmd("BongThumbLive.CaptureEditorWindow " + SHOT + "v22_taunts_after_save.png"))),
 (98, lambda: revert()),
 (120, lambda: (cmd("BongThumbLive.Status"), files_log("after revert save"))),
 (125, lambda: cmd("BongThumbLive.OpenSettings")),
 (132, lambda: cmd("BongThumbLive.CaptureEditorWindow " + SHOT + "v22_settings.png")),
 (134, lambda: log("READY_FOR_HOVER")),
 (400, lambda: (cmd("BongThumbLive.Status"), cmd("QUIT_EDITOR"))),
]
def tick(dt):
    t = time.time() - T0
    if st["i"] < len(plan) and t > plan[st["i"]][0]:
        i = st["i"]; st["i"] += 1
        log("step %d at %.0fs" % (i, t))
        try: plan[i][1]()
        except Exception as e: log("step %d error: %s" % (i, e))
st["h"] = unreal.register_slate_post_tick_callback(tick)
log("registered")
