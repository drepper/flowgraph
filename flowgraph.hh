// Layout of directed graphs for display on a character terminal.
#ifndef FLOWGRAPH_HH_
# define FLOWGRAPH_HH_ 1

# include <cstddef>
# include <functional>
# include <generator>
# include <optional>
# include <stdexcept>
# include <string>
# include <string_view>
# include <variant>
# include <vector>

# include <stdint.h>

namespace flowgraph {

  // ---------------------------------------------------------------- input ---

  enum struct node_kind { begin, inner, ret };

  // A graph is described to this library one piece at a time: a node, or an
  // edge between two nodes.  Where the pieces come from -- a file, a
  // database, a data structure of the caller's own -- is no business of this
  // library, which never sees more than one of them at a time.
  //
  // A node is known by a number the caller picks.  It has to be unique and
  // that is all: nothing here takes it for an index, so the numbers need be
  // neither small nor consecutive.  Whatever the caller calls its nodes among
  // itself stays with the caller.  The label is the one piece of text this
  // library needs, because it draws it, and it is borrowed only for the call
  // that hands the node over.
  struct node_record {
    unsigned long id = 0;   // unique, nothing more
    std::string_view label; // shown in the drawing
    unsigned long weight = 0;
    node_kind kind = node_kind::inner;
  };

  struct edge_record {
    unsigned long from = 0; // the numbers of the two
    unsigned long to = 0;   // nodes it connects
  };

  using graph_item = std::variant<node_record, edge_record>;

  // The coroutine that hands the pieces out, one per call.
  using graph_source = std::generator<graph_item>;

  // What a graph could not be built from.  'record' counts the pieces the
  // source handed out, from zero, and says which one the complaint is about;
  // it is 'nowhere' when the complaint is about the graph as a whole.  Only
  // the source knows where its pieces came from and what it calls its nodes,
  // so what() names neither: it says what is wrong in the terms this library
  // has, and the source is expected to say it again in its own.
  struct graph_error : std::runtime_error {
    static constexpr std::size_t nowhere = static_cast<std::size_t>(-1);

    // no node at all, none to begin at, a node handed over twice, or an edge
    // naming one that never came
    enum struct reason { no_nodes, no_begin, duplicate, unknown };

    reason why;
    std::size_t record = nowhere; // which piece of the source
    unsigned long node = 0;       // the node it is about, if any

    graph_error(reason r, std::size_t rec, unsigned long n, const std::string& msg) : std::runtime_error(msg), why(r), record(rec), node(n) {}
  };

  // How the graph is kept once it has been taken in.
  struct node_desc {
    unsigned long id = 0; // what the caller calls it
    std::string label;    // name shown in the drawing
    unsigned long weight = 0;
    node_kind kind = node_kind::inner;
  };

  struct edge_desc {
    std::size_t from;
    std::size_t to;
  };

  struct graph {
    static constexpr std::size_t npos = static_cast<std::size_t>(-1);

    std::vector<node_desc> nodes{};
    std::vector<edge_desc> edges{};
    std::vector<std::size_t> begins{}; // indices of the begin nodes

    graph() = default;

    //! Take in everything the source hands out.  An edge may name nodes that
    //! the source only gets to later.  Throws graph_error for a number used
    //! twice, for an edge naming a node that never comes, for a graph without
    //! nodes and for one with no node to begin at.
    //! \param items where the pieces come from
    explicit graph(graph_source items) post(! nodes.empty() && ! begins.empty());

    //! Look a node up by the number the caller gave it.
    //! \param id the number
    //! \return its index, npos if there is no such node
    std::size_t find(unsigned long id) const noexcept post(r : r == npos || r < nodes.size());
  };

  // --------------------------------------------------------------- layout ---

  struct config {
    unsigned default_width = 11; // minimum node width, forced odd
    unsigned max_height = 20;    // upper bound for a node's lines
    unsigned min_height = 3;     // desired lower bound, never < 3
    unsigned node_gap = 3;       // free columns between two nodes
    unsigned arrow_gap = 5;      // rows that make a second
                                 // arrow head on one vertical
                                 // worth having
  };

  struct point {
    int row = 0;
    int col = 0;

    auto operator<=>(const point&) const noexcept = default;
  };

  enum struct arrow { none, up, down, left, right };

  // A sequence of alternating horizontal and vertical segments.  Consecutive
  // points share a row or a column.  'head' is drawn at the last point.
  struct polyline {
    std::vector<point> pts;
    arrow head = arrow::none;
  };

  struct layout_node {
    unsigned long id = 0; // what the caller calls it
    std::string label;
    unsigned long weight = 0;
    node_kind kind = node_kind::inner;
    int layer = 0;  // distance from the start
    int row = 0;    // top border
    int col = 0;    // left border
    int width = 0;  // including both borders
    int height = 0; // including both borders

    int center_col() const noexcept pre(width > 0) { return col + width / 2; }
    int label_row() const noexcept pre(height > 0) { return row + height / 2; }
    int bottom_row() const noexcept pre(height > 0) { return row + height - 1; }
    int right_col() const noexcept pre(width > 0) { return col + width - 1; }

    //! Is the cell inside the box, border included?
    //! \param q the cell
    //! \return true if so
    bool covers(point q) const noexcept { return q.row >= row && q.row <= bottom_row() && q.col >= col && q.col <= right_col(); }
  };

  struct layout_edge {
    std::size_t from = 0; // index into layout::nodes
    std::size_t to = 0;
    bool backward = false;
    polyline route; // starts on 'from', ends on 'to'
  };

  // Complete description of the drawing.  All coordinates are 0 based and
  // relative to the upper left corner of the drawing.
  struct layout {
    std::vector<layout_node> nodes{};
    std::vector<layout_edge> edges{};
    std::vector<polyline> marks{}; // entry/exit indicators
    int rows = 0;
    int cols = 0;
  };

  layout layout_graph(const graph& g, const config& cfg = {}) pre(! g.nodes.empty()) post(l : l.nodes.size() == g.nodes.size() && l.edges.size() == g.edges.size() && l.rows > 0 && l.cols > 0);

  // ------------------------------------------------------------ appearance ---

  struct rgb {
    ::uint16_t red = 0, green = 0, blue = 0;

    bool operator==(const rgb&) const noexcept = default;
  };

  // How a line is drawn.  'blink' is drawn solid and made to blink.
  enum struct line_style { solid, blink, dense_dash, dash, sparse_dash };

  struct attributes {
    std::optional<rgb> color{}; // unset: the terminal's own
    line_style style = line_style::solid;
  };

  // What the painter is asked about: a node by its number and its kind, an
  // edge by the numbers of its two ends and whether it runs backwards.  The
  // numbers are the caller's own, so it can make of them what it likes.
  struct node_item {
    unsigned long id = 0;
    node_kind kind = node_kind::inner;
  };

  struct edge_item {
    unsigned long from = 0;
    unsigned long to = 0;
    bool backward = false;
  };

  using draw_item = std::variant<node_item, edge_item>;

  // Asked once for every node and once for every edge before anything is
  // drawn.  It is not kept beyond the call it is handed to.
  using painter = std::function_ref<attributes(const draw_item&)>;

  //! The painter used when none is given: plain solid lines in the
  //! terminal's own color.
  //! \return the default attributes
  inline attributes plain(const draw_item&) noexcept
  {
    return {};
  }

  // ---------------------------------------------------------------- output ---

  // Draw the part of L visible in the viewport [start_row,start_row+height) x
  // [start_col,start_col+width).  Exactly 'height' lines are written, each
  // terminated by a newline; trailing blanks are suppressed.  The cursor is
  // assumed to be at the beginning of the first line of the viewport.
  void draw(const layout& l, int start_row, int height, int start_col, int width, std::ostream& out, painter how = plain) pre(height >= 0) pre(width >= 0);

  // What every cell of the viewport carries, ignoring the labels.  The boxes
  // are solid: without their labels an outline alone is easy to lose among
  // the edges.  The result has height * width entries, row by row.
  enum struct cell_kind : ::uint8_t { empty, node, edge };

  struct cell_paint {
    cell_kind kind = cell_kind::empty;
    std::optional<rgb> color; // what the painter asked for
  };

  std::vector<cell_paint> classify(const layout& l, int start_row, const int height, int start_col, const int width, painter how = plain) pre(height >= 0) pre(width >= 0) post(r : r.size() == std::size_t(height) * std::size_t(width));

  // The same viewport as an XPM image, without the labels: one cell becomes
  // 'magnify' x 'magnify' pixels, the boxes filled in one color, the edges
  // drawn in another.  'dark' swaps the palette for one on a black ground.
  void write_xpm(const layout& l, int start_row, int height, int start_col, int width, unsigned magnify, bool dark, std::ostream& out, painter how = plain) pre(height >= 0) pre(width >= 0) pre(magnify > 0);

} // namespace flowgraph

#endif // flowgraph.hh
