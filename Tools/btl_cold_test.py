# Read-only cold scroll test in /Game/Props. No asset loads by script, no saves. Tag in btl_cold_tag.txt.
import unreal, time, os
T0 = time.time(); st = {"i": 0}
TMP = "C:/Users/Revaptor_FX/AppData/Local/Temp/"
TAG = open(TMP + "btl_cold_tag.txt").read().strip()
SHOTS = TMP + "btl_shots/"
FOLDER = "/Game/Props"
def log(m): unreal.log("BTLD " + m)
def cmd(c): unreal.SystemLibrary.execute_console_command(None, c)
def shot(n):
    log("shot %s t=%.2f" % (n, time.time() - T0)); cmd("BongThumbLive.CaptureEditorWindow " + SHOTS + "cold_%s_%s.png" % (TAG, n))
def status(): log("status t=%.2f" % (time.time() - T0)); cmd("BongThumbLive.Status"); cmd("BongThumbLive.DumpCache")
def setup():
    os.makedirs(SHOTS, exist_ok=True)
    cmd("BongThumbLive.Set Mode VisibleInBrowser"); cmd("BongThumbLive.Set MaxFPS 60")
    unreal.EditorUtilityLibrary.sync_browser_to_folders([FOLDER]); log("opened t=%.2f" % (time.time() - T0))
plan = [(20, setup)]
for d in (3, 8, 15, 15.3, 25, 25.3):
    plan.append((20 + d, (lambda n: lambda: shot(n))("s0_%04.1f" % d)))
plan.append((45.6, status))
t = 47
for step in range(1, 7):
    plan.append((t, (lambda s: lambda: (log("scroll step %d t=%.2f" % (s, time.time() - T0)), cmd("BongThumbLive.ScrollBrowser 4")))(step)))
    for d in (1.0, 1.3, 5.0, 5.3, 12.0, 12.3):
        plan.append((t + d, (lambda n: lambda: shot(n))("s%d_%04.1f" % (step, d))))
    plan.append((t + 12.6, status))
    t += 14
plan += [(t + 1, lambda: cmd("QUIT_EDITOR"))]
def tick(dt):
    if st["i"] < len(plan) and time.time() - T0 > plan[st["i"]][0]:
        i = st["i"]; st["i"] += 1
        try: plan[i][1]()
        except Exception as e: log("step %d error %s" % (i, e))
st["h"] = unreal.register_slate_post_tick_callback(tick)
