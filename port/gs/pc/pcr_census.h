// KF4_PC_CENSUS=1 (diagnostic, --renderer pc): what the game sends the GS, counted by kind (pcr_census.cpp)
#pragma once

#include <map>
#include <memory>

#include "pcr_gif.h"

namespace pcr {

class Census {
public:
    Census();
    ~Census();
    void primitive(const GsRegisterFile &file, const GifPrimitive &primitive);
    void upload(const TransferRegisters &transfer, size_t bytes);
    void copy(const TransferRegisters &transfer, const char *kind);
    void other(const char *what);
    void display(uint64_t pmode, uint64_t dispfb1, uint64_t dispfb2);
    void print() const;

private:
    class Tally;
    std::unique_ptr<Tally> tally_;
    std::map<uint32_t, uint32_t> drawn_; // a target's first block to past its last, roughly
};

} // namespace pcr
