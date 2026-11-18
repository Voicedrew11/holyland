// The hands mod's ImGui side (port/include/port/hands.h): the Gameplay page's section. The cross and the hotkey's
// count are the game's own drawing (game/hands_cross.c), and the equipment screen's "Put in" box too
// (game/hands_put.c).
#include "imgui.h"

extern "C" {
#include "port/controls.h"
#include "port/host.h"
#include "port/platform.h"
#include "port/hands.h"
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
