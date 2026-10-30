# Read-only diagnosis of stuck anim tiles in /Game/SM2_Work (no saves).
import unreal, time, json
T0 = time.time(); st = {"i": 0}
TMP = "C:/Users/Revaptor_FX/AppData/Local/Temp/"
FOLDER = "/Game/SM2_Work"
def log(m): unreal.log("BTLD " + m)
def cmd(c): unreal.SystemLibrary.execute_console_command(None, c)
def tagpath(v):
    v = str(v); return v.split("'")[1] if "'" in v else v
def listing():
    ar = unreal.AssetRegistryHelpers.get_asset_registry()
    a = [x for x in ar.get_assets_by_path(FOLDER, recursive=False) if str(x.asset_class_path.asset_name) == "AnimSequence"]
    log("direct anims in %s: %d" % (FOLDER, len(a)))
    st["assets"] = a
def dump():
    ar = unreal.AssetRegistryHelpers.get_asset_registry()
    meshes = ar.get_assets(unreal.ARFilter(package_paths=["/Game"], recursive_paths=True, class_paths=[unreal.TopLevelAssetPath("/Script/Engine", "SkeletalMesh")]))
    used = set(tagpath(m.get_tag_value("Skeleton")) for m in meshes)
    rows = []
    for a in sorted(st["assets"], key=lambda x: str(x.asset_name)):
        loaded_before = a.is_asset_loaded()
        o = unreal.load_asset(str(a.package_name))
        r = {"name": str(a.asset_name), "loaded_before": loaded_before}
        for p in ("additive_anim_type", "ref_pose_type", "ref_pose_seq", "ref_frame_index", "preview_pose_asset"):
            try: r[p] = str(o.get_editor_property(p))
            except Exception as e: r[p] = "err"
        try: sk = o.get_editor_property("skeleton"); r["skeleton"] = sk.get_path_name() if sk else None
        except Exception as e: r["skeleton"] = "err %s" % e
        r["skeleton_has_mesh"] = r["skeleton"] in used
        try: r["preview_mesh"] = str(o.get_preview_mesh().get_path_name()) if o.get_preview_mesh() else None
        except Exception: r["preview_mesh"] = "n/a"
        try: r["len"] = o.get_play_length(); r["frames"] = o.get_editor_property("number_of_sampled_frames") if False else unreal.AnimationLibrary.get_num_frames(o)
        except Exception as e: r["len"] = "err %s" % e
        rows.append(r)
    open(TMP + "btl_sm2_diag.json", "w").write(json.dumps(rows, indent=1))
    log("dumped %d rows" % len(rows))
plan = [
 (20, lambda: (cmd("BongThumbLive.Set Mode VisibleInBrowser"), cmd("BongThumbLive.Set MaxFPS 60"), listing(), unreal.EditorUtilityLibrary.sync_browser_to_folders([FOLDER]))),
 (100, lambda: (cmd("BongThumbLive.Status"), cmd("BongThumbLive.CaptureEditorWindow " + TMP + "btl_shots/sm2_before_a.png"))),
 (160, lambda: (cmd("BongThumbLive.Status"), cmd("BongThumbLive.CaptureEditorWindow " + TMP + "btl_shots/sm2_before_b.png"))),
 (165, lambda: dump()),
 (175, lambda: cmd("QUIT_EDITOR")),
]
def tick(dt):
    if st["i"] < len(plan) and time.time() - T0 > plan[st["i"]][0]:
        i = st["i"]; st["i"] += 1
        try: plan[i][1]()
        except Exception as e: log("step %d error %s" % (i, e))
st["h"] = unreal.register_slate_post_tick_callback(tick)
