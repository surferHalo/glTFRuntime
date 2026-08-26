#pragma once

#include "CoreMinimal.h"

/**
 * Cooperative cancellation shared by every stage of one native glTFRuntime
 * load. The handle contains no UObject state and is safe to read from worker
 * threads. Cancellation suppresses later UObject construction/finalization and
 * guarantees that the terminal callback still runs once with a null result.
 */
class GLTFRUNTIME_API FglTFRuntimeAsyncOperation final
{
public:
	void Cancel()
	{
		bCancelled.Store(true);
	}

	bool IsCancelled() const
	{
		return bCancelled.Load();
	}

private:
	TAtomic<bool> bCancelled{ false };
};
