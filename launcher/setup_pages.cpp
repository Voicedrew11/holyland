// The setup's pages (setup_pages.h)
#include "setup_pages.h"

#include <algorithm>
#include <cstdio>
#include <string>

#include "imgui.h"
#include "window.h"

namespace launcher {

namespace {

constexpr float kDropHeightDp = 210.0f;
constexpr float kDropWidthDp = 660.0f;

// a line of text centred on the panel
void centred_text(const ImVec4 &colour, const char *text) {
    float width = ImGui::CalcTextSize(text).x;
    float available = ImGui::GetContentRegionAvail().x;
    if (width < available)
        ImGui::SetCursorPosX(ImGui::GetCursorPosX() + (available - width) * 0.5f);
    ImGui::TextColored(colour, "%s", text);
}

// text centred in a box, at `y` from its top
void text_in_box(ImDrawList *draw, ImVec2 top_left, ImVec2 size, float y, ImFont *font, float font_px,
                 const ImVec4 &colour, const char *text) {
    ImVec2 text_size = font->CalcTextSizeA(font_px, size.x, 0.0f, text);
    draw->AddText(font, font_px, ImVec2(top_left.x + (size.x - text_size.x) * 0.5f, top_left.y + y),
                  ImGui::GetColorU32(colour), text);
}

// a disc, drawn: a ring with a hole, `radius` px, centred on `centre`
void draw_disc_glyph(ImDrawList *draw, ImVec2 centre, float radius, const ImVec4 &colour, float line) {
    ImU32 ink = ImGui::GetColorU32(colour);
    draw->AddCircle(centre, radius, ink, 0, line);
    draw->AddCircle(centre, radius * 0.28f, ink, 0, line);
}

// a button that looks like a link: underlined, amber under the pointer; centred on the panel's line
bool centred_link(const char *label) {
    ImVec2 size = ImGui::CalcTextSize(label);
    ImGui::SetCursorPosX(ImGui::GetCursorPosX() + (ImGui::GetContentRegionAvail().x - size.x) * 0.5f);
    ImVec2 at = ImGui::GetCursorScreenPos();
    bool clicked = ImGui::InvisibleButton(label, size);
    bool hovered = ImGui::IsItemHovered();
    if (hovered)
        ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
    ImU32 ink = ImGui::GetColorU32(hovered ? kAmber : kDim);
    ImDrawList *draw = ImGui::GetWindowDrawList();
    draw->AddText(at, ink, label);
    float underline_y = at.y + size.y + 1.0f;
    draw->AddLine(ImVec2(at.x, underline_y), ImVec2(at.x + size.x, underline_y), ink, 1.0f);
    return clicked;
}

class DiscPage final : public SetupPage {
public:
    explicit DiscPage(DiscKeeper &keeper) : keeper_(keeper) {}

    PageAnswer draw(Window &window) override {
        if (window.take_chooser_failure()) {
            // no chooser through SDL (no desktop portal, no zenity): the launcher's old way, else a typed path
            if (auto chosen = keeper_.pick_disc_the_old_way()) {
                if (offer(*chosen))
                    return PageAnswer::next;
            } else {
                typing_ = true;
            }
        }
        if (auto given = window.take_file()) {
            if (offer(*given))
                return PageAnswer::next;
        }
        draw_drop_area(window);
        draw_needs();
        if (!typing_) {
            draw_error();
            ImGui::Spacing();
            typing_ = centred_link("Type the path instead");
            return PageAnswer::stay;
        }
        return draw_typed_path() ? PageAnswer::next : PageAnswer::stay;
    }

private:
    bool offer(const std::filesystem::path &disc) {
        typed_ = disc.u8string();
        return keeper_.keep_disc(disc);
    }

    // the area to drop the disc on; a click opens the system's chooser. One centred stack: the disc, what to
    // do, how.
    void draw_drop_area(Window &window) {
        float scale = window.scale();
        float width = std::min(kDropWidthDp * scale, ImGui::GetContentRegionAvail().x);
        ImVec2 size(width, kDropHeightDp * scale);
        ImGui::SetCursorPosX(ImGui::GetCursorPosX() + (ImGui::GetContentRegionAvail().x - width) * 0.5f);
        ImVec2 top_left = ImGui::GetCursorScreenPos();
        bool choosing = window.choosing_file();
        if (ImGui::InvisibleButton("##drop", size) && !choosing)
            window.choose_file("King's Field IV disc (.iso, .chd, or a copied disc's SYSTEM.CNF)",
                               "iso;ISO;chd;CHD;cnf;CNF");
        bool over = window.file_over();
        bool lit = over || (ImGui::IsItemHovered() && !choosing);
        if (ImGui::IsItemHovered() && !choosing)
            ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);

        ImDrawList *draw = ImGui::GetWindowDrawList();
        ImVec2 bottom_right(top_left.x + size.x, top_left.y + size.y);
        draw->AddRect(top_left, bottom_right, ImGui::GetColorU32(lit ? kAmber : kFrame), 6.0f * scale, 0,
                      (lit ? 2.5f : 1.5f) * scale);

        ImFont *title = window.title_font();
        ImFont *text = ImGui::GetFont();
        float glyph_radius = 22.0f * scale, title_px = 34.0f * scale, text_px = ImGui::GetFontSize();
        float gap = 14.0f * scale;
        float stack = glyph_radius * 2 + gap + title_px + gap * 0.5f + text_px;
        float y = (size.y - stack) * 0.5f;
        const ImVec4 &ink = lit ? kAmber : ImGui::GetStyle().Colors[ImGuiCol_Text];
        draw_disc_glyph(draw, ImVec2(top_left.x + size.x * 0.5f, top_left.y + y + glyph_radius), glyph_radius, ink,
                        2.0f * scale);
        y += glyph_radius * 2 + gap;
        const char *heading = over ? "Let go to use it" : choosing ? "Choosing..." : "Drop your disc here";
        text_in_box(draw, top_left, size, y, title, title_px, ink, heading);
        y += title_px + gap * 0.5f;
        text_in_box(draw, top_left, size, y, text, text_px, kDim,
                    choosing ? "in the window that opened" : "or click to find it: an .iso, a .chd, or a copied disc's folder");
        ImGui::Spacing();
    }

    // the one disc that works, under the area, in plain text: the thing a player has to get right
    void draw_needs() {
        centred_text(ImGui::GetStyle().Colors[ImGuiCol_Text], "Needs King's Field: The Ancient City, USA release (SLUS-20318)");
        centred_text(kDim, "The Japanese and European releases will not work.");
    }

    void draw_error() {
        if (keeper_.why_not().empty())
            return;
        ImGui::Spacing();
        ImGui::PushTextWrapPos(0.0f);
        centred_text(kEmber, keeper_.why_not().c_str());
        ImGui::PopTextWrapPos();
    }

    // a path typed in, the error under it, the path kept to fix; true once the keeper kept it
    bool draw_typed_path() {
        ImGui::Spacing();
        char buffer[1024];
        std::snprintf(buffer, sizeof(buffer), "%s", typed_.c_str());
        ImGui::SetNextItemWidth(-ImGui::CalcTextSize("Use this").x - ImGui::GetStyle().FramePadding.x * 2 -
                                ImGui::GetStyle().ItemSpacing.x);
        bool entered = ImGui::InputTextWithHint("##path", "/path/to/the disc.iso", buffer, sizeof(buffer),
                                                ImGuiInputTextFlags_EnterReturnsTrue);
        typed_ = buffer;
        ImGui::SameLine();
        bool use = ImGui::Button("Use this") || entered;
        bool kept = use && !typed_.empty() && keeper_.keep_disc(std::filesystem::u8path(typed_));
        if (!keeper_.why_not().empty()) {
            ImGui::PushTextWrapPos(0.0f);
            ImGui::TextColored(kEmber, "%s", keeper_.why_not().c_str());
            ImGui::PopTextWrapPos();
        }
        return kept;
    }

    DiscKeeper &keeper_;
    bool typing_ = false;
    std::string typed_;
};

} // namespace

std::unique_ptr<SetupPage> make_disc_page(DiscKeeper &keeper) {
    return std::make_unique<DiscPage>(keeper);
}

} // namespace launcher
