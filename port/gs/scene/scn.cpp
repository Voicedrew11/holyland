// The scene renderer's C entry (scn.h)
#include "scn.h"

#include "pcr.h"
#include "pcr_shell.h"
#include "scn_build.h"

extern "C" int scn_open(void *shared_context) {
    pcr::use_drawing(scn::make_builder, "the scene renderer");
    return pcr_open(shared_context);
}
