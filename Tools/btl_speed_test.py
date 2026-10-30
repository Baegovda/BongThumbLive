# Cold-cache speed A/B test (read-only). Config: btl_speed_cfg.txt = tag | folder | console commands separated by ';'
import unreal, time, os
T0 = time.time(); st = {"i": 0, "last": None, "sec": int(time.time()), "fr": 0}
TMP = "C:/Users/Revaptor_FX/AppData/Local/Temp/"
TAG, FOLDER, CV = [x.strip() for x in open(TMP + "btl_speed_cfg.txt").read().split("|")]
SHOTS = TMP + "btl_shots/"
def log(m): unreal.log("BTLD " + m)
def cmd(c): unreal.SystemLibrary.execute_console_command(None, c)
def shot(n): log("shot %s t=%.2f" % (n, time.time() - T0)); cmd("BongThumbLive.CaptureEditorWindow " + SHOTS + "spd_%s_%s.png" % (TAG, n))
def setup():
    os.makedirs(SHOTS, exist_ok=True)
    cmd("BongThumbLive.Set Mode VisibleInBrowser"); cmd("BongThumbLive.Set MaxFPS 60"); cmd("BongThumbLive.TileTrace 1")
    for c in [x for x in CV.split(";") if x.strip()]: cmd(c.strip()); log("cvar " + c.strip())
    unreal.EditorUtilityLibrary.sync_browser_to_folders([FOLDER]); log("opened t=%.2f" % (time.time() - T0)); st["measure"] = True
plan = [(20, setup)]
for k in range(21, 140, 2): plan.append((k, (lambda: (cmd("BongThumbLive.Status")))))
for d in (1, 3, 8, 22): plan.append((20 + d, (lambda n: lambda: shot(n))("open_%02d" % d)))
t = 45
for i, where in enumerate(("bottom", "top", "bottom")):
    plan.append((t, (lambda w: lambda: (log("scroll %s t=%.2f" % (w, time.time() - T0)), cmd("BongThumbLive.ScrollBrowser " + w)))(where)))
    for d in (1, 3, 20): plan.append((t + d, (lambda n: lambda: shot(n))("%d%s_%02d" % (i, where, d))))
    t += 24
plan.append((t + 45, lambda: (cmd("BongThumbLive.Status"), shot("final"))))
plan.append((t + 47, lambda: cmd("QUIT_EDITOR")))
plan.sort(key=lambda p: p[0])
def tick(dt):
    now = time.time()
    if st.get("measure"):
        if st["last"] and now - st["last"] > 0.05: log("HITCH %.0f t=%.2f" % ((now - st["last"]) * 1000, now - T0))
        s = int(now)
        if s != st["sec"]: log("SEC fps=%d t=%.0f" % (st["fr"], now - T0)); st["sec"] = s; st["fr"] = 0
        st["fr"] += 1
    st["last"] = now
    while st["i"] < len(plan) and now - T0 > plan[st["i"]][0]:
        i = st["i"]; st["i"] += 1
        try: plan[i][1]()
        except Exception as e: log("step %d error %s" % (i, e))
st["h"] = unreal.register_slate_post_tick_callback(tick)
