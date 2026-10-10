# Host presentation admission: exact backend and host policy

Supply `PS2RECOMP_DIR`, `GS_PRIOR_RUNTIME_DIR` (checked pre-W frontend source)
and `PARALLEL_GS_DIR` explicitly; build outside source trees. Only authored tests
are distributed. Reference frontend and exact candidate UploadFrame body are
derived from actual supplied sources into build scratch with source hash guards.

Eight cases compare complete RGBA plus metadata and all 4 MiB VRAM on the same
actual Vulkan backend: unchanged, mutation, movie parity, mode, pending producer,
reset, concurrent reset and backend replacement. CPU GS is used only for the
frontend's neutral constructor; comparison backends are explicitly Vulkan.
GPU feature rejection/failure is not silently replaced by software/auto fallback.
Candidate/reference admission timing differs intentionally; completed images
must agree. Standalone Flush/Sync and production backend locks are unchanged.

The ninth case compiles the exact production UploadFrame body with authored
host/texture substitutes. It checks initial neutral black, same-tick retries,
unchanged completed image retention and reset blanking; it is host cache policy
proof, not real OpenGL/Vulkan upload conversion proof. The relocated graph passed
all nine CTests on native Windows x64/MSVC 19.44, RelWithDebInfo. The eight backend
cases selected NVIDIA GeForce RTX 4090 explicitly; full RGBA, metadata and VRAM
comparisons passed. The CMake graph selects the DLL CRT before paraLLEl-GS so its
standalone static CRT default does not conflict with the fixture. WSL Dozen and
Linux execution remain unverified here.
Authored GPU cases retain GPL-3.0-only; preparation plumbing GPL-3.0-or-later.
