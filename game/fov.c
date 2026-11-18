/* fov.c: PC-only. The game's side of --fov (port_fov_scale(), port/include/port/host.h).

   The game's own projection (GameLoop_SetCameraMatrices) has a 640-pixel-wide picture, 2048 as its
   centre and 512 as the distance the picture plane is from the eye: the horizontal field is then
   2 atan(320 / 512) = 64 degrees, and its vertical field 2 atan(224 / 512) = 47.25. Scaling the
   projection's x and y by the same factor changes both fields together, as a zoom; --widescreen
   scales x alone, widening the same height to 16:9.

   port_fov_scale() turns the chosen horizontal field (at 4:3) into that factor: the tangent of half
   the game's field over the tangent of half the chosen one. 64 is exactly 1, so a run with the
   default sends the game the same matrices as before. */
#include "port/platform.h"

float cosf(float);
float sinf(float);

#define FOV_HALF_TANGENT 0.625f /* tan(32 degrees), the game's own: 320 / 512 */

float port_fov_scale(void) {
    float half;

    if (port_options.fov <= 0 || port_options.fov == 64) {
        return 1.0f;
    }
    half = (float)port_options.fov * 0.5f * 0.017453292f;
    return FOV_HALF_TANGENT * cosf(half) / sinf(half);
}
