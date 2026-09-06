#pragma once

#include "CoreMinimal.h"

/**
 * Cooperative cancellation shared by every stage of one native glTFRuntime
 * load. Cancel/IsCancelled are thread-safe; loading APIs start on the game thread.
 * A request cancelled before entry creates no asset/mesh. Running file parsing,
 * primitive decoding and mesh building are not preempted: cancellation is checked
 * between stages and before game-thread mesh finalization. Finalization already
 * entered runs to completion, then cancellation is checked before delivery.
 * Each API invokes its bound terminal callback once on the game thread (possibly
 * inline for an early rejection), with null on failure or observed cancellation.
 * Cancellation after delivery does not revoke a result. Callers must retain their
 * delegate target and worker inputs until that callback, not merely until Cancel.
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
