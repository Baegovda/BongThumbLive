import shutil,io
base=r"K:\Unreal Projects\UE_VFX_57\Plugins\BongThumbLive\Source\BongThumbLive"
sh=base+r"\Public\BongThumbLiveSettings.h"; mc=base+r"\Private\BongThumbLiveModule.cpp"
shutil.copy2(sh,sh+".bak4"); shutil.copy2(mc,mc+".bak12")
def rd(p): return open(p,encoding="utf-8-sig").read()
def wr(p,s): open(p,"w",encoding="utf-8-sig",newline="").write(s)
s=rd(sh); nl="\r\n" if "\r\n" in s else "\n"
enum=nl.join(['UENUM(BlueprintType)','enum class EBongSpinnerStyle : uint8','{','\t/** 원 위를 도는 호 (기본). */','\tSpin UMETA(DisplayName = "회전하는 호"),','\t/** 커졌다 작아지는 원. */','\tPulse UMETA(DisplayName = "맥박 (커졌다 작아짐)"),','\t/** 원을 따라 도는 점 8개. */','\tDots UMETA(DisplayName = "도는 점"),','\t/** 좌우로 오가는 막대. */','\tBar UMETA(DisplayName = "막대 왕복"),','};','',''])
a="/**\r\n * Project Settings" if "\r\n" in s else "/**\n * Project Settings"
assert a in s; s=s.replace(a,enum+a,1)
anchor="\tbool bShowLoadingSpinner = true;"; assert anchor in s
add=nl.join(['','','\t/** 로딩 표시 색입니다. 바로 적용됩니다. */','\tUPROPERTY(config, EditAnywhere, BlueprintReadWrite, Category = "재생", meta = (DisplayName = "로딩 표시 색", EditCondition = "bEnabled && bShowLoadingSpinner", HideAlphaChannel))','\tFColor LoadingSpinnerColor = FColor(255, 210, 0, 255);','','\t/** 로딩 표시 모양입니다. 바로 적용됩니다. */','\tUPROPERTY(config, EditAnywhere, BlueprintReadWrite, Category = "재생", meta = (DisplayName = "로딩 표시 모양", EditCondition = "bEnabled && bShowLoadingSpinner"))','\tEBongSpinnerStyle LoadingSpinnerStyle = EBongSpinnerStyle::Spin;','','\t/** 로딩 표시 움직임 빠르기입니다(1 = 기본). 바로 적용됩니다. */','\tUPROPERTY(config, EditAnywhere, BlueprintReadWrite, Category = "재생", meta = (DisplayName = "로딩 표시 속도", EditCondition = "bEnabled && bShowLoadingSpinner", ClampMin = "0.1", ClampMax = "5.0", UIMin = "0.25", UIMax = "3.0"))','\tfloat LoadingSpinnerSpeed = 1.0f;'])
s=s.replace(anchor,anchor+add,1); wr(sh,s)
m=rd(mc)
old_spin="const float Spin = (float)FMath::Fmod(Now * 1.25, 1.0) * UE_TWO_PI; // 1.25 turns per second"
assert old_spin in m
m=m.replace(old_spin,"""const UBongThumbLiveSettings* SS = GetDefault<UBongThumbLiveSettings>();
		const EBongSpinnerStyle Style = SS->LoadingSpinnerStyle;
		const double Speed = FMath::Clamp((double)SS->LoadingSpinnerSpeed, 0.1, 5.0);
		const FLinearColor Col = FLinearColor(FColor(SS->LoadingSpinnerColor.R, SS->LoadingSpinnerColor.G, SS->LoadingSpinnerColor.B, 255));
		const float Phase = (float)FMath::Fmod(Now * 1.25 * Speed, 1.0); // 0..1, 1.25 cycles per second at speed 1
		const float Spin = Phase * UE_TWO_PI;""",1)
old_draw="""			FSlateDrawElement::MakeLines(OutDrawElements, LayerId + 2, Geo, Arc, ESlateDrawEffect::None, FLinearColor(1.f, 1.f, 1.f, 0.75f * Fade), true, Thick);"""
assert old_draw in m.replace("\r\n","\n")
crlf="\r\n" in m; m=m.replace("\r\n","\n")
new_draw="""			const FLinearColor Fg(Col.R, Col.G, Col.B, 0.9f * Fade);
			switch (Style)
			{
			case EBongSpinnerStyle::Pulse:
			{
				const float P = 0.5f - 0.5f * FMath::Cos(Phase * UE_TWO_PI);
				TArray<FVector2f> Circ;
				for (int32 i = 0; i <= 32; ++i)
				{
					const float A = (float)i / 32.0f * UE_TWO_PI;
					Circ.Add(C + FVector2f(FMath::Cos(A), FMath::Sin(A)) * (R * (0.35f + 0.65f * P)));
				}
				FSlateDrawElement::MakeLines(OutDrawElements, LayerId + 2, Geo, Circ, ESlateDrawEffect::None, FLinearColor(Col.R, Col.G, Col.B, (0.4f + 0.55f * (1.0f - P)) * Fade), true, Thick);
				break;
			}
			case EBongSpinnerStyle::Dots:
			{
				for (int32 k = 0; k < 8; ++k)
				{
					const float A = (float)k / 8.0f * UE_TWO_PI;
					const float Lag = FMath::Fmod(Phase - (float)k / 8.0f + 1.0f, 1.0f); // head dot brightest
					const FVector2f P0 = C + FVector2f(FMath::Cos(A), FMath::Sin(A)) * R;
					TArray<FVector2f> Dot; Dot.Add(P0 - FVector2f(Thick * 0.5f / Scale, 0.f)); Dot.Add(P0 + FVector2f(Thick * 0.5f / Scale, 0.f));
					FSlateDrawElement::MakeLines(OutDrawElements, LayerId + 2, Geo, Dot, ESlateDrawEffect::None, FLinearColor(Col.R, Col.G, Col.B, (0.15f + 0.8f * (1.0f - Lag)) * Fade), true, Thick * 1.6f);
				}
				break;
			}
			case EBongSpinnerStyle::Bar:
			{
				const float X = FMath::Sin(Phase * UE_TWO_PI) * R * 0.6f;
				TArray<FVector2f> Track; Track.Add(C + FVector2f(-R, 0.f)); Track.Add(C + FVector2f(R, 0.f));
				TArray<FVector2f> Seg; Seg.Add(C + FVector2f(X - R * 0.4f, 0.f)); Seg.Add(C + FVector2f(X + R * 0.4f, 0.f));
				FSlateDrawElement::MakeLines(OutDrawElements, LayerId + 2, Geo, Track, ESlateDrawEffect::None, FLinearColor(Col.R, Col.G, Col.B, 0.25f * Fade), true, Thick);
				FSlateDrawElement::MakeLines(OutDrawElements, LayerId + 2, Geo, Seg, ESlateDrawEffect::None, Fg, true, Thick);
				break;
			}
			default:
				FSlateDrawElement::MakeLines(OutDrawElements, LayerId + 2, Geo, Arc, ESlateDrawEffect::None, Fg, true, Thick);
				break;
			}"""
m=m.replace(old_draw,new_draw,1)
if crlf: m=m.replace("\n","\r\n")
wr(mc,m); print("ok")
