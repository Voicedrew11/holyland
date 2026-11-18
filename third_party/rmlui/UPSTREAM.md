# RmlUi

[RmlUi](https://github.com/mikke89/RmlUi) 6.3 (tag `6.3`, commit `ba95ffe8bfb6370efb2cdcca927eaad4710c5413`),
copied as it is. MIT licence: see `LICENSE.txt`.

The player menu (`port/ui/`): documents and style sheets laid out and drawn as triangles, which
`port/ui/ui_render.cpp` records and the Vulkan presenter draws. Built by `cmake/rmlui.cmake` with its FreeType font
engine (`third_party/freetype`).

Kept: `Include/RmlUi/{Core,Config,Core.h}` (with `Core/Containers/`: robin_hood and itlib, MIT, their licence in
`Containers/LICENSE.txt`) and `Source/Core`. Left out: the debugger, Lua, SVG and Lottie plugins,
the backends, samples, tests and its CMake files. To update: copy the same folders from a newer release, change the
tag above, check `cmake/rmlui.cmake`'s source list against `Source/Core/CMakeLists.txt`, and run `ui_*` tests.
