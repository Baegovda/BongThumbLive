# Read-only: SM2_Work stuck-tile + scroll test (no saves, no asset edits). Tag via env BTL_TAG.
import unreal, time, json, os
T0 = time.time(); st = {"i": 0}
TMP = "C:/Users/Revaptor_FX/AppData/Local/Temp/"
TAG = open(TMP + "btl_sm2_tag.txt").read().strip()
SHOTS = TMP + "btl_shots/"
FOLDER = "/Game/SM2_Work"
PAT = ("__ADD", "titus_assault__tacticus_wpn1_", "intercessor_title", "title_screen")
def log(m): unreal.log("BTLD " + m)
def cmd(c): unreal.SystemLibrary.execute_console_command(None, c)
def shot(n):
    log("shot %s t=%.2f" % (n, time.time() - T0)); cmd("BongThumbLive.CaptureEditorWindow " + SHOTS + "sm2_%s_%s.png" % (TAG, n))
def tagpath(v):
    v = str(v); return v.split("'")[1] if "'" in v else v
def dump():
    ar = unreal.AssetRegistryHelpers.get_asset_registry()
    anims = [a for a in ar.get_assets_by_path(FOLDER, recursive=True) if str(a.asset_class_path.asset_name) == "AnimSequence" and not str(a.package_path).startswith("/Game/SM2_Work/Original")]
    meshes = ar.get_assets(unreal.ARFilter(package_paths=["/Game"], recursive_paths=True, class_paths=[unreal.TopLevelAssetPath("/Script/Engine", "SkeletalMesh")]))
    used = {}
    for m in meshes: used.setdefault(tagpath(m.get_tag_value("Skeleton")), []).append(str(m.package_name))
    rows = []
    for a in sorted(anims, key=lambda x: str(x.package_name)):
        r = {"path": str(a.package_name), "loaded_before": a.is_asset_loaded()}
        if any(p in str(a.asset_name) for p in PAT):
            o = unreal.load_asset(str(a.package_name))
            for p in ("additive_anim_type", "ref_pose_type", "ref_pose_seq", "ref_frame_index", "preview_pose_asset"):
                try:
                    v = o.get_editor_property(p); r[p] = v.get_path_name() if hasattr(v, "get_path_name") else str(v)
                except Exception: r[p] = "err"
            try:
                sk = o.get_editor_property("skeleton"); r["skeleton"] = sk.get_path_name() if sk else None
                r["meshes_on_skeleton"] = used.get(r["skeleton"], [])[:3]
                r["compatible"] = [c.get_path_name() if hasattr(c, "get_path_name") else str(c) for c in (sk.get_editor_property("compatible_skeletons") or [])] if sk else []
                pm = sk.get_preview_mesh() if sk else None; r["skel_preview_mesh"] = pm.get_path_name() if pm else None
            except Exception as e: r["skeleton_err"] = str(e)
            try: r["anim_preview_mesh"] = o.get_preview_mesh().get_path_name() if o.get_preview_mesh() else None
            except Exception: r["anim_preview_mesh"] = "n/a"
            try: r["len"] = o.get_play_length(); r["frames"] = unreal.AnimationLibrary.get_num_frames(o)
            except Exception as e: r["len"] = "err %s" % e
        rows.append(r)
    open(TMP + "btl_sm2_diag_%s.json" % TAG, "w").write(json.dumps(rows, indent=1))
    log("dumped %d rows" % len(rows))
def setup():
    os.makedirs(SHOTS, exist_ok=True)
    cmd("BongThumbLive.Set Mode VisibleInBrowser"); cmd("BongThumbLive.Set MaxFPS 60")
    unreal.EditorUtilityLibrary.sync_browser_to_folders([FOLDER])
plan = [(20, setup), (85, lambda: cmd("BongThumbLive.Status")), (86, lambda: shot("a_initial"))]
t = 92
for where in ("bottom", "top"):
    plan.append((t, (lambda w: lambda: (log("scroll %s t=%.2f" % (w, time.time() - T0)), cmd("BongThumbLive.ScrollBrowser " + w)))(where)))
    for d in (0.5, 1.5, 3.0, 8.0):
        plan.append((t + d, (lambda n: lambda: shot(n))("%s_%04.1f" % (where, d))))
    plan.append((t + 8.5, lambda: cmd("BongThumbLive.Status")))
    t += 15
plan += [(t + 2, dump), (t + 12, lambda: cmd("BongThumbLive.Status")), (t + 14, lambda: cmd("QUIT_EDITOR"))]
def tick(dt):
    if st["i"] < len(plan) and time.time() - T0 > plan[st["i"]][0]:
        i = st["i"]; st["i"] += 1
        try: plan[i][1]()
        except Exception as e: log("step %d error %s" % (i, e))
st["h"] = unreal.register_slate_post_tick_callback(tick)
