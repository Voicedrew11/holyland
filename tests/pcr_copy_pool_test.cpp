// The PC renderer's surface texture copies (port/gs/pc/pcr_copy_pool.h): an image is given to one draw at a time and
// again once its lease is gone, cleared only when new or when the rectangle copied does not cover what was written,
// and let go when not given for KEPT_TAKES takes.
#include "pcr_copy_pool.h"

#include <cstdio>
#include <memory>

using namespace pcr;

namespace {

int failures = 0;

#define CHECK(cond)                                                                                                    \
    do {                                                                                                               \
        if (!(cond)) {                                                                                                 \
            std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);                                                \
            failures++;                                                                                                \
        }                                                                                                              \
    } while (0)

using Handle = std::shared_ptr<int>; // (an image: let go when the last handle is)
using Pool = CopyPool<Handle>;

const ImageKind frame = {6144, 2048, 91, 1, 0x6};
const ImageKind small = {768, 512, 91, 1, 0x6};

int made = 0;
Handle make() {
    return std::make_shared<int>(++made);
}

void given_again_when_its_lease_is_gone() {
    Pool pool;
    made = 0;
    Pool::Copy a = pool.take(frame, 3840, 1792, make);
    CHECK(a.image && *a.image == 1 && a.clear_first); // (new: cleared)
    Pool::Copy b = pool.take(frame, 3840, 1792, make); // (a's draw not let go yet: another image)
    CHECK(b.image && *b.image == 2 && b.clear_first);
    Pool::Lease kept_by_a_batch_copy = a.lease;
    a = Pool::Copy{};
    Pool::Copy c = pool.take(frame, 3840, 1792, make); // (a copy of a's lease is still held)
    CHECK(c.image && *c.image == 3);
    kept_by_a_batch_copy.reset();
    Pool::Copy d = pool.take(frame, 3840, 1792, make);
    CHECK(d.image && *d.image == 1 && !d.clear_first); // (the same rectangle over the last: no clear)
    CHECK(made == 3 && pool.count() == 3);
}

void by_kind() {
    Pool pool;
    made = 0;
    pool.take(frame, 3840, 1792, make); // (its lease let go at once)
    Pool::Copy s = pool.take(small, 768, 512, make);
    CHECK(s.image && *s.image == 2 && s.clear_first);
    Pool::Copy f = pool.take(frame, 3840, 1792, make);
    CHECK(f.image && *f.image == 1 && !f.clear_first);
}

void cleared_when_the_rectangle_does_not_cover_the_last() {
    Pool pool;
    made = 0;
    CHECK(pool.take(frame, 3840, 1792, make).clear_first);
    CHECK(!pool.take(frame, 3840, 1792, make).clear_first);
    CHECK(!pool.take(frame, 3840, 2048, make).clear_first); // (covers the last)
    CHECK(pool.take(frame, 3840, 1792, make).clear_first);  // (rows of the last would be left)
    CHECK(pool.take(frame, 1024, 1792, make).clear_first);  // (columns of the last would be left)
    CHECK(!pool.take(frame, 1024, 1792, make).clear_first);
    CHECK(!pool.take(frame, 6144, 2048, make).clear_first);
    CHECK(made == 1);
}

void no_memory() {
    Pool pool;
    Pool::Copy none = pool.take(frame, 1, 1, [] { return Handle(); });
    CHECK(!none.image && !none.lease && pool.count() == 0);
}

void let_go_when_unused() {
    Pool pool;
    made = 0;
    std::weak_ptr<int> old = pool.take(frame, 3840, 1792, make).image;
    Pool::Copy held = pool.take(small, 768, 512, make); // (held all along: never let go)
    held = pool.take(small, 768, 512, make);
    for (uint64_t i = 0; i < Pool::KEPT_TAKES - 2; i++) {
        pool.take(small, 768, 512, make);
    }
    CHECK(!old.expired());
    pool.take(small, 768, 512, make);
    pool.take(small, 768, 512, make);
    CHECK(old.expired());
    CHECK(held.image && pool.count() == 2); // (the one held, the one given each take)
    Pool::Copy again = pool.take(frame, 3840, 1792, make);
    CHECK(again.image && again.clear_first); // (made new)
}

} // namespace

int main() {
    given_again_when_its_lease_is_gone();
    by_kind();
    cleared_when_the_rectangle_does_not_cover_the_last();
    no_memory();
    let_go_when_unused();
    if (failures) {
        std::printf("%d failures\n", failures);
        return 1;
    }
    std::printf("pcr_copy_pool: ok\n");
    return 0;
}
