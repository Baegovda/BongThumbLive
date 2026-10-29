import unreal, time
T0 = time.time(); st = {"i": 0}
def cmd(c): unreal.SystemLibrary.execute_console_command(None, c)
plan = [(20, lambda: (cmd("BongThumbLive.Set Mode VisibleInBrowser"), unreal.EditorUtilityLibrary.sync_browser_to_folders(["/Game/SM2_Work/Tests/sani_before/hologram_planet/localbake"]))),
        (50, lambda: cmd("BongThumbLive.Status")), (52, lambda: cmd("QUIT_EDITOR"))]
def tick(dt):
    if st["i"] < len(plan) and time.time() - T0 > plan[st["i"]][0]:
        i = st["i"]; st["i"] += 1
        try: plan[i][1]()
        except Exception as e: unreal.log("BTLV error %s" % e)
st["h"] = unreal.register_slate_post_tick_callback(tick)
