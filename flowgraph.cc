#include "flowgraph.hh"

#include <algorithm>
#include <array>
#include <chrono>
#include <climits>
#include <cmath>
#include <expected>
#include <flat_map>
#include <flat_set>
#include <format>
#include <functional>
#include <inplace_vector>
#include <mdspan>
#include <numeric>
#include <ostream>
#include <print>
#include <ranges>
#include <span>
#include <utility>

#include <stdint.h>
#include <unigbrk.h>
#include <unistr.h>
#include <unitypes.h>
#include <uniwidth.h>

namespace flowgraph {

  namespace {

    //! Text as libunistring wants to see it.  Everything here is UTF-8 and
    //! libunistring is what knows about UTF-8.
    //! \param s the text
    //! \return its bytes
    const uint8_t* u8(std::string_view s) noexcept
    {
      return reinterpret_cast<const uint8_t*>(s.data());
    }

    //! Number of columns the string occupies on a terminal.  A double wide
    //! character counts twice, one that combines with the one before it not
    //! at all, and a control character is not counted either.
    //! \param s the text, UTF-8
    //! \return the width in columns
    int disp_width(const std::string_view s) noexcept post(r : r >= 0)
    {
      return std::max(0, ::u8_width(u8(s), s.size(), "UTF-8"));
    }

// GCC 16 sees the promise of a coroutine allocated with ::operator new and
// released through the class own operator delete, and takes the two for a
// mismatched pair.  They are not: libstdc++ hands the release back to
// ::operator delete with the very size it allocated.  The warning is about
// <generator>, not about anything here.
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wmismatched-new-delete"

    //! The grapheme clusters of a string: what a terminal draws as one thing,
    //! which may be more than one code point and may be two columns wide.
    //! \param s the text, UTF-8, which has to outlive the walk
    //! \return each cluster and the number of columns it takes
    std::generator<std::pair<std::string_view, int>> clusters(std::string_view s)
    {
      const uint8_t* const end = u8(s) + s.size();
      for (const uint8_t* p = u8(s); p < end;) {
        const uint8_t* const next = ::u8_grapheme_next(p, end);
        const std::string_view one(reinterpret_cast<const char*>(p), size_t(next - p));
        co_yield {one, disp_width(one)};
        p = next;
      }
    }

#pragma GCC diagnostic pop

    //! The integers from one value up to but not including another, none if
    //! the second is not beyond the first -- what a counting loop would do.
    //! \param a the first value
    //! \param b the bound
    //! \return the view
    template<std::integral T>
    constexpr auto span(T a, T b) noexcept
    {
      return std::views::iota(a, std::max(a, b));
    }

    //! The cells on the straight line from one point to another, the first
    //! included and the second left out.
    //! \param a where the line starts
    //! \param b where it ends
    //! \return a view of the cells in order
    auto towards(point a, point b) pre(a.row == b.row || a.col == b.col)
    {
      const int dr = (b.row > a.row) - (b.row < a.row);
      const int dc = (b.col > a.col) - (b.col < a.col);
      return std::views::iota(0, std::abs(b.row - a.row) + std::abs(b.col - a.col)) | std::views::transform([=](int i) { return point{a.row + i * dr, a.col + i * dc}; });
    }

    //! Every cell of a polyline in drawing order, each of them once.
    //! \param p the polyline
    //! \return a view of the cells
    auto cells(const polyline& p)
    {
      return std::views::concat(p.pts | std::views::pairwise | std::views::transform([](auto ab) { return towards(std::get<0>(ab), std::get<1>(ab)); }) | std::views::join, p.pts | std::views::reverse | std::views::take(1));
    }

    //! Which cells an edge covers and which way it runs through each: bit 1
    //! for horizontally, bit 2 for vertically.  A corner has both.
    //! \param p the route
    //! \return the cells and their directions
    std::flat_map<point, int> coverage(const polyline& p) post(m : (p.pts.size() < 2) == m.empty())
    {
      // The cells are gathered first and put in order in one go.  Filling the map cell by cell
      // would move the ones already in it out of the way over and over, which for a long route
      // costs more than the sorting does.
      std::vector<std::pair<point, int>> all;
      std::ranges::for_each(p.pts | std::views::pairwise, [&all](auto ab) {
        const auto [a, b] = ab;
        const int bit = a.row != b.row ? 2 : 1;
        std::ranges::for_each(std::views::concat(towards(a, b), std::views::single(b)), [&all, bit](point q) { all.emplace_back(q, bit); });
      });
      std::ranges::sort(all);

      // A cell the route passes through twice, which is to say one it turns in, carries both
      // directions.
      std::vector<point> cells;
      std::vector<int> bits;
      std::ranges::for_each(all, [&cells, &bits](const auto& qb) {
        if (! cells.empty() && cells.back() == qb.first)
          bits.back() |= qb.second;
        else {
          cells.push_back(qb.first);
          bits.push_back(qb.second);
        }
      });
      return std::flat_map<point, int>(std::sorted_unique, std::move(cells), std::move(bits));
    }

    //! Cells where one edge runs straight across another.  Every such cell
    //! is counted once, and once more when the two edges meet in a node:
    //! those are the avoidable ones, the reader expects the edges at a node
    //! to fan out rather than to cut through each other.
    //! \param l the layout
    //! \return the number of such cells, those between related edges twice
    size_t crossings(const layout& l)
    {
      // Where an edge passes straight through a cell, and which way.  A cell an edge turns in
      // carries both directions and can be crossed by nothing, so it is left out right away.
      struct pass {
        point cell;
        int bits = 0;      // 1 for horizontally, 2 for vertically
        size_t edge = 0;   // index into l.edges

        auto operator<=>(const pass&) const noexcept = default;
      };
      std::vector<pass> passes;
      std::ranges::for_each(l.edges | std::views::enumerate, [&passes](const auto& ie) {
        const auto& [i, e] = ie;
        std::ranges::for_each(coverage(e.route), [&passes, i](const auto& qb) {
          if (qb.second == 1 || qb.second == 2)
            passes.emplace_back(qb.first, qb.second, size_t(i));
        });
      });
      // In this order the cells come one after the other and the two directions apart, which is
      // all the counting below needs.
      std::ranges::sort(passes);

      const auto related = [&l](size_t a, size_t b) {
        const layout_edge& x = l.edges[a];
        const layout_edge& y = l.edges[b];
        return x.from == y.from || x.from == y.to || x.to == y.from || x.to == y.to;
      };

      // Two edges cross in a cell exactly when one of them runs through it horizontally and the
      // other vertically.  Counting the two groups of every cell therefore says how many pairs
      // cross there, and the pairs of edges never have to be gone through one by one.
      size_t res = 0;
      for (size_t first = 0; first < passes.size();) {
        size_t vert = first;
        while (vert < passes.size() && passes[vert].cell == passes[first].cell && passes[vert].bits == 1)
          ++vert;
        size_t end = vert;
        while (end < passes.size() && passes[end].cell == passes[first].cell)
          ++end;

        res += (vert - first) * (end - vert);
        for (size_t a = first; a < vert; ++a)
          for (size_t b = vert; b < end; ++b)
            res += size_t(related(passes[a].edge, passes[b].edge));
        first = end;
      }
      return res;
    }

    //! Does the point lie on the polyline?
    //! \param p the polyline
    //! \param q the point
    //! \return true if it does
    bool on_route(const polyline& p, point q) noexcept
    {
      return std::ranges::any_of(p.pts | std::views::pairwise, [q](auto ab) {
        const auto [a, b] = ab;
        return (a.row == b.row && q.row == a.row && q.col >= std::min(a.col, b.col) && q.col <= std::max(a.col, b.col)) || (a.col == b.col && q.col == a.col && q.row >= std::min(a.row, b.row) && q.row <= std::max(a.row, b.row));
      });
    }

    //! Drop duplicate and collinear points so that the polyline contains
    //! exactly one point per change of direction.
    //! \param p the points, changed in place
    void simplify(std::vector<point>& p)
    {
      const auto [dup, end] = std::ranges::unique(p);
      p.erase(dup, end);
      contract_assert(std::ranges::adjacent_find(p) == p.end());
      if (p.size() < 3)
        return;
      const auto bends = [](auto abc) {
        const auto [a, b, c] = abc;
        return ! ((a.row == b.row && b.row == c.row) || (a.col == b.col && b.col == c.col));
      };
      std::vector<point> s = std::views::concat(p | std::views::take(1), p | std::views::adjacent<3> | std::views::filter(bends) | std::views::transform([](auto abc) { return std::get<1>(abc); }), p | std::views::reverse | std::views::take(1)) | std::ranges::to<std::vector>();
      p = std::move(s);
    }

  } // namespace

  size_t graph::find(unsigned long id) const noexcept
  {
    const auto p = std::ranges::find(nodes, id, &node_desc::id);
    return p == nodes.end() ? npos : size_t(p - nodes.begin());
  }

  graph::graph(graph_source items)
  {
    // An edge may name a node the source only gets to later, so the numbers
    // are kept until everything has come in and only resolved then.
    struct pending {
      unsigned long from, to;
      size_t record;
    };
    std::vector<pending> raw;

    size_t record = 0;
    for (const graph_item& item : items) {
      if (const node_record* const nd = std::get_if<node_record>(&item); nd != nullptr) {
        if (find(nd->id) != npos) [[unlikely]]
          throw graph_error(graph_error::reason::duplicate, record, nd->id, std::format("node {} handed over twice", nd->id));
        if (nd->kind == node_kind::begin)
          begins.push_back(nodes.size());
        nodes.push_back({nd->id, std::string(nd->label), nd->weight, nd->kind});
      } else {
        const edge_record& e = std::get<edge_record>(item);
        raw.push_back({e.from, e.to, record});
      }
      ++record;
    }

    if (nodes.empty()) [[unlikely]]
      throw graph_error(graph_error::reason::no_nodes, graph_error::nowhere, 0, "no nodes");
    if (begins.empty()) [[unlikely]]
      throw graph_error(graph_error::reason::no_begin, graph_error::nowhere, 0, "no node to begin at");

    edges = raw | std::views::transform([this](const pending& p) {
              const size_t a = find(p.from), b = find(p.to);
              if (a == npos) [[unlikely]]
                throw graph_error(graph_error::reason::unknown, p.record, p.from, std::format("no node {} ever came", p.from));
              if (b == npos) [[unlikely]]
                throw graph_error(graph_error::reason::unknown, p.record, p.to, std::format("no node {} ever came", p.to));
              return edge_desc{a, b};
            }) |
            std::ranges::to<std::vector>();
  }

  namespace {

    // A vertex of the layered graph.  Long forward edges are broken up by
    // dummy vertices so that they never run through a node box.
    struct vertex {
      int layer = 0;
      int width = 1;             // 1 for dummies
      double x = 0;              // center column
      size_t node = graph::npos; // real node or npos
    };

    // One horizontal run inside the band between two layers.  It connects the
    // exit trunk of one vertex with the entry trunk of another one; runs that
    // share a trunk have to end up in the same row so that the trunk has a
    // single merge resp. divergence point.
    struct band_item {
      size_t ta, tb; // trunk identifiers
      int ca, cb;    // columns of the two trunks
      size_t edge;
      int hop;            // >=0 forward hop, -1/-2 backward
      int ra = 0, rb = 0; // rows of the two trunks
      int jog = 0;        // column connecting the rows
    };

  } // namespace

  namespace {

    constexpr uint8_t UP = 1;
    constexpr uint8_t RIGHT = 2;
    constexpr uint8_t DOWN = 4;
    constexpr uint8_t LEFT = 8;

    // Box drawing character for every combination of connected sides.  The
    // three further tables are for the mixed frames: 'dh' draws the
    // horizontal sides doubled (begin nodes), 'dv' the vertical ones (return
    // nodes).
    constexpr std::array<char32_t, 16> line_char = {U' ', U'│', U'─', U'└', U'│', U'│', U'┌', U'├', U'─', U'┘', U'─', U'┴', U'┐', U'┤', U'┬', U'┼'};
    constexpr std::array<char32_t, 16> dh_char = {U' ', U'│', U'═', U'╘', U'│', U'│', U'╒', U'╞', U'═', U'╛', U'═', U'╧', U'╕', U'╡', U'╤', U'╪'};
    constexpr std::array<char32_t, 16> dv_char = {U' ', U'║', U'─', U'╙', U'║', U'║', U'╓', U'╟', U'─', U'╜', U'─', U'╨', U'╖', U'╢', U'╥', U'╫'};
    constexpr std::array<char32_t, 16> dd_char = {U' ', U'║', U'═', U'╚', U'║', U'║', U'╔', U'╠', U'═', U'╝', U'═', U'╩', U'╗', U'╣', U'╦', U'╬'};

    // The straight pieces per line style, horizontal and vertical.  Corners
    // and junctions have no dashed form and stay solid.
    constexpr std::array<std::array<char32_t, 2>, 5> dash_char = {{
      {U'─', U'│'}, // solid
      {U'─', U'│'}, // blink, solid but blinking
      {U'╌', U'╎'}, // dense dash
      {U'┄', U'┆'}, // dash
      {U'┈', U'┊'}  // sparse dash
    }};

    //! The glyph of an arrow head.
    //! \param a which way it points
    //! \return the glyph, a blank for none
    constexpr std::string_view arrow_char(arrow a) noexcept
    {
      switch (a) {
      case arrow::up:
        return "\N{BLACK UP-POINTING TRIANGLE}";
      case arrow::down:
        return "\N{BLACK DOWN-POINTING TRIANGLE}";
      case arrow::left:
        return "\N{BLACK LEFT-POINTING TRIANGLE}";
      case arrow::right:
        return "\N{BLACK RIGHT-POINTING TRIANGLE}";
      default:
        return " ";
      }
    }

    //! Which way a step from one cell to the next points.
    //! \param a the cell
    //! \param b its neighbour
    //! \return the arrow pointing from the one to the other
    constexpr arrow toward(point a, point b) noexcept
    {
      return b.row < a.row ? arrow::up : b.row > a.row ? arrow::down : b.col > a.col ? arrow::right : arrow::left;
    }

    //! The cell an arrow head points away from.
    //! \param q where the head is
    //! \param a which way it points
    //! \return the neighbouring cell behind it
    constexpr point behind(point q, arrow a) noexcept
    {
      switch (a) {
      case arrow::up:
        return {q.row + 1, q.col};
      case arrow::down:
        return {q.row - 1, q.col};
      case arrow::left:
        return {q.row, q.col + 1};
      case arrow::right:
        return {q.row, q.col - 1};
      default:
        return q;
      }
    }

    // What one cell of the drawing carries.
    struct cell {
      uint8_t mask = 0;        // the sides that are connected
      uint8_t dbl = 0;         // ... drawn with double lines
      bool joint = false;      // real junction, not a crossing
      uint8_t style = 0;       // 1 + line_style, solid wins
      uint8_t kind = 0;        // box border or edge
      uint32_t owner = 0;      // which node or edge, 1 based
      uint32_t vowner = 0;     // ... of the vertical alone
      uint8_t vstyle = 0;      // ... and its style
      std::string_view over{}; // a glyph placed over the line
      uint32_t over_owner = 0; // whose it is, 0 for nobody
      bool over_wide = false;  // ... and it covers the cell after
      bool covered = false;    // covered by a wide glyph

      //! Do lines cross here without meeting?  The vertical one is drawn
      //! through then and the horizontal one is interrupted.
      bool crossing() const noexcept { return ! joint && (mask & (LEFT | RIGHT)) != 0 && (mask & (UP | DOWN)) != 0; }

      //! Whose is what is really drawn here: a glyph placed over the line, at
      //! a crossing the vertical line, otherwise whatever got here first.
      uint32_t shown_owner() const noexcept { return over_owner != 0 ? over_owner : crossing() && vowner != 0 ? vowner : owner; }

      //! The style of the line really drawn here.
      uint8_t shown_style() const noexcept { return crossing() && vowner != 0 ? vstyle : style; }
    };

    // A rectangular part of the drawing: the viewport, or all of it.
    struct canvas {
      static constexpr uint8_t is_node = 1;
      static constexpr uint8_t is_edge = 2;
      static constexpr uint8_t solid = 1 + std::to_underlying(line_style::solid);

      int r0, c0, h, w;
      std::vector<cell> store;
      std::mdspan<cell, std::dextents<size_t, 2>> grid;
      uint8_t drawing = is_edge; // what is being drawn just now
      uint8_t style_now = solid;
      uint32_t owner_now = 0;
      uint32_t over_now = 0;

      canvas(int r, int c, int hh, int ww) pre(hh >= 0) pre(ww >= 0) : r0(r), c0(c), h(hh), w(ww), store(size_t(hh) * size_t(ww)), grid(store.data(), size_t(hh), size_t(ww)) {}

      canvas(const canvas&) = delete ("the grid refers to the store");
      canvas& operator=(const canvas&) = delete ("the grid refers to the store");

      bool in(point q) const noexcept { return q.row >= r0 && q.row < r0 + h && q.col >= c0 && q.col < c0 + w; }

      //! The cell at a position of the drawing, if it is part of the canvas.
      std::optional<cell&> at(point q) noexcept
      {
        if (! in(q))
          return std::nullopt;
        return grid[size_t(q.row - r0), size_t(q.col - c0)];
      }

      std::optional<const cell&> at(point q) const noexcept
      {
        if (! in(q))
          return std::nullopt;
        return grid[size_t(q.row - r0), size_t(q.col - c0)];
      }

      //! Connect sides of a cell for whatever is being drawn.
      //! \param q the cell
      //! \param b the sides
      //! \param two whether they are drawn with double lines
      void add(point q, uint8_t b, bool two) noexcept
      {
        if (b == 0)
          return;
        const std::optional<cell&> x = at(q);
        if (! x)
          return;
        x->mask |= b;
        if (two)
          x->dbl |= b;
        // The box wins over an edge, otherwise whoever came first; a solid
        // line wins over a dashed one, so a shared trunk stays solid.
        if (x->kind == 0 || (drawing == is_node && x->kind != is_node)) {
          x->kind = drawing;
          x->owner = owner_now;
          x->style = style_now;
        } else if (style_now == solid)
          x->style = solid;
        // A crossing draws the vertical line through and interrupts the
        // horizontal one, so the cell shows a line that need not be the one
        // that got there first.  Keep track of whose the vertical is.
        if ((b & (UP | DOWN)) != 0) {
          if (x->vowner == 0) {
            x->vowner = owner_now;
            x->vstyle = style_now;
          } else if (style_now == solid)
            x->vstyle = solid;
        }
      }

      //! Note that lines really meet in a cell.
      void mark(point q) noexcept
      {
        if (const std::optional<cell&> x = at(q))
          x->joint = true;
      }

      //! Place a glyph over whatever line is in a cell.  A double wide one
      //! also takes the cell to its right: the terminal moves on by two
      //! columns of its own accord, so nothing may be written there.
      //! \param q the cell
      //! \param ch the glyph, UTF-8, which has to outlive the canvas
      //! \param width how many columns it takes
      void put(point q, std::string_view ch, int width = 1) noexcept pre(width >= 1)
      {
        if (const std::optional<cell&> x = at(q)) {
          x->over = ch;
          x->over_owner = over_now;
          x->over_wide = width > 1;
          x->covered = false;
        }
        if (width > 1)
          if (const std::optional<cell&> y = at({q.row, q.col + 1})) {
            y->over = {};
            y->over_owner = over_now;
            y->over_wide = false;
            y->covered = true;
          }
      }

      //! A horizontal line, both ends included.
      void hline(int r, int a, int b, bool two = false) noexcept
      {
        if (a == b)
          return;
        const int lo = std::min(a, b), hi = std::max(a, b);
        std::ranges::for_each(span(std::max(lo, c0), std::min(hi, c0 + w - 1) + 1), [=, this](int c) { add({r, c}, (c > lo ? LEFT : 0) | (c < hi ? RIGHT : 0), two); });
      }

      //! A vertical line, both ends included.
      void vline(int c, int a, int b, bool two = false) noexcept
      {
        if (a == b)
          return;
        const int lo = std::min(a, b), hi = std::max(a, b);
        std::ranges::for_each(span(std::max(lo, r0), std::min(hi, r0 + h - 1) + 1), [=, this](int r) { add({r, c}, (r > lo ? UP : 0) | (r < hi ? DOWN : 0), two); });
      }

      //! Draw a polyline with its head.
      void trace(const polyline& p) noexcept
      {
        std::ranges::for_each(p.pts | std::views::pairwise, [this](auto ab) {
          const auto [a, b] = ab;
          if (a.row == b.row)
            hline(a.row, a.col, b.col);
          else if (a.col == b.col)
            vline(a.col, a.row, b.row);
        });
        // Only the points of a polyline are places where lines really meet;
        // everything else that has both directions is a crossing.
        std::ranges::for_each(p.pts, [this](point q) { mark(q); });
        if (p.head != arrow::none && ! p.pts.empty())
          put(p.pts.back(), arrow_char(p.head));
      }
    };

    //! Append a code point to a UTF-8 string.
    //! \param s the string
    //! \param c the code point
    void encode(std::string& s, char32_t c)
    {
      std::array<uint8_t, 8> buf;
      const int k = ::u8_uctomb(buf.data(), ::ucs4_t(c), ::ptrdiff_t(buf.size()));
      if (k > 0) [[likely]]
        s.append(reinterpret_cast<const char*>(buf.data()), static_cast<size_t>(k));
    }

    //! Ask the painter about every node and every edge, once each.
    //! \param l the layout
    //! \param how the painter
    //! \return the answers, indexed 1..nodes for the nodes and on from there
    //!         for the edges; index 0 is the answer for anything unowned
    std::vector<attributes> enquire(const layout& l, painter how) post(at : at.size() == 1 + l.nodes.size() + l.edges.size())
    {
      return std::views::concat(std::views::single(attributes{}), l.nodes | std::views::transform([how](const layout_node& nd) { return how(node_item{nd.id, nd.kind}); }), l.edges | std::views::transform([how, &l](const layout_edge& e) {
                                                                                                                                                                              return how(edge_item{l.nodes[e.from].id, l.nodes[e.to].id, e.backward});
                                                                                                                                                                            })) |
             std::ranges::to<std::vector>();
    }

    //! Everything but the labels: the boxes and the edges.  Borders come
    //! first, they are ordinary lines and therefore merge with the edges
    //! arriving at or leaving from them.
    //! \param l the layout
    //! \param cv where to draw
    //! \param at what the painter said, see enquire()
    void paint(const layout& l, canvas& cv, const std::vector<attributes>& at)
    {
      cv.drawing = canvas::is_node;
      cv.style_now = canvas::solid;
      std::ranges::for_each(l.nodes | std::views::enumerate, [&cv](auto ind) {
        const auto& [ni, nd] = ind;
        cv.owner_now = uint32_t(1 + ni);
        // begin nodes have doubled top and bottom sides, return nodes doubled
        // left and right ones
        const bool dh = nd.kind == node_kind::begin;
        const bool dv = nd.kind == node_kind::ret;
        cv.hline(nd.row, nd.col, nd.right_col(), dh);
        cv.hline(nd.bottom_row(), nd.col, nd.right_col(), dh);
        cv.vline(nd.col, nd.row, nd.bottom_row(), dv);
        cv.vline(nd.right_col(), nd.row, nd.bottom_row(), dv);
        cv.mark({nd.row, nd.col});
        cv.mark({nd.row, nd.right_col()});
        cv.mark({nd.bottom_row(), nd.col});
        cv.mark({nd.bottom_row(), nd.right_col()});
      });
      cv.drawing = canvas::is_edge;
      std::ranges::for_each(l.edges | std::views::enumerate, [&cv, &at, &l](auto ie) {
        const auto& [ei, e] = ie;
        cv.owner_now = uint32_t(1 + l.nodes.size() + ei);
        cv.style_now = 1 + std::to_underlying(at[cv.owner_now].style);
        cv.trace(e.route);
      });
      cv.owner_now = 0;
      cv.style_now = canvas::solid;
      std::ranges::for_each(l.marks, [&cv](const polyline& p) { cv.trace(p); });
    }

    //! Is every mark a single cell with an arrow head on it?
    //! \param marks the marks
    //! \return true if so
    bool all_heads(const std::vector<polyline>& marks) noexcept
    {
      return std::ranges::all_of(marks, [](const polyline& p) { return p.pts.size() == 1 && p.head != arrow::none; });
    }

    //! An arrow head on everything running into a junction, so that it is
    //! plain which way the lines meet there and where one ends rather than
    //! carries on.  A junction is a cell where three or more directions really
    //! join: a crossing is none, and a border is none either, it carries its
    //! own head already.
    //! \param l the layout, complete but for these heads
    //! \return the heads, to be added to the marks
    std::vector<polyline> junction_heads(const layout& l) post(r : all_heads(r))
    {
      canvas all(0, 0, l.rows, l.cols);
      paint(l, all, enquire(l, plain)); // the shape is all that counts

      const auto junction = [&all](point q) {
        const std::optional<const cell&> x = all.at(q);
        return x && x->joint && std::popcount(x->mask) >= 3;
      };
      const auto border = [&l](point q) { return std::ranges::any_of(l.nodes, [q](const layout_node& nd) { return nd.covers(q); }); };
      const auto onto_crossing = [&all](point q) {
        const std::optional<const cell&> x = all.at(q);
        return ! x || ((x->mask & (LEFT | RIGHT)) != 0 && (x->mask & (UP | DOWN)) != 0);
      };

      return l.edges | std::views::transform([&](const layout_edge& e) {
               const std::vector<point> seq = cells(e.route) | std::ranges::to<std::vector>();
               return seq | std::views::pairwise | std::views::filter([&](auto ab) {
                        const auto [a, b] = ab;
                        return junction(b) && ! border(b) && ! junction(a) && ! border(a) && ! onto_crossing(a); // that would hide it
                      }) |
                      std::views::transform([](auto ab) {
                        const auto [a, b] = ab;
                        return polyline{{a}, toward(a, b)};
                      }) |
                      std::ranges::to<std::vector>();
             }) |
             std::views::join | std::ranges::to<std::vector>();
    }

    // Thrown where the search notices that its time is up.
    struct cancelled {};

    // How long the search may go on.  It is asked wherever the work could
    // take a while, so that giving up costs at most one such step.
    struct watchdog {
      std::chrono::steady_clock::time_point until{};
      bool armed = false;

      explicit watchdog(std::chrono::milliseconds limit) noexcept : until(std::chrono::steady_clock::now() + limit), armed(limit > std::chrono::milliseconds::zero()) {}

      //! Give up if the time the caller allowed has gone by.
      void check() const
      {
        if (armed && std::chrono::steady_clock::now() > until) [[unlikely]]
          throw cancelled{};
      }
    };

    layout make_layout(const graph& g, const config& cfg_in, const watchdog& dog)
    {
      config cfg = cfg_in;
      cfg.default_width = std::max(cfg.default_width | 1u, 5u); // always odd
      cfg.min_height = std::max(cfg.min_height, 3u);
      cfg.max_height = std::max(cfg.max_height, cfg.min_height);
      cfg.node_gap = std::max(cfg.node_gap, 1u);

      const size_t n = g.nodes.size();
      const size_t ne = g.edges.size();
      const auto node_ids = std::views::iota(0zu, n);
      const auto edge_ids = std::views::iota(0zu, ne);

      // -- classify the edges.  Everything that closes a cycle in a depth first
      //    search becomes a backward edge; the rest forms a DAG.
      std::vector<std::vector<size_t>> oute(n);
      std::ranges::for_each(g.edges | std::views::enumerate, [&oute](auto ie) {
        const auto& [e, ed] = ie;
        oute[ed.from].push_back(e);
      });

      std::vector<uint8_t> color(n, 0); // 0 white, 1 gray, 2 black
      std::vector<bool> back(ne, false);
      const auto forward = [&back](size_t e) { return ! back[e]; };

      // With an explicit stack: recursion would go as deep as the longest path.
      const auto dfs = [&](size_t root) {
        if (color[root] != 0)
          return;
        color[root] = 1;
        std::vector<std::pair<size_t, size_t>> st{{root, 0zu}};
        while (! st.empty()) {
          auto& [u, k] = st.back();
          if (k == oute[u].size()) {
            color[u] = 2;
            st.pop_back();
            continue;
          }
          const size_t e = oute[u][k++];
          const size_t v = g.edges[e].to;
          if (color[v] == 1)
            back[e] = true;
          else if (color[v] == 0) {
            color[v] = 1;
            st.emplace_back(v, 0);
          }
        }
      };
      std::ranges::for_each(std::views::concat(g.begins, node_ids), dfs);

      // -- layer assignment: longest path from the sources of the DAG.
      std::vector<int> layer(n, 0);
      {
        std::vector<size_t> indeg(n, 0zu);
        std::ranges::for_each(edge_ids | std::views::filter(forward), [&](size_t e) { ++indeg[g.edges[e].to]; });
        std::vector<size_t> q = node_ids | std::views::filter([&indeg](size_t i) { return indeg[i] == 0zu; }) | std::ranges::to<std::vector>();
        for (size_t h = 0zu; h < q.size(); ++h) { // grows while it is walked
          const size_t u = q[h];
          std::ranges::for_each(oute[u] | std::views::filter(forward), [&](size_t e) {
            const size_t v = g.edges[e].to;
            layer[v] = std::max(layer[v], layer[u] + 1);
            if (--indeg[v] == 0)
              q.push_back(v);
          });
        }
      }
      const int nlayer = std::ranges::max(layer) + 1;
      contract_assert(nlayer >= 1);
      const auto layers = std::views::iota(0, nlayer);

      // -- size of the node boxes.
      const std::vector<int> nwidth = g.nodes | std::views::transform([&cfg](const node_desc& nd) { return std::max<int>(cfg.default_width, disp_width(nd.label) + 4) | 1; }) | std::ranges::to<std::vector>();
      const std::vector<int> nheight = [&] {
        const auto [wmin, wmax] = std::ranges::minmax(g.nodes | std::views::transform(&node_desc::weight));
        // Heights are strictly proportional to the weights.  Pick the smallest
        // scale that gives the lightest node min_height lines; if that pushes
        // the heaviest node past max_height the spread of the weights is too
        // large and the upper bound wins.
        double k = double(cfg.min_height) / std::max(1ul, wmin);
        if (wmax * k > cfg.max_height)
          k = double(cfg.max_height) / std::max(1ul, wmax);
        return g.nodes | std::views::transform([&cfg, k](const node_desc& nd) { return int(std::clamp<long>(std::lround(nd.weight * k), 3, cfg.max_height)); }) | std::ranges::to<std::vector>();
      }();

      // -- build the layered vertex set, adding dummies for long edges.  The
      //    real nodes come first, so node i is vertex i.
      std::vector<vertex> vs = node_ids | std::views::transform([&](size_t i) { return vertex{layer[i], nwidth[i], 0.0, i}; }) | std::ranges::to<std::vector>();

      std::vector<std::vector<size_t>> chain(ne);
      std::ranges::for_each(edge_ids | std::views::filter(forward), [&](size_t e) {
        const size_t u = g.edges[e].from, v = g.edges[e].to;
        contract_assert(layer[v] > layer[u]); // a forward edge goes down
        const size_t first = vs.size();
        vs.append_range(span(layer[u] + 1, layer[v]) | std::views::transform([](int l) { return vertex{l, 1, 0.0, graph::npos}; }));
        chain[e] = std::views::concat(std::views::single(u), span(first, vs.size()), std::views::single(v)) | std::ranges::to<std::vector>();
      });

      const size_t nv = vs.size();
      const auto vertex_ids = std::views::iota(0zu, nv);
      std::vector<std::vector<size_t>> vsucc(nv), vpred(nv);
      std::ranges::for_each(chain | std::views::transform([](const auto& ch) { return ch | std::views::pairwise; }) | std::views::join, [&](auto ab) {
        const auto [a, b] = ab;
        vsucc[a].push_back(b);
        vpred[b].push_back(a);
      });
      using adjacency = std::vector<std::vector<size_t>>;

      // -- initial order inside the layers: depth first from the begin node, so
      //    that the children of a node end up next to each other.
      std::vector<std::vector<size_t>> order(nlayer);
      {
        std::vector<bool> seen(nv, false);
        const auto visit = [&](size_t root) {
          if (seen[root])
            return;
          seen[root] = true;
          order[vs[root].layer].push_back(root);
          std::vector<std::pair<size_t, size_t>> st{{root, 0zu}};
          while (! st.empty()) {
            auto& [u, k] = st.back();
            if (k == vsucc[u].size()) {
              st.pop_back();
              continue;
            }
            const size_t v = vsucc[u][k++];
            if (! seen[v]) {
              seen[v] = true;
              order[vs[v].layer].push_back(v);
              st.emplace_back(v, 0zu);
            }
          }
        };
        std::ranges::for_each(std::views::concat(g.begins, vertex_ids), visit);
      }

      // -- reduce crossings with a few barycenter sweeps.
      {
        std::vector<double> pos(nv, 0.0);
        const auto reindex = [&](int l) {
          std::ranges::for_each(order[l] | std::views::enumerate, [&pos](auto iv) {
            const auto [i, v] = iv;
            pos[v] = double(i);
          });
        };
        std::ranges::for_each(layers, reindex);

        // the mean position of the neighbours, or the own one without any
        const auto mean = [&pos](const std::vector<size_t>& adj, double own) { return adj.empty() ? own : std::ranges::fold_left(adj | std::views::transform([&pos](size_t u) { return pos[u]; }), 0.0, std::plus{}) / double(adj.size()); };
        const auto sweep = [&](int l, const adjacency& adj) {
          const std::vector<double> key = order[l] | std::views::transform([&](size_t v) { return mean(adj[v], pos[v]); }) | std::ranges::to<std::vector>();
          std::vector<size_t> idx = std::views::iota(0zu, key.size()) | std::ranges::to<std::vector>();
          std::ranges::stable_sort(idx, {}, [&key](size_t i) { return key[i]; });
          order[l] = idx | std::views::transform([&lay = order[l]](size_t i) { return lay[i]; }) | std::ranges::to<std::vector>();
          reindex(l);
        };

        // Exchanging two neighbours whenever that removes more crossings than
        // it makes finds orders the averaging above walks past -- a chain of
        // dummies on the wrong side of a box, say.
        const auto before = [&pos](size_t v, size_t w, const adjacency& adj) {
          return std::ranges::count_if(std::views::cartesian_product(adj[v], adj[w]), [&pos](auto ab) {
            const auto [a, b] = ab;
            return pos[a] > pos[b];
          });
        };
        const auto transpose = [&] {
          for (bool moved = true; moved;) {
            dog.check();
            moved = false;
            for (const int l : layers)
              for (const size_t i : span(size_t(1), order[l].size())) {
                const size_t v = order[l][i - 1], w = order[l][i];
                if (before(v, w, vpred) + before(v, w, vsucc) <= before(w, v, vpred) + before(w, v, vsucc))
                  continue;
                std::swap(order[l][i - 1], order[l][i]);
                reindex(l);
                moved = true;
              }
          }
        };

        for ([[maybe_unused]] const int it : std::views::iota(0, 4)) {
          std::ranges::for_each(layers | std::views::drop(1), [&](int l) { sweep(l, vpred); });
          std::ranges::for_each(layers | std::views::reverse | std::views::drop(1), [&](int l) { sweep(l, vsucc); });
          transpose();
        }
      }

      // -- horizontal placement.
      const auto sep = [&](size_t a, size_t b) { return (vs[a].width + vs[b].width) / 2.0 + cfg.node_gap; };
      std::vector<size_t> where(nv, 0zu); // index inside its layer
      {
        std::ranges::for_each(order, [&](const std::vector<size_t>& lay) {
          double cur = 0.0;
          std::ranges::for_each(lay, [&](size_t v) {
            vs[v].x = cur + vs[v].width / 2.0;
            cur += vs[v].width + cfg.node_gap;
          });
        });

        const auto compact = [&](int l, const std::vector<double>& tgt) {
          const std::vector<size_t>& lay = order[l];
          if (lay.empty())
            return;
          std::vector<double> p(lay.size());
          std::ranges::for_each(lay | std::views::enumerate, [&](auto iv) {
            const auto [i, v] = iv;
            p[i] = i == 0 ? tgt[i] : std::max(tgt[i], p[i - 1] + sep(lay[i - 1], v));
          });
          // The forward pass only ever pushes to the right; shift the whole
          // layer back so that it stays centered on the desired positions.
          const double d = std::ranges::fold_left(std::views::zip_transform(std::minus{}, tgt, p), 0.0, std::plus{}) / double(lay.size());
          if (d < 0)
            std::ranges::for_each(p, [d](double& v) { v += d; });
          std::ranges::for_each(std::views::zip(lay, p), [&vs](auto vp) { vs[std::get<0>(vp)].x = std::get<1>(vp); });
        };

        const auto pass = [&](int l, const adjacency& adj) {
          compact(l, order[l] | std::views::transform([&](size_t v) { return adj[v].empty() ? vs[v].x : std::ranges::fold_left(adj[v] | std::views::transform([&vs](size_t u) { return vs[u].x; }), 0.0, std::plus{}) / double(adj[v].size()); }) | std::ranges::to<std::vector>());
        };

        for ([[maybe_unused]] const int it : std::views::iota(0, 8)) {
          std::ranges::for_each(layers | std::views::drop(1), [&](int l) { pass(l, vpred); });
          std::ranges::for_each(layers | std::views::reverse | std::views::drop(1), [&](int l) { pass(l, vsucc); });
        }

        std::ranges::for_each(order, [&where](const std::vector<size_t>& lay) { std::ranges::for_each(lay | std::views::enumerate, [&where](auto iv) { where[std::get<1>(iv)] = size_t(std::get<0>(iv)); }); });
      }

      // Round to whole columns and re-establish the minimum distances, left to
      // right.  All separations are integral (widths are odd, dummies are 1
      // wide), so this hardly moves anything.
      const auto settle = [&](auto&& xof) {
        std::ranges::for_each(order, [&](const std::vector<size_t>& lay) {
          double prev = 0.0;
          std::ranges::for_each(lay | std::views::enumerate, [&](auto iv) {
            const auto [i, v] = iv;
            const double c = i == 0 ? std::round(xof(v)) : std::max(std::round(xof(v)), prev + sep(lay[i - 1], v));
            xof(v) = c;
            prev = c;
          });
        });
      };
      settle([&vs](size_t v) -> double& { return vs[v].x; });

      // -- Brandes/Koepf horizontal coordinates, as an alternative to the
      //    averaging above.  Four passes align every vertex with the median of
      //    its neighbours -- from above and from below, packed to the left and
      //    to the right -- and the median of the four results is taken.  Long
      //    edges win their conflicts, so they come out as straight verticals.
      const auto brandes_koepf = [&]() {
        std::flat_set<std::pair<size_t, size_t>> marked;
        const auto inner = [&](size_t v) {
          if (vs[v].node != graph::npos)
            return graph::npos;
          const auto p = std::ranges::find(vpred[v], graph::npos, [&vs](size_t u) { return vs[u].node; });
          return p == vpred[v].end() ? graph::npos : *p;
        };
        std::ranges::for_each(order | std::views::pairwise, [&](auto updn) {
          const auto& [up, dn] = updn;
          if (up.empty() || dn.empty())
            return;
          size_t k0 = 0zu;
          size_t l = 0zu;
          std::ranges::for_each(dn | std::views::enumerate, [&](auto lv) {
            const auto [l1, d] = lv;
            const size_t iv = inner(d);
            if (size_t(l1) + 1 != dn.size() && iv == graph::npos)
              return;
            const size_t k1 = iv != graph::npos ? where[iv] : up.size() - 1;
            std::ranges::for_each(span(l, size_t(l1) + 1), [&](size_t j) { std::ranges::for_each(vpred[dn[j]] | std::views::filter([&](size_t u) { return where[u] < k0 || where[u] > k1; }), [&](size_t u) { marked.emplace(u, dn[j]); }); });
            l = size_t(l1) + 1;
            k0 = k1;
          });
        });

        const auto pass = [&](bool up, bool rev) {
          std::vector<std::vector<size_t>> lay = order;
          if (rev)
            std::ranges::for_each(lay, [](std::vector<size_t>& x) { std::ranges::reverse(x); });
          std::vector<size_t> pos(nv, 0zu);
          std::ranges::for_each(lay, [&pos](const std::vector<size_t>& x) { std::ranges::for_each(x | std::views::enumerate, [&pos](auto iv) { pos[std::get<1>(iv)] = size_t(std::get<0>(iv)); }); });
          const adjacency& adj = up ? vsucc : vpred;

          std::vector<size_t> root(nv), next(nv), sink(nv);
          std::ranges::iota(root, 0zu);
          std::ranges::iota(next, 0zu);
          std::ranges::iota(sink, 0zu);
          std::vector<double> shift(nv, 1e18), x(nv, 1e18);

          // vertical alignment: every vertex joins the block of a median
          // neighbour when nothing marked and nothing already aligned is in the
          // way
          std::ranges::for_each(layers | std::views::transform([up, nlayer](int li) { return up ? nlayer - 1 - li : li; }), [&](int l) {
            long r = -1;
            std::ranges::for_each(lay[l], [&](size_t v) {
              std::vector<size_t> nb = adj[v];
              if (nb.empty())
                return;
              std::ranges::stable_sort(nb, {}, [&pos](size_t a) { return pos[a]; });
              const size_t d = nb.size();
              for (const size_t m : {(d - 1) / 2, d / 2}) {
                if (next[v] != v)
                  break;
                const size_t u = nb[m];
                const bool bad = up ? marked.contains({v, u}) : marked.contains({u, v});
                if (bad || r >= long(pos[u]))
                  continue;
                next[u] = v;
                root[v] = root[u];
                next[v] = root[v];
                r = long(pos[u]);
              }
            });
          });

          // horizontal compaction: place every block as far left as its
          // neighbours allow, classes of blocks that touch shift together
          const auto place = [&](this auto& self, size_t v) -> void {
            if (x[v] < 1e17)
              return;
            x[v] = 0.0;
            size_t w = v;
            do {
              if (pos[w] > 0) {
                const size_t left = lay[vs[w].layer][pos[w] - 1];
                const size_t u = root[left];
                self(u);
                if (sink[v] == v)
                  sink[v] = sink[u];
                if (sink[v] != sink[u])
                  shift[sink[u]] = std::min(shift[sink[u]], x[v] - x[u] - sep(left, w));
                else
                  x[v] = std::max(x[v], x[u] + sep(left, w));
              }
              w = next[w];
            } while (w != v);
          };
          std::ranges::for_each(vertex_ids | std::views::filter([&root](size_t v) { return root[v] == v; }), place);

          return vertex_ids | std::views::transform([&](size_t v) {
                   const double r = x[root[v]] + (shift[sink[root[v]]] < 1e17 ? shift[sink[root[v]]] : 0);
                   return rev ? -r : r;
                 }) |
                 std::ranges::to<std::vector>();
        };

        std::array<std::vector<double>, 4> cand = {pass(false, false), pass(false, true), pass(true, false), pass(true, true)};
        const auto lo_of = [&](const std::vector<double>& c) { return std::ranges::min(vertex_ids | std::views::transform([&](size_t v) { return c[v] - vs[v].width / 2.0; })); };
        const auto hi_of = [&](const std::vector<double>& c) { return std::ranges::max(vertex_ids | std::views::transform([&](size_t v) { return c[v] + vs[v].width / 2.0; })); };
        const std::array<double, 4> lo = {lo_of(cand[0]), lo_of(cand[1]), lo_of(cand[2]), lo_of(cand[3])};
        const std::array<double, 4> hi = {hi_of(cand[0]), hi_of(cand[1]), hi_of(cand[2]), hi_of(cand[3])};
        // align all four with the narrowest: the left packed ones by their left
        // edge, the right packed ones by their right edge
        const std::array<double, 4> width = {hi[0] - lo[0], hi[1] - lo[1], hi[2] - lo[2], hi[3] - lo[3]};
        const size_t best = size_t(std::ranges::min_element(width) - width.begin());
        for (const size_t k : std::views::iota(size_t(0), cand.size())) {
          const double d = k % 2 == 0 ? lo[best] - lo[k] : hi[best] - hi[k];
          std::ranges::for_each(cand[k], [d](double& v) { v += d; });
        }
        return vertex_ids | std::views::transform([&cand](size_t v) {
                 std::array<double, 4> t = {cand[0][v], cand[1][v], cand[2][v], cand[3][v]};
                 std::ranges::sort(t);
                 return (t[1] + t[2]) / 2;
               }) |
               std::ranges::to<std::vector>();
      };

      // Everything but the last step of the column assignment is fixed now.
      // That step -- pulling a node into the column of a neighbour -- is worth
      // more for the parents in one graph and for the children in another, so
      // the positions are kept and it is repeated for both.
      const std::vector<double> base_x = vs | std::views::transform(&vertex::x) | std::ranges::to<std::vector>();
      std::vector<double> base_bk = brandes_koepf();
      settle([&base_bk](size_t v) -> double& { return base_bk[v]; });

      const auto vcol = [&vs](size_t v) { return int(vs[v].x); };
      int right_max = 0;

      const auto columns = [&](bool pred_first, bool use_bk, bool align) {
        std::ranges::for_each(std::views::zip(vs, use_bk ? base_bk : base_x), [](auto vb) { std::get<0>(vb).x = std::get<1>(vb); });

        // Pull a node into the column of one of its neighbours wherever the
        // gap to its own neighbours allows it: the edge between the two then
        // runs straight down, which costs neither a bend nor a crossing.
        // Lining a node up with a parent rather than with the nearest
        // neighbour makes the alignment carry on down the chain, which
        // straightens whole runs of edges at once -- when it fits.
        for ([[maybe_unused]] const int it : std::views::iota(0, 4))
          std::ranges::for_each(order, [&](const std::vector<size_t>& lay) {
            std::ranges::for_each(lay | std::views::enumerate, [&](auto iv) {
              const auto [i, v] = iv;
              const double lo = i == 0 ? -1e9 : vs[lay[i - 1]].x + sep(lay[i - 1], v);
              const double hi = size_t(i) + 1 == lay.size() ? 1e9 : vs[lay[i + 1]].x - sep(v, lay[i + 1]);
              const auto column = [&vs](int rank) { return [&vs, rank](size_t u) { return std::pair{vs[u].x, rank}; }; };
              auto fits = std::views::concat(vpred[v] | std::views::transform(column(0)), vsucc[v] | std::views::transform(column(1))) | std::views::filter([lo, hi](auto cr) { return cr.first >= lo && cr.first <= hi; });
              const auto nearest = std::ranges::min_element(fits, {}, [&, x = vs[v].x](auto cr) { return std::pair{pred_first ? cr.second : 0, std::abs(cr.first - x)}; });
              if (nearest != fits.end())
                vs[v].x = (*nearest).first;
            });
          });

        // Straighten the dummy chains: an edge that runs through the same
        // column in every layer it crosses needs a single bend instead of one
        // per layer.  Longest chains first, they have the most to gain.
        std::vector<size_t> byl = edge_ids | std::views::filter([&chain](size_t e) { return chain[e].size() > 2; }) | std::ranges::to<std::vector>();
        std::ranges::stable_sort(byl, std::ranges::greater{}, [&chain](size_t e) { return chain[e].size(); });
        std::ranges::for_each(byl, [&](size_t e) {
          const std::span<const size_t> dummies = std::span(chain[e]).subspan(1, chain[e].size() - 2);
          const auto left_bound = [&](size_t d) {
            const std::vector<size_t>& lay = order[vs[d].layer];
            return where[d] == 0 ? -1e9 : vs[lay[where[d] - 1]].x + sep(lay[where[d] - 1], d);
          };
          const auto right_bound = [&](size_t d) {
            const std::vector<size_t>& lay = order[vs[d].layer];
            return where[d] + 1 == lay.size() ? 1e9 : vs[lay[where[d] + 1]].x - sep(d, lay[where[d] + 1]);
          };
          const double lo = std::ranges::max(dummies | std::views::transform(left_bound));
          const double hi = std::ranges::min(dummies | std::views::transform(right_bound));
          if (lo > hi)
            return;
          const double cu = vs[chain[e].front()].x, cv = vs[chain[e].back()].x;
          const double want = cu >= lo && cu <= hi ? cu : cv >= lo && cv <= hi ? cv : std::clamp((cu + cv) / 2, lo, hi);
          std::ranges::for_each(dummies, [&vs, want](size_t d) { vs[d].x = std::round(want); });
        });

        // A long edge that still bends only because something stands where it
        // wants to run can often have that pushed aside: everything in a layer
        // may slide as far as the room its own neighbours leave, and the node
        // the edge ends in may slide too.  Whether that is worth what it costs
        // the others is not decided here -- the layout is built both ways and
        // the better one kept.
        //
        // Move a vertex to a column, pushing what stands in the way aside.
        // Everything keeps its order and its distances, and nothing leaves the
        // columns the drawing already occupies, so it does not grow wider.
        const auto edges_of = [&vs](size_t v) { return std::pair{vs[v].x - vs[v].width / 2.0, vs[v].x + vs[v].width / 2.0}; };
        const double gl = std::ranges::min(vertex_ids | std::views::transform([&](size_t v) { return edges_of(v).first; }));
        const double gr = std::ranges::max(vertex_ids | std::views::transform([&](size_t v) { return edges_of(v).second; }));
        const auto slide = [&](size_t v, double t, bool apply) {
          const auto inside = [&](size_t u, double p) { return p - vs[u].width / 2.0 >= gl && p + vs[u].width / 2.0 <= gr; };
          if (! inside(v, t))
            return false;
          const std::vector<size_t>& lay = order[vs[v].layer];
          const size_t k = where[v];
          std::vector<std::pair<size_t, double>> moved;
          double need = t;
          for (size_t j = k; j-- > 0;) { // the ones to its left, leftwards
            need -= sep(lay[j], lay[j + 1]);
            if (vs[lay[j]].x <= need)
              break; // and so is everything beyond it
            if (! inside(lay[j], need))
              return false;
            moved.emplace_back(lay[j], need);
          }
          need = t;
          for (size_t j = k + 1; j < lay.size(); ++j) { // and to its right
            need += sep(lay[j - 1], lay[j]);
            if (vs[lay[j]].x >= need)
              break;
            if (! inside(lay[j], need))
              return false;
            moved.emplace_back(lay[j], need);
          }
          if (apply) {
            std::ranges::for_each(moved, [&vs](auto vp) { vs[std::get<0>(vp)].x = std::get<1>(vp); });
            vs[v].x = t;
          }
          return true;
        };

        if (align)
          std::ranges::for_each(byl, [&](size_t e) {
            // straight along the column of the one end or of the other, so long
            // as everything else of the edge can be brought there
            for (const bool from_source : {true, false}) {
              const double t = std::round(vs[from_source ? chain[e].front() : chain[e].back()].x);
              const std::span<const size_t> rest = from_source ? std::span(chain[e]).subspan(1) : std::span(chain[e]).first(chain[e].size() - 1);
              if (std::ranges::all_of(rest, [&](size_t v) { return slide(v, t, false); })) {
                std::ranges::for_each(rest, [&](size_t v) { slide(v, t, true); });
                break;
              }
            }
          });

        const double minx = std::ranges::min(vs | std::views::transform([](const vertex& v) { return v.x - v.width / 2.0; }));
        std::ranges::for_each(vs, [minx](vertex& v) { v.x -= minx; });

        right_max = std::ranges::max(vertex_ids | std::views::transform([&](size_t v) { return vcol(v) + vs[v].width / 2; }));
        contract_assert(right_max >= 0);
      };

      // The channels of the backward edges can either be squeezed in beside
      // the layers they span or be put next to the drawing.  Narrow is nice but
      // fewer bends are nicer, so lay the rest out both ways and keep the
      // better one.
      const auto build = [&](int mode, bool into_first, bool wide_first, bool pred_first, bool use_bk, bool swap_cols, bool align) {
        dog.check();
        layout out;
        columns(pred_first, use_bk, align);
        const auto backward = [&back](size_t e) { return back[e]; };
        const auto shares_node = [&g](size_t e, size_t f) {
          const edge_desc& x = g.edges[e];
          const edge_desc& y = g.edges[f];
          return x.from == y.from || x.from == y.to || x.to == y.from || x.to == y.to;
        };
        // the columns a vertex takes up, with a free one on either side
        const auto blocks = [&](size_t v, int c) { return c >= vcol(v) - vs[v].width / 2 - 1 && c <= vcol(v) + vs[v].width / 2 + 1; };

        // -- every backward edge gets a private channel beside the layers it
        //    runs through.  It only has to keep clear of what stands in those
        //    layers, not of the whole drawing.  Short jumps come first, they get
        //    the channel closest in.
        std::vector<int> chan_col(ne, -1);
        {
          std::vector<size_t> bl = edge_ids | std::views::filter(backward) | std::ranges::to<std::vector>();
          const auto jump = [&](size_t e) { return layer[g.edges[e].from] - layer[g.edges[e].to]; };
          if (wide_first)
            std::ranges::stable_sort(bl, std::ranges::greater{}, jump);
          else
            std::ranges::stable_sort(bl, std::ranges::less{}, jump);
          std::vector<std::array<int, 3>> put; // first layer, last layer, column
          std::ranges::for_each(bl, [&](size_t e) {
            const size_t src = g.edges[e].from, dst = g.edges[e].to;
            const int l1 = layer[dst];
            const int l2 = layer[src];
            const auto busy = [&](int c) { return std::ranges::any_of(vertex_ids, [&](size_t v) { return vs[v].layer >= l1 && vs[v].layer <= l2 && blocks(v, c); }) || std::ranges::any_of(put, [&](const std::array<int, 3>& p) { return p[0] <= l2 && l1 <= p[1] && std::abs(p[2] - c) < 2; }); };

            // How much of what else belongs to the two nodes would the
            // channel have to cut through?  Both its runs are counted, against
            // everything a sibling edge puts into the same band -- its trunks,
            // its dummies and its own channel.  A channel on the other side of
            // a node often avoids all of it.
            const int cs = vcol(src), cd = vcol(dst);
            const int be = l2 + 1;
            const int bn = l1;
            const auto in_band = [&](size_t f, int band) {
              std::inplace_vector<int, 3> cc;
              if (back[f]) {
                const size_t uf = g.edges[f].from, vf = g.edges[f].to;
                if (band == layer[uf] + 1)
                  cc.push_back(vcol(uf));
                if (band == layer[vf])
                  cc.push_back(vcol(vf));
                if (chan_col[f] >= 0 && band >= layer[vf] && band <= layer[uf] + 1)
                  cc.push_back(chan_col[f]);
              } else
                cc.append_range(chain[f] | std::views::filter([&vs, band](size_t x) { return vs[x].layer == band || vs[x].layer + 1 == band; }) | std::views::transform(vcol));
              return cc;
            };
            const auto between = [](int a, int b, int x) { return std::min(a, b) < x && x < std::max(a, b); };
            const auto cuts = [&](int c) {
              return std::ranges::fold_left(edge_ids | std::views::filter([&](size_t f) { return f != e && shares_node(e, f); }), 0, [&](int k, size_t f) {
                return k + int(std::ranges::count_if(in_band(f, be), [&](int x) { return between(cs, c, x); })) + int(std::ranges::count_if(in_band(f, bn), [&](int x) { return between(cd, c, x); }));
              });
            };

            const int room = 2 + 2 * int(bl.size());
            const int limit = right_max + room;
            auto free = span(mode == 2 ? right_max + 2 : -room, limit + 1) | std::views::filter(std::not_fn(busy));
            const auto choice = std::ranges::min_element(free, {}, [&](int t) { return std::array<int, 3>{cuts(t), std::max(0, t - right_max) + std::max(0, -t), mode == 0 ? -t : mode == 3 ? t : std::abs(t - cs)}; });
            const int c = choice != free.end() ? *choice : *std::ranges::find_if(std::views::iota(std::max(right_max + 2, 0)), std::not_fn(busy));
            chan_col[e] = c;
            put.push_back({l1, l2, c});
          });
        }

        // A channel that ended up between the dummies of one of the other
        // edges at its two nodes and those nodes crosses that chain twice for
        // nothing.  The two columns can simply be exchanged: same bends, same
        // width, one crossing less at each end.
        if (swap_cols)
          std::ranges::for_each(edge_ids | std::views::filter(backward), [&](size_t e) {
            const size_t src = g.edges[e].from;
            const size_t dst = g.edges[e].to;
            const int l1 = layer[dst];
            const int l2 = layer[src];
            const int c = chan_col[e];

            // the column of the chain of f if the two can be exchanged
            const auto exchange = [&](size_t f) -> std::optional<int> {
              if (back[f] || chain[f].size() < 3 || ! shares_node(e, f))
                return std::nullopt;
              const std::span<const size_t> dummies = std::span(chain[f]).subspan(1, chain[f].size() - 2);

              // its dummies have to stand in one column, else there is nothing
              // to exchange
              const int d = vcol(dummies.front());
              if (d == c || ! std::ranges::all_of(dummies, [&](size_t x) { return vcol(x) == d; }))
                return std::nullopt;

              // how often the rising line cuts the runs of that chain, now and
              // with the two columns exchanged
              const auto cuts_chain = [&](int cc, int dd) {
                return std::ranges::count_if(chain[f] | std::views::pairwise | std::views::enumerate, [&](auto iab) {
                  const auto [i, ab] = iab;
                  const auto [a, b] = ab;
                  const int band = vs[b].layer;
                  if (band < l1 || band > l2 + 1)
                    return false;
                  const int a1 = i == 0 ? vcol(a) : dd;
                  const int a2 = size_t(i) + 2 == chain[f].size() ? vcol(b) : dd;
                  return std::min(a1, a2) < cc && cc < std::max(a1, a2);
                });
              };
              if (cuts_chain(d, c) >= cuts_chain(c, d))
                return std::nullopt;

              // the channel must keep clear of everything that stays where it
              // is, and the dummies must still fit between their neighbours.
              // The chain is a single column, just as the channel is, so it
              // needs no more room beside a box than the channel had.
              const bool blocked = std::ranges::any_of(vertex_ids, [&](size_t v) { return vs[v].layer >= l1 && vs[v].layer <= l2 && ! std::ranges::contains(dummies, v) && blocks(v, d); }) ||
                                   std::ranges::any_of(
                                       dummies,
                                       [&](size_t x) {
                                         const std::vector<size_t>& lay = order[vs[x].layer];
                                         const bool left = where[x] > 0 && c <= vcol(lay[where[x] - 1]) + vs[lay[where[x] - 1]].width / 2 + 1;
                                         const bool right = where[x] + 1 < lay.size() && c >= vcol(lay[where[x] + 1]) - vs[lay[where[x] + 1]].width / 2 - 1;
                                         return left || right;
                                       }
                                   ) ||
                                   std::ranges::any_of(edge_ids, [&](size_t o) { return back[o] && o != e && chan_col[o] >= 0 && layer[g.edges[o].to] <= l2 + 1 && l1 <= layer[g.edges[o].from] + 1 && (std::abs(chan_col[o] - d) < 2 || std::abs(chan_col[o] - c) < 2); });
              return blocked ? std::nullopt : std::optional(d);
            };

            std::ranges::for_each(edge_ids | std::views::transform([&](size_t f) { return std::pair{f, exchange(f)}; }) | std::views::cache_latest | std::views::filter([](const auto& fd) { return fd.second.has_value(); }) | std::views::take(1), [&](const auto& fd) {
              chan_col[e] = *fd.second;
              std::ranges::for_each(std::span(chain[fd.first]).subspan(1, chain[fd.first].size() - 2), [&vs, c](size_t x) { vs[x].x = c; });
            });
          });

        // A channel to the left of everything moves the drawing over.
        {
          const int shift = std::ranges::fold_left(edge_ids | std::views::filter(backward) | std::views::transform([&chan_col](size_t e) { return chan_col[e]; }), 0, std::ranges::min);
          if (shift < 0) {
            std::ranges::for_each(vs, [shift](vertex& v) { v.x -= shift; });
            std::ranges::for_each(edge_ids | std::views::filter(backward), [&chan_col, shift](size_t e) { chan_col[e] -= shift; });
            right_max -= shift;
          }
        }

        // A channel crosses the bands between its two ends; no run may change
        // rows in its column there.
        std::vector<std::vector<int>> chan_cross(nlayer + 1);
        std::ranges::for_each(edge_ids | std::views::filter(backward), [&](size_t e) { std::ranges::for_each(span(layer[g.edges[e].to], layer[g.edges[e].from] + 2), [&](int b) { chan_cross[b].push_back(chan_col[e]); }); });

        // -- assign the horizontal runs of the bands to rows.  All runs that
        //    touch the same trunk must share a row, otherwise a node would have
        //    more than one merge or divergence point.  Groups whose column ranges
        //    do not overlap can share a row.
        //    Trunk ids: exit of v = v, entry of v = nv + v, channel of e = 2nv + e.
        std::vector<std::vector<band_item>> items(nlayer + 1);
        std::ranges::for_each(edge_ids, [&](size_t e) {
          if (back[e]) {
            const size_t u = g.edges[e].from, v = g.edges[e].to;
            items[layer[u] + 1].push_back({u, 2 * nv + e, vcol(u), chan_col[e], e, -1});
            items[layer[v]].push_back({2 * nv + e, nv + v, chan_col[e], vcol(v), e, -2});
          } else
            std::ranges::for_each(chain[e] | std::views::pairwise | std::views::enumerate, [&](auto iab) {
              const auto [i, ab] = iab;
              const auto [a, b] = ab;
              items[vs[b].layer].push_back({a, nv + b, vcol(a), vcol(b), e, int(i)});
            });
        });

        std::vector<int> nbus(nlayer + 1, 0);
        for (const int b : std::views::iota(0, nlayer + 1)) {
          dog.check();
          std::vector<band_item>& it = items[b];
          if (it.empty())
            continue;
          const auto runs = std::views::iota(size_t(0), it.size());

          // -- the trunks of the band.  A trunk connects either upwards (the exit
          //    of a node above, a channel leaving the band) or downwards (the
          //    entry of a node below, a channel arriving from below).
          std::flat_map<size_t, size_t> local;
          std::vector<int> tcol;
          std::vector<uint8_t> tup;
          const auto tid = [&](size_t t, int col, bool up) {
            const auto [p, fresh] = local.try_emplace(t, tcol.size());
            if (fresh) {
              tcol.push_back(col);
              tup.push_back(up);
            }
            return p->second;
          };
          // numbered in the order the runs name them: ties further down are
          // broken by these numbers
          std::vector<size_t> ta(it.size()), tb(it.size());
          std::ranges::for_each(it | std::views::enumerate, [&](auto ix) {
            const auto& [i, x] = ix;
            ta[i] = tid(x.ta, x.ca, x.hop >= 0 || x.hop == -1);
            tb[i] = tid(x.tb, x.cb, x.hop == -1);
          });
          const size_t nt = tcol.size();
          const auto trunks = std::views::iota(size_t(0), nt);
          const auto other_end = [&](size_t i, size_t t) { return ta[i] == t ? tb[i] : ta[i]; };
          const auto straight = [&it](size_t i) { return it[i].ca == it[i].cb; };

          std::vector<std::vector<size_t>> trun(nt);
          std::vector<size_t> nrun(nt, 0); // the runs that need a row
          std::flat_set<std::pair<size_t, size_t>> linked;
          std::ranges::for_each(runs, [&](size_t i) {
            trun[ta[i]].push_back(i);
            trun[tb[i]].push_back(i);
            if (! straight(i)) {
              ++nrun[ta[i]];
              ++nrun[tb[i]];
            }
            linked.emplace(std::min(ta[i], tb[i]), std::max(ta[i], tb[i]));
          });

          std::vector<int> jog(it.size(), 0);
          std::vector<int> ira(it.size(), 0), irb(it.size(), 0);
          constexpr size_t none = static_cast<size_t>(-1);

          // First try with shared rows, then without, and only if that still
          //  puts two unrelated trunks into one column give every trunk a row.
          for (int attempt = 0;; ++attempt) {
            dog.check();
            const bool merge = attempt == 0;
            const bool strict = attempt >= 2;
            // Everything that meets in one row must meet in one trunk, otherwise
            // the row would suggest connections that do not exist.  So every run
            // lies in the row of one of its two trunks and the other one simply
            // continues its vertical up resp. down to that row.  A trunk can do
            // that for a single run only -- a second one would give it a second
            // merge resp. divergence point -- everything else it carries meets in
            // a row of its own.
            std::vector<size_t> owner(it.size(), none);
            std::vector<uint8_t> stretch(nt, 0);
            std::vector<size_t> sfor(nt, none);
            const auto unowned = [&owner, &straight](size_t i) { return owner[i] == none && ! straight(i); };
            const auto give = [&](size_t i, size_t to, size_t stretched) {
              owner[i] = to;
              stretch[stretched] = 1;
              sfor[stretched] = i;
            };

            if (! strict) {
              // a trunk with a single run never needs a row of its own
              std::ranges::for_each(runs | std::views::filter(std::not_fn(straight)), [&](size_t i) {
                const bool one_a = nrun[ta[i]] == 1;
                const bool one_b = nrun[tb[i]] == 1;
                if (one_a && ! one_b)
                  give(i, tb[i], ta[i]);
                else if (one_b)
                  give(i, ta[i], tb[i]);
              });

              // A trunk whose own column already carries a run straight through
              // must not stretch: the run and the stretched vertical would share
              // the column and part again further down.  That only bites if the
              // trunk keeps a row of its own, which it does not when the run it
              // stretches for is its only one.
              std::vector<uint8_t> fixed(nt, 0);
              std::ranges::for_each(runs | std::views::filter(straight), [&](size_t i) {
                for (const size_t t : {ta[i], tb[i]})
                  if (nrun[t] >= 2)
                    fixed[t] = 1;
              });

              // The runs between two busy trunks form a graph on the trunks in
              // which every trunk may stretch once.  A spanning forest hands the
              // stretch of every non-root trunk to the run it was reached by,
              // which is the most runs that can be served.
              std::vector<std::vector<size_t>> open(nt);
              std::ranges::for_each(runs | std::views::filter(unowned), [&](size_t i) {
                open[ta[i]].push_back(i);
                open[tb[i]].push_back(i);
              });
              // Every row costs one cell where its trunk fans out, so start at
              // the trunks that already carry runs: they own the tree edges and
              // no further trunk needs a row of its own.
              std::vector<size_t> owned(nt, 0);
              std::ranges::for_each(owner | std::views::filter([](size_t o) { return o != none; }), [&owned](size_t o) { ++owned[o]; });
              std::vector<size_t> start = trunks | std::ranges::to<std::vector>();
              std::ranges::stable_sort(start, [&owned, &open, &tup, into_first](size_t x, size_t y) {
                // an edge kept in the row of the trunk it runs into does not
                // have to cross what else runs out of its source -- which helps
                // in some graphs and hurts in others, so both are tried
                if (into_first && tup[x] != tup[y])
                  return tup[x] < tup[y];
                // a run joins a fan that is already there rather than opening
                // a second one just above or below it
                if (owned[x] != owned[y])
                  return owned[x] > owned[y];
                return open[x].size() > open[y].size();
              });
              std::vector<uint8_t> seen(nt, 0);
              std::ranges::for_each(start | std::views::filter([&](size_t s) { return seen[s] == 0 && ! open[s].empty(); }), [&](size_t s) {
                seen[s] = 1;
                std::vector<size_t> q{s};
                for (size_t h = 0; h < q.size(); ++h) // grows while it is walked
                  std::ranges::for_each(open[q[h]] | std::views::filter([&owner](size_t i) { return owner[i] == none; }), [&, t = q[h]](size_t i) {
                    const size_t v = other_end(i, t);
                    if (seen[v] != 0 || fixed[v] != 0)
                      return;
                    seen[v] = 1;
                    give(i, t, v);
                    q.push_back(v);
                  });
              });
              std::ranges::for_each(
                  runs | std::views::filter(unowned),
                  [&](size_t i) { // closes a cycle
                    if (stretch[tb[i]] == 0 && fixed[tb[i]] == 0)
                      give(i, ta[i], tb[i]);
                    else if (stretch[ta[i]] == 0 && fixed[ta[i]] == 0)
                      give(i, tb[i], ta[i]);
                  }
              );
            }

            // -- Two trunks may share one row when what lands in it can still be
            //    told apart: either all of it meets in one trunk, or the runs do
            //    not overlap and each is long enough to carry a direction mark.
            //    Sharing saves a row and turns two corners into one junction.
            std::vector<size_t> grp = trunks | std::ranges::to<std::vector>();
            std::vector<uint8_t> gup(tup);
            if (merge) {
              std::vector<uint8_t> hasrow(nt, 0);
              std::ranges::for_each(owner | std::views::filter([](size_t o) { return o != none; }), [&hasrow](size_t o) { hasrow[o] = 1; });

              std::ranges::for_each(runs | std::views::filter([&owner](size_t i) { return owner[i] != none; }), [&](size_t i) {
                const size_t x = ta[i], y = tb[i];
                if (grp[x] == grp[y] || hasrow[x] == 0 || hasrow[y] == 0)
                  return;
                if ((sfor[x] != none && sfor[x] != i) || (sfor[y] != none && sfor[y] != i))
                  return;

                // nothing busy may reach into the shared row from outside
                const auto reaches_in = [&](size_t t) {
                  return std::ranges::any_of(trun[t], [&](size_t j) {
                    const size_t o = other_end(j, t);
                    return j != i && o != x && o != y && nrun[o] > 1;
                  });
                };
                if (reaches_in(x) || reaches_in(y))
                  return;
                // what would land in the row, each run once
                std::vector<size_t> rr;
                std::ranges::for_each(std::views::concat(trun[x], trun[y]) | std::views::filter([&](size_t j) { return (j == i || owner[j] == x || owner[j] == y) && ! std::ranges::contains(rr, j); }), [&rr](size_t j) { rr.push_back(j); });

                contract_assert(! rr.empty()); // the run itself at the least
                const bool star = std::ranges::any_of(std::array{ta[rr[0]], tb[rr[0]]}, [&](size_t t) { return std::ranges::all_of(rr, [&](size_t j) { return ta[j] == t || tb[j] == t; }); });
                const auto runs_into = [&](size_t p, size_t q2) {
                  const int lo1 = std::min(it[p].ca, it[p].cb), hi1 = std::max(it[p].ca, it[p].cb);
                  const int lo2 = std::min(it[q2].ca, it[q2].cb), hi2 = std::max(it[q2].ca, it[q2].cb);
                  return lo1 + 1 <= hi2 - 1 && lo2 + 1 <= hi1 - 1;
                };
                if (! star && std::ranges::any_of(std::views::cartesian_product(rr, rr), [&](auto pq) {
                      const auto [p, q2] = pq;
                      return p < q2 && runs_into(p, q2);
                    }))
                  return;

                const size_t gx = grp[x], gy = grp[y];
                std::ranges::replace(grp, gy, gx);
                gup[gx] = 1;
                owner[i] = x;
                for (const size_t t : {x, y})
                  if (sfor[t] == i) {
                    stretch[t] = 0;
                    sfor[t] = none;
                  }
              });
            }

            // -- the columns and the extent of the rows
            std::flat_set<int> occ(std::from_range, std::views::concat(tcol, chan_cross[b]));
            std::vector<uint8_t> owns(nt, 0);
            std::vector<int> rlo(nt, INT_MAX), rhi(nt, INT_MIN);
            const auto cover = [&rlo, &rhi, &owns, &grp](size_t t, int c1, int c2) {
              const size_t gt = grp[t];
              owns[gt] = 1;
              rlo[gt] = std::min({rlo[gt], c1, c2});
              rhi[gt] = std::max({rhi[gt], c1, c2});
            };

            std::ranges::for_each(runs, [&](size_t i) {
              if (straight(i)) {
                jog[i] = it[i].cb; // straight through, no row
                return;
              }
              if (owner[i] != none) {
                jog[i] = it[i].cb; // one straight run, no column
                cover(owner[i], it[i].ca, it[i].cb);
                return;
              }
              // next to the end whose trunk carries fewer runs; the busier
              // trunk merges its runs anyway.  The nearest free column, first
              // towards the other end, then away from it.
              const bool at_a = nrun[ta[i]] <= nrun[tb[i]];
              const int anchor = at_a ? it[i].ca : it[i].cb;
              const int other = at_a ? it[i].cb : it[i].ca;
              const int dir = other < anchor ? -1 : 1;
              auto outward = std::views::concat(std::views::single(anchor), std::views::iota(1) | std::views::transform([anchor, dir](int d) { return std::array{anchor + d * dir, anchor - d * dir}; }) | std::views::join);
              const int c = *std::ranges::find_if(outward, [&occ](int p) { return p >= 0 && ! occ.contains(p); });
              occ.insert(c);
              jog[i] = c;
              cover(ta[i], it[i].ca, c);
              cover(tb[i], it[i].cb, c);
            });

            // -- rows.  Rows whose runs do not overlap are reused; trunks coming
            //    from above are served first so that they stay in the upper rows.
            //    That way a trunk that continues to the row of a neighbour always
            //    continues in the direction it already runs in and keeps its
            //    single fan point.
            std::vector<int> used;
            const auto place = [&used](int lo, int hi, size_t from) {
              const auto r = std::ranges::find_if(std::views::iota(from, used.size()), [&used, lo](size_t k) { return used[k] < lo; });
              if (r == std::views::iota(from, used.size()).end()) {
                used.push_back(hi);
                return int(used.size() - 1);
              }
              used[*r] = hi;
              return int(*r);
            };

            std::vector<int> trow(nt, 0);
            size_t from = 0;
            for (const int half : {1, 0}) {
              std::vector<size_t> part = trunks | std::views::filter([&](size_t t) { return grp[t] == t && owns[t] != 0 && int(gup[t]) == half; }) | std::ranges::to<std::vector>();
              std::ranges::stable_sort(part, {}, [&rlo](size_t t) { return rlo[t]; });
              std::ranges::for_each(part, [&](size_t t) { trow[t] = place(rlo[t], rhi[t], from); });
              from = used.size();
            }

            std::ranges::for_each(trunks, [&](size_t t) { trow[t] = trow[grp[t]]; });
            std::ranges::for_each(runs, [&](size_t i) {
              ira[i] = owner[i] != none ? trow[owner[i]] : trow[ta[i]];
              irb[i] = owner[i] != none ? trow[owner[i]] : trow[tb[i]];
            });

            // -- two trunks in the same column may only overlap if a run of this
            //    band connects them; otherwise the drawing would show an edge
            //    that does not exist.  Fall back to the strict layout if so.
            std::vector<int> vlo(nt, INT_MAX), vhi(nt, INT_MIN);
            std::ranges::for_each(runs, [&](size_t i) {
              vlo[ta[i]] = std::min(vlo[ta[i]], ira[i]);
              vhi[ta[i]] = std::max(vhi[ta[i]], ira[i]);
              vlo[tb[i]] = std::min(vlo[tb[i]], irb[i]);
              vhi[tb[i]] = std::max(vhi[tb[i]], irb[i]);
            });
            const bool clash = std::ranges::any_of(
                                   std::views::cartesian_product(trunks, trunks),
                                   [&](auto xy) {
                                     const auto [x, y] = xy;
                                     return x != y && tcol[x] == tcol[y] && tup[x] && ! tup[y] && vhi[x] >= vlo[y] && ! linked.contains({std::min(x, y), std::max(x, y)});
                                   }
                               )
                               // A run straight down has to go downwards, but only where both of
                               // its trunks really keep a row: one that carries nothing else has
                               // none, and then the two are simply one continuous vertical.
                               || std::ranges::any_of(runs, [&](size_t i) { return straight(i) && owns[grp[ta[i]]] != 0 && owns[grp[tb[i]]] != 0 && ira[i] > irb[i]; });

            if (! clash || strict) {
              std::ranges::for_each(runs, [&](size_t i) {
                it[i].ra = ira[i];
                it[i].rb = irb[i];
                it[i].jog = jog[i];
              });
              nbus[b] = int(used.size());
              break;
            }
          }
        }

        // -- vertical placement.  Rows are handed out in the order in which they
        //    can be drawn -- the rows of a band, then the nodes of the layer
        //    below it -- and everything is put as high up as it can go without
        //    running into anything above it.  Two things only constrain each
        //    other if their columns overlap, so a tall node does not push down
        //    what stands beside it.
        using colspan = std::pair<int, int>;
        const auto bands = std::views::iota(0, nlayer + 1);
        std::vector<std::vector<colspan>> bspan(nlayer + 1);
        std::ranges::for_each(bands, [&](int b) {
          bspan[b].assign(size_t(nbus[b]), {INT_MAX, INT_MIN});
          const auto widen = [&bspan, b](int r, int c1, int c2) {
            colspan& s = bspan[b][size_t(r)];
            s = {std::min({s.first, c1, c2}), std::max({s.second, c1, c2})};
          };
          std::ranges::for_each(
              items[b] | std::views::filter([](const band_item& bi) { return bi.ca != bi.cb; }),
              [&widen](const band_item& bi) { // straight ones use no row
                if (bi.ra == bi.rb)
                  widen(bi.ra, bi.ca, bi.cb);
                else {
                  widen(bi.ra, bi.ca, bi.jog);
                  widen(bi.rb, bi.jog, bi.cb);
                }
              }
          );
        });

        const auto meet = [](colspan x, colspan y) { return x.first <= y.second && y.first <= x.second; };
        const auto ncols = [&](size_t i) { return colspan{vcol(i) - nwidth[i] / 2, vcol(i) + nwidth[i] / 2}; };
        // the rows of all bands before one, as pairs of band and row
        const auto band_rows = [&nbus](int upto) { return std::views::iota(0, upto) | std::views::transform([&nbus](int b2) { return std::views::iota(0, nbus[b2]) | std::views::transform([b2](int r2) { return std::pair{b2, r2}; }); }) | std::views::join; };

        // nodes that one node reaches directly and that sit in the same layer
        // have to start in the same row
        std::vector<size_t> same = node_ids | std::ranges::to<std::vector>();
        {
          const auto root = [&same](this auto& self, size_t i) -> size_t { return same[i] == i ? i : same[i] = self(same[i]); };
          std::ranges::for_each(node_ids, [&](size_t u) {
            std::flat_map<int, size_t> first; // one per layer
            std::ranges::for_each(oute[u] | std::views::filter(forward), [&](size_t e) {
              const size_t v = g.edges[e].to;
              const auto [p, fresh] = first.try_emplace(layer[v], v);
              if (! fresh)
                same[root(v)] = root(p->second);
            });
          });
          std::ranges::for_each(node_ids, root);
        }

        std::vector<std::vector<int>> busabs(nlayer + 1);
        std::vector<int> ntop(n, 0);
        // the first row below everything above a band that meets some columns
        const auto below_rows = [&](int upto, colspan s, int init) {
          return std::ranges::fold_left(band_rows(upto) | std::views::filter([&](auto br) { return meet(bspan[br.first][size_t(br.second)], s); }) | std::views::transform([&](auto br) { return busabs[br.first][size_t(br.second)] + 1; }), init, std::ranges::max);
        };
        const auto below_nodes = [&](int upto, colspan s, int init) { return std::ranges::fold_left(node_ids | std::views::filter([&](size_t j) { return layer[j] < upto && meet(ncols(j), s); }) | std::views::transform([&](size_t j) { return ntop[j] + nheight[j]; }), init, std::ranges::max); };
        for (const int b : bands) {
          busabs[b].assign(size_t(nbus[b]), 0);
          for (const int r : std::views::iota(0, nbus[b])) // each row below the one before
            busabs[b][size_t(r)] = below_nodes(b, bspan[b][size_t(r)], below_rows(b, bspan[b][size_t(r)], r > 0 ? busabs[b][size_t(r) - 1] + 1 : 0));
          if (b == nlayer)
            break;

          std::vector<int> lb(n, 0);
          auto in_layer = node_ids | std::views::filter([&layer, b](size_t i) { return layer[i] == b; });
          std::ranges::for_each(in_layer, [&](size_t i) { lb[same[i]] = std::max(lb[same[i]], below_nodes(b, ncols(i), below_rows(b + 1, ncols(i), 0))); });
          std::ranges::for_each(in_layer, [&](size_t i) { ntop[i] = lb[same[i]]; });
        }

        const int total = std::max(
            std::ranges::fold_left(node_ids | std::views::transform([&](size_t i) { return ntop[i] + nheight[i]; }), 0, std::ranges::max), std::ranges::fold_left(band_rows(nlayer + 1) | std::views::transform([&](auto br) { return busabs[br.first][size_t(br.second)] + 1; }), 0, std::ranges::max)
        );

        // A return node without successors drops to the end of the drawing when
        // nothing stands in its way.  It only moves if it gets all the way
        // down: anything short of that would just make the edge leading into it
        // longer for nothing, and it is the only reason worth giving up the
        // common starting row with its siblings for.
        std::ranges::for_each(node_ids | std::views::filter([&](size_t i) { return g.nodes[i].kind == node_kind::ret && oute[i].empty(); }), [&](size_t i) {
          const colspan s = ncols(i);
          const int room = std::ranges::fold_left(
              std::views::concat(
                  band_rows(nlayer + 1) | std::views::filter([&](auto br) { return br.first > layer[i] && meet(bspan[br.first][size_t(br.second)], s); }) | std::views::transform([&](auto br) { return busabs[br.first][size_t(br.second)]; }),
                  node_ids | std::views::filter([&](size_t j) { return layer[j] > layer[i] && meet(ncols(j), s); }) | std::views::transform([&](size_t j) { return ntop[j]; })
              ),
              total, std::ranges::min
          );
          if (room == total)
            ntop[i] = std::max(ntop[i], room - nheight[i]);
        });

        // -- the node boxes are now fully determined.
        out.nodes = node_ids |
                    std::views::transform([&](size_t i) { return layout_node{.id = g.nodes[i].id, .label = g.nodes[i].label, .weight = g.nodes[i].weight, .kind = g.nodes[i].kind, .layer = layer[i], .row = ntop[i], .col = vcol(i) - nwidth[i] / 2, .width = nwidth[i], .height = nheight[i]}; }) |
                    std::ranges::to<std::vector>();

        // -- routing.  Every edge leaves through the single exit point in the
        //    middle of the bottom border and arrives at the single entry point in
        //    the middle of the top border.
        struct run_pos {
          int ra = 0;
          int rb = 0;
          int jog = 0;
        };
        std::vector<std::vector<run_pos>> hop_pos = edge_ids | std::views::transform([&chain](size_t e) { return std::vector<run_pos>(chain[e].empty() ? 0 : chain[e].size() - 1); }) | std::ranges::to<std::vector>();
        std::vector<run_pos> bexit(ne), bentry(ne);
        std::ranges::for_each(bands, [&](int b) {
          std::ranges::for_each(items[b], [&, b](const band_item& bi) {
            // a band that carries nothing but straight runs has no rows at all
            const auto at = [&busabs, b](int r) { return busabs[b].empty() ? 0 : busabs[b][size_t(r)]; };
            const run_pos rp = {at(bi.ra), at(bi.rb), bi.jog};
            if (bi.hop >= 0)
              hop_pos[bi.edge][size_t(bi.hop)] = rp;
            else if (bi.hop == -1)
              bexit[bi.edge] = rp;
            else
              bentry[bi.edge] = rp;
          });
        });

        out.edges = edge_ids | std::views::transform([&](size_t e) {
                      layout_edge le{.from = g.edges[e].from, .to = g.edges[e].to, .backward = back[e], .route = {{}, arrow::down}};
                      if (! back[e]) {
                        contract_assert(chain[e].size() >= 2);
                        const point start{out.nodes[g.edges[e].from].bottom_row(), vcol(chain[e][0])};
                        le.route.pts = std::views::concat(std::views::single(start), chain[e] | std::views::pairwise | std::views::enumerate | std::views::transform([&](auto iab) {
                                                                                       const auto [i, ab] = iab;
                                                                                       const auto [a, b] = ab;
                                                                                       const run_pos& rp = hop_pos[e][size_t(i)];
                                                                                       std::inplace_vector<point, 5> p = {{rp.ra, vcol(a)}, {rp.ra, rp.jog}, {rp.rb, rp.jog}, {rp.rb, vcol(b)}};
                                                                                       if (vs[b].node != graph::npos) // a dummy just carries on
                                                                                         p.push_back({out.nodes[vs[b].node].row, vcol(b)});
                                                                                       return p;
                                                                                     }) | std::views::join) |
                                       std::ranges::to<std::vector>();
                      } else {
                        const layout_node& u = out.nodes[g.edges[e].from];
                        const layout_node& v = out.nodes[g.edges[e].to];
                        le.route.pts = {{u.bottom_row(), u.center_col()}, {bexit[e].ra, u.center_col()}, {bexit[e].ra, bexit[e].jog},   {bexit[e].rb, bexit[e].jog},    {bexit[e].rb, chan_col[e]},
                                        {bentry[e].ra, chan_col[e]},      {bentry[e].ra, bentry[e].jog}, {bentry[e].rb, bentry[e].jog}, {bentry[e].rb, v.center_col()}, {v.row, v.center_col()}};
                      }
                      simplify(le.route.pts);
                      return le;
                    }) |
                    std::ranges::to<std::vector>();

        // The channel is the part that runs against the flow; say so.
        out.marks = edge_ids | std::views::filter(backward) | std::views::transform([&](size_t e) { return polyline{{{(bexit[e].rb + bentry[e].ra) / 2, chan_col[e]}}, arrow::up}; }) | std::ranges::to<std::vector>();

        // A backward edge runs against the flow; where it joins the forward
        // edges again say so with an arrow head just before the junction.
        out.marks.append_range(
            edge_ids | std::views::filter(backward) | std::views::transform([&](size_t e) -> std::optional<polyline> {
              const std::vector<point>& p = out.edges[e].route.pts;
              if (p.size() < 3zu)
                return std::nullopt;
              const point j = p[p.size() - 2zu]; // turn into the entry trunk
              const point q = p[p.size() - 3zu];
              const bool met = std::ranges::any_of(out.edges, [j](const layout_edge& f) { return ! f.backward && on_route(f.route, j); });
              if (! met || q.row != j.row || std::abs(q.col - j.col) < 2)
                return std::nullopt;
              const int d = q.col > j.col ? 1 : -1;
              return polyline{{{j.row, j.col + d}}, d > 0 ? arrow::left : arrow::right};
            }) |
            std::views::join
        ); // an optional is a range of at most one

        const auto everything =
            std::views::concat(out.edges | std::views::transform([](const layout_edge& e) -> const std::vector<point>& { return e.route.pts; }) | std::views::join, out.marks | std::views::transform([](const polyline& p) -> const std::vector<point>& { return p.pts; }) | std::views::join);
        out.rows = std::ranges::fold_left(everything | std::views::transform([](point p) { return p.row + 1; }), total, std::ranges::max);
        out.cols = std::ranges::fold_left(everything | std::views::transform([](point p) { return p.col + 1; }), right_max + 1, std::ranges::max);
        return out;
      };

      // Bends first, then the crossings -- those between two edges that meet
      // in a node count twice, they are the ones the reader trips over, but
      // not so much that a layout may buy one of them with several others.
      // Then the width, then the height, and last of all how long the edges
      // came out.
      using rating = std::array<size_t, 5>;
      const auto score = [](const layout& l) {
        const size_t bends = std::ranges::fold_left(l.edges | std::views::transform([](const layout_edge& e) { return e.route.pts.size() > 2 ? e.route.pts.size() - 2 : 0; }), size_t(0), std::plus{});
        const size_t len = std::ranges::fold_left(
            l.edges | std::views::transform([](const layout_edge& e) { return e.route.pts | std::views::pairwise; }) | std::views::join | std::views::transform([](auto ab) {
              const auto [a, b] = ab;
              return size_t(std::abs(b.row - a.row) + std::abs(b.col - a.col));
            }),
            size_t(0), std::plus{}
        );
        return rating{bends, crossings(l), size_t(l.cols), size_t(l.rows), len};
      };

      // Every way of building, the first of them being the starting point.
      constexpr std::array<bool, 2> both = {false, true};
      auto variants = std::views::cartesian_product(std::views::iota(0, 4), both, both, both, both, both, both);
      std::pair<layout, rating> found = std::ranges::fold_left(
          variants | std::views::drop(1),
          [&] {
            layout first = std::apply(build, variants.front());
            rating r = score(first);
            return std::pair{std::move(first), r};
          }(),
          [&](std::pair<layout, rating> sofar, auto v) {
            layout cand = std::apply(build, v);
            const rating r = score(cand);
            if (r < sofar.second)
              return std::pair{std::move(cand), r};
            return sofar;
          }
      );
      layout best = std::move(found.first);

      // -- The mark in the middle of a channel says which way it runs.  Where
      //    one of the heads at the junctions sits close by on the same vertical
      //    it says so already and the middle one goes.
      {
        const std::vector<polyline> heads = junction_heads(best);
        const auto said_already = [&heads, &cfg](const polyline& m) { return std::ranges::any_of(heads, [&](const polyline& h) { return h.pts[0].col == m.pts[0].col && std::abs(h.pts[0].row - m.pts[0].row) < int(cfg.arrow_gap); }); };
        best.marks = std::views::concat(best.marks | std::views::filter(std::not_fn(said_already)), heads) | std::ranges::to<std::vector>();
      }

      return best;
    }

  } // namespace

  layout_result layout_graph(const graph& g, const config& cfg)
  {
    layout_result res{};
    try {
      res = make_layout(g, cfg, watchdog(cfg.timeout));
    }
    catch (const cancelled&) {
      res = std::unexpected(layout_problem::timeout);
    }
    return res;
  }

  namespace {

    //! The glyph for a cell, before anything placed over it.
    //! \param x the cell
    //! \return the box drawing character
    char32_t glyph(const cell& x) noexcept
    {
      uint8_t m = x.mask;
      if (x.crossing())
        m &= UP | DOWN; // a crossing, not a junction
      const uint8_t d = x.dbl & m;
      const char32_t ch = d == 0 ? line_char[m] : (d & (UP | DOWN)) == 0 ? dh_char[m] : (d & (LEFT | RIGHT)) == 0 ? dv_char[m] : dd_char[m];
      // Dashes exist for straight pieces only; a corner or a junction of a
      // dashed edge keeps the solid glyph.
      const uint8_t st = x.shown_style();
      if (d != 0 || st <= canvas::solid)
        return ch;
      const bool horizontal = (m & (UP | DOWN)) == 0 && (m & (LEFT | RIGHT)) != 0;
      const bool vertical = (m & (LEFT | RIGHT)) == 0 && (m & (UP | DOWN)) != 0;
      return horizontal ? dash_char[st - 1][0] : vertical ? dash_char[st - 1][1] : ch;
    }

    //! The SGR sequence that selects a color, or the terminal's own.
    //! \param c the color, unset for the terminal's own
    //! \return the escape sequence
    std::string color_sgr(const std::optional<rgb>& c)
    {
      return c ? std::format("\033[38;2;{};{};{}m", c->red >> 8, c->green >> 8, c->blue >> 8) : std::string("\033[39m");
    }

  } // namespace

  void draw(const layout& l, int start_row, int height, int start_col, int width, std::ostream& out, painter how)
  {
    const std::vector<attributes> at = enquire(l, how);
    canvas cv(start_row, start_col, height, width);
    paint(l, cv, at);

    // The labels overwrite whatever is underneath, cut to what fits.  What
    // is placed is a whole grapheme cluster: a character that combines with
    // the one before it belongs in the same cell, and a double wide one takes
    // the cell after it as well.
    std::ranges::for_each(l.nodes, [&cv](const layout_node& nd) {
      const int room = nd.width - 2;
      int c = nd.col + 1 + (room - std::min(disp_width(nd.label), room)) / 2;
      int used = 0;
      for (const auto& [text, w] : clusters(nd.label)) {
        if (w <= 0)
          continue; // nothing to put anywhere
        if (used + w > room)
          break;
        cv.put({nd.label_row(), c}, text, w);
        c += w;
        used += w;
      }
    });

    // Arrow heads win over the borders they sit on.  Such a head ends a line
    // that need not be the edge's own: where edges merge, the trunk belongs to
    // whichever of them drew it first, and where they cross, to the vertical
    // one.  Take the color of whatever is really drawn in the cell the head
    // points away from, so that a head never differs from the line it ends --
    // in the whole drawing, not just the viewport, so that the color does
    // not depend on which part of it is shown.  Unless no line is drawn there
    // at all: two boxes may sit right on top of one another, and that border
    // belongs to the node it leaves.
    canvas all(0, 0, l.rows, l.cols);
    paint(l, all, at);
    std::ranges::for_each(l.edges | std::views::enumerate, [&](auto ie) {
      const auto& [ei, e] = ie;
      if (e.route.head == arrow::none || e.route.pts.empty())
        return;
      const std::optional<const cell&> x = all.at(behind(e.route.pts.back(), e.route.head));
      cv.over_now = x && x->kind == canvas::is_edge ? x->shown_owner() : uint32_t(1 + l.nodes.size() + ei);
      cv.put(e.route.pts.back(), arrow_char(e.route.head));
    });
    cv.over_now = 0;

    // The frame of a node and every edge in the color the painter named, and
    // blinking where it asked for that.  A label belongs to no node and to no
    // edge as far as the canvas is concerned, so it keeps the terminal's own
    // color whatever the frame around it does.
    const auto blinks = [&](const cell& x) {
      const uint32_t own = x.shown_owner();
      return own > l.nodes.size() && at[own].style == line_style::blink;
    };

    std::string line;
    for (const size_t r : std::views::iota(size_t(0), size_t(height))) {
      line.clear();
      std::optional<rgb> set;
      bool set_blink = false, touched = false;
      size_t vis = 0;
      for (const size_t c : std::views::iota(size_t(0), size_t(width))) {
        const cell& x = cv.grid[r, c];
        // The right half of a double wide glyph: the terminal moved on by two
        // columns of its own accord and nothing belongs here -- unless the
        // glyph itself is outside the viewport, and then a blank keeps the
        // rest of the line where it belongs.
        if (x.covered) {
          if (c == 0)
            line += ' ';
          continue;
        }
        // A glyph whose second half would fall outside the viewport cannot be
        // drawn at all; a blank keeps the width right.
        const bool cut = x.over_wide && c + 1 == size_t(width);
        const char32_t ch = x.over.empty() ? glyph(x) : U'\0';
        const bool blank = x.over.empty() ? ch == U' ' : cut || x.over == " ";
        const std::optional<rgb> want = blank ? std::nullopt : at[x.shown_owner()].color;
        const bool want_blink = ! blank && blinks(x);
        if (want_blink != set_blink) {
          line += want_blink ? "\033[5m" : "\033[25m";
          set_blink = want_blink;
          touched = true;
        }
        if (want != set) {
          line += color_sgr(want);
          set = want;
          touched = true;
        }
        if (x.over.empty())
          encode(line, ch);
        else if (cut)
          line += ' ';
        else
          line += x.over;
        if (! blank)
          vis = line.size();
      }
      // Trimming can cut a sequence that switched something off again, so
      // put back whatever was ever turned on on this line.  Nothing may leak
      // out of the viewport: what follows the drawing is not ours to color.
      line.resize(vis); // no trailing blanks
      std::println(out, "{}{}", line, touched ? "\033[0m" : "");
    }
  }

  std::vector<cell_paint> classify(const layout& l, int start_row, const int height, int start_col, const int width, painter how)
  {
    const std::vector<attributes> at = enquire(l, how);
    canvas cv(start_row, start_col, height, width);
    paint(l, cv, at);

    std::vector<cell_paint> kd = cv.store | std::views::transform([&at](const cell& x) { return x.mask == 0 ? cell_paint{} : cell_paint{x.kind == canvas::is_node ? cell_kind::node : cell_kind::edge, at[x.owner].color}; }) | std::ranges::to<std::vector>();

    // the boxes are filled
    std::ranges::for_each(l.nodes | std::views::enumerate, [&](auto ind) {
      const auto& [ni, nd] = ind;
      const auto rows = span(std::max(nd.row, start_row), std::min(nd.bottom_row(), start_row + height - 1) + 1);
      const auto cols = span(std::max(nd.col, start_col), std::min(nd.right_col(), start_col + width - 1) + 1);
      std::ranges::for_each(std::views::cartesian_product(rows, cols), [&](auto rc) {
        const auto [r, c] = rc;
        kd[size_t(r - start_row) * size_t(width) + size_t(c - start_col)] = {cell_kind::node, at[1 + ni].color};
      });
    });

    return kd;
  }

  void write_xpm(const layout& l, int start_row, int height, int start_col, int width, unsigned magnify, bool dark, std::ostream& out, painter how)
  {
    const std::vector<cell_paint> kd = classify(l, start_row, height, start_col, width, how);

    // The ground, the boxes and the edges have their own colors; whatever
    // the painter asked for takes their place.  The palette is whatever ends
    // up being used, the ground first.
    using color = std::array<uint8_t, 3>;
    static constexpr std::array<std::array<color, 3>, 2> preset = {{{{{0xff, 0xff, 0xff}, {0x00, 0x00, 0x00}, {0x2a, 0x6f, 0xd6}}}, {{{0x00, 0x00, 0x00}, {0xe8, 0xe8, 0xe8}, {0x6e, 0xa8, 0xfe}}}}};
    const std::array<color, 3>& base = preset[dark ? 1 : 0];
    const auto color_of = [&base](const cell_paint& p) { return p.color ? color{static_cast<uint8_t>(p.color->red >> 8), static_cast<uint8_t>(p.color->green >> 8), static_cast<uint8_t>(p.color->blue >> 8)} : base[std::to_underlying(p.kind)]; };

    std::vector<color> palette{base[0]};
    const std::vector<size_t> index = kd | std::views::transform([&](const cell_paint& p) -> size_t {
                                        if (p.kind == cell_kind::empty)
                                          return 0;
                                        const color want = color_of(p);
                                        const auto q = std::ranges::find(palette, want);
                                        if (q != palette.end())
                                          return size_t(q - palette.begin());
                                        palette.push_back(want);
                                        return palette.size() - 1;
                                      }) |
                                      std::ranges::to<std::vector>();

    // One symbol per color, two characters per pixel once one is not enough.
    static constexpr std::string_view alpha = " .#+@*=-;:,oOxX0123456789abcdefghijklmnpqrstuv"
                                              "wyzABCDEFGHIJKLMNPQRSTUVWYZ";
    contract_assert(palette.size() <= alpha.size() * alpha.size());
    const unsigned cpp = palette.size() <= alpha.size() ? 1 : 2;
    const auto symbol = [cpp](size_t k) { return cpp == 2 ? std::string{alpha[k / alpha.size()], alpha[k % alpha.size()]} : std::string{alpha[k % alpha.size()]}; };

    // One line of pixels per cell row, each repeated 'magnify' times.
    const std::vector<std::string> pixel_rows =
        index | std::views::chunk(size_t(width)) |
        std::views::transform([&](auto row) { return std::format("\"{}\"", row | std::views::transform([&](size_t k) { return std::views::repeat(symbol(k), magnify) | std::views::join | std::ranges::to<std::string>(); }) | std::views::join | std::ranges::to<std::string>()); }) |
        std::views::transform([magnify](std::string s) { return std::views::repeat(std::move(s), magnify) | std::ranges::to<std::vector>(); }) | std::views::join | std::ranges::to<std::vector>();

    std::print(out, "/* XPM */\nstatic char * flowgraph[] = {{\n\"{} {} {} {}\",\n", size_t(width) * magnify, size_t(height) * magnify, palette.size(), cpp);
    std::ranges::for_each(palette | std::views::enumerate, [&](auto kc) {
      const auto& [k, c] = kc;
      std::print(out, "\"{} c #{:02x}{:02x}{:02x}\",\n", symbol(size_t(k)), c[0], c[1], c[2]);
    });
    std::print(out, "{}\n}};\n", pixel_rows | std::views::join_with(std::string_view(",\n")) | std::ranges::to<std::string>());
  }

} // namespace flowgraph
