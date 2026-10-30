p=r"K:\Unreal Projects\UE_VFX_57\Plugins\BongThumbLive\Source\BongThumbLive\Private\BongThumbLiveModule.cpp"
m=open(p,encoding="utf-8-sig").read()
a="FSlateDrawElement::MakeLines(OutDrawElements, LayerId, Geo, Ring,"
b="FSlateDrawElement::MakeLines(OutDrawElements, LayerId + 1, Geo, Ring,"
assert a in m and b in m
m=m.replace(a,"if (Style == EBongSpinnerStyle::Spin) FSlateDrawElement::MakeLines(OutDrawElements, LayerId, Geo, Ring,",1)
m=m.replace(b,"if (Style == EBongSpinnerStyle::Spin) FSlateDrawElement::MakeLines(OutDrawElements, LayerId + 1, Geo, Ring,",1)
open(p,"w",encoding="utf-8-sig",newline="").write(m); print("ok")
