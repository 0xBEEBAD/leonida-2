# Retail boot advancement - v30

Experimental compatibility branch based on commit 5bc205a.

## Observed boot results

- 52,354 graphics trace events.
- 50,450 successful graphics pipeline creations.
- 42 startup compute pipelines resolved.
- 4,200 successful DirectStorage decompressions.
- 625 command lists created.
- 450 command allocators created.
- 27 core and four PC graphics regression tests passed.
- Packed16 GPU roundtrip and descriptor integration passed.
- Runtime reached approximately 109.5 seconds before exception.

## Remaining limitations

Xbox fence slot 12 requires a live completion-counter address.
AMD MONITORX and MWAITX compatibility remains under development.
The first displayed game frame has not yet been verified.
Depth/UAV allocation and other compatibility paths are experimental.

## Test hardware

- Intel Core i9-14900KF
- NVIDIA GeForce RTX 3080 Ti
- Windows

This branch contains source changes only. Game binaries, assets,
translated game shaders, saves and generated runtime builds
are excluded. The observed boot also depended on a separately
prepared local shader library.
