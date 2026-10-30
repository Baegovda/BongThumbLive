import unreal, time, os
T0=time.time(); st={"i":0}
TMP="C:/Users/Revaptor_FX/AppData/Local/Temp/"; SHOTS=TMP+"btl_shots/spin24b/"
def log(m): unreal.log("BTLD "+m)
def cmd(c): unreal.SystemLibrary.execute_console_command(None,c)
def S(): return unreal.get_default_object(unreal.load_class(None,"/Script/BongThumbLive.BongThumbLiveSettings"))
def shot(n): log("shot %s t=%.2f"%(n,time.time()-T0)); cmd("BongThumbLive.CaptureEditorWindow "+SHOTS+n+".png")
def setp(style=None,color=None,speed=None):
    s=S()
    try:
        if style: s.set_editor_property("loading_spinner_style", getattr(unreal.BongSpinnerStyle,style))
        if color: s.set_editor_property("loading_spinner_color", unreal.Color(r=color[0],g=color[1],b=color[2],a=255))
        if speed: s.set_editor_property("loading_spinner_speed", speed)
        log("set %s %s %s -> %s %s %s"%(style,color,speed,s.get_editor_property("loading_spinner_style"),s.get_editor_property("loading_spinner_color"),s.get_editor_property("loading_spinner_speed")))
    except Exception as e: log("set error %s"%e)
def setup():
    os.makedirs(SHOTS,exist_ok=True); s=S(); s.set_editor_property("show_loading_spinner",True)
    log("defaults color=%s style=%s speed=%s"%(s.get_editor_property("loading_spinner_color"),s.get_editor_property("loading_spinner_style"),s.get_editor_property("loading_spinner_speed")))
    cmd("BongThumbLive.Set Mode VisibleInBrowser"); unreal.EditorUtilityLibrary.sync_browser_to_folders(["/Game/Props/PrecisionSniperRifle/Character/Animations"]); log("opened")
seq=[("SPIN",None,None,"a_spin_yellow"),("PULSE",None,None,"b_pulse"),("DOTS",None,None,"c_dots"),("BAR",None,None,"d_bar"),("SPIN",(255,40,40),None,"e_spin_red"),("SPIN",(60,160,255),3.0,"f_spin_blue_speed3")]
plan=[(20,setup)]
t=21
for rnd,where in enumerate(("open","bottom","top")):
    if where!="open": plan.append((t,(lambda w:lambda:cmd("BongThumbLive.ScrollBrowser "+w))(where))); t+=0.5
    for st_,col,sp,name in seq:
        plan.append((t,(lambda a,b,c:lambda:setp(a,b if b else (255,210,0),c if c else 1.0))(st_,col,sp)))
        plan.append((t+0.4,(lambda n:lambda:shot(n))("%d_%s"%(rnd,name))))
        plan.append((t+0.6,(lambda n:lambda:shot(n))("%d_%s_b"%(rnd,name))))
        t+=1.0
    t+=1
plan.append((t,lambda:(setp("SPIN",(255,210,0),1.0),cmd("BongThumbLive.Status"),log("DONE"))))
def tick(dt):
    if st["i"]<len(plan) and time.time()-T0>plan[st["i"]][0]:
        i=st["i"]; st["i"]+=1
        try: plan[i][1]()
        except Exception as e: log("step %d error %s"%(i,e))
st["h"]=unreal.register_slate_post_tick_callback(tick)

