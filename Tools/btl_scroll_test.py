# Read-only scroll test: no loads by script, no saves. Tag in btl_scroll_tag.txt.
import unreal, time, os
T0 = time.time(); st = {"i": 0}
TMP = "C:/Users/Revaptor_FX/AppData/Local/Temp/"
TAG = open(TMP + "btl_scroll_tag.txt").read().strip()
SHOTS = TMP + "btl_shots/"
FOLDER = "/Game/SM2_Work"
def log(m): unreal.log("BTLD " + m)
def cmd(c): unreal.SystemLibrary.execute_console_command(None, c)
def shot(n):
    log("shot %s t=%.2f" % (n, time.time() - T0)); cmd("BongThumbLive.CaptureEditorWindow " + SHOTS + "scr_%s_%s.png" % (TAG, n))
def setup():
    os.makedirs(SHOTS, exist_ok=True)
    cmd("BongThumbLive.Set Mode VisibleInBrowser"); cmd("BongThumbLive.Set MaxFPS 60")
    unreal.EditorUtilityLibrary.sync_browser_to_folders([FOLDER])
plan = [(20, setup), (80, lambda: cmd("BongThumbLive.Status")), (81, lambda: shot("a0")), (81.3, lambda: shot("a1"))]
t = 85
for where in ("bottom", "top", "bottom"):
    plan.append((t, (lambda w: lambda: (log("scroll %s t=%.2f" % (w, time.time() - T0)), cmd("BongThumbLive.ScrollBrowser " + w)))(where)))
    for d in (1.0, 1.3, 4.0, 4.3, 10.0, 10.3):
        plan.append((t + d, (lambda n: lambda: shot(n))("%s%d_%04.1f" % (where, int(t), d))))
    plan.append((t + 10.6, lambda: cmd("BongThumbLive.Status")))
    t += 14
plan += [(t + 1, lambda: cmd("QUIT_EDITOR"))]
def tick(dt):
    if st["i"] < len(plan) and time.time() - T0 > plan[st["i"]][0]:
        i = st["i"]; st["i"] += 1
        try: plan[i][1]()
        except Exception as e: log("step %d error %s" % (i, e))
st["h"] = unreal.register_slate_post_tick_callback(tick)
