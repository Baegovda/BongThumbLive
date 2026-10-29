# Read-only: compare hologram_planet skeletons and scan the project for anims whose skeleton has no skeletal mesh.
import unreal, time, collections
T0 = time.time(); st = {"done": False}
LB = "/Game/SM2_Work/Tests/sani_before/hologram_planet/localbake/hologram_planet_Skeleton"
PR = "/Game/SM2_Work/Props/hologram_planet/hologram_planet_Skeleton"
ANIM = "/Game/SM2_Work/Tests/sani_before/hologram_planet/localbake/hologram_planet__anim"
MESH = "/Game/SM2_Work/Tests/sani_before/hologram_planet/localbake/hologram_planet"
def log(m): unreal.log("BTLS " + m)
def bones_of_mesh(m):
    c = unreal.new_object(unreal.SkeletalMeshComponent)
    c.set_skeletal_mesh_asset(m)
    return [str(c.get_bone_name(i)) for i in range(c.get_num_bones())]
def tagpath(v):
    v = str(v)
    return v.split("'")[1] if "'" in v else v
def run():
    lb = unreal.load_asset(LB); pr = unreal.load_asset(PR); an = unreal.load_asset(ANIM); me = unreal.load_asset(MESH)
    log("lb=%s pr=%s anim=%s mesh=%s" % (lb, pr, an, me))
    log("skeleton methods: " + ",".join(x for x in dir(lb) if not x.startswith("_") and ("bone" in x.lower() or "compat" in x.lower() or "pose" in x.lower() or "preview" in x.lower())))
    for name, s in (("lb", lb), ("pr", pr)):
        try: log("%s compatible_skeletons=%s" % (name, s.get_editor_property("compatible_skeletons")))
        except Exception as e: log("%s compatible_skeletons read error %s" % (name, e))
        try: log("%s preview mesh=%s" % (name, s.get_preview_mesh()))
        except Exception as e: log("%s preview err %s" % (name, e))
    mb = bones_of_mesh(me); log("mesh bones %d: %s" % (len(mb), mb[:40]))
    try:
        tr = [str(x) for x in unreal.AnimationLibrary.get_animation_track_names(an)]
        log("anim tracks %d: %s" % (len(tr), tr[:40])); log("tracks missing in mesh: %s" % [t for t in tr if t not in mb])
    except Exception as e: log("tracks err %s" % e)
    try: log("anim preview mesh=%s" % an.get_preview_mesh())
    except Exception as e: log("anim preview err %s" % e)
    # project scan
    ar = unreal.AssetRegistryHelpers.get_asset_registry()
    def get(cls):
        f = unreal.ARFilter(package_paths=["/Game"], recursive_paths=True, class_paths=[unreal.TopLevelAssetPath("/Script/Engine", cls)], recursive_classes=True)
        return ar.get_assets(f)
    meshes = get("SkeletalMesh"); anims = get("AnimSequence"); skels = get("Skeleton")
    used = collections.defaultdict(list)
    for m in meshes: used[tagpath(m.get_tag_value("Skeleton"))].append(str(m.package_name))
    byname = collections.defaultdict(list)
    for s in skels: byname[str(s.asset_name)].append(str(s.package_name))
    animskel = collections.Counter(tagpath(a.get_tag_value("Skeleton")) for a in anims)
    log("scan: %d meshes, %d anims, %d skeletons" % (len(meshes), len(anims), len(skels)))
    n = 0
    for sk, cnt in sorted(animskel.items()):
        pkg = sk.split(".")[0]
        if sk in used or pkg.startswith("/Game/SM2_Work/Original") or pkg.startswith("/Game/Work/"): continue
        nm = sk.split(".")[-1]
        same = [p for p in byname.get(nm, []) if p != pkg]
        users = {p: used.get(p + "." + nm, []) for p in same}
        log("NOMESH skeleton %s anims=%d same-name skeletons with meshes: %s" % (sk, cnt, {k: v[:3] for k, v in users.items() if v}))
        n += 1
    log("scan done: %d skeletons used by anims but by no skeletal mesh" % n)
def tick(dt):
    if not st["done"] and time.time() - T0 > 25:
        st["done"] = True
        try: run()
        except Exception as e: log("error %s" % e)
        unreal.SystemLibrary.execute_console_command(None, "QUIT_EDITOR")
st["h"] = unreal.register_slate_post_tick_callback(tick)
