// The setup's pages (setup_pages.h)
#include "setup_pages.h"

#include "window.h"

namespace launcher {

namespace {

// The disc page's parts (launcher.rcss: #drop, #path-row...), given to the setup's #page once
constexpr const char *kDiscPageRml = R"(
<div id="drop" data-action="choose-disc">
	<div class="disc"><div></div></div>
	<h1 id="drop-heading">Drop your disc here</h1>
	<p id="drop-sub" class="dim">or click to find it: an .iso, a .chd, or a copied disc's folder</p>
</div>
<p>Needs King's Field: The Ancient City, USA release (SLUS-20318)</p>
<p class="dim">The Japanese and European releases will not work.</p>
<p id="disc-error" class="error"></p>
<div id="path-row" class="row hidden">
	<input id="path" type="text" data-enter="use-path"/>
	<span class="button" data-action="use-path">Use this</span>
</div>
<p id="path-error" class="error"></p>
<span id="type-link" class="link" data-action="type-path">Type the path instead</span>
)";

class DiscPage final : public SetupPage {
public:
    explicit DiscPage(DiscKeeper &keeper) : keeper_(keeper) {}

    PageAnswer draw(Window &window) override {
        window.set_rml("page", kDiscPageRml);
        if (window.take_chooser_failure()) {
            // no chooser through SDL (no desktop portal, no zenity): the launcher's old way, else a typed path
            if (auto chosen = keeper_.pick_disc_the_old_way()) {
                if (offer(window, *chosen))
                    return PageAnswer::next;
            } else {
                typing_ = true;
            }
        }
        if (auto given = window.take_file()) {
            if (offer(window, *given))
                return PageAnswer::next;
        }
        bool choosing = window.choosing_file();
        if (window.take_action("choose-disc") && !choosing)
            window.choose_file("King's Field IV disc (.iso, .chd, or a copied disc's SYSTEM.CNF)",
                               "iso;ISO;chd;CHD;cnf;CNF");
        if (window.take_action("type-path"))
            typing_ = true;
        if ((window.take_action("use-path")) && !window.field("path").empty() &&
            keeper_.keep_disc(std::filesystem::u8path(window.field("path"))))
            return PageAnswer::next;
        show(window, choosing);
        return PageAnswer::stay;
    }

private:
    bool offer(Window &window, const std::filesystem::path &disc) {
        window.set_field("path", disc.u8string()); // (kept to fix, should the player type it instead)
        return keeper_.keep_disc(disc);
    }

    void show(Window &window, bool choosing) {
        bool over = window.file_over();
        window.set_classes("drop", over ? "over" : choosing ? "choosing" : "");
        window.set_text("drop-heading", over ? "Let go to use it" : choosing ? "Choosing..." : "Drop your disc here");
        window.set_text("drop-sub", choosing ? "in the window that opened"
                                             : "or click to find it: an .iso, a .chd, or a copied disc's folder");
        // the error by where the disc came from: under the typed path while it is open, else under the drop area
        const std::string &why = keeper_.why_not();
        window.set_text("disc-error", typing_ ? "" : why);
        window.set_text("path-error", typing_ ? why : "");
        window.set_classes("path-row", typing_ ? "row" : "row hidden");
        window.set_classes("type-link", typing_ ? "link hidden" : "link");
    }

    DiscKeeper &keeper_;
    bool typing_ = false;
};

} // namespace

std::unique_ptr<SetupPage> make_disc_page(DiscKeeper &keeper) {
    return std::make_unique<DiscPage>(keeper);
}

} // namespace launcher
