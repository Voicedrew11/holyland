# Khronos's synchronization2 layer

`VK_LAYER_KHRONOS_synchronization2` adds `VK_KHR_synchronization2` to a Vulkan driver that does not have it, by
turning its calls into the older barriers and submits. Granite cannot make a device without the extension, so a
driver from before Vulkan 1.3 could not run the game. The game builds the layer into `vk_layers/` beside `kf4` and
turns it on only when a GPU needs it: `port/gs/pc/pcr_sync2.h`, `cmake/vk_sync2_layer.cmake`.

Only the files the layer's build reads are here, unchanged:

| Folder | From | Version |
|---|---|---|
| `extension-layer/` | [Vulkan-ExtensionLayer](https://github.com/KhronosGroup/Vulkan-ExtensionLayer) `layers/` and `utils/` | `v1.4.350` (`8cf7d783e40f6d22a828345210e54cc4289d4ba8`) |
| `utility-libraries/` | [Vulkan-Utility-Libraries](https://github.com/KhronosGroup/Vulkan-Utility-Libraries) `include/` and `src/` (layer settings, safe structs) | `v1.4.358` (`ca91449653e1a29b4b3c1876d7f6e1dcda08cfdd`) |

The Utility Libraries' version matches Granite's Vulkan headers (`third_party/parallel-gs/Granite/third_party/khronos/
vulkan-headers`, 1.4.358), which the layer is built with. When Granite's headers move, move `utility-libraries/` to
the same tag. The safe structs are generated from the headers' registry, so the two must agree. Take
`extension-layer/` from the newest Vulkan-ExtensionLayer tag at or below it.

To update: check out both repositories at the tags, copy the same files over, and build `kf4_vk_sync2_layer`. If the
build reads a new file, `clang++ -MM` on the sources lists it.

Both are Apache-2.0 (`LICENSE`).
