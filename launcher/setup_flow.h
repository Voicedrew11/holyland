// The first start's setup (docs/port/LAUNCHER.md, "First start"): a row of pages under the Holyland logo, one at
// a time. The disc comes first (setup_pages.h); the options a player picks before the first game go after it, each
// a page of its own. The flow knows only the order; each page knows what it asks and where it keeps the answer.
// Main thread.
#pragma once

#include <cstddef>
#include <memory>
#include <vector>

namespace launcher {

class Window;

enum class PageAnswer {
    stay, // still asking
    next, // answered: on to the page after
};

class SetupPage {
public:
    virtual ~SetupPage() = default;
    // a frame of the page, in the window's panel under the logo
    virtual PageAnswer draw(Window &window) = 0;
};

class SetupFlow {
public:
    explicit SetupFlow(std::vector<std::unique_ptr<SetupPage>> pages);

    // every page answered (at once when there are none)
    bool finished() const { return at_ >= pages_.size(); }
    // the page shown; only while not finished
    SetupPage &page() { return *pages_[at_]; }
    std::size_t page_number() const { return at_; } // from 0
    std::size_t page_count() const { return pages_.size(); }

    void answer(PageAnswer answer);
    bool can_go_back() const { return at_ > 0 && !finished(); }
    void back();

private:
    std::vector<std::unique_ptr<SetupPage>> pages_;
    std::size_t at_ = 0;
};

} // namespace launcher
