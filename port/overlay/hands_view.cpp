// The hands mod's ImGui drawing (port/include/port/hands.h): in play, the count of the hotkey's item on its card,
// in the style of the extended stats mod's lines (port/overlay/stats_menus.cpp): its serif, a dark shadow, the
// cross's fade; and the Gameplay page's section. The cross itself is the game's own drawing
// (game/hands_cross.c), and the equipment screen's "Put in" box too (game/hands_put.c).
//
// The places are the HUD's 640 x 448 picture, shown whole at 4:3 and, with widescreen, squeezed into three
// quarters of the window.
#include <cfloat>
#include <cmath>
#include <cstdio>
#include <cstring>

#include "imgui.h"

extern "C" {
#include "port/controls.h"
#include "port/host.h"
#include "port/platform.h"
#include "port/hands.h"
}

namespace {

const ImVec4 colour_name(0.93f, 0.92f, 0.89f, 1.0f);  // the game's text, a little brighter

const unsigned long long FRESH_VBLANKS = 3; // vblanks the cross counts as up after it was last drawn

// The menus' serif, added by the extended stats mod (stats_menus_fonts); else ImGui's own
ImFont *serif() {
    static ImFont *found;
    static bool looked;
    if (!looked) {
        looked = true;
        ImFontAtlas *atlas = ImGui::GetIO().Fonts;
        for (int i = 1; i < atlas->Fonts.Size; i++) {
            const char *name = atlas->Fonts[i]->GetDebugName();
            if (name != nullptr && (std::strstr(name, "times") || std::strstr(name, "Serif")))
                found = atlas->Fonts[i];
        }
    }
    return found != nullptr ? found : ImGui::GetFont();
}

struct Frame {
    float x0, y0, sx, sy; // window = x0 + menu x * sx

    ImVec2 at(float x, float y) const { return ImVec2(std::floor(x0 + x * sx + 0.5f), std::floor(y0 + y * sy + 0.5f)); }
};

// The HUD's: with widescreen squeezed as the menus, but from the window's left edge unless the player keeps the
// HUD in the middle (port/platform/gs.c, PORT_WIDESCREEN_HUD_LEFT)
Frame hud_frame(int x, int y, int width, int height) {
    ImVec2 scale = ImGui::GetIO().DisplayFramebufferScale;
    float fx = scale.x > 0.0f ? scale.x : 1.0f, fy = scale.y > 0.0f ? scale.y : 1.0f;
    float px = (float)x / fx, py = (float)y / fy, pw = (float)width / fx, ph = (float)height / fy;
    if (pw < 32.0f || ph < 32.0f) {
        const ImGuiViewport *vp = ImGui::GetMainViewport();
        px = vp->Pos.x, py = vp->Pos.y, pw = vp->Size.x, ph = vp->Size.y;
    }
    Frame f;
    if (port_widescreen()) {
        f.x0 = !port_options.widescreen_hud_centred ? px : px + pw * 0.125f;
        f.sx = pw * 0.75f / 640.0f;
    } else {
        f.x0 = px;
        f.sx = pw / 640.0f;
    }
    f.y0 = py;
    f.sy = ph / 448.0f;
    return f;
}

ImU32 colour(const ImVec4 &c, float alpha) {
    return ImGui::ColorConvertFloat4ToU32(ImVec4(c.x, c.y, c.z, c.w * alpha));
}

void text(ImDrawList *dl, float size, ImVec2 at, const ImVec4 &c, float alpha, const char *t) {
    float shadow = std::fmax(1.0f, std::floor(size / 14.0f));
    dl->AddText(serif(), size, ImVec2(at.x + shadow, at.y + shadow), colour(ImVec4(0, 0, 0, 0.85f), alpha), t);
    dl->AddText(serif(), size, at, colour(c, alpha), t);
}

float width_of(float size, const char *t) {
    return serif()->CalcTextSizeA(size, FLT_MAX, 0.0f, t).x;
}

bool cross_up() {
    const PortHandsCross &c = port_hands_cross;
    return c.vblank != 0 && port_vblank_count() - c.vblank <= FRESH_VBLANKS && c.count >= 0;
}

// The hotkey's count, at the bottom right of its card, in the HUD's white
void count(ImDrawList *dl, const Frame &f) {
    const PortHandsCross &c = port_hands_cross;
    char number[8];
    std::snprintf(number, sizeof(number), "%d", c.count);
    float size = 15.0f * f.sy;
    ImVec2 corner = f.at(c.count_x, c.count_y);
    text(dl, size, ImVec2(corner.x - width_of(size, number), corner.y - size), colour_name, c.alpha, number);
}

} // namespace

extern "C" int hands_view_visible(void) {
    return port_hands.enabled && cross_up();
}

extern "C" void hands_view_draw(int x, int y, int width, int height) {
    if (!hands_view_visible())
        return;
    count(ImGui::GetBackgroundDrawList(), hud_frame(x, y, width, height));
}

extern "C" void hands_view_mods_section(void) {
    ImGui::SeparatorText("Hands");
    bool on = port_hands.enabled != 0;
    if (ImGui::Checkbox("Hands and hotkeys", &on))
        port_hands_set_enabled(on);
    ImGui::SetItemTooltip("Two weapons in the right hand, two spells in the left, and five hotkeys, shown\n"
                          "as a cross of cards at the bottom left. In play the controller's d-pad swaps\n"
                          "them (Right the weapon, Left the spell, Down and Up the hotkey); on the keyboard\n"
                          "the keys on the Controls page. Select uses the hotkey. Choose what each slot\n"
                          "holds on the equipment screen: Cross on an item asks which slot. (--hands)");
    if (port_hands.forced) {
        ImGui::SameLine();
        ImGui::TextDisabled("(on for this run: command line)");
    }
    if (!on)
        return;
    bool fade = port_hands.fade != 0;
    if (ImGui::Checkbox("Fade the cross when idle", &fade))
        port_hands_set_fade(fade);
    ImGui::SetItemTooltip("After a few seconds with no hand key pressed the cross fades to a quarter.");
}
