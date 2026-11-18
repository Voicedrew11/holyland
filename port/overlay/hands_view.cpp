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

namespace {

// A combo of `names`; returns the one chosen, else `current`
int choose(const char *label, int current, const char *const *names, int count) {
    int chosen = current;
    if (ImGui::BeginCombo(label, names[current])) {
        for (int i = 0; i < count; i++) {
            if (ImGui::Selectable(names[i], i == current))
                chosen = i;
        }
        ImGui::EndCombo();
    }
    return chosen;
}

} // namespace

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

    float percent = port_hands.scale * 100.0f;
    if (ImGui::SliderFloat("Cross size", &percent, PORT_HANDS_SCALE_MIN * 100.0f, PORT_HANDS_SCALE_MAX * 100.0f,
                           "%.0f%%", ImGuiSliderFlags_AlwaysClamp))
        port_hands_set_scale(percent / 100.0f);
    ImGui::SetItemTooltip("The cross's size: its cards, pictures and count together.");

    int corner = choose("Cross corner", (int)port_hands.corner, port_hands_corner_names, PORT_HANDS_CORNER_COUNT);
    if (corner != (int)port_hands.corner)
        port_hands_set_corner((PortHandsCorner)corner);
    ImGui::SetItemTooltip("The corner of the screen the cross is in. Top left puts it under the health and\n"
                          "magic gauges.");

    int layout = choose("Cross layout", (int)port_hands.layout, port_hands_layout_names, PORT_HANDS_LAYOUT_COUNT);
    if (layout != (int)port_hands.layout)
        port_hands_set_layout((PortHandsLayout)layout);
    ImGui::SetItemTooltip("Cross: the hotkey's card, the middle one, a little lower. Row: the three in line.");

    int cards = choose("Card shape", (int)port_hands.cards, port_hands_cards_names, PORT_HANDS_CARDS_COUNT);
    if (cards != (int)port_hands.cards)
        port_hands_set_cards((PortHandsCards)cards);
    ImGui::SetItemTooltip("Tall cards, or square ones.");
}
