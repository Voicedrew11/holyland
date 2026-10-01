# patches/

Patches applied to [PS2Recomp](https://github.com/ran-j/PS2Recomp) by
`scripts/03-build-runner.sh`, in filename order, on top of the commit pinned
as `PS2X_REF` in `scripts/common.sh`.

| Patch | Why |
|---|---|
| `0001-gs-gif-nloop-zero-means-32768.patch` | A GIF tag with `NLOOP=0` means 32768 loops on hardware. The runtime treated it as 0, so a 512 KB texture upload was parsed as register writes and corrupted the display registers (no text or textures). |

PS2Recomp is GPL-3.0, so these patches are derivative works under the same
license. The rest of this repository is MIT (see `../LICENSE`).
