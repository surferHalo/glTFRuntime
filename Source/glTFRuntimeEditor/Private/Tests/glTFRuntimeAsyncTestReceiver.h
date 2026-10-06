// Copyright 2026, glTFRuntime contributors.

#pragma once

#include "CoreMinimal.h"
#include "UObject/Object.h"
#include "glTFRuntimeAsyncTestReceiver.generated.h"

class UglTFRuntimeAsset;
class UStaticMesh;
class USkeletalMesh;

// Dynamic delegates require a reflected target. This class belongs only to the
// Editor test module; the runtime plugin has no test hooks or test UObject types.
UCLASS(Transient)
class UglTFRuntimeAsyncTestReceiver : public UObject
{
	GENERATED_BODY()

public:
	UFUNCTION()
	void RecordAsset(UglTFRuntimeAsset* InAsset);

	UFUNCTION()
	void RecordMesh(UStaticMesh* InMesh);

	UFUNCTION()
	void RecordSkeletalMesh(USkeletalMesh* InMesh);

	UPROPERTY()
	UglTFRuntimeAsset* Asset = nullptr;

	UPROPERTY()
	UStaticMesh* Mesh = nullptr;

	UPROPERTY()
	USkeletalMesh* SkeletalMesh = nullptr;

	int32 CallbackCount = 0;
	bool bCallbacksOnGameThread = true;
	bool bAssetRootedDuringCallback = false;
};
