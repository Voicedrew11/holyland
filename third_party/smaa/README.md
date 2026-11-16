# SMAA (third party)

Jorge Jimenez et al.'s SMAA, from https://github.com/iryoku/smaa at commit
`71c806a838bdd7d517df19192a20f0c61b3ca29d` (2013-11-06), MIT (`LICENSE.txt`). Copied unchanged:

- `SMAA.hlsl`: the method, included by `port/gs/pc/shaders/pcr_smaa.h` as GLSL (`SMAA_GLSL_4`);
- `Textures/AreaTex.h`, `Textures/SearchTex.h`: its lookup textures, uploaded by `port/gs/pc/pcr_antialias.cpp`.

Used by the PC renderer's SMAA (`PORT_AA_SMAA`). Take a newer commit by copying the three files again
(the textures and the shader change together) and naming the commit here.
