// The setup's pages (setup_pages.h)
#include "setup_pages.h"

#include <algorithm>
#include <cstdio>
#include <string>

#include "imgui.h"
#include "window.h"

namespace launcher {

namespace {

constexpr float kDropHeightDp = 250.0f;
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
        return draw_typed_path() ? PageAnswer::next : PageAnswer::stay;
    }

private:
    bool offer(const std::filesystem::path &disc) {
        typed_ = disc.u8string();
        return keeper_.keep_disc(disc);
    }

    // the area to drop the disc on; a click opens the system's chooser
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
        bool lit = window.file_over() || (ImGui::IsItemHovered() && !choosing);
        if (ImGui::IsItemHovered() && !choosing)
            ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);

        ImDrawList *draw = ImGui::GetWindowDrawList();
        ImVec2 bottom_right(top_left.x + size.x, top_left.y + size.y);
        float rounding = 6.0f * scale;
        draw->AddRect(top_left, bottom_right, ImGui::GetColorU32(lit ? kAmber : kFrame), rounding, 0, 1.5f * scale);

        ImFont *title = window.title_font();
        ImFont *text = ImGui::GetFont();
        float title_px = 36.0f * scale, text_px = ImGui::GetFontSize(), small_px = 18.0f * scale;
        const char *heading = window.file_over() ? "Let go to use it" : choosing ? "Choosing..." : "Drop your disc here";
        text_in_box(draw, top_left, size, size.y * 0.24f, title, title_px, ImGui::GetStyle().Colors[ImGuiCol_Text], heading);
        text_in_box(draw, top_left, size, size.y * 0.24f + title_px * 1.4f, text, text_px, kDim,
                    choosing ? "in the window that opened" : "or click to find it");
        text_in_box(draw, top_left, size, size.y - small_px * 3.6f, text, small_px, kDim,
                    "An .iso or .chd image, or a copied disc's folder");
        text_in_box(draw, top_left, size, size.y - small_px * 2.1f, text, small_px, kDim,
                    "King's Field: The Ancient City, USA (SLUS-20318)");

        ImGui::Spacing();
        if (!keeper_.why_not().empty()) {
            ImGui::PushTextWrapPos(0.0f);
            centred_text(kEmber, keeper_.why_not().c_str());
            ImGui::PopTextWrapPos();
        }
    }

    // a path typed in, behind a link; true once the keeper kept it
    bool draw_typed_path() {
        if (!typing_) {
            const char *link = "Type a path instead";
            float width = ImGui::CalcTextSize(link).x;
            ImGui::SetCursorPosX(ImGui::GetCursorPosX() + (ImGui::GetContentRegionAvail().x - width) * 0.5f);
            ImGui::PushStyleColor(ImGuiCol_Text, kDim);
            ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0, 0, 0, 0));
            ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 0.0f);
            ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(0, 0));
            typing_ = ImGui::SmallButton(link);
            ImGui::PopStyleVar(2);
            ImGui::PopStyleColor(2);
            return false;
        }
        char buffer[1024];
        std::snprintf(buffer, sizeof(buffer), "%s", typed_.c_str());
        ImGui::SetNextItemWidth(-ImGui::CalcTextSize("Use this").x - ImGui::GetStyle().FramePadding.x * 2 -
                                ImGui::GetStyle().ItemSpacing.x);
        bool entered = ImGui::InputTextWithHint("##path", "/path/to/the disc.iso", buffer, sizeof(buffer),
                                                ImGuiInputTextFlags_EnterReturnsTrue);
        typed_ = buffer;
        ImGui::SameLine();
        bool use = ImGui::Button("Use this") || entered;
        return use && !typed_.empty() && keeper_.keep_disc(std::filesystem::u8path(typed_));
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
