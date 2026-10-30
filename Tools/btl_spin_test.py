# Read-only spinner test (pkg19). No asset loads/saves by script. Tag in btl_spin_tag.txt, folder in btl_spin_folder.txt
import unreal, time, os
T0 = time.time(); st = {"i": 0}
TMP = "C:/Users/Revaptor_FX/AppData/Local/Temp/"
TAG = open(TMP + "btl_spin_tag.txt").read().strip()
FOLDER = open(TMP + "btl_spin_folder.txt").read().strip()
SHOTS = TMP + "btl_shots/"
def log(m): unreal.log("BTLD " + m)
def cmd(c): unreal.SystemLibrary.execute_console_command(None, c)
def shot(n):
    log("shot %s t=%.2f" % (n, time.time() - T0)); cmd("BongThumbLive.CaptureEditorWindow " + SHOTS + "spin_%s_%s.png" % (TAG, n))
def status(n): log("status %s t=%.2f" % (n, time.time() - T0)); cmd("BongThumbLive.Status")
def spin(on):
    try:
        S = unreal.get_default_object(unreal.load_class(None, "/Script/BongThumbLive.BongThumbLiveSettings"))
        S.set_editor_property("show_loading_spinner", on); log("spinner setting -> %s" % on)
    except Exception as e: log("spinner set error %s" % e)
def setup():
    os.makedirs(SHOTS, exist_ok=True); spin(True)
    cmd("BongThumbLive.Set Mode VisibleInBrowser"); cmd("BongThumbLive.Set MaxFPS 60")
    unreal.EditorUtilityLibrary.sync_browser_to_folders([FOLDER]); log("opened t=%.2f" % (time.time() - T0))
plan = [(20, setup)]
for d in (0.3, 1, 3, 8): plan.append((20 + d, (lambda n: lambda: shot(n))("open_%04.1f" % d)))
plan.append((29, lambda: status("open")))
t = 45
for i, where in enumerate(("bottom", "top", "bottom")):
    plan.append((t, (lambda w: lambda: (log("scroll %s t=%.2f" % (w, time.time() - T0)), cmd("BongThumbLive.ScrollBrowser " + w)))(where)))
    for d in (0.3, 1, 3, 16, 20):
        plan.append((t + d, (lambda n: lambda: shot(n))("%d%s_%04.1f" % (i, where, d))))
    plan.append((t + 1.5, (lambda n: lambda: status(n))("%d%s_1.5" % (i, where))))
    plan.append((t + 20.5, (lambda n: lambda: status(n))("%d%s_20" % (i, where))))
    t += 24
plan.append((t, lambda: (spin(False), cmd("BongThumbLive.ClearMemoryCache"))))
plan.append((t + 1, lambda: (log("scroll top off"), cmd("BongThumbLive.ScrollBrowser top"))))
for d in (0.3, 1, 3): plan.append((t + 1 + d, (lambda n: lambda: shot(n))("off_%04.1f" % d)))
plan.append((t + 2.5, lambda: status("off_1.5")))
plan.append((t + 6, lambda: spin(True)))
plan.append((t + 8, lambda: cmd("QUIT_EDITOR")))
def tick(dt):
    if st["i"] < len(plan) and time.time() - T0 > plan[st["i"]][0]:
        i = st["i"]; st["i"] += 1
        try: plan[i][1]()
        except Exception as e: log("step %d error %s" % (i, e))
st["h"] = unreal.register_slate_post_tick_callback(tick)
