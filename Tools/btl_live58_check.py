"""Read-only live check in a real project (no asset writes).

Opens the anim folder in the Content Browser, then logs Status and saves editor
screenshots at fixed delays. Invokes Content Browser tab 1 first (opens it in layouts
that only have the Content Drawer, else just focuses it). Quits the editor at the end.
Env: BTL_LIVE_FOLDER (default PrecisionSniperRifle animations), BTL_LIVE_SHOTS (output dir).
"""
import os, time, unreal
folder = os.environ.get('BTL_LIVE_FOLDER', '/Game/Props/PrecisionSniperRifle/Character/Animations')
shots = os.environ.get('BTL_LIVE_SHOTS', 'C:/Users/Revaptor_FX/AppData/Local/Temp/btl_shots/live58')
os.makedirs(shots, exist_ok=True)
cmd = lambda c: unreal.SystemLibrary.execute_console_command(None, c)
started = time.monotonic()
# Pairs of shots 0.4 s apart show whether tiles move.
plan = [(8, 'open'), (11, 'sync'), (14, 'shot:03'), (19, 'shot:08'), (26, 'shot:15'), (26.4, 'shot:15b'),
        (41, 'shot:30'), (41.4, 'shot:30b'), (43, 'quit')]
state = {'i': 0}
def tick(dt):
    age = time.monotonic() - started
    while state['i'] < len(plan) and age >= plan[state['i']][0]:
        step = plan[state['i']][1]; state['i'] += 1
        if step == 'open':
            cmd('BongThumbLive.OpenBrowserTab')
        elif step == 'sync':
            unreal.EditorUtilityLibrary.sync_browser_to_folders([folder])
            # Type filters (saved engine-wide in %LOCALAPPDATA%/UnrealEngine/Editor/FilterBar.json) can hide
            # every anim tile. Back that file up before the run and restore it afterwards.
            cmd('BongThumbLive.ClearBrowserFilters')
        elif step.startswith('shot:'):
            tag = step[5:]
            cmd('BongThumbLive.Status')
            cmd('BongThumbLive.CaptureEditorWindow %s/live_%s.png' % (shots, tag))
        elif step == 'quit':
            unreal.unregister_slate_post_tick_callback(state['h'])
            cmd('QUIT_EDITOR')
state['h'] = unreal.register_slate_post_tick_callback(tick)
