#include "runtime/vblank_field_pacer.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <iostream>
#include <random>
#include <stdexcept>
#include <string>
#include <vector>

using namespace std::chrono_literals;
using Time = VBlankFieldPacer::TimePoint;
constexpr auto period = 16667us;
constexpr auto duration = 500us;
constexpr auto clockTick = VBlankFieldPacer::Duration{1};
uint64_t checks = 0;

void require(bool value, const char *message)
{
    ++checks;
    if (!value) throw std::runtime_error(message);
}

Time at(int64_t microseconds) { return Time{} + std::chrono::microseconds(microseconds); }
int64_t us(Time value) { return std::chrono::duration_cast<std::chrono::microseconds>(value.time_since_epoch()).count(); }

VBlankFieldPacer fresh()
{
    VBlankFieldPacer result;
    result.reset(Time{}, period, duration);
    return result;
}

void fixedCases()
{
    auto pacer = fresh();
    require(pacer.firstStart() == at(16667), "First field preserves original period");
    pacer.recordStart(at(0), 1); // Last gameplay parity at the start of this example.
    pacer.recordStart(at(25000), 0);
    require(pacer.nextStartAfter(at(25000), 1) == at(33334), "Late opposite field must reach gameplay at 33.334ms");
    require(at(25000) + period == at(41667), "Prior rebase adds avoidable full field wait");

    pacer = fresh();
    pacer.recordStart(at(25000), 1);
    const Time opposite = pacer.nextStartAfter(at(25000), 0);
    require(opposite == at(41667), "First unseen opposite parity retains one complete field period");
    pacer.recordStart(opposite, 0);
    const Time gameplay = pacer.nextStartAfter(opposite, 1);
    require(gameplay == at(58334), "Late gameplay field receives no shortened next same-parity interval");
    require(gameplay != at(66668), "Parity bound is not rounded to another nominal slot");

    pacer = fresh();
    pacer.recordStart(at(100000), 1);
    require(pacer.nextStartAfter(at(100000), 0) == at(116667), "First unseen parity after a large overrun is not snapped to a grid");
    pacer.recordStart(at(116667), 0);
    require(pacer.nextStartAfter(at(116667), 1) == at(133334), "Normal parity spacing resumes without replaying missed fields");

    pacer = fresh();
    pacer.recordStart(at(0), 0);
    pacer.recordStart(at(33200), 1);
    require(pacer.nextStartAfter(at(33200), 0) == at(33700) + clockTick, "Start strictly follows old End by one clock tick");
    pacer.recordStart(at(32834), 1); // End exactly at the next nominal slot.
    require(pacer.nextStartAfter(at(32834), 0) == at(33334) + clockTick, "Next Start strictly follows End when the parity limit is equal");
    pacer.recordStart(at(32833), 1); // End one microsecond before that slot.
    require(pacer.nextStartAfter(at(32833), 0) == at(33334), "One microsecond before End boundary keeps valid phase slot");

    pacer = fresh();
    pacer.recordStart(at(0), 1);
    pacer.recordStart(at(35000), 0);
    require(pacer.nextStartAfter(at(35000), 1) == at(35500) + clockTick,
            "35ms of work recovers after End instead of waiting until the50.001ms grid slot");
    pacer.recordStart(at(100000), 0);
    require(pacer.nextStartAfter(at(100000), 1) == at(100500) + clockTick,
            "A large overrun adds no invented fields or extra grid-slot wait");

    pacer.recordStart(at(200000), 0);
    pacer.reset(at(700000), period, duration);
    require(pacer.firstStart() == at(716667), "Lifecycle reset establishes a fresh epoch");
    pacer.recordStart(at(716667), 1);
    require(pacer.nextStartAfter(at(716667), 0) == at(733334), "Lifecycle reset discards previous parity timestamps");
}

struct Event
{
    Time emitted;
    unsigned parity;
    Time end;
    Time next;
    uint64_t guestCycle;
};

std::vector<Event> simulate(const std::vector<int64_t> &work, unsigned presentationRate)
{
    auto pacer = fresh();
    Time now{}, deadline = pacer.firstStart();
    std::array<Time, 2> last{};
    std::array<bool, 2> haveLast{};
    std::vector<Event> events;
    events.reserve(work.size());
    uint64_t tick = 0, cycle = 0;
    // Exact production cycle offsets (ceil(microseconds *294912000/1e6)).
    constexpr uint64_t periodCycles = (16667ull * 294912000ull + 999999ull) / 1000000ull;
    constexpr uint64_t endCycles = (500ull * 294912000ull + 999999ull) / 1000000ull;
    for (int64_t busy : work)
    {
        // Host presentation reads must not mutate the independent guest pacer.
        // 0 represents uncapped presentation: many extra reads between fields.
        const unsigned reads = presentationRate == 0 ? 64u : std::max(1u, presentationRate / 60u);
        if (tick)
            for (unsigned read = 0; read < reads; ++read)
                require(pacer.nextStartAfter(now, unsigned((tick + 1u) & 1u)) == deadline,
                        "Host presentation reads changed the guest deadline");
        now = std::max(now + std::chrono::microseconds(busy), deadline);
        const unsigned parity = unsigned(++tick & 1u);
        pacer.recordStart(now, parity);
        if (haveLast[parity])
            require(now - last[parity] >= period * 2, "Same parity exceeded30Hz");
        last[parity] = now;
        haveLast[parity] = true;
        const Time end = now + duration;
        const Time next = pacer.nextStartAfter(now, parity ^ 1u);
        require(next > end, "New Start overtakes VBlankEnd");
        require(next > now, "Catch-up Start was queued immediately");
        const uint64_t currentCycle = cycle + periodCycles;
        require(currentCycle + endCycles < currentCycle + periodCycles, "Exact guest End/Start cycle order");
        cycle = currentCycle;
        events.push_back({now, parity, end, next, cycle});
        deadline = next;
    }
    require(tick == work.size(), "Missed host slots created extra guest ticks");
    require(cycle == tick * periodCycles, "Missed slots changed exact guest period cycles");
    return events;
}

void sameEvents(const std::vector<Event> &a, const std::vector<Event> &b)
{
    require(a.size() == b.size(), "Host presentation rate changed emitted tick count");
    for (size_t index = 0; index < a.size(); ++index)
        require(a[index].emitted == b[index].emitted && a[index].parity == b[index].parity &&
            a[index].end == b[index].end && a[index].next == b[index].next && a[index].guestCycle == b[index].guestCycle,
            "60/300/uncapped presentation changed independent field cadence");
}

void workloads()
{
    std::vector<std::vector<int64_t>> cases = {
        std::vector<int64_t>(2000, 0), std::vector<int64_t>(2000, 1),
        std::vector<int64_t>(2000, 16667), std::vector<int64_t>(2000, 25000),
        std::vector<int64_t>(2000, 100000)
    };
    std::vector<int64_t> alternating, recovery;
    for (unsigned i = 0; i < 4000; ++i)
    {
        alternating.push_back(i & 1u ? 0 : 25000);
        recovery.push_back(i % 101u < 20u ? 83336 : (i % 101u < 40u ? 25000 : 0));
    }
    cases.push_back(alternating);
    cases.push_back(recovery);
    std::mt19937 random(0x60f1e1du);
    std::vector<int64_t> randomized;
    randomized.reserve(200000);
    for (unsigned i = 0; i < 200000; ++i)
    {
        static constexpr int64_t boundary[] = {0, 1, 499, 500, 501, 16166, 16167, 16666,
            16667, 16668, 25000, 32833, 32834, 33200, 33333, 33334, 33335, 66668, 100000, 10000000};
        randomized.push_back(i & 1u ? random() % 200001u : boundary[random() % std::size(boundary)]);
    }
    cases.push_back(randomized);
    for (const auto &work : cases)
    {
        const auto at60 = simulate(work, 60);
        sameEvents(at60, simulate(work, 300));
        sameEvents(at60, simulate(work, 0));
    }
    const auto constant = simulate(std::vector<int64_t>(2000, 0), 60);
    for (size_t i = 0; i < constant.size(); ++i)
        require(constant[i].emitted == at(int64_t(i + 1u) * 16667), "Zero work did not retain60Hz nominal phase");
    const auto alternating25 = simulate(alternating, 60);
    for (size_t i = 3; i < alternating25.size(); i += 2)
        require(alternating25[i].emitted - alternating25[i - 2].emitted == period * 2,
                "Repeated late opposite fields recreated additive full-field waiting");

    for (const int64_t busy : {34000, 35000, 40000})
    {
        std::vector<int64_t> repeated(4000);
        for (size_t i = 1; i < repeated.size(); i += 2)
            repeated[i] = busy;
        const auto at60 = simulate(repeated, 60);
        sameEvents(at60, simulate(repeated, 300));
        sameEvents(at60, simulate(repeated, 0));
        for (size_t i = 2; i < at60.size(); i += 2)
            require(at60[i].emitted - at60[i - 2].emitted == std::chrono::microseconds(busy) + duration + clockTick,
                    "Repeated34-40ms work was quantized to another16.667ms host slot");
    }
}

int main(int argc, char **argv)
{
    try
    {
        if (argc == 2 && std::string(argv[1]) == "--legacy-control")
        {
            // The exact previous production rule is max(planned,emitted)+T.
            const Time oldNext = std::max(at(16667), at(25000)) + period;
            require(oldNext != at(33334), "Legacy-control no longer detects additive field wait");
            std::cout << "PASS legacy rebase produces41667us instead of33334us\n";
            return 0;
        }
        if (argc == 2 && std::string(argv[1]) == "--grid-control")
        {
            auto pacer = fresh();
            pacer.recordStart(at(0), 1);
            pacer.recordStart(at(35000), 0);
            const Time gridNext = at(0) + period * ((at(35000) + duration - at(0)) / period + 1);
            require(gridNext == at(50001), "Previous grid-control rule changed");
            require(pacer.nextStartAfter(at(35000), 1) == at(35500) + clockTick,
                    "Actual helper retained avoidable grid quantization");
            require(gridNext > pacer.nextStartAfter(at(35000), 1), "Grid control did not reproduce unnecessary waiting");
            std::cout << "PASS grid quantization produces 50001 us instead of 35500 us plus one clock tick\n";
            return 0;
        }
        if (argc != 1) throw std::runtime_error("Unknown argument");
        fixedCases();
        workloads();
        std::cout << "PASS field pacing checks=" << checks << " randomized-work-events=200000 rates=60,300,uncapped\n";
        return 0;
    }
    catch (const std::exception &error)
    {
        std::cerr << "FAIL " << error.what() << '\n';
        return 1;
    }
}
