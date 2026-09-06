# Maintained glTFRuntime fork

The maintained repository and sole push destination for this fork is
<https://github.com/surferHalo/glTFRuntime.git>, configured as `origin`.
The original project is <https://github.com/rdeioris/glTFRuntime>.
Integrate upstream changes through this fork; consumers must pin a commit that
has already been pushed here. Do not point a consumer's submodule at the original
repository while pinning a fork-only commit.

## Scope and ownership

The fork adds generic native asynchronous loading with cooperative cancellation,
safe in-flight asset GC ownership, and terminal callbacks on recursive mesh
failure. It does not own application scenes, sessions, actors, cache policy, or
network state. Those remain the consuming application's responsibility.

The public native entry points are
`UglTFRuntimeFunctionLibrary::glTFLoadAssetFromFilenameAsyncCancellable` and
`UglTFRuntimeAsset::LoadStaticMeshRecursiveAsyncCancellable`. They share an
`FglTFRuntimeAsyncOperation` across the file and mesh stages. The corresponding
Blueprint entry points use the same implementations without exposing a token.

## Cancellation and lifetime contract

- Start a load on the game thread. `Cancel()` and `IsCancelled()` are thread-safe.
- A token cancelled before entry produces a null callback without allocating an
  asset or mesh. Early rejection may invoke the callback synchronously.
- Active file parsing, primitive decoding and mesh building are not preempted.
  They may finish their current dependency call and allocate temporary resources.
  Cancellation is observed between stages and before game-thread mesh finalization.
- Cancellation observed before finalization prevents resource initialization.
  Finalization that has already started completes, then the token is checked again
  before delivery. A later cancellation cannot revoke an already delivered result.
- Each entry point invokes its bound terminal callback exactly once on the game
  thread, returning null on failure or observed cancellation. A failed recursive
  parse never finalizes the context's preallocated, unbuilt mesh.
- The file loader roots its private asset through callback delivery. A caller
  that needs the result afterward must retain its own strong UObject reference.
  Dynamic delegates do not own their targets: retain the callback target and
  immutable input files until completion, including after cancellation. Keep the
  source asset and mesh Outer alive throughout mesh loading.
- These guarantees require the game-thread task queue to continue running; they
  do not promise callbacks after the engine has terminated.

## Validation

Build the consuming UE Editor target and run
`Automation RunTests glTFRuntime.UnitTests.AsyncLifecycle` with
`-TestExit="Automation Test Queue Empty" -unattended -nop4 -NullRHI -nosound`.
The tests use existing parser delegates to exercise cancellation inside primitive
decoding, after worker mesh building, and inside finalization. They also cover
file success/failure, forced GC during file loading, cancellation before entry
and delivery, malformed scenes/nodes/meshes, missing parsers, exactly one
game-thread callback, and collectible resources after settlement.

Run the broader `glTFRuntime.UnitTests` suite after upstream merges. Consumers
must also validate their own teardown, cache leases, and scene-generation guards.
