// The automap mod's drawing (port/include/port/automap.h, docs/port/AUTOMAP.md): a corner minimap while
// playing and the full map on the Map key (M), drawn with Dear ImGui over the game's picture, in the look of
// the KF3 port's automap (kfii-recomp, patches/Map/AutomapView*.cs): walkable floors filled and shaded by
// their height against the player's (lower is darker), a light band where they meet walls, water blue, lava
// orange, poison green, the void hatched grey, markers with icons, and a label for the marker under the mouse.
//
// The full map holds the game while it is open (port_automap_holds_game) and takes the mouse: the wheel
// zooms toward the cursor, a left drag moves, a right click fits the map again. The map key or Escape closes
// it. Everything here runs on the game's thread, inside the overlay's ImGui frame.
#include <SDL3/SDL.h>

#include <algorithm>
#include <cfloat>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "imgui.h"

extern "C" {
#include "port/automap.h"
#include "port/controls.h"
#include "port/platform.h"
#include "port/player_ui.h"
}

#include "automap_view.h"

namespace {

// ---- Looks ---------------------------------------------------------------------------------------------
ImU32 col(int r, int g, int b, float a = 1.0f) {
    a = a < 0.0f ? 0.0f : a > 1.0f ? 1.0f : a;
    return IM_COL32(r, g, b, (int)(a * 255.0f + 0.5f));
}

ImU32 fade(ImU32 c, float alpha) {
    if (alpha >= 0.999f)
        return c;
    return (c & 0x00FFFFFFu) | ((ImU32)((float)(c >> 24) * alpha) << 24);
}

struct Palette {
    ImU32 wall = col(215, 200, 160), water = col(50, 88, 140, 0.92f), fire = col(215, 75, 20, 0.95f);
    ImU32 poison = col(80, 150, 55, 0.92f), hurt = col(165, 60, 150, 0.92f);
    ImU32 death = col(46, 46, 50, 0.92f), hatch = col(78, 78, 84, 0.9f);
    ImU32 door = col(190, 120, 50), locked = col(210, 60, 50), chest = col(230, 170, 60);
    ImU32 person = col(255, 60, 170), outline = col(0, 0, 0, 0.8f), save = col(80, 220, 230);
    ImU32 exit = col(250, 250, 255), secret = col(170, 90, 220), item = col(250, 220, 90);
    ImU32 enemy = col(235, 50, 50), barrel = col(150, 95, 50), barrel_rim = col(225, 190, 140);
    ImU32 bone = col(225, 220, 200), stone = col(170, 170, 175), trap = col(220, 50, 40);
    ImU32 lever = col(120, 190, 240), pedestal = col(240, 200, 80), passage = col(205, 150, 255);
    ImU32 text = col(240, 230, 210), frame = col(200, 180, 130);
};
const Palette palette;

// Screen up is the world's +z, as on the game's own picture maps (the Caretaker's Map puts the player at
// y = 273 - z * k: MapViewer_LocateOnCaretakersMap); the King's Map turns with the player instead.
const float ZSIGN = -1.0f;

// ---- State --------------------------------------------------------------------------------------------
bool full_was_open;
bool shot_open;           // KF4_OVERLAY_SHOT's map
int shown_browsed = -1;   // the map the full map showed last (port_automap_browsing), to fit it again when it changes
std::vector<std::string> test_browse; // KF4_AUTOMAP_BROWSE: a step for each test picture of the full map
size_t test_browse_next;
bool test_browse_read;

// the markers drawn: the player's map's, or the browsed map's on the full map
const PortAutomapMarker *shown_list;
int shown_count;
float zoom = 1.0f;        // the full map's zoom (1 fits what has been seen)
float centre_x, centre_z; // its centre, in world units
bool centred;             // moved by the player from the fitted centre
bool test_hover_read;
bool test_hover;
float test_hover_x, test_hover_z;

// The levels. Passages at different heights overlap on a plan seen from above; the level shown (the
// player's, his slice) is drawn in full, the others greyed and darkened the further they are from its
// height, and the slice's edges cast a dark shadow onto the levels beside and under it, so that a passage
// running under another does not look joined to it. A slice is the floor connected to the player's: from
// the layers under his feet to the layers of the cells beside them within a step of CLIMB (as the reveal's
// rays walk the floor), staying within SLICE_BAND of his height; walls bound it. On the full map PgUp and
// PgDn (or Shift with the wheel) show the levels above and below, Home his own again.
const int CLIMB = 150;      // the most the floor rises or falls from cell to cell within a level
const int SLICE_BAND = 700; // and at most this far from the level's height

int view_level;      // the full map's level: 0 the player's, n levels above (+) or below (-) it
int view_level_y;    // its height, worked out when view_level changes
long long view_level_seed; // and the cell its slice grows from
bool view_level_set; // view_level_y is worked out
bool test_level_read;
int test_level;      // KF4_AUTOMAP_LEVEL: the level the test picture of the full map shows

// the colours of the cells, kept while nothing they depend on changes
struct Cache {
    unsigned version = ~0u;
    int map = -1;
    int band = 0x7FFFFFFF;
    int reveal = -1;
    long long seed = -2;        // the player's cell the slice grew from (-1: from the height alone)
    std::vector<ImU32> colours; // 0: nothing drawn
    std::vector<uint8_t> chasm; // cells hatched as the void
    std::vector<uint8_t> open;  // cells drawn as floor (for the edges)
    std::vector<uint8_t> level; // 1: what the cell shows is of the slice
    std::vector<int16_t> shown_y; // the height of what the cell shows (NO_Y: nothing)
    int x0, z0, x1, z1;         // what is drawn, in cells (x1 < x0: nothing)
} cache;

// ---- The cells ------------------------------------------------------------------------------------------
int chosen_layer(const PortAutomapPlan *p, int i, int j, int py) {
    const PortAutomapLayer *l = &p->layers[((size_t)j * p->width + i) * PORT_AUTOMAP_LAYERS];
    int last = -1;

    for (int n = 0; n < PORT_AUTOMAP_LAYERS && l[n].y != PORT_AUTOMAP_NO_Y; n++) {
        if (l[n].flags & PORT_AUTOMAP_FAR)
            continue;
        if (l[n].y >= py - 400)
            return n;
        last = n;
    }
    return last;
}

bool layer_visible(const PortAutomapPlan *p, size_t cell, int n) {
    return port_automap.reveal || (p->seen[cell] & (1u << n));
}

ImU32 floor_colour(int y, int py) {
    int d = y - py; // positive is lower
    int step = (int)std::lround((double)d / 350.0);
    step = step < -4 ? -4 : step > 4 ? 4 : step;
    int k = -step * 7;
    auto clamp = [](int v, int lo, int hi) { return v < lo ? lo : v > hi ? hi : v; };
    return col(clamp(78 + k, 20, 150), clamp(70 + k, 18, 140), clamp(56 + k, 14, 120), 0.9f);
}

// a colour of another level than the one shown: greyed and darkened, the more the further it is; a hazard
// (water, lava, poison) keeps more of its colour, to be told apart still
ImU32 other_level(ImU32 c, int dy, bool hazard) {
    float k = dy < 700 ? 0.62f : dy < 1400 ? 0.5f : 0.4f, keep = hazard ? 0.75f : 0.45f;
    if (hazard)
        k = k + (1.0f - k) * 0.35f;
    float r = (float)(c & 0xFF), g = (float)((c >> 8) & 0xFF), b = (float)((c >> 16) & 0xFF);
    float grey = (r + g + b) / 3.0f;
    r = (grey + (r - grey) * keep) * k;
    g = (grey + (g - grey) * keep) * k;
    b = (grey + (b - grey) * keep) * k;
    return IM_COL32((int)r, (int)g, (int)b, (int)(c >> 24));
}

// the slice at height y: the layers connected to the seeds (the layers within CLIMB of y in the seed cell
// and the cells around it; with no seed cell, or none there, every such layer of the map) without climbing
// more than CLIMB from cell to cell or leaving SLICE_BAND of y; a wall or the void is reached, not crossed
std::vector<uint8_t> slice_of(const PortAutomapPlan *p, long long seed_cell, int y) {
    size_t cells = (size_t)p->width * p->height;
    std::vector<uint8_t> in(cells * PORT_AUTOMAP_LAYERS, 0);
    std::vector<uint32_t> todo;
    auto passable = [](const PortAutomapLayer &l) { return !(l.flags & PORT_AUTOMAP_WALL) && l.kind != PORT_AUTOMAP_DEATH; };
    auto seed = [&](size_t cell) {
        const PortAutomapLayer *l = &p->layers[cell * PORT_AUTOMAP_LAYERS];
        for (int n = 0; n < PORT_AUTOMAP_LAYERS && l[n].y != PORT_AUTOMAP_NO_Y; n++)
            if (!(l[n].flags & PORT_AUTOMAP_FAR) && passable(l[n]) && std::abs(l[n].y - y) <= CLIMB) {
                in[cell * PORT_AUTOMAP_LAYERS + n] = 1;
                todo.push_back((uint32_t)(cell * PORT_AUTOMAP_LAYERS + n));
            }
    };
    if (seed_cell >= 0) {
        int si = (int)(seed_cell % p->width), sj = (int)(seed_cell / p->width);
        for (int dj = -1; dj <= 1; dj++)
            for (int di = -1; di <= 1; di++)
                if (si + di >= 0 && sj + dj >= 0 && si + di < p->width && sj + dj < p->height)
                    seed((size_t)(sj + dj) * p->width + si + di);
    }
    if (todo.empty())
        for (size_t cell = 0; cell < cells; cell++)
            seed(cell);
    const int di[4] = {1, -1, 0, 0}, dj[4] = {0, 0, 1, -1};
    while (!todo.empty()) {
        uint32_t node = todo.back();
        todo.pop_back();
        const PortAutomapLayer &l = p->layers[node];
        if (!passable(l))
            continue;
        size_t cell = node / PORT_AUTOMAP_LAYERS;
        int i = (int)(cell % p->width), j = (int)(cell / p->width);
        for (int k = 0; k < 4; k++) {
            int ii = i + di[k], jj = j + dj[k];
            if (ii < 0 || jj < 0 || ii >= p->width || jj >= p->height)
                continue;
            size_t o = (size_t)jj * p->width + ii;
            const PortAutomapLayer *m = &p->layers[o * PORT_AUTOMAP_LAYERS];
            for (int n = 0; n < PORT_AUTOMAP_LAYERS && m[n].y != PORT_AUTOMAP_NO_Y; n++) {
                size_t to = o * PORT_AUTOMAP_LAYERS + n;
                if (in[to] || (m[n].flags & PORT_AUTOMAP_FAR) || std::abs(m[n].y - l.y) > CLIMB || std::abs(m[n].y - y) > SLICE_BAND)
                    continue;
                in[to] = 1;
                todo.push_back((uint32_t)to);
            }
        }
    }
    return in;
}

void rebuild_cache(const PortAutomapPlan *p, int py, long long seed_cell) {
    size_t cells = (size_t)p->width * p->height;
    cache.colours.assign(cells, 0);
    cache.chasm.assign(cells, 0);
    cache.open.assign(cells, 0);
    cache.level.assign(cells, 0);
    cache.shown_y.assign(cells, PORT_AUTOMAP_NO_Y);
    cache.x0 = p->width;
    cache.z0 = p->height;
    cache.x1 = cache.z1 = -1;
    std::vector<uint8_t> slice = slice_of(p, seed_cell, py);
    std::vector<int8_t> shown(cells, -1);
    for (int j = 0; j < p->height; j++) {
        for (int i = 0; i < p->width; i++) {
            size_t cell = (size_t)j * p->width + i;
            const PortAutomapLayer *l = &p->layers[cell * PORT_AUTOMAP_LAYERS];
            // the cell's layer of the slice, if it has one that has been seen
            int n = -1, best_d = 0x7FFFFFFF;
            for (int m = 0; m < PORT_AUTOMAP_LAYERS && l[m].y != PORT_AUTOMAP_NO_Y; m++)
                if (slice[cell * PORT_AUTOMAP_LAYERS + m] && layer_visible(p, cell, m) && std::abs(l[m].y - py) < best_d) {
                    n = m;
                    best_d = std::abs(l[m].y - py);
                }
            if (n >= 0) {
                shown[cell] = (int8_t)n;
                cache.level[cell] = 1;
                continue;
            }
            n = chosen_layer(p, i, j, py);
            if (n < 0)
                continue;
            if (!layer_visible(p, cell, n)) {
                // another layer of the cell that has been seen, nearest the height shown
                int best = -1;
                for (int m = 0; m < PORT_AUTOMAP_LAYERS && l[m].y != PORT_AUTOMAP_NO_Y; m++) {
                    if ((l[m].flags & PORT_AUTOMAP_FAR) || !layer_visible(p, cell, m))
                        continue;
                    int d = std::abs(l[m].y - py);
                    if (d < best_d) {
                        best = m;
                        best_d = d;
                    }
                }
                n = best;
            }
            if (n >= 0)
                shown[cell] = (int8_t)n;
        }
    }
    for (int j = 0; j < p->height; j++) {
        for (int i = 0; i < p->width; i++) {
            size_t cell = (size_t)j * p->width + i;
            ImU32 c = 0;
            int ly = PORT_AUTOMAP_NO_Y;
            bool hazard = false;
            if (shown[cell] >= 0) {
                const PortAutomapLayer &l = p->layers[cell * PORT_AUTOMAP_LAYERS + shown[cell]];
                ly = l.y;
                hazard = l.kind != PORT_AUTOMAP_FLOOR && l.kind != PORT_AUTOMAP_DEATH && !(l.flags & PORT_AUTOMAP_WALL);
                switch (l.kind) {
                case PORT_AUTOMAP_WATER: c = palette.water; break;
                case PORT_AUTOMAP_POISON: c = palette.poison; break;
                case PORT_AUTOMAP_FIRE: c = palette.fire; break;
                case PORT_AUTOMAP_HURT: c = palette.hurt; break;
                case PORT_AUTOMAP_DEATH: c = palette.death; cache.chasm[cell] = 1; break;
                default: c = floor_colour(l.y, py); break;
                }
                if ((l.flags & PORT_AUTOMAP_WALL) && l.kind != PORT_AUTOMAP_DEATH)
                    c = palette.wall;
                else
                    cache.open[cell] = 1;
            } else if (p->solid_top[cell] != PORT_AUTOMAP_NO_Y) {
                // rock with no floor: a wall beside a floor that is shown (of the slice, when one is)
                bool near = false, near_slice = false;
                for (int dj = -1; dj <= 1; dj++)
                    for (int di = -1; di <= 1; di++) {
                        int ii = i + di, jj = j + dj;
                        if (ii < 0 || jj < 0 || ii >= p->width || jj >= p->height)
                            continue;
                        size_t o = (size_t)jj * p->width + ii;
                        if (shown[o] < 0)
                            continue;
                        const PortAutomapLayer &l = p->layers[o * PORT_AUTOMAP_LAYERS + shown[o]];
                        if (l.kind != PORT_AUTOMAP_DEATH && p->solid_top[cell] < l.y - 40 && p->solid_bottom[cell] > l.y - 1500) {
                            near = true;
                            if (cache.level[o] && !near_slice) {
                                near_slice = true;
                                ly = l.y;
                            } else if (!near_slice) {
                                ly = l.y;
                            }
                        }
                    }
                if (near) {
                    c = palette.wall;
                    cache.level[cell] = near_slice ? 1 : 0;
                }
            }
            if (c != 0 && !cache.level[cell])
                c = other_level(c, ly == PORT_AUTOMAP_NO_Y ? 1000 : std::abs(ly - py), hazard);
            cache.colours[cell] = c;
            cache.shown_y[cell] = (int16_t)ly;
            if (c != 0) {
                cache.x0 = i < cache.x0 ? i : cache.x0;
                cache.x1 = i > cache.x1 ? i : cache.x1;
                cache.z0 = j < cache.z0 ? j : cache.z0;
                cache.z1 = j > cache.z1 ? j : cache.z1;
            }
        }
    }
}

// the level n steps up (n > 0) or down (n < 0) from the slice at y grown from seed_cell: each step the
// nearest floor seen in that direction, not of the slice before, whose own slice is more than a scrap
// (LEVEL_CELLS layers not shown before), and its cell, the next slice's seed; where there is none, as far
// as it got
const int LEVEL_CELLS = 60;

int level_height(const PortAutomapPlan *p, int y, long long &seed_cell, int n) {
    size_t cells = (size_t)p->width * p->height;
    while (n != 0) {
        std::vector<uint8_t> shown = slice_of(p, seed_cell, y);
        // the floors in that direction, nearest first
        std::vector<std::pair<int, uint32_t>> candidates;
        for (size_t cell = 0; cell < cells; cell++) {
            const PortAutomapLayer *l = &p->layers[cell * PORT_AUTOMAP_LAYERS];
            for (int m = 0; m < PORT_AUTOMAP_LAYERS && l[m].y != PORT_AUTOMAP_NO_Y; m++) {
                if ((l[m].flags & (PORT_AUTOMAP_FAR | PORT_AUTOMAP_WALL)) || l[m].kind == PORT_AUTOMAP_DEATH ||
                    shown[cell * PORT_AUTOMAP_LAYERS + m] || !layer_visible(p, cell, m))
                    continue;
                int d = n > 0 ? y - l[m].y : l[m].y - y; // up is a smaller y
                if (d > CLIMB)
                    candidates.push_back(std::make_pair(d, (uint32_t)(cell * PORT_AUTOMAP_LAYERS + m)));
            }
        }
        std::sort(candidates.begin(), candidates.end());
        std::vector<uint8_t> tried(shown.size(), 0);
        bool found = false;
        for (const auto &c : candidates) {
            if (tried[c.second])
                continue;
            long long cell = c.second / PORT_AUTOMAP_LAYERS;
            int cy = p->layers[c.second].y;
            std::vector<uint8_t> next = slice_of(p, cell, cy);
            int fresh = 0;
            for (size_t k = 0; k < next.size(); k++)
                if (next[k]) {
                    tried[k] = 1;
                    fresh += !shown[k];
                }
            if (fresh >= LEVEL_CELLS) {
                y = cy;
                seed_cell = cell;
                found = true;
                break;
            }
        }
        if (!found)
            break;
        n += n > 0 ? -1 : 1;
    }
    return y;
}

// the plan, with the cells' colours made again when what they depend on has changed: the minimap always
// shows the player's level, the full map the level picked (view_level)
const PortAutomapPlan *current_plan(const PortAutomapPlan *p, const PortAutomapPlayer &player, bool full) {
    if (p == nullptr)
        return nullptr;
    int py = (int)player.y;
    long long seed = -1;
    int pi = (int)std::floor((player.x - p->x0) / PORT_AUTOMAP_CELL), pj = (int)std::floor((player.z - p->z0) / PORT_AUTOMAP_CELL);
    if (pi >= 0 && pj >= 0 && pi < p->width && pj < p->height)
        seed = (long long)pj * p->width + pi;
    if (full && view_level != 0) {
        if (!view_level_set) {
            view_level_seed = seed;
            view_level_y = level_height(p, py, view_level_seed, view_level);
            view_level_set = true;
        }
        py = view_level_y;
        seed = view_level_seed;
    }
    int band = (int)std::floor((float)py / 100.0f);
    // a move to another cell of the slice shown, at the same height, keeps the slice
    bool same_slice = cache.seed == seed || (seed >= 0 && cache.seed >= 0 && (size_t)seed < cache.level.size() && cache.level[seed]);
    if (cache.version != p->version || cache.map != p->map || cache.band != band || cache.reveal != port_automap.reveal ||
        !same_slice) {
        rebuild_cache(p, py, seed);
        cache.version = p->version;
        cache.map = p->map;
        cache.band = band;
        cache.reveal = port_automap.reveal;
        cache.seed = seed;
    }
    return p;
}

// a world point on the screen: cx, cz at o, s pixels a world unit
struct View {
    ImVec2 o;
    float cx, cz, s;
    ImVec2 at(float x, float z) const { return ImVec2(o.x + (x - cx) * s, o.y + (z - cz) * s * ZSIGN); }
    float world_x(float sx) const { return cx + (sx - o.x) / s; }
    float world_z(float sy) const { return cz + (sy - o.y) / (s * ZSIGN); }
};

ImVec2 cell_corner(const View &v, const PortAutomapPlan *p, int i, int j) {
    return v.at(p->x0 + (float)i * PORT_AUTOMAP_CELL, p->z0 + (float)j * PORT_AUTOMAP_CELL);
}

// shadow: how far (in pixels) the slice's edges darken the other levels beside them
void draw_cells(ImDrawList *dl, const PortAutomapPlan *p, const View &v, ImVec2 c0, ImVec2 c1, float alpha, float shadow) {
    // the cells in the clip rectangle
    float wx0 = v.world_x(c0.x), wx1 = v.world_x(c1.x);
    float wz0 = v.world_z(c0.y), wz1 = v.world_z(c1.y);
    if (wx0 > wx1)
        std::swap(wx0, wx1);
    if (wz0 > wz1)
        std::swap(wz0, wz1);
    int i0 = (int)std::floor((wx0 - p->x0) / PORT_AUTOMAP_CELL) - 1, i1 = (int)std::ceil((wx1 - p->x0) / PORT_AUTOMAP_CELL) + 1;
    int j0 = (int)std::floor((wz0 - p->z0) / PORT_AUTOMAP_CELL) - 1, j1 = (int)std::ceil((wz1 - p->z0) / PORT_AUTOMAP_CELL) + 1;
    i0 = i0 < 0 ? 0 : i0;
    j0 = j0 < 0 ? 0 : j0;
    i1 = i1 >= p->width ? p->width - 1 : i1;
    j1 = j1 >= p->height ? p->height - 1 : j1;
    float px = v.s * PORT_AUTOMAP_CELL; // a cell's side in pixels
    for (int j = j0; j <= j1; j++) {
        size_t row = (size_t)j * p->width;
        int run = -1;
        ImU32 run_col = 0;
        for (int i = i0; i <= i1 + 1; i++) {
            ImU32 c = i <= i1 ? cache.colours[row + i] : 0;
            if (c != run_col || i > i1) {
                if (run_col != 0 && run >= 0) {
                    ImVec2 a = cell_corner(v, p, run, j), b = cell_corner(v, p, i, j + 1);
                    dl->AddRectFilled(ImVec2(std::fmin(a.x, b.x), std::fmin(a.y, b.y)),
                                      ImVec2(std::fmax(a.x, b.x), std::fmax(a.y, b.y)), fade(run_col, alpha));
                }
                run = i;
                run_col = c;
            }
        }
        // the void, hatched once a cell is big enough to show it
        if (px >= 4.0f)
            for (int i = i0; i <= i1; i++)
                if (cache.chasm[row + i] && ((i + j) & 3) == 0) {
                    ImVec2 a = cell_corner(v, p, i, j), b = cell_corner(v, p, i + 1, j + 1);
                    dl->AddLine(a, b, fade(palette.hatch, alpha * (cache.level[row + i] ? 1.0f : 0.45f)), std::fmax(1.0f, px * 0.25f));
                }
    }
    auto inside = [&](int i, int j) { return i >= 0 && j >= 0 && i < p->width && j < p->height; };
    auto drawn = [&](int i, int j) { return inside(i, j) && cache.colours[(size_t)j * p->width + i] != 0; };
    auto in_slice = [&](int i, int j) { return drawn(i, j) && cache.level[(size_t)j * p->width + i]; };
    // a step between the levels: not where a slope merely leaves the slice's band
    auto step = [&](int ai, int aj, int bi, int bj) {
        int ya = cache.shown_y[(size_t)aj * p->width + ai], yb = cache.shown_y[(size_t)bj * p->width + bi];
        return ya == PORT_AUTOMAP_NO_Y || yb == PORT_AUTOMAP_NO_Y || std::abs(ya - yb) > CLIMB;
    };
    const int di[4] = {1, -1, 0, 0}, dj[4] = {0, 0, 1, -1};
    // the slice's shadow on the other levels: a dark band out from each edge where a cell of the slice
    // meets a cell of another level
    if (shadow > 0.0f) {
        ImU32 dark = col(0, 0, 0, 0.85f * alpha), clear = col(0, 0, 0, 0.0f);
        for (int j = j0; j <= j1; j++)
            for (int i = i0; i <= i1; i++) {
                if (!in_slice(i, j))
                    continue;
                for (int k = 0; k < 4; k++) {
                    int ii = i + di[k], jj = j + dj[k];
                    if (!drawn(ii, jj) || in_slice(ii, jj) || !step(i, j, ii, jj))
                        continue;
                    ImVec2 q0 = cell_corner(v, p, i, j), q1 = cell_corner(v, p, i + 1, j + 1);
                    ImVec2 lo(std::fmin(q0.x, q1.x), std::fmin(q0.y, q1.y)), hi(std::fmax(q0.x, q1.x), std::fmax(q0.y, q1.y));
                    // the other cell's side of this one on the screen (screen y runs against z)
                    float sx = (float)di[k], sy = (float)dj[k] * ZSIGN;
                    if (sx > 0)
                        dl->AddRectFilledMultiColor(ImVec2(hi.x, lo.y), ImVec2(hi.x + shadow, hi.y), dark, clear, clear, dark);
                    else if (sx < 0)
                        dl->AddRectFilledMultiColor(ImVec2(lo.x - shadow, lo.y), ImVec2(lo.x, hi.y), clear, dark, dark, clear);
                    else if (sy > 0)
                        dl->AddRectFilledMultiColor(ImVec2(lo.x, hi.y), ImVec2(hi.x, hi.y + shadow), dark, dark, clear, clear);
                    else
                        dl->AddRectFilledMultiColor(ImVec2(lo.x, lo.y - shadow), ImVec2(hi.x, lo.y), clear, clear, dark, dark);
                }
            }
    }
    // the edges of the floor against nothing at all (no floor, no wall), and of the slice's floor against
    // another level's: a line, as runs
    float lw = std::fmax(1.0f, px * 0.3f);
    auto open = [&](int i, int j) { return inside(i, j) && cache.open[(size_t)j * p->width + i]; };
    auto edge_between = [&](int ai, int aj, int bi, int bj) {
        return (open(ai, aj) && !drawn(bi, bj)) || (open(bi, bj) && !drawn(ai, aj)) ||
               (open(ai, aj) && in_slice(ai, aj) && drawn(bi, bj) && !in_slice(bi, bj) && step(ai, aj, bi, bj)) ||
               (open(bi, bj) && in_slice(bi, bj) && drawn(ai, aj) && !in_slice(ai, aj) && step(ai, aj, bi, bj));
    };
    ImU32 wall = fade(palette.wall, alpha);
    for (int j = j0; j <= j1 + 1; j++) {
        int start = -1;
        for (int i = i0; i <= i1 + 1; i++) {
            bool edge = i <= i1 && edge_between(i, j, i, j - 1);
            if (edge && start < 0)
                start = i;
            if (!edge && start >= 0) {
                dl->AddLine(cell_corner(v, p, start, j), cell_corner(v, p, i, j), wall, lw);
                start = -1;
            }
        }
    }
    for (int i = i0; i <= i1 + 1; i++) {
        int start = -1;
        for (int j = j0; j <= j1 + 1; j++) {
            bool edge = j <= j1 && edge_between(i, j, i - 1, j);
            if (edge && start < 0)
                start = j;
            if (!edge && start >= 0) {
                dl->AddLine(cell_corner(v, p, i, start), cell_corner(v, p, i, j), wall, lw);
                start = -1;
            }
        }
    }
}

// ---- Markers ----------------------------------------------------------------------------------------------
float text_size_for(float height) {
    return std::fmax(14.0f, height / 38.0f);
}

void text(ImDrawList *dl, ImVec2 at, float size, ImU32 c, const char *s) {
    dl->AddText(ImGui::GetFont(), size, ImVec2(at.x + 1, at.y + 1), col(0, 0, 0, 0.9f), s);
    dl->AddText(ImGui::GetFont(), size, at, c, s);
}

// the first line of a marker's label
std::string first_line(const char *s) {
    const char *nl = std::strchr(s, '\n');
    return nl ? std::string(s, nl) : std::string(s);
}

// a way into another map: the cells where this map's floor meets the other's, hatched white, a small marker
// in their middle, and its name on the full map
void draw_exit(ImDrawList *dl, const PortAutomapPlan *p, const View &v, const PortAutomapMarker &m, ImVec2 c0, ImVec2 c1,
               bool full, float ts) {
    ImVec2 a = v.at(m.x, m.z), b = v.at(m.x + m.length, m.z + m.depth);
    ImVec2 lo(std::fmin(a.x, b.x), std::fmin(a.y, b.y)), hi(std::fmax(a.x, b.x), std::fmax(a.y, b.y));
    if (hi.x < c0.x || lo.x > c1.x || hi.y < c0.y || lo.y > c1.y)
        return;
    int i0 = (int)std::floor((m.x - p->x0) / PORT_AUTOMAP_CELL), i1 = (int)std::ceil((m.x + m.length - p->x0) / PORT_AUTOMAP_CELL);
    int j0 = (int)std::floor((m.z - p->z0) / PORT_AUTOMAP_CELL), j1 = (int)std::ceil((m.z + m.depth - p->z0) / PORT_AUTOMAP_CELL);
    float lw = std::fmax(1.0f, v.s * PORT_AUTOMAP_CELL * 0.3f);
    bool any = false;
    // the cells of the floor at the join's height (not a passage above or under it), or, when none of
    // them shows that floor, every floor cell of the join
    bool at_height = false;
    for (int j = j0 < 0 ? 0 : j0; j < j1 && j < p->height && !at_height; j++)
        for (int i = i0 < 0 ? 0 : i0; i < i1 && i < p->width; i++) {
            size_t cell = (size_t)j * p->width + i;
            if (cache.open[cell] && std::abs(cache.shown_y[cell] - (int)m.y) <= 400) {
                at_height = true;
                break;
            }
        }
    for (int j = j0 < 0 ? 0 : j0; j < j1 && j < p->height; j++)
        for (int i = i0 < 0 ? 0 : i0; i < i1 && i < p->width; i++) {
            size_t cell = (size_t)j * p->width + i;
            if (!cache.open[cell] || (at_height && std::abs(cache.shown_y[cell] - (int)m.y) > 400))
                continue;
            any = true;
            float k = cache.level[cell] ? 1.0f : 0.4f;
            ImVec2 q0 = cell_corner(v, p, i, j), q1 = cell_corner(v, p, i + 1, j + 1);
            dl->AddRectFilled(ImVec2(std::fmin(q0.x, q1.x), std::fmin(q0.y, q1.y)), ImVec2(std::fmax(q0.x, q1.x), std::fmax(q0.y, q1.y)),
                              fade(palette.exit & 0x50FFFFFF, k));
            if (((i + j) % 3) == 0)
                dl->AddLine(ImVec2(q0.x, q1.y), ImVec2(q1.x, q0.y), fade(palette.exit & 0xB0FFFFFF, k), lw);
        }
    if (!any)
        return;
    // a small marker in the middle of the way: an arrow-like diamond, white
    {
        ImVec2 mid((lo.x + hi.x) * 0.5f, (lo.y + hi.y) * 0.5f);
        float d = full ? 6.0f : 3.5f;
        dl->AddQuadFilled(ImVec2(mid.x, mid.y - d), ImVec2(mid.x + d, mid.y), ImVec2(mid.x, mid.y + d), ImVec2(mid.x - d, mid.y),
                          palette.exit);
        dl->AddQuad(ImVec2(mid.x, mid.y - d), ImVec2(mid.x + d, mid.y), ImVec2(mid.x, mid.y + d), ImVec2(mid.x - d, mid.y),
                    palette.outline, 1.0f);
    }
    if (full) {
        std::string label = first_line(m.text);
        ImVec2 ext = ImGui::GetFont()->CalcTextSizeA(ts, FLT_MAX, 0.0f, label.c_str());
        ImVec2 at((lo.x + hi.x - ext.x) * 0.5f, hi.y + 3.0f);
        if (hi.x - lo.x < hi.y - lo.y)
            at = ImVec2(hi.x + 4.0f, (lo.y + hi.y - ext.y) * 0.5f);
        if (at.x + ext.x > c1.x)
            at.x = lo.x - 4.0f - ext.x;
        text(dl, at, ts, palette.exit, label.c_str());
    }
}

void draw_icon(ImDrawList *dl, const View &v, const PortAutomapMarker &m, float r) {
    ImVec2 p = v.at(m.x, m.z);
    bool done = (m.flags & PORT_AUTOMAP_MARK_DONE) != 0;
    switch (m.kind) {
    case PORT_AUTOMAP_MARK_DOOR:
    case PORT_AUTOMAP_MARK_LOCKED_DOOR: {
        ImU32 c = m.kind == PORT_AUTOMAP_MARK_DOOR ? palette.door : palette.locked;
        float length = m.length > 0.0f ? m.length : 250.0f;
        float ax = m.length > 0.0f ? m.x : m.x - std::cos(m.angle) * length * 0.5f;
        float az = m.length > 0.0f ? m.z : m.z - std::sin(m.angle) * length * 0.5f;
        ImVec2 a = v.at(ax, az), b = v.at(ax + std::cos(m.angle) * length, az + std::sin(m.angle) * length);
        float w = std::fmax(3.0f, v.s * 90.0f);
        dl->AddLine(a, b, palette.outline, w + 2.0f);
        dl->AddLine(a, b, c, w);
        break;
    }
    case PORT_AUTOMAP_MARK_CHEST: {
        ImVec2 h(r * 0.8f, r * 0.6f);
        if (done) {
            dl->AddRectFilled(ImVec2(p.x - h.x, p.y - h.y), ImVec2(p.x + h.x, p.y + h.y), col(20, 17, 12, 0.85f));
            dl->AddRect(ImVec2(p.x - h.x, p.y - h.y), ImVec2(p.x + h.x, p.y + h.y), palette.chest, 0.0f, 0, std::fmax(1.5f, r * 0.25f));
        } else {
            dl->AddRectFilled(ImVec2(p.x - h.x, p.y - h.y), ImVec2(p.x + h.x, p.y + h.y), palette.chest);
            dl->AddRect(ImVec2(p.x - h.x, p.y - h.y), ImVec2(p.x + h.x, p.y + h.y), palette.outline, 0.0f, 0, 1.0f);
        }
        break;
    }
    case PORT_AUTOMAP_MARK_CONTAINER: {
        float br = r * 0.6f;
        dl->AddCircleFilled(p, br, done ? col(70, 50, 30, 0.85f) : palette.barrel, 10);
        dl->AddCircle(p, br, palette.barrel_rim, 10, std::fmax(1.0f, br * 0.3f));
        break;
    }
    case PORT_AUTOMAP_MARK_REMAINS: {
        // a skull: the jaw, the cranium, two eyes
        float k = r * 0.75f;
        ImU32 bone = done ? col(150, 145, 130) : palette.bone, dark = col(25, 20, 15);
        dl->AddRectFilled(ImVec2(p.x - k * 0.45f, p.y + k * 0.25f), ImVec2(p.x + k * 0.45f, p.y + k * 0.85f), bone, k * 0.15f);
        dl->AddCircleFilled(p, k * 0.75f, bone, 14);
        dl->AddCircle(p, k * 0.75f, palette.outline, 14, 1.0f);
        dl->AddCircleFilled(ImVec2(p.x - k * 0.3f, p.y + k * 0.05f), k * 0.2f, dark, 8);
        dl->AddCircleFilled(ImVec2(p.x + k * 0.3f, p.y + k * 0.05f), k * 0.2f, dark, 8);
        break;
    }
    case PORT_AUTOMAP_MARK_EXAMINE: {
        float k = r * 0.45f;
        dl->AddCircleFilled(p, k, palette.stone, 10);
        dl->AddCircle(p, k, palette.outline, 10, 1.0f);
        break;
    }
    case PORT_AUTOMAP_MARK_SAVE: {
        float k = r * 1.0f;
        ImVec2 t(p.x, p.y - k), rt(p.x + k * 0.7f, p.y), b(p.x, p.y + k), lf(p.x - k * 0.7f, p.y);
        dl->AddQuadFilled(t, rt, b, lf, palette.save);
        dl->AddQuad(t, rt, b, lf, palette.outline, 1.2f);
        break;
    }
    case PORT_AUTOMAP_MARK_PERSON:
        if (m.flags & PORT_AUTOMAP_MARK_ABSENT) {
            // not here now (the secrets cheat): a faint ring, the person's colour
            dl->AddCircleFilled(p, r * 0.8f, fade(palette.person, 0.18f), 16);
            dl->AddCircle(p, r * 0.8f, fade(palette.person, 0.6f), 16, std::fmax(1.0f, r * 0.15f));
            break;
        }
        dl->AddCircleFilled(p, r * 0.95f, palette.person, 16);
        dl->AddCircle(p, r * 0.95f, col(255, 255, 255), 16, std::fmax(1.5f, r * 0.2f));
        dl->AddCircle(p, r * 0.95f + std::fmax(1.5f, r * 0.2f) * 0.5f + 0.5f, palette.outline, 16, 1.0f);
        break;
    case PORT_AUTOMAP_MARK_ENEMY:
        if (m.flags & PORT_AUTOMAP_MARK_ABSENT) {
            // a browsed map's enemy: where it comes into the world, a faint ring
            dl->AddCircleFilled(p, r * 0.6f, fade(palette.enemy, 0.15f), 12);
            dl->AddCircle(p, r * 0.6f, fade(palette.enemy, 0.6f), 12, std::fmax(1.0f, r * 0.12f));
            break;
        }
        dl->AddCircleFilled(p, r * 0.65f, palette.enemy, 12);
        dl->AddCircle(p, r * 0.65f, palette.outline, 12, 1.0f);
        break;
    case PORT_AUTOMAP_MARK_ITEM:
        if (m.flags & (PORT_AUTOMAP_MARK_GIVEN | PORT_AUTOMAP_MARK_MAYBE)) {
            // out of sight until the story hands it over, or there only by chance: a faint ring, the item's colour
            dl->AddCircleFilled(p, r * 0.5f, fade(palette.item, 0.2f), 10);
            dl->AddCircle(p, r * 0.5f, fade(palette.item, 0.7f), 10, std::fmax(1.0f, r * 0.12f));
            break;
        }
        dl->AddCircleFilled(p, r * 0.5f, palette.item, 10);
        dl->AddCircle(p, r * 0.5f, palette.outline, 10, 1.0f);
        break;
    case PORT_AUTOMAP_MARK_TRAP: {
        float k = r * 0.8f;
        ImVec2 a(p.x, p.y - k), b(p.x + k * 0.85f, p.y + k * 0.6f), c(p.x - k * 0.85f, p.y + k * 0.6f);
        dl->AddTriangleFilled(a, b, c, palette.trap);
        dl->AddTriangle(a, b, c, palette.outline, 1.0f);
        break;
    }
    case PORT_AUTOMAP_MARK_SECRET: {
        float k = r * 0.7f;
        dl->AddRectFilled(ImVec2(p.x - k, p.y - k * 0.4f), ImVec2(p.x + k, p.y + k * 0.4f), palette.secret);
        dl->AddRect(ImVec2(p.x - k, p.y - k * 0.4f), ImVec2(p.x + k, p.y + k * 0.4f), palette.outline, 0.0f, 0, 1.0f);
        break;
    }
    case PORT_AUTOMAP_MARK_SWITCH: {
        // a lever: a base and a stick, tilted the other way once pulled
        float k = r * 0.75f, tilt = done ? -0.6f : 0.6f;
        dl->AddRectFilled(ImVec2(p.x - k * 0.6f, p.y + k * 0.2f), ImVec2(p.x + k * 0.6f, p.y + k * 0.6f), palette.stone);
        dl->AddLine(ImVec2(p.x, p.y + k * 0.3f), ImVec2(p.x + tilt * k, p.y - k * 0.8f), palette.lever, std::fmax(2.0f, k * 0.3f));
        dl->AddCircleFilled(ImVec2(p.x + tilt * k, p.y - k * 0.8f), k * 0.25f, palette.lever, 8);
        break;
    }
    case PORT_AUTOMAP_MARK_PEDESTAL: {
        float k = r * 0.7f;
        dl->AddRectFilled(ImVec2(p.x - k * 0.7f, p.y - k * 0.2f), ImVec2(p.x + k * 0.7f, p.y + k * 0.7f), palette.stone);
        dl->AddCircleFilled(ImVec2(p.x, p.y - k * 0.45f), k * 0.35f, done ? palette.pedestal : col(60, 60, 60), 10);
        dl->AddRect(ImVec2(p.x - k * 0.7f, p.y - k * 0.2f), ImVec2(p.x + k * 0.7f, p.y + k * 0.7f), palette.outline, 0.0f, 0, 1.0f);
        break;
    }
    case PORT_AUTOMAP_MARK_PASSAGE: {
        float k = r * 1.1f;
        dl->AddCircleFilled(p, k, col(40, 20, 70, 0.9f), 20);
        dl->AddCircle(p, k, palette.passage, 20, std::fmax(1.5f, k * 0.14f));
        for (int t = 0; t < 2; t++) {
            float a0 = (float)t * 3.14159265f / 3.0f - 3.14159265f / 2.0f;
            ImVec2 q[3];
            for (int n = 0; n < 3; n++) {
                float a = a0 + (float)n * 3.14159265f * 2.0f / 3.0f;
                q[n] = ImVec2(p.x + std::cos(a) * k * 0.7f, p.y + std::sin(a) * k * 0.7f);
            }
            dl->AddTriangle(q[0], q[1], q[2], palette.passage, std::fmax(1.0f, k * 0.09f));
        }
        break;
    }
    case PORT_AUTOMAP_MARK_LADDER: {
        float k = r * 0.7f;
        ImU32 c = col(200, 160, 100);
        dl->AddLine(ImVec2(p.x - k * 0.4f, p.y - k), ImVec2(p.x - k * 0.4f, p.y + k), c, 2.0f);
        dl->AddLine(ImVec2(p.x + k * 0.4f, p.y - k), ImVec2(p.x + k * 0.4f, p.y + k), c, 2.0f);
        for (int n = -1; n <= 1; n++)
            dl->AddLine(ImVec2(p.x - k * 0.4f, p.y + n * k * 0.6f), ImVec2(p.x + k * 0.4f, p.y + n * k * 0.6f), c, 2.0f);
        break;
    }
    case PORT_AUTOMAP_MARK_LIFT: {
        float k = r * 0.7f;
        dl->AddRectFilled(ImVec2(p.x - k, p.y - k * 0.6f), ImVec2(p.x + k, p.y + k * 0.6f), col(120, 120, 130));
        dl->AddTriangleFilled(ImVec2(p.x, p.y - k * 0.5f), ImVec2(p.x + k * 0.4f, p.y), ImVec2(p.x - k * 0.4f, p.y), col(240, 240, 240));
        dl->AddTriangleFilled(ImVec2(p.x, p.y + k * 0.5f), ImVec2(p.x - k * 0.4f, p.y + k * 0.05f), ImVec2(p.x + k * 0.4f, p.y + k * 0.05f), col(240, 240, 240));
        break;
    }
    default:
        break;
    }
    // the items cheat: a gold ring round a container, chest or holder that still holds something
    if (port_automap.items && (m.flags & PORT_AUTOMAP_MARK_HOLDS) &&
        (m.kind == PORT_AUTOMAP_MARK_CHEST || m.kind == PORT_AUTOMAP_MARK_CONTAINER || m.kind == PORT_AUTOMAP_MARK_REMAINS))
        dl->AddCircle(p, r * 1.15f, palette.item, 16, std::fmax(1.5f, r * 0.22f));
    // the hints cheat: a small "?" where there is a guide note
    if (port_automap.hints && (m.flags & PORT_AUTOMAP_MARK_HINT)) {
        ImVec2 bp(p.x + r * 0.95f, p.y - r * 0.95f);
        float br = std::fmax(4.0f, r * 0.5f);
        dl->AddCircleFilled(bp, br, col(60, 160, 255), 12);
        float ts = br * 1.6f;
        ImVec2 ext = ImGui::GetFont()->CalcTextSizeA(ts, FLT_MAX, 0.0f, "?");
        dl->AddText(ImGui::GetFont(), ts, ImVec2(bp.x - ext.x * 0.5f, bp.y - ext.y * 0.5f), col(255, 255, 255), "?");
    }
}

// whether a marker is on the level shown: its cell shows the slice, at about the marker's height
bool on_level_shown(const PortAutomapPlan *p, const PortAutomapMarker &m) {
    int i = (int)std::floor((m.x - p->x0) / PORT_AUTOMAP_CELL), j = (int)std::floor((m.z - p->z0) / PORT_AUTOMAP_CELL);
    if (i < 0 || j < 0 || i >= p->width || j >= p->height)
        return true;
    size_t cell = (size_t)j * p->width + i;
    if (cache.colours[cell] == 0)
        return true; // nothing drawn there: no level to tell
    return cache.level[cell] && std::abs(cache.shown_y[cell] - (int)m.y) <= 400;
}

void draw_markers(ImDrawList *dl, const PortAutomapPlan *p, const View &v, ImVec2 c0, ImVec2 c1, bool full, float r, float ts) {
    const PortAutomapMarker *list = shown_list;
    int count = shown_count;
    for (int i = 0; i < count; i++)
        if (list[i].kind == PORT_AUTOMAP_MARK_EXIT)
        {
            // one label for the joins to the same map that lie close together
            bool labelled = false;
            for (int k = 0; k < i && full; k++)
                if (list[k].kind == PORT_AUTOMAP_MARK_EXIT && first_line(list[k].text) == first_line(list[i].text) &&
                    std::fabs(list[k].x - list[i].x) < 2500.0f && std::fabs(list[k].z - list[i].z) < 2500.0f)
                    labelled = true;
            draw_exit(dl, p, v, list[i], c0, c1, full && !labelled, ts);
        }
    for (int pass = 0; pass < 2; pass++)
        for (int i = 0; i < count; i++) {
            const PortAutomapMarker &m = list[i];
            if (m.kind == PORT_AUTOMAP_MARK_EXIT)
                continue;
            // people and enemies over the rest
            bool top = m.kind == PORT_AUTOMAP_MARK_PERSON || m.kind == PORT_AUTOMAP_MARK_ENEMY;
            if (top != (pass == 1))
                continue;
            ImVec2 q = v.at(m.x, m.z);
            float pad = 4.0f * r + m.length * v.s;
            if (q.x < c0.x - pad || q.y < c0.y - pad || q.x > c1.x + pad || q.y > c1.y + pad)
                continue;
            int start = dl->VtxBuffer.Size;
            draw_icon(dl, v, m, r);
            if (!on_level_shown(p, m))
                for (int k = start; k < dl->VtxBuffer.Size; k++) {
                    ImU32 &c = dl->VtxBuffer[k].col;
                    c = (c & 0x00FFFFFFu) | ((ImU32)((float)(c >> 24) * 0.4f) << 24);
                }
        }
}

// the player's arrow, KF3's (and kf1-enhanced's): it points the way he faces
void draw_player(ImDrawList *dl, const View &v, const PortAutomapPlayer &player, float size) {
    ImVec2 p = v.at(player.x, player.z);
    // the game moves the player along (sin h, cos h) as he walks forward (Player_Move: RotY of +z)
    ImVec2 fwd(std::sin(player.heading), std::cos(player.heading) * ZSIGN);
    ImVec2 side(-fwd.y, fwd.x);
    ImVec2 tip(p.x + fwd.x * size, p.y + fwd.y * size);
    ImVec2 back(p.x - fwd.x * size * 0.45f, p.y - fwd.y * size * 0.45f);
    ImVec2 left(p.x - fwd.x * size * 0.7f + side.x * size * 0.65f, p.y - fwd.y * size * 0.7f + side.y * size * 0.65f);
    ImVec2 right(p.x - fwd.x * size * 0.7f - side.x * size * 0.65f, p.y - fwd.y * size * 0.7f - side.y * size * 0.65f);
    dl->AddQuadFilled(tip, left, back, right, col(255, 250, 220));
    dl->AddQuad(tip, left, back, right, col(0, 0, 0), 1.5f);
}

// the marker under the mouse: the nearest within reach (pixels), or a way whose stretch it is over; -1 none
int pick_marker(const View &v, ImVec2 mouse, float reach) {
    const PortAutomapMarker *list = shown_list;
    int count = shown_count;
    float best = reach * reach;
    int bi = -1;
    for (int i = 0; i < count; i++) {
        const PortAutomapMarker &m = list[i];
        ImVec2 p = v.at(m.x, m.z);
        float d;
        if (m.kind == PORT_AUTOMAP_MARK_EXIT) {
            ImVec2 a = v.at(m.x, m.z), b = v.at(m.x + m.length, m.z + m.depth);
            bool over = mouse.x >= std::fmin(a.x, b.x) && mouse.x <= std::fmax(a.x, b.x) && mouse.y >= std::fmin(a.y, b.y) &&
                        mouse.y <= std::fmax(a.y, b.y);
            d = over ? best * 0.99f : FLT_MAX;
        } else {
            if ((m.kind == PORT_AUTOMAP_MARK_DOOR || m.kind == PORT_AUTOMAP_MARK_LOCKED_DOOR) && m.length > 0.0f)
                p = v.at(m.x + std::cos(m.angle) * m.length * 0.5f, m.z + std::sin(m.angle) * m.length * 0.5f);
            d = (p.x - mouse.x) * (p.x - mouse.x) + (p.y - mouse.y) * (p.y - mouse.y);
        }
        if (d < best) {
            best = d;
            bi = i;
        }
    }
    return bi;
}

// the marker under the mouse, labelled beside it
void draw_hover(ImDrawList *dl, const View &v, ImVec2 mouse, ImVec2 g0, ImVec2 g1, float ts, float reach) {
    int bi = pick_marker(v, mouse, reach);
    if (bi < 0)
        return;
    const char *label = shown_list[bi].text;
    ImFont *font = ImGui::GetFont();
    ImVec2 ext = font->CalcTextSizeA(ts, FLT_MAX, ts * 26.0f, label);
    float pad = ts * 0.4f;
    ImVec2 at(mouse.x + ts, mouse.y + ts * 0.6f);
    if (at.x + ext.x + pad > g1.x)
        at.x = mouse.x - ts - ext.x - pad;
    if (at.y + ext.y + pad > g1.y)
        at.y = mouse.y - ext.y - pad * 2.0f;
    at.x = std::fmax(at.x, g0.x + pad);
    at.y = std::fmax(at.y, g0.y + pad);
    dl->AddRectFilled(ImVec2(at.x - pad, at.y - pad), ImVec2(at.x + ext.x + pad, at.y + ext.y + pad), col(20, 17, 12, 0.92f), pad);
    dl->AddRect(ImVec2(at.x - pad, at.y - pad), ImVec2(at.x + ext.x + pad, at.y + ext.y + pad), palette.frame, pad);
    dl->AddText(font, ts, at, palette.text, label, nullptr, ts * 26.0f);
}

bool any_marker(int kind, int flags = 0) {
    const PortAutomapMarker *list = shown_list;
    int count = shown_count;
    for (int i = 0; i < count; i++)
        if (list[i].kind == kind && (list[i].flags & flags) == flags)
            return true;
    return false;
}

bool any_layer(const PortAutomapPlan *p, int kind) {
    size_t cells = (size_t)p->width * p->height;
    for (size_t c = 0; c < cells; c++)
        if (cache.colours[c] != 0) {
            for (int n = 0; n < PORT_AUTOMAP_LAYERS; n++)
                if (p->layers[c * PORT_AUTOMAP_LAYERS + n].kind == kind && layer_visible(p, c, n) &&
                    !(p->layers[c * PORT_AUTOMAP_LAYERS + n].flags & PORT_AUTOMAP_FAR))
                    return true;
        }
    return false;
}

void draw_legend(ImDrawList *dl, const PortAutomapPlan *p, ImVec2 at, float ts) {
    auto entry = [&](ImU32 c, const char *label, int shape) {
        ImVec2 cc(at.x + ts * 0.5f, at.y + ts * 0.55f);
        if (shape == 1)
            dl->AddLine(ImVec2(cc.x - ts * 0.4f, cc.y), ImVec2(cc.x + ts * 0.4f, cc.y), c, std::fmax(2.0f, ts * 0.18f));
        else if (shape == 2)
            dl->AddRect(ImVec2(cc.x - ts * 0.35f, cc.y - ts * 0.3f), ImVec2(cc.x + ts * 0.35f, cc.y + ts * 0.3f), c, 0.0f, 0, std::fmax(1.5f, ts * 0.12f));
        else if (shape == 3)
            dl->AddCircleFilled(cc, ts * 0.3f, c, 12);
        else if (shape == 4) { // faint: not there now
            dl->AddCircleFilled(cc, ts * 0.3f, fade(c, 0.2f), 12);
            dl->AddCircle(cc, ts * 0.3f, fade(c, 0.65f), 12, std::fmax(1.0f, ts * 0.08f));
        }
        else
            dl->AddRectFilled(ImVec2(cc.x - ts * 0.35f, cc.y - ts * 0.3f), ImVec2(cc.x + ts * 0.35f, cc.y + ts * 0.3f), c);
        text(dl, ImVec2(at.x + ts * 1.3f, at.y), ts, col(230, 220, 200), label);
        at.y += ts * 1.2f;
    };
    entry(col(78, 70, 56), "Floor (darker is lower)", 0);
    entry(palette.wall, "Wall", 0);
    if (any_layer(p, PORT_AUTOMAP_WATER))
        entry(palette.water, "Water", 0);
    if (any_layer(p, PORT_AUTOMAP_FIRE))
        entry(palette.fire, "Lava (burns)", 0);
    if (any_layer(p, PORT_AUTOMAP_POISON))
        entry(palette.poison, "Poison", 0);
    if (any_layer(p, PORT_AUTOMAP_HURT))
        entry(palette.hurt, "Acid (wears equipment)", 0);
    if (any_layer(p, PORT_AUTOMAP_DEATH))
        entry(palette.death, "Deadly: falling in kills", 0);
    if (any_marker(PORT_AUTOMAP_MARK_EXIT))
        entry(palette.exit, "Way to another map", 2);
    entry(palette.door, "Door", 1);
    entry(palette.locked, "Locked door", 1);
    entry(palette.chest, "Chest", 0);
    entry(palette.chest, "Chest (opened)", 2);
    if (any_marker(PORT_AUTOMAP_MARK_CONTAINER))
        entry(palette.barrel, "Barrel, pot, box", 3);
    if (any_marker(PORT_AUTOMAP_MARK_REMAINS))
        entry(palette.bone, "Bones, grave", 3);
    if (any_marker(PORT_AUTOMAP_MARK_SAVE))
        entry(palette.save, "Save point", 0);
    if (any_marker(PORT_AUTOMAP_MARK_SWITCH))
        entry(palette.lever, "Switch", 1);
    if (any_marker(PORT_AUTOMAP_MARK_PEDESTAL))
        entry(palette.pedestal, "Pedestal, marker", 0);
    if (any_marker(PORT_AUTOMAP_MARK_PASSAGE))
        entry(palette.passage, "Symbol of passage", 2);
    if (any_marker(PORT_AUTOMAP_MARK_EXAMINE))
        entry(palette.stone, "Something to look at", 3);
    entry(palette.person, "Person", 3);
    if (any_marker(PORT_AUTOMAP_MARK_PERSON, PORT_AUTOMAP_MARK_ABSENT))
        entry(palette.person, "Person, not here now", 4);
    if (port_automap.items)
        entry(palette.item, "Item", 3);
    if (any_marker(PORT_AUTOMAP_MARK_ITEM, PORT_AUTOMAP_MARK_GIVEN))
        entry(palette.item, "Item the story gives", 4);
    if (port_automap.enemies)
        entry(palette.enemy, "Enemy", 3);
    if (port_automap.secrets) {
        entry(palette.trap, "Trap", 0);
        entry(palette.secret, "Hidden wall", 0);
    }
    if (port_automap.items)
        entry(palette.item, "Holds something (ring)", 2);
    if (port_automap.hints)
        entry(col(60, 160, 255), "Guide note (?)", 3);
    at.y += ts;
    char key[64] = "M";
    int binding = port_controls_binding(ACTION_MAP, 0);
    if (binding != 0)
        port_controls_binding_label(binding, key, sizeof(key));
    char help[384];
    std::snprintf(help, sizeof(help), "Scroll: zoom\nDrag: move\nRight click: fit\nPgUp/PgDn, Shift+scroll: other levels\nPoint at a marker for details\nClick a way: show that map\nBackspace: back, Home: your map\n%s or Escape closes the map", key);
    text(dl, at, ts * 0.85f, col(170, 160, 140), help);
}

// the picture's place, in ImGui's coordinates
void picture(int x, int y, int width, int height, ImVec2 &p0, ImVec2 &p1) {
    ImVec2 scale = ImGui::GetIO().DisplayFramebufferScale;
    float sx = scale.x > 0.0f ? scale.x : 1.0f, sy = scale.y > 0.0f ? scale.y : 1.0f;
    p0 = ImVec2((float)x / sx, (float)y / sy);
    p1 = ImVec2((float)(x + width) / sx, (float)(y + height) / sy);
    if (p1.x - p0.x < 32.0f || p1.y - p0.y < 32.0f) {
        const ImGuiViewport *vp = ImGui::GetMainViewport();
        p0 = vp->WorkPos;
        p1 = ImVec2(vp->WorkPos.x + vp->WorkSize.x, vp->WorkPos.y + vp->WorkSize.y);
    }
}

// the minimap's square in the picture g0-g1, from the settings (also with it off)
void minimap_square(ImVec2 g0, ImVec2 g1, ImVec2 &lo, ImVec2 &hi) {
    float gh = g1.y - g0.y;
    float side = gh * (float)port_automap.minimap_size / 100.0f, margin = gh * 0.025f;
    int corner = port_automap.minimap_corner;
    bool right = corner == 0 || corner == 1, bottom = corner == 1 || corner == 3;
    // top right below the compass, top left below the HP and MP gauges
    float top = corner == 0 ? g0.y + gh * 0.16f : bottom ? g1.y - side - margin : g0.y + gh * 0.22f;
    lo = ImVec2(right ? g1.x - side - margin : g0.x + margin, top);
    hi = ImVec2(lo.x + side, lo.y + side);
}

void draw_minimap(const PortAutomapPlayer &player, ImVec2 g0, ImVec2 g1) {
    shown_count = port_automap_markers(&shown_list);
    const PortAutomapPlan *p = current_plan(port_automap_plan(), player, false);
    if (p == nullptr)
        return;
    ImVec2 lo, hi;
    minimap_square(g0, g1, lo, hi);
    float side = hi.x - lo.x;
    ImDrawList *dl = ImGui::GetBackgroundDrawList();
    float alpha = (float)port_automap.opacity / 100.0f;
    dl->AddRectFilled(lo, hi, col(10, 9, 8, 0.75f * alpha / 0.85f), side * 0.04f);
    dl->PushClipRect(lo, hi, true);
    View v;
    v.o = ImVec2((lo.x + hi.x) * 0.5f, (lo.y + hi.y) * 0.5f);
    v.cx = player.x;
    v.cz = player.z;
    v.s = side / (2.0f * (float)port_automap.minimap_zoom * 100.0f);
    draw_cells(dl, p, v, lo, hi, alpha, std::fmax(4.0f, side * 0.03f));
    float r = std::fmax(3.5f, side * 0.022f);
    draw_markers(dl, p, v, lo, hi, false, r, 0.0f);
    draw_player(dl, v, player, std::fmax(5.0f, side * 0.045f));
    dl->PopClipRect();
    dl->AddRect(lo, hi, col(200, 180, 130, 0.8f), side * 0.04f, 0, 1.5f);
}

// the map browser's buttons in the full map's top right corner (shown while another map is browsed): Back and
// Home, as label and rectangle; the one under a click, -1 none
struct Button {
    const char *label;
    ImVec2 lo, hi;
};

int browse_buttons(ImDrawList *dl, ImVec2 g0, ImVec2 g1, float ts, ImVec2 mouse, bool click, Button out[2]) {
    float gh = g1.y - g0.y, pad = ts * 0.45f;
    const char *labels[2] = {"Back (Backspace)", "Home (Home)"};
    float x = g1.x - gh * 0.04f;
    int hit = -1;
    for (int k = 1; k >= 0; k--) {
        ImVec2 ext = ImGui::GetFont()->CalcTextSizeA(ts, FLT_MAX, 0.0f, labels[k]);
        out[k].label = labels[k];
        out[k].hi = ImVec2(x, g0.y + gh * 0.02f + ext.y + pad * 2.0f);
        out[k].lo = ImVec2(x - ext.x - pad * 2.0f, g0.y + gh * 0.02f);
        x = out[k].lo.x - ts * 0.6f;
        bool over = mouse.x >= out[k].lo.x && mouse.x < out[k].hi.x && mouse.y >= out[k].lo.y && mouse.y < out[k].hi.y;
        dl->AddRectFilled(out[k].lo, out[k].hi, over ? col(70, 60, 40, 0.95f) : col(30, 26, 20, 0.92f), pad);
        dl->AddRect(out[k].lo, out[k].hi, palette.frame, pad, 0, 1.5f);
        text(dl, ImVec2(out[k].lo.x + pad, out[k].lo.y + pad), ts, palette.text, labels[k]);
        if (over && click)
            hit = k;
    }
    return hit;
}

// a way's marker clicked (or named by KF4_AUTOMAP_BROWSE): its map is shown, when it can be
void browse_way(const PortAutomapMarker &m) {
    if (m.kind != PORT_AUTOMAP_MARK_EXIT || !(m.flags & PORT_AUTOMAP_MARK_BROWSE))
        return;
    if (!port_automap_browse(m.map, m.to[0], m.to[1], m.to[2]))
        std::fprintf(stderr, "[automap] map %03d cannot be shown\n", m.map);
}

// KF4_AUTOMAP_BROWSE=step,step,...: as each test picture of the full map opens it, the next step: a map's
// number clicks the way into it on the map shown, "back" and "home" the buttons, "self" shows the player's own
// map built as a browsed map is (port_automap_browse_self_test), nothing does nothing
void test_browse_step() {
    if (!test_browse_read) {
        test_browse_read = true;
        if (const char *t = std::getenv("KF4_AUTOMAP_BROWSE")) {
            std::string spec = t;
            size_t at = 0;
            while (at <= spec.size()) {
                size_t end = spec.find(',', at);
                test_browse.push_back(spec.substr(at, end == std::string::npos ? std::string::npos : end - at));
                if (end == std::string::npos)
                    break;
                at = end + 1;
            }
        }
    }
    if (test_browse_next >= test_browse.size())
        return;
    std::string step = test_browse[test_browse_next++];
    if (step == "back") {
        port_automap_browse_back();
    } else if (step == "home") {
        port_automap_browse_home();
    } else if (step == "self") { // the player's own map built as a browsed one, to check its markers
        port_automap_browse_self_test();
    } else if (!step.empty()) {
        int map = std::atoi(step.c_str());
        const PortAutomapMarker *list;
        int count = port_automap_browsing() >= 0 ? port_automap_browse_markers(&list) : port_automap_markers(&list);
        for (int i = 0; i < count; i++)
            if (list[i].kind == PORT_AUTOMAP_MARK_EXIT && list[i].map == map) {
                if (!(list[i].flags & PORT_AUTOMAP_MARK_BROWSE))
                    std::fprintf(stderr, "[automap] KF4_AUTOMAP_BROWSE: the way into map %03d is not clickable\n", map);
                browse_way(list[i]);
                return;
            }
        std::fprintf(stderr, "[automap] KF4_AUTOMAP_BROWSE: no way into map %03d on the map shown\n", map);
    }
}

void draw_full(const PortAutomapPlayer &own, ImVec2 g0, ImVec2 g1) {
    if (!test_level_read) {
        test_level_read = true;
        if (const char *t = std::getenv("KF4_AUTOMAP_LEVEL"))
            test_level = std::atoi(t);
    }
    if (shot_open && test_level != 0 && view_level != test_level) {
        view_level = test_level;
        view_level_set = false;
    }
    // the map shown: the player's own, or the one browsed (seen from the way it was reached by)
    int browsing = port_automap_browsing();
    PortAutomapPlayer player = own;
    const PortAutomapPlan *shown = port_automap_plan();
    if (browsing >= 0) {
        port_automap_browse_focus(&player);
        shown = port_automap_browse_plan();
        shown_count = port_automap_browse_markers(&shown_list);
    } else {
        shown_count = port_automap_markers(&shown_list);
    }
    if (browsing != shown_browsed) {
        // another map: fitted again, at the level it is seen from
        shown_browsed = browsing;
        zoom = 1.0f;
        centred = false;
        if (!shot_open || test_level == 0) {
            view_level = 0;
            view_level_set = false;
        }
    }
    const PortAutomapPlan *p = current_plan(shown, player, true);
    if (p == nullptr)
        return;
    float gw = g1.x - g0.x, gh = g1.y - g0.y;
    ImDrawList *dl = ImGui::GetBackgroundDrawList();
    dl->AddRectFilled(g0, g1, col(6, 5, 4, (float)port_automap.full_opacity / 100.0f));

    // what has been drawn (all of it with the reveal cheat) and the player, with a little room
    float x0 = player.x, x1 = player.x, z0 = player.z, z1 = player.z;
    if (cache.x1 >= cache.x0) {
        x0 = std::fmin(x0, p->x0 + cache.x0 * PORT_AUTOMAP_CELL);
        x1 = std::fmax(x1, p->x0 + (cache.x1 + 1) * PORT_AUTOMAP_CELL);
        z0 = std::fmin(z0, p->z0 + cache.z0 * PORT_AUTOMAP_CELL);
        z1 = std::fmax(z1, p->z0 + (cache.z1 + 1) * PORT_AUTOMAP_CELL);
    }
    x0 -= 800.0f;
    z0 -= 800.0f;
    x1 += 800.0f;
    z1 += 800.0f;
    float legend_w = gh * 0.30f;
    float area_x = g0.x + gh * 0.04f, area_y = g0.y + gh * 0.08f;
    float area_w = gw - legend_w - gh * 0.08f, area_h = gh * 0.88f;
    // fitted, but no closer than a few corridors across
    float fit = std::fmin(std::fmin(area_w / (x1 - x0), area_h / (z1 - z0)), gh / 2500.0f);
    if (!centred) {
        centre_x = (x0 + x1) * 0.5f;
        centre_z = (z0 + z1) * 0.5f;
    }
    View v;
    v.o = ImVec2(area_x + area_w * 0.5f, area_y + area_h * 0.5f);
    v.cx = centre_x;
    v.cz = centre_z;
    v.s = fit * zoom;

    ImGuiIO &io = ImGui::GetIO();
    ImVec2 mouse = io.MousePos;
    bool inside = mouse.x >= area_x && mouse.x < area_x + area_w && mouse.y >= area_y && mouse.y < area_y + area_h;
    // a click: the left button let go without dragging the map
    bool click = !shot_open && ImGui::IsMouseReleased(ImGuiMouseButton_Left) && io.MouseDragMaxDistanceSqr[0] < 36.0f;
    if (!shot_open) {
        float wheel = io.MouseWheel;
        if (wheel != 0.0f && io.KeyShift) {
            // Shift with the wheel: the level above or below
            view_level += wheel > 0.0f ? 1 : -1;
            view_level_set = false;
            wheel = 0.0f;
        }
        if (wheel != 0.0f) {
            ImVec2 zp = inside ? mouse : v.o;
            float wx = v.world_x(zp.x), wz = v.world_z(zp.y);
            wheel = wheel < -4.0f ? -4.0f : wheel > 4.0f ? 4.0f : wheel;
            zoom = zoom * std::pow(1.25f, wheel);
            zoom = zoom < 1.0f ? 1.0f : zoom > 40.0f ? 40.0f : zoom;
            v.s = fit * zoom;
            centre_x = wx - (zp.x - v.o.x) / v.s;
            centre_z = wz - (zp.y - v.o.y) / (v.s * ZSIGN);
            centred = true;
        }
        if (inside && ImGui::IsMouseDown(ImGuiMouseButton_Left) && (io.MouseDelta.x != 0.0f || io.MouseDelta.y != 0.0f)) {
            centre_x -= io.MouseDelta.x / v.s;
            centre_z -= io.MouseDelta.y / (v.s * ZSIGN);
            centred = true;
        }
        if (inside && ImGui::IsMouseClicked(ImGuiMouseButton_Right)) {
            zoom = 1.0f;
            centred = false;
            centre_x = (x0 + x1) * 0.5f;
            centre_z = (z0 + z1) * 0.5f;
            v.s = fit;
        }
    }
    centre_x = centre_x < x0 ? x0 : centre_x > x1 ? x1 : centre_x;
    centre_z = centre_z < z0 ? z0 : centre_z > z1 ? z1 : centre_z;
    v.cx = centre_x;
    v.cz = centre_z;

    ImVec2 c0(area_x, area_y), c1(area_x + area_w, area_y + area_h);
    float ts = text_size_for(gh);
    dl->PushClipRect(c0, c1, true);
    draw_cells(dl, p, v, c0, c1, 1.0f, std::fmax(10.0f, std::fmin(20.0f, v.s * PORT_AUTOMAP_CELL * 0.8f)));
    float r = std::fmax(7.0f, v.s * 110.0f);
    r = r > gh / 40.0f ? gh / 40.0f : r;
    r = r < 7.0f ? 7.0f : r;
    draw_markers(dl, p, v, c0, c1, true, r, ts);
    if (browsing < 0)
        draw_player(dl, v, player, std::fmax(9.0f, std::fmin(gh / 45.0f, v.s * 160.0f)));
    dl->PopClipRect();
    if (!test_hover_read) {
        test_hover_read = true;
        if (const char *t = std::getenv("KF4_AUTOMAP_HOVER"))
            test_hover = std::sscanf(t, "%f,%f", &test_hover_x, &test_hover_z) == 2;
    }
    if (test_hover) {
        mouse = v.at(test_hover_x, test_hover_z);
        inside = true;
    }
    float reach = std::fmax(12.0f, r * 1.4f);
    if (inside)
        draw_hover(dl, v, mouse, g0, g1, ts, reach);

    text(dl, ImVec2(g0.x + gh * 0.04f, g0.y + gh * 0.02f), ts * 1.4f, col(240, 220, 170), player.title);
    char sub[96];
    char level[64] = "";
    if (view_level != 0)
        std::snprintf(level, sizeof(level), "   %d level%s %s %s (Home: %s)", std::abs(view_level),
                      std::abs(view_level) == 1 ? "" : "s", view_level > 0 ? "above" : "below",
                      browsing >= 0 ? "the way in" : "yours", browsing >= 0 ? "your map" : "yours");
    std::snprintf(sub, sizeof(sub), "map %03d%s%s", player.map, browsing >= 0 ? " (browsing)" : "", level);
    ImVec2 ext = ImGui::GetFont()->CalcTextSizeA(ts * 1.4f, FLT_MAX, 0.0f, player.title);
    text(dl, ImVec2(g0.x + gh * 0.04f + ext.x + ts, g0.y + gh * 0.02f + ts * 0.35f), ts * 0.9f, col(170, 160, 140), sub);
    draw_legend(dl, p, ImVec2(g1.x - legend_w, g0.y + gh * 0.1f), ts);

    // the map browser: Back and Home, and a click on a way into another map shows it
    if (browsing >= 0) {
        Button buttons[2];
        int hit = browse_buttons(dl, g0, g1, ts * 0.85f, io.MousePos, click, buttons);
        if (hit == 0) {
            port_automap_browse_back();
            return;
        }
        if (hit == 1) {
            port_automap_browse_home();
            return;
        }
    }
    if (click && inside) {
        int bi = pick_marker(v, mouse, reach);
        if (bi >= 0)
            browse_way(shown_list[bi]);
    }
}

bool map_key(const SDL_Event *event) {
    for (int slot = 0; slot < BINDING_SLOTS; slot++) {
        int b = port_controls_binding(ACTION_MAP, slot);
        if (b == BINDING_NONE)
            continue;
        if (event->type == SDL_EVENT_KEY_DOWN && BINDING_KIND(b) == BINDING_KEY && BINDING_CODE(b) == (int)event->key.scancode)
            return true;
        if (event->type == SDL_EVENT_GAMEPAD_BUTTON_DOWN && BINDING_KIND(b) == BINDING_PAD && BINDING_CODE(b) == event->gbutton.button)
            return true;
    }
    return false;
}

} // namespace

// ---- For overlay.cpp ----------------------------------------------------------------------------------------
bool automap_view_visible() {
    if (!port_automap.enabled)
        return false;
    if (port_automap_full_open())
        return true;
    PortAutomapPlayer player;
    port_automap_player(&player);
    return port_automap.minimap && player.shown && port_automap_plan() != nullptr;
}

bool automap_view_minimap_place(int x, int y, int width, int height, float box[4], int *corner, float *opacity) {
    ImVec2 g0, g1, lo, hi;
    picture(x, y, width, height, g0, g1);
    minimap_square(g0, g1, lo, hi);
    box[0] = lo.x, box[1] = lo.y, box[2] = hi.x, box[3] = hi.y;
    *corner = port_automap.minimap_corner;
    *opacity = (float)port_automap.opacity / 100.0f;
    if (!port_automap.enabled || !port_automap.minimap || port_automap_full_open())
        return false;
    PortAutomapPlayer player;
    port_automap_player(&player);
    return player.shown && port_automap_plan() != nullptr;
}

bool automap_view_event(const SDL_Event *event) {
    if (!port_automap_full_open())
        return false;
    if (event->type == SDL_EVENT_KEY_DOWN) {
        SDL_Scancode key = event->key.scancode;
        if (key == SDL_SCANCODE_PAGEUP || key == SDL_SCANCODE_PAGEDOWN || key == SDL_SCANCODE_HOME) {
            // Home: the player's own map (the map browser) at his own level
            view_level = key == SDL_SCANCODE_HOME ? 0 : view_level + (key == SDL_SCANCODE_PAGEUP ? 1 : -1);
            view_level_set = false;
            if (key == SDL_SCANCODE_HOME)
                port_automap_browse_home();
            return true;
        }
        if (key == SDL_SCANCODE_BACKSPACE) { // the map browser: the map shown before
            if (port_automap_browsing() >= 0)
                port_automap_browse_back();
            return true;
        }
    }
    bool repeat = event->type == SDL_EVENT_KEY_DOWN && event->key.repeat;
    if (!repeat && (map_key(event) || (event->type == SDL_EVENT_KEY_DOWN && event->key.scancode == SDL_SCANCODE_ESCAPE) ||
                    (event->type == SDL_EVENT_GAMEPAD_BUTTON_DOWN && event->gbutton.button == SDL_GAMEPAD_BUTTON_EAST))) {
        port_automap_set_full_open(0);
        return true;
    }
    return false;
}

void automap_view_open_for_shot(bool open) {
    shot_open = open;
    zoom = 1.0f;
    centred = false;
    port_automap_set_full_open(open ? 1 : 0);
    // overlay.cpp opens the map at each vblank until its picture is taken: a step for each picture
    static uint64_t last_open = 0;
    uint64_t now = port_vblank_count();
    if (open && port_automap_full_open() && (last_open == 0 || now > last_open + 1))
        test_browse_step();
    last_open = now;
}

void automap_view_draw(int x, int y, int width, int height) {
    bool full = port_automap_full_open();
    // (the game gets no input while the full map is open, and the mouse is let go: port_overlay_holds_input decides
    // that at each vblank, since this runs only while the overlay draws something)
    if (full != full_was_open) {
        if (full) {
            zoom = 1.0f;
            centred = false;
            view_level = 0;
            view_level_set = false;
        }
        full_was_open = full;
    }
    if (!port_automap.enabled)
        return;
    PortAutomapPlayer player;
    port_automap_player(&player);
    ImVec2 g0, g1;
    picture(x, y, width, height, g0, g1);
    if (full)
        draw_full(player, g0, g1);
    else if (!port_player_ui_open() && port_automap.minimap && player.shown)
        draw_minimap(player, g0, g1);
}
