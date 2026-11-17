"""FX framing check (read-only): opens Niagara folders in a Content Browser tab and captures the window with
BongThumbLive.FXAutoFrame 0 (bounds framing) and 1 (auto-framing on covered pixels), then Status and quit.
Env: BTL_FX_WAIT (s before the first captures of a folder), BTL_FX_FOLDERS (';'-separated content paths), BTL_FX_SHOTS (output dir). Run via btl_launch_bg.ps1.
"""
import os, time, unreal
folders = [f for f in os.environ.get('BTL_FX_FOLDERS', '/Game/FX/01_A_Grade/FxER_StylizedSlash/Niagara/Stylize').split(';') if f]
shots = os.environ.get('BTL_FX_SHOTS', 'C:/Users/Revaptor_FX/AppData/Local/Temp/btl_shots/fxframe')
WAIT = float(os.environ.get('BTL_FX_WAIT', '40'))  # first visit: load + Niagara compile; later visits are quick
os.makedirs(shots, exist_ok=True)
cmd = lambda c: unreal.SystemLibrary.execute_console_command(None, c)
T0 = time.monotonic()
plan = [(8, lambda: (cmd('BongThumbLive.Set Mode VisibleInBrowser'), cmd('BongThumbLive.OpenBrowserTab')))]
t = 10
for i, f in enumerate(folders):
    for mode in (0, 1):
        plan.append((t, (lambda f, m: lambda: (cmd('BongThumbLive.FXAutoFrame %d' % m), cmd('BongThumbLive.ClearMemoryCache'),
                                              unreal.EditorUtilityLibrary.sync_browser_to_folders([f]), cmd('BongThumbLive.ClearBrowserFilters')))(f, mode)))
        # FX scenes are rebuilt when the tile is drawn again; give the effects time to load, warm up and calibrate.
        w = WAIT if mode == 0 else 20  # auto-frame samples are capped per engine frame, a folder needs ~10-15 s
        for d in (w, w + 0.4):
            plan.append((t + d, (lambda n: lambda: cmd('BongThumbLive.CaptureEditorWindow %s/%s.png' % (shots, n)))('f%d_auto%d_%s' % (i, mode, 'a' if d == w else 'b'))))
        plan.append((t + w + 0.5, lambda: cmd('BongThumbLive.Status')))
        t += w + 3
plan.append((t, lambda: cmd('BongThumbLive.FXAutoFrame 1')))
plan.append((t + 1, lambda: cmd('QUIT_EDITOR')))
st = {'i': 0}
def tick(dt):
    age = time.monotonic() - T0
    while st['i'] < len(plan) and age >= plan[st['i']][0]:
        i = st['i']; st['i'] += 1
        try:
            plan[i][1]()
        except Exception as e:
            unreal.log_warning('BTLD step %d error %s' % (i, e))
st['h'] = unreal.register_slate_post_tick_callback(tick)
