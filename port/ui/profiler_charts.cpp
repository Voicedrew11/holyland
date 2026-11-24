// The profiler overlay's charts (profiler_charts.hpp): each an element that keeps its chart and makes its rectangles
// again when the chart or its size changes.
#include "profiler_charts.hpp"

#include <RmlUi/Core/Element.h>
#include <RmlUi/Core/ElementInstancer.h>
#include <RmlUi/Core/Factory.h>
#include <RmlUi/Core/Geometry.h>
#include <RmlUi/Core/Mesh.h>
#include <RmlUi/Core/MeshUtilities.h>
#include <RmlUi/Core/RenderManager.h>

#include <algorithm>
#include <utility>

namespace ui {

namespace {

const Rml::Colourb BACKGROUND(8, 10, 13, 120);
const Rml::Colourb BAR(111, 143, 184, 255);      // a picture (ice, dimmed)
const Rml::Colourb SPIKE(196, 106, 74, 255);     // over the spike line (ember)
const Rml::Colourb MARKED(240, 244, 250, 255);   // the timeline's picture
const Rml::Colourb GPU(217, 179, 106, 230);      // the GPU busy in it
const Rml::Colourb BUDGET(143, 208, 160, 200);   // the screen's time a picture, the target rate
const Rml::Colourb SPIKE_LINE(196, 106, 74, 110);
const Rml::Colourb COLUMN(168, 196, 232, 110);
const Rml::Colourb EDGE(143, 208, 160, 230);     // the timeline's picture edges
const Rml::Colourb TICK(221, 227, 236, 40);      // the timeline's time scale

void quad(Rml::Mesh &mesh, float x, float y, float w, float h, Rml::Colourb colour) {
    if (w <= 0 || h <= 0) {
        return;
    }
    Rml::MeshUtilities::GenerateQuad(mesh, Rml::Vector2f(x, y), Rml::Vector2f(w, h), colour.ToPremultiplied());
}

// An element that draws its own rectangles over its content box
class ChartElement : public Rml::Element {
public:
    using Rml::Element::Element;

protected:
    // The chart's rectangles for a content box of size (pixels), from its top left
    virtual void build(Rml::Mesh &mesh, Rml::Vector2f size) const = 0;
    void changed() {
        stale_ = true;
    }

    void OnRender() override {
        Rml::Vector2f size = GetBox().GetSize(Rml::BoxArea::Content);
        if (stale_ || size != built_size_) {
            Rml::Mesh mesh;
            quad(mesh, 0, 0, size.x, size.y, BACKGROUND);
            build(mesh, size);
            geometry_ = GetRenderManager()->MakeGeometry(std::move(mesh));
            built_size_ = size;
            stale_ = false;
        }
        geometry_.Render(GetAbsoluteOffset(Rml::BoxArea::Content));
    }

private:
    Rml::Geometry geometry_;
    Rml::Vector2f built_size_{-1, -1};
    bool stale_ = true;
};

class BarsElement final : public ChartElement {
public:
    using ChartElement::ChartElement;
    void set(BarsChart chart) {
        chart_ = std::move(chart);
        changed();
    }

private:
    void build(Rml::Mesh &mesh, Rml::Vector2f size) const override {
        const auto &bars = chart_.bars;
        if (bars.empty() || chart_.top_ms <= 0) {
            return;
        }
        size_t shown = std::min(bars.size(), size_t(std::max(size.x / 2.0f, 1.0f)));
        size_t first = bars.size() - shown;
        float bar_w = size.x / float(shown);
        auto height_of = [&](float ms) { return std::min(ms / chart_.top_ms, 1.0f) * size.y; };
        for (size_t i = first; i < bars.size(); i++) {
            const FrameBar &b = bars[i];
            float x = float(i - first) * bar_w, w = std::max(bar_w - 1.0f, 1.0f);
            float h = height_of(b.ms), g = height_of(b.gpu_ms);
            Rml::Colourb colour = int64_t(b.index) == chart_.marked ? MARKED : b.ms > chart_.spike_ms ? SPIKE : BAR;
            quad(mesh, x, size.y - h, w, h, colour);
            quad(mesh, x + bar_w * 0.25f, size.y - g, std::max(bar_w * 0.5f, 1.0f), g, GPU);
        }
        for (auto line : {std::pair(chart_.budget_ms, BUDGET), std::pair(chart_.spike_ms, SPIKE_LINE)}) {
            if (line.first > 0 && line.first < chart_.top_ms) {
                quad(mesh, 0, size.y - height_of(line.first), size.x, 1.0f, line.second);
            }
        }
    }

    BarsChart chart_;
};

class ColumnsElement final : public ChartElement {
public:
    using ChartElement::ChartElement;
    void set(ColumnsChart chart) {
        chart_ = std::move(chart);
        changed();
    }

private:
    void build(Rml::Mesh &mesh, Rml::Vector2f size) const override {
        if (chart_.values.empty() || chart_.top <= 0) {
            return;
        }
        size_t columns = std::max(chart_.columns, chart_.values.size());
        size_t first = columns - chart_.values.size();
        float w = size.x / float(columns);
        for (size_t i = 0; i < chart_.values.size(); i++) {
            float h = std::clamp(chart_.values[i] / chart_.top, 0.0f, 1.0f) * size.y;
            quad(mesh, float(first + i) * w, size.y - h, std::max(w - 1.0f, 1.0f), h, COLUMN);
        }
        if (chart_.target > 0 && chart_.target < chart_.top) {
            quad(mesh, 0, size.y - chart_.target / chart_.top * size.y, size.x, 1.0f, BUDGET);
        }
    }

    ColumnsChart chart_;
};

class TimelineElement final : public ChartElement {
public:
    using ChartElement::ChartElement;
    void set(TimelineChart chart) {
        chart_ = std::move(chart);
        changed();
    }

private:
    void build(Rml::Mesh &mesh, Rml::Vector2f size) const override {
        float row_h = size.y / float(std::max(chart_.rows, 1));
        // (boxes under a pixel wide: one drawn a pixel a row, the rest left out)
        std::vector<float> last_x(size_t(std::max(chart_.rows, 1)), -1e9f);
        for (const TimelineBox &box : chart_.boxes) {
            if (box.row < 0 || box.row >= chart_.rows) {
                continue;
            }
            float x0 = std::clamp(box.from_share, 0.0f, 1.0f) * size.x;
            float x1 = std::clamp(box.to_share, 0.0f, 1.0f) * size.x;
            float &last = last_x[size_t(box.row)];
            if (x1 - x0 < 1.0f && x0 < last + 1.0f) {
                continue;
            }
            last = x1;
            quad(mesh, x0, float(box.row) * row_h, std::max(x1 - x0, 1.0f), std::max(row_h - 1.0f, 1.0f), box.colour);
        }
        for (float share : chart_.ticks) {
            if (share >= 0 && share <= 1) {
                quad(mesh, share * size.x, 0, 1.0f, size.y, TICK);
            }
        }
        for (float share : chart_.lines) {
            if (share >= 0 && share <= 1) {
                quad(mesh, share * size.x, 0, 1.0f, size.y, EDGE);
            }
        }
    }

    TimelineChart chart_;
};

const char *const BARS_TAG = "profile-bars";
const char *const COLUMNS_TAG = "profile-columns";
const char *const TIMELINE_TAG = "profile-timeline";

// (by its tag: only the instancers below make elements of these tags)
template <typename Chart, typename Element> bool set_on(Rml::Element *element, const char *tag, Chart chart) {
    if (element == nullptr || element->GetTagName() != tag) {
        return false;
    }
    static_cast<Element *>(element)->set(std::move(chart));
    return true;
}

} // namespace

void register_profiler_charts() {
    static Rml::ElementInstancerGeneric<BarsElement> bars;
    static Rml::ElementInstancerGeneric<ColumnsElement> columns;
    static Rml::ElementInstancerGeneric<TimelineElement> timeline;
    Rml::Factory::RegisterElementInstancer(BARS_TAG, &bars);
    Rml::Factory::RegisterElementInstancer(COLUMNS_TAG, &columns);
    Rml::Factory::RegisterElementInstancer(TIMELINE_TAG, &timeline);
}

bool set_chart(Rml::Element *element, BarsChart chart) {
    return set_on<BarsChart, BarsElement>(element, BARS_TAG, std::move(chart));
}

bool set_chart(Rml::Element *element, ColumnsChart chart) {
    return set_on<ColumnsChart, ColumnsElement>(element, COLUMNS_TAG, std::move(chart));
}

bool set_chart(Rml::Element *element, TimelineChart chart) {
    return set_on<TimelineChart, TimelineElement>(element, TIMELINE_TAG, std::move(chart));
}

} // namespace ui
