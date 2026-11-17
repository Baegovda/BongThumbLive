#if WITH_DEV_AUTOMATION_TESTS
#include "BongThumbLiveFXRenderer.h"
#include "BongThumbLiveModule.h"
#include "AssetCompilingManager.h"
#include "AssetThumbnail.h"
#include "CanvasTypes.h"
#include "Engine/TextureRenderTarget2D.h"
#include "Misc/AutomationTest.h"
#include "RenderingThread.h"
#include "ShaderCompiler.h"
#include "ThumbnailRendering/ThumbnailManager.h"
#include "Particles/ParticleSystem.h"
#include "NiagaraSystem.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FBongAssetTypesRegressionTest, "BongThumbLive.Rendering.AssetTypes", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FBongAssetTypesRegressionTest::RunTest(const FString&)
{
	// The last path is optional: a real Cascade effect from the copied animation fixture project (BTL_Cascade, made by
	// the 5.8 factory, has no emitters and draws nothing).
	const TCHAR* Paths[] = {TEXT("/Game/BTLRegression/BTL_Time.BTL_Time"), TEXT("/Game/BTLRegression/BTL_Cascade.BTL_Cascade"), TEXT("/Game/BTLRegression/BTL_Niagara.BTL_Niagara"),
		TEXT("/Game/Props/PrecisionSniperRifle/Weapons/Rifle/Particles/PS_MuzzleSmoke.PS_MuzzleSmoke")};
	const TCHAR* OptionalPath = Paths[3];
	UTextureRenderTarget2D* Target = NewObject<UTextureRenderTarget2D>();
	Target->AddToRoot();
	Target->InitCustomFormat(64, 64, PF_B8G8R8A8, false);
	Target->UpdateResourceImmediate(true);
	for (const TCHAR* Path : Paths)
	{
		UObject* Asset = LoadObject<UObject>(nullptr, Path, nullptr, Path == OptionalPath ? LOAD_Quiet | LOAD_NoWarn : LOAD_None);
		if (!Asset && Path == OptionalPath) { AddInfo(TEXT("Optional Cascade fixture not present; skipped")); continue; }
		if (!Asset) { AddError(FString::Printf(TEXT("Missing disposable rendering fixture %s"), Path)); continue; }
		FAssetCompilingManager::Get().FinishAllCompilation();
		if (GShaderCompilingManager) { GShaderCompilingManager->FinishAllCompilation(); }
		if (UNiagaraSystem* System = Cast<UNiagaraSystem>(Asset))
		{
			System->PollForCompilationComplete(true); // start a deferred compile request (see IsReadyToSimulate)
			System->WaitForCompilationComplete(true, false);
		}
		FThumbnailRenderingInfo* Info = UThumbnailManager::Get().GetRenderingInfo(Asset);
		if (!Info || !Info->Renderer) { AddError(TEXT("No thumbnail renderer")); continue; }
		TestTrue(TEXT("Plugin owns each supported fixture renderer"), Info->Renderer->IsA<UBongThumbLiveMaterialRenderer>() || Info->Renderer->IsA<UBongThumbLiveFXRenderer>());
		TestTrue(TEXT("Ready fixture is accepted"), Info->Renderer->CanVisualizeAsset(Asset));
		FTextureRenderTargetResource* Resource = Target->GameThread_GetRenderTargetResource();
		// Effects: draw for ~1.5 s of real time so particles exist (each live draw simulates the elapsed time).
		const bool bEffect = Info->Renderer->IsA<UBongThumbLiveFXRenderer>();
		const int32 Draws = bEffect ? 45 : 1;
		int32 MaxLit = 0, MinOpaque = 4096;
		for (int32 i = 0; i < Draws; ++i)
		{
			FCanvas Canvas(Resource, nullptr, FGameTime::GetTimeSinceAppStart(), GMaxRHIFeatureLevel);
			Canvas.Clear(FLinearColor::Black);
			Info->Renderer->Draw(Asset, 0, 0, 64, 64, Resource, &Canvas, false);
			Canvas.Flush_GameThread(); FlushRenderingCommands();
			if (bEffect)
			{
				// Bursts come and go: judge visibility by the most visible moment, opacity by every moment.
				TArray<FColor> Frame;
				if (Resource->ReadPixels(Frame))
				{
					int32 Lit = 0, Opaque = 0;
					for (const FColor& C : Frame) { Lit += FMath::Max3(C.R, C.G, C.B) > 32 ? 1 : 0; Opaque += C.A > 128 ? 1 : 0; }
					MaxLit = FMath::Max(MaxLit, Lit);
					MinOpaque = FMath::Min(MinOpaque, Opaque);
				}
				FPlatformProcess::Sleep(1.0f / 30.0f);
			}
		}
		TArray<FColor> Pixels;
		TestTrue(TEXT("Rendered thumbnail can be read back"), Resource->ReadPixels(Pixels));
		TestEqual(TEXT("Full 64x64 thumbnail rendered"), Pixels.Num(), 4096);
		if (bEffect)
		{
			// The effect must actually be visible (UE 5.8 regressions: no Niagara simulation advance, checker sky sphere,
			// transparent alpha all gave an "empty" thumbnail that the size checks above still accepted).
			const UParticleSystem* PS = Cast<UParticleSystem>(Asset);
			const bool bHasEmitters = !PS || PS->Emitters.Num() > 0;
			AddInfo(FString::Printf(TEXT("%s: max %d lit / min %d opaque of 4096 pixels over %d draws (emitters: %d)"), *Asset->GetName(), MaxLit, MinOpaque, Draws, bHasEmitters ? 1 : 0));
			if (bHasEmitters)
			{
				TestTrue(FString::Printf(TEXT("%s draws visible particles"), *Asset->GetName()), MaxLit >= 20);
			}
			TestTrue(FString::Printf(TEXT("%s tile is opaque"), *Asset->GetName()), MinOpaque >= 4000);
		}
	}
	TestTrue(TEXT("Both Niagara and Cascade live scenes were simulated"), UBongThumbLiveFXRenderer::GetNumLiveScenes() >= 2);
	UBongThumbLiveFXRenderer::PruneAllScenes(-1.0);
	Target->RemoveFromRoot();
	return true;
}
#endif
