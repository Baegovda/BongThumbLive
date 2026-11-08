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

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FBongAssetTypesRegressionTest, "BongThumbLive.Rendering.AssetTypes", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FBongAssetTypesRegressionTest::RunTest(const FString&)
{
	const TCHAR* Paths[] = {TEXT("/Game/BTLRegression/BTL_Time.BTL_Time"), TEXT("/Game/BTLRegression/BTL_Cascade.BTL_Cascade"), TEXT("/Game/BTLRegression/BTL_Niagara.BTL_Niagara")};
	UTextureRenderTarget2D* Target = NewObject<UTextureRenderTarget2D>();
	Target->AddToRoot();
	Target->InitCustomFormat(64, 64, PF_B8G8R8A8, false);
	Target->UpdateResourceImmediate(true);
	for (const TCHAR* Path : Paths)
	{
		UObject* Asset = LoadObject<UObject>(nullptr, Path);
		if (!Asset) { AddError(FString::Printf(TEXT("Missing disposable rendering fixture %s"), Path)); continue; }
		FAssetCompilingManager::Get().FinishAllCompilation();
		if (GShaderCompilingManager) { GShaderCompilingManager->FinishAllCompilation(); }
		FThumbnailRenderingInfo* Info = UThumbnailManager::Get().GetRenderingInfo(Asset);
		if (!Info || !Info->Renderer) { AddError(TEXT("No thumbnail renderer")); continue; }
		TestTrue(TEXT("Plugin owns each supported fixture renderer"), Info->Renderer->IsA<UBongThumbLiveMaterialRenderer>() || Info->Renderer->IsA<UBongThumbLiveFXRenderer>());
		TestTrue(TEXT("Ready fixture is accepted"), Info->Renderer->CanVisualizeAsset(Asset));
		FTextureRenderTargetResource* Resource = Target->GameThread_GetRenderTargetResource();
		FCanvas Canvas(Resource, nullptr, FGameTime::GetTimeSinceAppStart(), GMaxRHIFeatureLevel);
		Canvas.Clear(FLinearColor::Black);
		Info->Renderer->Draw(Asset, 0, 0, 64, 64, Resource, &Canvas, false);
		Canvas.Flush_GameThread(); FlushRenderingCommands();
		TArray<FColor> Pixels;
		TestTrue(TEXT("Rendered thumbnail can be read back"), Resource->ReadPixels(Pixels));
		TestEqual(TEXT("Full 64x64 thumbnail rendered"), Pixels.Num(), 4096);
	}
	TestTrue(TEXT("Both Niagara and Cascade live scenes were simulated"), UBongThumbLiveFXRenderer::GetNumLiveScenes() >= 2);
	UBongThumbLiveFXRenderer::PruneAllScenes(-1.0);
	Target->RemoveFromRoot();
	return true;
}
#endif
