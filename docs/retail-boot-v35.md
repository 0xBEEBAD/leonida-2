# Retail boot advancement - v35

Successor to the published retail-boot-v30 checkpoint.

## Compatibility improvements

- Live Xbox fence completion-counter implementation.
- CPU and GPU queue signalling regression tests.
- Scoped Intel fallback for AMD MONITORX and MWAITX.
- Sampled A8 one-dimensional texture allocation and creation.
- Optional device slot 81 query failure handling.
- Local missing compute shader capture diagnostics.

## Observed results

- 65,646 graphics trace events at 240 seconds.
- 50,466 successful graphics pipeline creations.
- 434 recorded DrawInstanced calls.
- 869 successful ExecuteCommandLists calls.
- 868 successful queue Signal calls.
- Runtime remained active beyond the reporting limit.
- All 31 existing regression tests and special integration tests passed.
- 49 additional compute shader translations passed SPIR-V validation locally.

## Remaining limitations

The engine reaches a repeating rendering loop and attempts PresentX.
Successful visible frame presentation has not been verified.
Additional compute shaders and four failed pipeline creations remain unresolved.
Some compatibility implementations remain experimental.

Game executables, game assets, captured shader bytecode,
translated game shaders and generated runtime binaries are not included.
The observed results depend on a separately prepared local shader library.

## Test hardware

Intel Core i9-14900KF
NVIDIA GeForce RTX 3080 Ti
Windows
