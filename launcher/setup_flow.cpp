// The first start's setup (setup_flow.h)
#include "setup_flow.h"

namespace launcher {

SetupFlow::SetupFlow(std::vector<std::unique_ptr<SetupPage>> pages) : pages_(std::move(pages)) {}

void SetupFlow::answer(PageAnswer answer) {
    if (answer == PageAnswer::next && !finished())
        at_++;
}

void SetupFlow::back() {
    if (can_go_back())
        at_--;
}

} // namespace launcher
