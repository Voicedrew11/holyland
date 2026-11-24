// The profiler overlay's charts (profiler_overlay.hpp): RmlUi elements that draw their own rectangles, filling their
// content box, for what would be too many elements to lay out. Words over them (scales, lane names, zone names) are
// ordinary elements the overlay places on top.
//
//   <profile-bars>      a bar a picture (FrameBar), newest at the right, one every two pixels at most
//   <profile-columns>   a value a column (the pictures shown a second), against a top value
//   <profile-timeline>  rectangles, each in a row of the chart and across a share of its width
//
// register_profiler_charts once after Rml::Initialise, before a document with them loads.
#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

#include <RmlUi/Core/Types.h>

#include "frame_snapshot.hpp"

namespace Rml {
class Element;
}

namespace ui {

void register_profiler_charts();

struct BarsChart {
    std::vector<FrameBar> bars; // oldest first
    float top_ms = 1;           // the chart's height
    float spike_ms = 0;         // longer: a spike's colour
    float budget_ms = 0;        // a line across (0: none)
    int64_t marked = -1;        // this picture's bar is lit (the timeline's)
};

struct ColumnsChart {
    std::vector<float> values; // oldest first, in the rightmost columns
    size_t columns = 0;        // across (fewer than the values: one a value)
    float top = 1;
    float target = 0; // a line across (0: none)
};

struct TimelineBox {
    float from_share, to_share; // across the width, 0 to 1 (clipped to it)
    int row;                    // from the top, of rows
    Rml::Colourb colour;
};

struct TimelineChart {
    int rows = 1;
    std::vector<TimelineBox> boxes;
    std::vector<float> lines; // across shares: upright lines (the picture's edges)
    std::vector<float> ticks; // across shares: faint upright lines (the time scale's)
};

// The chart in `element` (a <profile-bars>, <profile-columns> or <profile-timeline>) set; false if it is another
bool set_chart(Rml::Element *element, BarsChart chart);
bool set_chart(Rml::Element *element, ColumnsChart chart);
bool set_chart(Rml::Element *element, TimelineChart chart);

} // namespace ui
