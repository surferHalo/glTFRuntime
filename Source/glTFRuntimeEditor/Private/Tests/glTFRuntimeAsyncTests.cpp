// Copyright 2026, glTFRuntime contributors.

#include "glTFRuntimeAsyncTestReceiver.h"
#include "glTFRuntimeEditor.h"
#include "glTFRuntimeFunctionLibrary.h"
#include "Engine/StaticMesh.h"
#include "Misc/AutomationTest.h"
#include "UObject/UObjectHash.h"
#include "UObject/UObjectIterator.h"

void UglTFRuntimeAsyncTestReceiver::RecordAsset(UglTFRuntimeAsset* InAsset)
{
	++CallbackCount;
	bCallbacksOnGameThread &= IsInGameThread();
	Asset = InAsset;
	bAssetRootedDuringCallback = InAsset && InAsset->IsRooted();
}

void UglTFRuntimeAsyncTestReceiver::RecordMesh(UStaticMesh* InMesh)
{
	++CallbackCount;
	bCallbacksOnGameThread &= IsInGameThread();
	Mesh = InMesh;
}

#if WITH_DEV_AUTOMATION_TESTS
namespace
{
struct FAsyncLoadTestState
{
	TStrongObjectPtr<UglTFRuntimeAsyncTestReceiver> Receiver{
		NewObject<UglTFRuntimeAsyncTestReceiver>() };
	TStrongObjectPtr<UglTFRuntimeAsset> SourceAsset;
	TWeakObjectPtr<UglTFRuntimeAsset> PendingFileAsset;
	TSharedRef<FglTFRuntimeAsyncOperation, ESPMode::ThreadSafe> Operation =
		MakeShared<FglTFRuntimeAsyncOperation, ESPMode::ThreadSafe>();
	TAtomic<bool> bPrimitiveReached{ false };
	TAtomic<bool> bBuildReached{ false };
	int32 FinalizeCount = 0;
	FDelegateHandle PrimitiveHook;
	FDelegateHandle BuildHook;
	FDelegateHandle FinalizeHook;

	void RemoveHooks()
	{
		FglTFRuntimeParser::OnPreLoadedPrimitive.Remove(PrimitiveHook);
		FglTFRuntimeParser::OnPostCreatedStaticMesh.Remove(BuildHook);
		FglTFRuntimeParser::OnPreInitStaticMeshResources.Remove(FinalizeHook);
	}
};

class FWaitForAsyncLoad final : public IAutomationLatentCommand
{
public:
	FWaitForAsyncLoad(FAutomationTestBase* InTest, const FString& InCase,
		TSharedRef<FAsyncLoadTestState, ESPMode::ThreadSafe> InState)
		: Test(InTest), Case(InCase), State(InState),
		Deadline(FPlatformTime::Seconds() + 15.0)
	{
	}

	bool Update() override
	{
		if (State->Receiver->CallbackCount == 0)
		{
			if (FPlatformTime::Seconds() < Deadline)
			{
				CollectGarbage(RF_NoFlags);
				return false;
			}
			Test->AddError(TEXT("Async load never delivered its terminal callback: ") + Case);
			State->Operation->Cancel();
			State->RemoveHooks();
			return true;
		}
		// Give the callback's stack and GC context settlement a further tick.
		if (!bObservedCallback)
		{
			bObservedCallback = true;
			return false;
		}

		State->RemoveHooks();
		Test->TestEqual(TEXT("Exactly one terminal callback"), State->Receiver->CallbackCount, 1);
		Test->TestTrue(TEXT("Callback runs on game thread"), State->Receiver->bCallbacksOnGameThread);
		const bool bFile = Case.StartsWith(TEXT("File"));
		const bool bSuccess = Case == TEXT("FileSuccessWithGC") || Case == TEXT("MeshSuccess");
		UObject* Result = bFile
			? static_cast<UObject*>(State->Receiver->Asset)
			: static_cast<UObject*>(State->Receiver->Mesh);
		if (bSuccess) Test->TestNotNull(TEXT("Successful load returns an object"), Result);
		else Test->TestNull(TEXT("Failed or cancelled load returns null"), Result);

		const bool bFinalizationEntered = Case == TEXT("MeshSuccess")
			|| Case == TEXT("MeshCancelledInsideFinalize");
		Test->TestEqual(TEXT("Only a successful, uncancelled build can enter finalization"),
			State->FinalizeCount, bFinalizationEntered ? 1 : 0);
		if (Case == TEXT("MeshCancelledDuringPrimitive"))
		{
			Test->TestTrue(TEXT("Cancellation was exercised inside primitive decoding"), State->bPrimitiveReached.Load());
			Test->TestFalse(TEXT("Cancellation stops the next mesh build stage"), State->bBuildReached.Load());
		}
		if (Case == TEXT("MeshCancelledBeforeFinalize"))
		{
			Test->TestTrue(TEXT("Worker build completed before cancellation"), State->bBuildReached.Load());
		}
		if (Case == TEXT("MeshCancelledBeforeStart"))
		{
			TArray<UObject*> Children;
			GetObjectsWithOuter(State->Receiver.Get(), Children);
			Test->TestEqual(TEXT("Pre-cancelled mesh load allocates no mesh"), Children.Num(), 0);
		}
		if (Case == TEXT("FileSuccessWithGC"))
		{
			Test->TestTrue(TEXT("Asset stays rooted through callback"), State->Receiver->bAssetRootedDuringCallback);
		}
		if (State->PendingFileAsset.IsValid())
		{
			Test->TestFalse(TEXT("Settled file load removes its temporary root"), State->PendingFileAsset->IsRooted());
		}

		TArray<UObject*> MeshChildren;
		GetObjectsWithOuter(State->Receiver.Get(), MeshChildren);
		TArray<TWeakObjectPtr<UObject>> WeakMeshChildren;
		for (UObject* Child : MeshChildren) WeakMeshChildren.Add(Child);
		MeshChildren.Reset();
		State->Receiver->Asset = nullptr;
		State->Receiver->Mesh = nullptr;
		State->SourceAsset.Reset();
		CollectGarbage(RF_NoFlags);
		Test->TestFalse(TEXT("Settled file asset is collectible"), State->PendingFileAsset.IsValid());
		for (const auto& Child : WeakMeshChildren)
		{
			Test->TestFalse(TEXT("Settled mesh resources are collectible"), Child.IsValid());
		}
		return true;
	}

private:
	FAutomationTestBase* Test;
	FString Case;
	TSharedRef<FAsyncLoadTestState, ESPMode::ThreadSafe> State;
	double Deadline;
	bool bObservedCallback = false;
};
}

IMPLEMENT_COMPLEX_AUTOMATION_TEST(FglTFRuntimeAsyncLifecycleTests,
	"glTFRuntime.UnitTests.AsyncLifecycle",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

void FglTFRuntimeAsyncLifecycleTests::GetTests(TArray<FString>& Names, TArray<FString>& Commands) const
{
	for (const TCHAR* Case : {
		TEXT("FileSuccessWithGC"), TEXT("FileFailure"), TEXT("FileConcurrentGC"),
		TEXT("FileCancelledBeforeStart"), TEXT("FileCancelledBeforeCallback"),
		TEXT("MeshSuccess"), TEXT("MeshMissingScene"), TEXT("MeshMissingNode"),
		TEXT("MeshBadTree"), TEXT("MeshMissingMesh"), TEXT("MeshBadPrimitive"),
		TEXT("MeshMissingParser"), TEXT("MeshCancelledBeforeStart"),
		TEXT("MeshCancelledDuringPrimitive"), TEXT("MeshCancelledBeforeFinalize"),
		TEXT("MeshCancelledInsideFinalize") })
	{
		Names.Add(Case);
		Commands.Add(Case);
	}
}

bool FglTFRuntimeAsyncLifecycleTests::RunTest(const FString& Case)
{
	if (Case == TEXT("FileConcurrentGC"))
	{
		// Exercise concurrent parser registration and cancelled-load settlement
		// while the game thread repeatedly collects, without retaining the loaders.
		for (int32 Index = 0; Index < 24; ++Index)
		{
			RunTest(Index % 2 == 0 ? TEXT("FileSuccessWithGC") : TEXT("FileCancelledBeforeCallback"));
		}
		return true;
	}
	const auto State = MakeShared<FAsyncLoadTestState, ESPMode::ThreadSafe>();
	if (Case.Contains(TEXT("CancelledBeforeStart"))) State->Operation->Cancel();
	if (Case.StartsWith(TEXT("File")))
	{
		glTFRuntime::Tests::FFixturePath Fixture(
			Case == TEXT("FileFailure") ? TEXT("missing-async-fixture.gltf") : TEXT("Triangle.gltf"));
		if (Case == TEXT("FileFailure"))
		{
			AddExpectedError(TEXT("Unable to open file ") + Fixture.Path,
				EAutomationExpectedErrorFlags::Contains, 1);
		}
		FglTFRuntimeConfig Config;
		Config.RuntimeContextString = FGuid::NewGuid().ToString();
		FglTFRuntimeHttpResponse Completed;
		Completed.BindDynamic(State->Receiver.Get(), &UglTFRuntimeAsyncTestReceiver::RecordAsset);
		UglTFRuntimeFunctionLibrary::glTFLoadAssetFromFilenameAsyncCancellable(
			Fixture.Path, false, Config, Completed, State->Operation);
		// The loader's asset is intentionally private until completion. Observe it
		// weakly so this test cannot accidentally provide the missing GC ownership.
		for (TObjectIterator<UglTFRuntimeAsset> It; It; ++It)
		{
			if (It->RuntimeContextString == Config.RuntimeContextString) State->PendingFileAsset = *It;
		}
		if (Case == TEXT("FileCancelledBeforeStart"))
		{
			TestFalse(TEXT("Pre-cancelled file load allocates no asset"), State->PendingFileAsset.IsValid());
		}
		else
		{
			TestTrue(TEXT("In-flight file asset can be observed without owning it"), State->PendingFileAsset.IsValid());
			if (Case == TEXT("FileCancelledBeforeCallback")) State->Operation->Cancel();
			CollectGarbage(RF_NoFlags);
			TestTrue(TEXT("In-flight file asset survives forced GC"), State->PendingFileAsset.IsValid());
		}
	}
	else
	{
		glTFRuntime::Tests::FFixturePath Fixture(
			Case == TEXT("MeshBadPrimitive") ? TEXT("BadMesh.gltf")
			: Case == TEXT("MeshBadTree") ? TEXT("BadScene.gltf") : TEXT("Triangle.gltf"));
		UglTFRuntimeAsset* Asset = nullptr;
		if (Case == TEXT("MeshMissingParser"))
		{
			Asset = NewObject<UglTFRuntimeAsset>();
			AddExpectedError(TEXT("No glTF Asset loaded."), EAutomationExpectedErrorFlags::Contains, 1);
		}
		else if (Case == TEXT("MeshMissingScene") || Case == TEXT("MeshMissingMesh"))
		{
			const FString Json = Case == TEXT("MeshMissingScene")
				? TEXT("{\"asset\":{\"version\":\"2.0\"}}")
				: TEXT("{\"asset\":{\"version\":\"2.0\"},\"nodes\":[{\"mesh\":99}],\"scenes\":[{\"nodes\":[0]}]}");
			Asset = UglTFRuntimeFunctionLibrary::glTFLoadAssetFromString(Json, FglTFRuntimeConfig());
		}
		else
		{
			Asset = UglTFRuntimeFunctionLibrary::glTFLoadAssetFromFilename(Fixture.Path, false, FglTFRuntimeConfig());
		}
		if (!TestNotNull(TEXT("Fixture creates an asset"), Asset)) return false;
		State->SourceAsset.Reset(Asset);
		FglTFRuntimeParser* Parser = Asset->GetParser().Get();
		State->PrimitiveHook = FglTFRuntimeParser::OnPreLoadedPrimitive.AddLambda(
			[State, Parser, Case](TSharedRef<FglTFRuntimeParser> InParser, TSharedRef<FJsonObject>, FglTFRuntimePrimitive&)
			{
				if (&InParser.Get() != Parser) return;
				State->bPrimitiveReached.Store(true);
				if (Case == TEXT("MeshCancelledDuringPrimitive")) State->Operation->Cancel();
			});
		State->BuildHook = FglTFRuntimeParser::OnPostCreatedStaticMesh.AddLambda(
			[State, Parser, Case](FglTFRuntimeStaticMeshContextRef Context)
			{
				if (&Context->Parser.Get() != Parser) return;
				State->bBuildReached.Store(true);
				if (Case == TEXT("MeshCancelledBeforeFinalize")) State->Operation->Cancel();
			});
		State->FinalizeHook = FglTFRuntimeParser::OnPreInitStaticMeshResources.AddLambda(
			[State, Parser, Case](FglTFRuntimeStaticMeshContextRef Context)
			{
				if (&Context->Parser.Get() != Parser) return;
				++State->FinalizeCount;
				if (Case == TEXT("MeshCancelledInsideFinalize")) State->Operation->Cancel();
			});
		FglTFRuntimeStaticMeshConfig Config;
		Config.Outer = State->Receiver.Get();
		Config.bAllowCPUAccess = true;
		FglTFRuntimeStaticMeshAsync Completed;
		Completed.BindDynamic(State->Receiver.Get(), &UglTFRuntimeAsyncTestReceiver::RecordMesh);
		Asset->LoadStaticMeshRecursiveAsyncCancellable(
			Case == TEXT("MeshMissingNode") ? TEXT("missing-node") : TEXT(""),
			{}, Completed, Config, State->Operation);
	}
	AddCommand(new FWaitForAsyncLoad(this, Case, State));
	return true;
}
#endif
