# Mark the two hologram_planet skeletons compatible (both directions) if the anim's bone tracks all exist on the mesh.
import unreal, time
T0 = time.time(); st = {"done": False}
LB = "/Game/SM2_Work/Tests/sani_before/hologram_planet/localbake/hologram_planet_Skeleton"
PR = "/Game/SM2_Work/Props/hologram_planet/hologram_planet_Skeleton"
ANIM = "/Game/SM2_Work/Tests/sani_before/hologram_planet/localbake/hologram_planet__anim"
MESH = "/Game/SM2_Work/Tests/sani_before/hologram_planet/localbake/hologram_planet"
def log(m): unreal.log("BTLF " + m)
def run():
    lb = unreal.load_asset(LB); pr = unreal.load_asset(PR); an = unreal.load_asset(ANIM); me = unreal.load_asset(MESH)
    c = unreal.new_object(unreal.SkeletalMeshComponent); c.set_skeletal_mesh_asset(me)
    mb = [str(c.get_bone_name(i)).lower() for i in range(c.get_num_bones())]
    tr = [str(x).lower() for x in unreal.AnimationLibrary.get_animation_track_names(an)]
    missing = [t for t in tr if t not in mb]
    log("mesh bones %d, anim tracks %d, tracks missing on mesh (case-insensitive): %s; mesh-only bones: %s" % (len(mb), len(tr), missing, [b for b in mb if b not in tr]))
    if missing:
        log("NOT FIXING: bone mismatch"); return
    lb.add_compatible_skeleton(pr); pr.add_compatible_skeleton(lb)
    log("compat lb=%s pr=%s" % (lb.get_editor_property("compatible_skeletons"), pr.get_editor_property("compatible_skeletons")))
    log("save lb=%s pr=%s" % (unreal.EditorAssetLibrary.save_loaded_asset(lb, False), unreal.EditorAssetLibrary.save_loaded_asset(pr, False)))
def tick(dt):
    if not st["done"] and time.time() - T0 > 25:
        st["done"] = True
        try: run()
        except Exception as e: log("error %s" % e)
        unreal.SystemLibrary.execute_console_command(None, "QUIT_EDITOR")
st["h"] = unreal.register_slate_post_tick_callback(tick)
