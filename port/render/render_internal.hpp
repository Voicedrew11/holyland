// The renderer core's insides, for the code that draws what it holds (the PC renderer: port/gs/pc/).
// C++ only; the game's side uses port/include/port/render.h.
#pragma once
#include <stdint.h>

#include <memory>
#include <vector>

extern "C" {
#include "port/render.h"
}

// One VU1 run of a mesh: what it kicks (the A+D packet at TOP+0xA0, then the strip) and its vertices
struct RdBatch {
    uint64_t tag[2];                 // the strip's GIF tag (NLOOP, PRE and PRIM, FLG, NREG, REGS)
    std::vector<uint64_t> ad;        // the A+D packet's writes kicked before it: register, value, register ...
    uint32_t start;                  // the MSCAL's address that starts it, or RD_PROGRAM_CONTINUE (an MSCNT)
    uint32_t first;                  // its first vertex in RdMeshData::vertices
    uint32_t count;                  // its vertices (the tag's NLOOP)
    // the triangles the GS makes of the strip (or list, or fan), the kicks the data's ADC bits leave out
    // already left out: three vertex numbers each (within the batch), the last the kick's
    std::vector<uint8_t> triangles;
    uint32_t alpha_lo, alpha_hi;     // the least and most alpha of its vertex colours (clamped to 0..255)
    // Program 0x280's (RD_VF_FLOAT_SKIN; render.cpp, take_batch_280): its second, sphere-mapped pass, when
    // the batch has one (TOP+0xA4.z): the A+D packet at TOP+0xA3 and the strip's tag at TOP+1, kicked
    // after the first strip with the same XYZF2s (so the same triangles)
    bool sphere = false;
    uint64_t tag2[2] = {0, 0};
    std::vector<uint64_t> ad2;
    // Program 0x700's (the effects; render.cpp, take_batch): its loop runs n + 1 times, so a "vertex n" past
    // the strip is transformed and clip-tested too, from the position qword at TOP+1+4n, and counts among
    // the clipped vertices that decide whether the strip is kicked at all (vu1_hle_700.c). That qword, as
    // VU1 memory has it when the run starts, if this block wrote it (in this run or an earlier one of the
    // block): past_known; else it is what the packets before the block left there, not known here.
    bool past_known = false;
    int32_t past[4] = {0, 0, 0, 0};
};

// A mesh: a block parsed. Vertices are as VU1 memory has them after VIF1's UNPACK: four qwords each
// (program 0x000: position, normal, ST, colour as 32-bit integers), 16 words a vertex. Program 0x280's
// (RD_VF_FLOAT_SKIN) take eight qwords, 32 words: the four of VU1 memory (floats), then what the vertex
// shader needs of the batch around it (rd_vertex_words; render.cpp, take_batch_280).
struct RdMeshData {
    RdMesh id;
    uint32_t key, gen;
    std::vector<RdBatch> batches;
    std::vector<int32_t> vertices;
    std::vector<uint8_t> block;      // the block's bytes as parsed (for GS captures: rd_capture_poll)
    uint32_t format;                 // its RdVertexFormat
};

// The words a vertex of a mesh of this format takes in RdMeshData::vertices
inline uint32_t rd_vertex_words(uint32_t format) {
    return format == RD_VF_FLOAT_SKIN ? 32u : format == RD_VF_GS_STRIP ? 8u : 16u;
}

// (the vertex oracle, kf4-gsreplay --nd-oracle: the parser's own code on what a VU1 run of program 0x280
// found) The batch at top of VU1 data memory mem (1024 qwords, all taken as written), as take_batch_280
// makes it, appended to mesh; and a block parsed as rd_mesh_from_block does. false: refused (rd_mesh_why)
bool rd_batch_280_from_vu(const uint32_t (*mem)[4], uint32_t top, uint32_t start, RdMeshData &mesh);
// (the oracle, program 0x700) the batch at top as take_batch makes it (RD_VF_PACKED16), all of mem taken as
// written (so past_known is set)
bool rd_batch_from_vu(const uint32_t (*mem)[4], uint32_t top, uint32_t start, RdMeshData &mesh);
bool rd_parse_block(const void *block, uint32_t qwords, RdVertexFormat fmt, RdMeshData &mesh);
// (the same for a run of program 0x5A0: take_batch's silhouette, RD_VF_SILHOUETTE)
bool rd_batch_5a0_from_vu(const uint32_t (*mem)[4], uint32_t top, uint32_t start, RdMeshData &mesh);

// The draw a PORT_GS_REG_NATIVE_DRAW value names, its camera and meshes, if its frame's slot still
// holds it (any thread)
bool rd_fetch(uint64_t value, RdDraw &draw, RdCamera &camera, std::vector<std::shared_ptr<const RdMeshData>> &meshes);
// A pass's words (rd_pass) out of the draw that carries them (RD_PROGRAM_PASS)
void rd_pass_of(const RdDraw &d, RdPass &p);
