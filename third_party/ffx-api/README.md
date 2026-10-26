The FidelityFX API's headers (`include/ffx_api/`), from AMD's FidelityFX SDK v1.1.4
(GPUOpen-LibrariesAndSDKs/FidelityFX-SDK, tag v1.1.4, `ffx-api/include/ffx_api`), unchanged; MIT (`LICENSE.txt`).
`--upscaler fsr3` (docs/port/UPSCALING.md, port/gs/native/upscale_ffx.cpp) calls FSR 3.1 through them. The upscaler
itself is AMD's signed `amd_fidelityfx_vk.dll` (the SDK's `PrebuiltSignedDLL`), loaded at run time from beside
kf4.exe and not kept here: CMake's `KF4_FFX_DLL` names one to copy there.
