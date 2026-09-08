// Driver: read a graph description, lay it out, draw a viewport of it.
#include "flowgraph.hh"
#include "pager.hh"

#include <algorithm>
#include <array>
#include <charconv>
#include <chrono>
#include <climits>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <flat_map> // IWYU pragma: keep
#include <fstream>
#include <iostream>
#include <optional>
#include <print>
#include <ranges>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <variant>

#include <stdint.h>

namespace {

  //! The text without the blanks at either end.  Nothing is copied: the
  //! result shares the bytes of the argument, which has to outlive it.
  //! \param s the text
  //! \return the part of it that is not blank
  constexpr std::string_view trim(const std::string_view s) noexcept post(r : r.size() <= s.size())
  {
    constexpr std::string_view blank = " \t\r\n";
    const size_t a = s.find_first_not_of(blank);
    return a == std::string_view::npos ? std::string_view{} : s.substr(a, s.find_last_not_of(blank) + 1 - a);
  }

  //! The next blank separated word of a line, which is moved past it.
  //! \param s the rest of the line
  //! \return the word, empty if there is none
  std::string_view word(std::string_view& s) noexcept
  {
    const size_t a = std::min(s.find_first_not_of(" \t"), s.size());
    const size_t b = std::min(s.find_first_of(" \t", a), s.size());
    const std::string_view w = s.substr(a, b - a);
    s.remove_prefix(b);
    return w;
  }

  // The library knows a node by a number and nothing else; the file knows it
  // by a name.  Which number a name gets is of no consequence, so these are
  // deliberately neither small nor consecutive: nothing that mistook them
  // for indices could go unnoticed.
  struct name_table {
    std::flat_map<std::string, unsigned long> number{};
    std::flat_map<unsigned long, std::string> back{};

    //! The number of a name, made up the first time the name turns up.
    //! \param s the name
    //! \return its number
    unsigned long of(std::string_view s) post(r : r != 0)
    {
      const auto [p, fresh] = number.try_emplace(std::string(s), 0);
      if (fresh) {
        p->second = 0x9e3779b97f4a7c15ull + number.size() * 0x100000001b3ull;
        back.emplace(p->second, p->first);
      }
      return p->second;
    }

    //! What the file called the node with that number.
    //! \param id the number
    //! \return the name, "?" for a number that never came from here
    std::string_view name(unsigned long id) const
    {
      const auto p = back.find(id);
      return p == back.end() ? "?" : std::string_view(p->second);
    }
  };

// GCC 16 sees the promise of a coroutine allocated with ::operator new and
// released through the class own operator delete, and takes the two for a
// mismatched pair.  They are not: libstdc++ hands the release back to
// ::operator delete with the very size it allocated.  The warning is about
// <generator>, not about anything here.
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wmismatched-new-delete"

  //! Hand out, one at a time, the nodes and edges a graph description file
  //! names: lines starting with 'B', 'N' or 'R' are nodes (name, weight,
  //! label), lines starting with 'E' are edges (from, to).  Empty
  //! lines and lines starting with '#' are ignored.  Throws
  //! std::runtime_error naming the line for anything malformed.
  //! \param in the stream to read
  //! \param fname its name, for the messages
  //! \param names where the names are turned into the numbers the library
  //!        goes by
  //! \param where filled in with the line every record came from, so that a
  //!        complaint about one of them can name it
  //! \return each record in turn
  flowgraph::graph_source records(std::istream& in, std::string_view fname, name_table& names, std::vector<unsigned>& where)
  {
    unsigned lineno = 0;
    const auto fail = [&fname, &lineno](std::string_view msg) { throw std::runtime_error(std::format("{}:{}: {}", fname, lineno, msg)); };

    for (std::string line; std::getline(in, line);) {
      ++lineno;
      const std::string_view t = trim(line);
      if (t.empty() || t[0] == '#')
        continue;

      const char type = t[0];
      std::string_view rest = t.substr(1);
      where.push_back(lineno);

      if (type == 'B' || type == 'N' || type == 'R') {
        const std::string_view id = word(rest);
        const std::string_view w = word(rest);
        if (id.empty() || w.empty()) [[unlikely]]
          fail("expected node name and weight");
        if (w.starts_with('-')) [[unlikely]]
          fail("negative weight");
        unsigned long weight = 0;
        if (std::from_chars(w.data(), w.data() + w.size(), weight).ptr != w.data() + w.size()) [[unlikely]]
          fail("expected node name and weight");
        // a node with nothing to say for itself is labelled with its name
        const std::string_view label = trim(rest);
        co_yield flowgraph::node_record{names.of(id), label.empty() ? id : label, weight, type == 'B' ? flowgraph::node_kind::begin : type == 'R' ? flowgraph::node_kind::ret : flowgraph::node_kind::inner};
      } else if (type == 'E') {
        const std::string_view a = word(rest), b = word(rest);
        if (a.empty() || b.empty()) [[unlikely]]
          fail("expected two node names");
        co_yield flowgraph::edge_record{names.of(a), names.of(b)};
      } else [[unlikely]]
        fail(std::format("unknown record type '{}'", type));
    }
  }

#pragma GCC diagnostic pop

  //! Read a graph description file.
  //! \param in the stream to read
  //! \param fname its name, for the messages
  //! \param names where the names of the nodes are left behind
  //! \return the graph
  flowgraph::graph read_graph(std::istream& in, std::string_view fname, name_table& names)
  {
    std::vector<unsigned> where; // the line each record came from
    try {
      return flowgraph::graph(records(in, fname, names, where));
    }
    catch (const flowgraph::graph_error& e) {
      // The graph knows what is wrong with itself, but not where it came from
      // and not what any of it is called.  Both are known here.
      using reason = flowgraph::graph_error::reason;
      const std::string msg = e.why == reason::no_nodes ? std::string("no nodes") : e.why == reason::no_begin ? std::string("no begin node") : e.why == reason::duplicate ? std::format("duplicate node name '{}'", names.name(e.node)) : std::format("unknown node '{}'", names.name(e.node));
      if (e.record < where.size())
        throw std::runtime_error(std::format("{}:{}: {}", fname, where[e.record], msg));
      throw std::runtime_error(std::format("{}: {}", fname, msg));
    }
  }

  //! A hash that does not change from one run to the next, so that the same
  //! graph always comes out looking the same.
  //! \param s the text to fold in
  //! \param h what to fold it into
  //! \return the new hash
  constexpr uint64_t fold(std::string_view s, uint64_t h) noexcept
  {
    return std::ranges::fold_left(s, h, [](uint64_t acc, char c) { return (acc ^ static_cast<uint8_t>(c)) * 0x100000001b3ull; });
  }

  //! Make up the appearance of a node or an edge from a hash of its names.
  //! The library hands out the numbers it was given; what those are called is
  //! known here.
  //! \param names what the nodes are called
  //! \param what the node or the edge being asked about
  //! \return a colour, and for an edge how its line is drawn
  flowgraph::attributes hashed(const name_table& names, const flowgraph::draw_item& what)
  {
    const uint64_t h = std::visit(
        [&names](const auto& x) {
          constexpr uint64_t seed = 0xcbf29ce484222325ull;
          if constexpr (std::same_as<std::remove_cvref_t<decltype(x)>, flowgraph::node_item>)
            return fold("node", fold(names.name(x.id), seed)) ^ std::to_underlying(x.kind);
          else
            return fold(x.backward ? "back" : "", fold(names.name(x.to), fold("->", fold(names.name(x.from), seed))));
        },
        what
    );

    // A fully saturated hue picked by the hash, then mixed with a quarter of
    // white so that it reads on a light and on a dark ground alike.
    const unsigned hue = static_cast<unsigned>(h >> 24) % 1536;
    const unsigned up = hue % 256, down = 255 - up;
    const std::array<std::array<unsigned, 3>, 6> ramp = {{{255, up, 0}, {down, 255, 0}, {0, 255, up}, {0, down, 255}, {up, 0, 255}, {255, 0, down}}};
    const auto chan = [&](unsigned i) {
      const unsigned v = 64 + ramp[hue / 256][i] * 3 / 4;
      return static_cast<uint16_t>(v << 8 | v); // 8 bits to 16
    };

    flowgraph::attributes a;
    a.color = flowgraph::rgb{chan(0), chan(1), chan(2)};
    if (std::holds_alternative<flowgraph::edge_item>(what)) {
      static constexpr std::array<flowgraph::line_style, 8> styles = {flowgraph::line_style::solid,      flowgraph::line_style::solid, flowgraph::line_style::solid,       flowgraph::line_style::solid,
                                                                      flowgraph::line_style::dense_dash, flowgraph::line_style::dash,  flowgraph::line_style::sparse_dash, flowgraph::line_style::blink};
      a.style = styles[h % styles.size()];
    }
    return a;
  }

  constexpr std::string_view usage = "Usage: flowgraph [OPTION]... [FILE]\n"
                                     "Lay out the directed graph described in FILE and draw it.\n"
                                     "Records: 'B'/'N'/'R' NAME WEIGHT LABEL for begin/inner/return nodes,\n"
                                     "'E' FROM TO for an edge.\n"
                                     "\n"
                                     "Viewport (default: the whole drawing, or the terminal with -p):\n"
                                     "  -r, --row=ROW        first row of the viewport (0 based)\n"
                                     "  -h, --height=N       number of rows of the viewport\n"
                                     "  -c, --col=COL        first column of the viewport (0 based)\n"
                                     "  -w, --width=N        number of columns of the viewport\n"
                                     "\n"
                                     "Layout:\n"
                                     "  -W, --node-width=N   default (minimum) node width, odd  [11]\n"
                                     "  -x, --max-width=N    node width never goes beyond this   [20]\n"
                                     "  -l, --label-width=N  a label wider than this is broken across\n"
                                     "                       the lines of the box                  [16]\n"
                                     "  -T, --target-width=N width the drawing should come out, which\n"
                                     "                       buys back room for cut labels          [0]\n"
                                     "  -M, --max-height=N   maximum number of lines per node   [20]\n"
                                     "  -m, --min-height=N   minimum number of lines per node   [3]\n"
                                     "  -g, --gap=N          free columns between two nodes     [3]\n"
                                     "  -a, --arrow-gap=N    rows that make a second arrow head on one\n"
                                     "                       vertical worth having                [5]\n"
                                     "  -t, --timeout=N      seconds the layout may take, 0 for no\n"
                                     "                       limit                                [20]\n"
                                     "\n"
                                     "Other:\n"
                                     "  -p, --page           show the drawing one screen at a time; cursor "
                                     "keys\n"
                                     "                       scroll, + and - change the picture beside it\n"
                                     "  -X, --xpm            write an XPM bitmap instead of characters\n"
                                     "  -z, --zoom=N         pixels per cell in the bitmap             [1]\n"
                                     "  -D, --dark           draw the bitmap on a black ground\n"
                                     "  -P, --plain          plain solid lines in the terminal's own colour\n"
                                     "  -d, --describe       print the layout description instead of drawing\n"
                                     "      --help           display this help and exit\n";

  //! A whole decimal number.
  //! \param s the text
  //! \return the number, nothing if the text is not one
  std::optional<long> number(std::string_view s) noexcept
  {
    long val = 0;
    const auto [end, ec] = std::from_chars(s.data(), s.data() + s.size(), val);
    if (s.empty() || ec != std::errc{} || end != s.data() + s.size()) [[unlikely]]
      return std::nullopt;
    return val;
  }

  constexpr std::string_view kindname(flowgraph::node_kind k) noexcept
  {
    switch (k) {
    case flowgraph::node_kind::begin:
      return "begin";
    case flowgraph::node_kind::ret:
      return "return";
    default:
      return "inner";
    }
  }

  //! The points of a polyline as text, " (row,col)" each.
  //! \param pts the points
  //! \return the text
  std::string points(const std::vector<flowgraph::point>& pts)
  {
    return pts | std::views::transform([](flowgraph::point p) { return std::format(" ({},{})", p.row, p.col); }) | std::views::join | std::ranges::to<std::string>();
  }

  //! Print the layout the way the checker wants to read it.
  //! \param l the layout
  //! \param names what the nodes are called
  //! \param out where to
  void describe(const flowgraph::layout& l, const name_table& names, std::ostream& out)
  {
    std::println(out, "drawing {} rows x {} columns", l.rows, l.cols);
    std::println(out, "nodes:");
    std::ranges::for_each(l.nodes, [&](const flowgraph::layout_node& nd) {
      std::println(out, "  {} \"{}\" weight={} {} layer={} at row={} col={} size={}x{}", names.name(nd.id), nd.label, nd.weight, kindname(nd.kind), nd.layer, nd.row, nd.col, nd.height, nd.width);
      // Only worth saying when the label is not drawn the way it came in.
      if (nd.lines.size() != 1 || nd.lines.front() != nd.label)
        std::println(out, "    text:{}", nd.lines | std::views::transform([](const std::string& s) { return std::format(" \"{}\"", s); }) | std::views::join | std::ranges::to<std::string>());
    });
    std::println(out, "edges:");
    std::ranges::for_each(l.edges, [&](const flowgraph::layout_edge& e) { std::println(out, "  {} -> {} ({}):{}", names.name(l.nodes[e.from].id), names.name(l.nodes[e.to].id), e.backward ? "backward" : "forward", points(e.route.pts)); });
    if (! l.marks.empty()) {
      std::println(out, "markers:");
      std::ranges::for_each(l.marks, [&out](const flowgraph::polyline& p) { std::println(out, " {}", points(p.pts)); });
    }
  }

  // An option that takes a number: its two spellings and where the number
  // goes.  Layout options are unsigned and may not be negative, the sizes of
  // the viewport may not be either, but its corner may lie outside.
  struct option {
    std::string_view lng;
    char shrt;
    long* value = nullptr;
    unsigned* uvalue = nullptr;
    long least = 0;
  };

} // namespace

int main(int argc, char* argv[])
{
  flowgraph::config cfg;
  long row = 0, col = 0, height = -1, width = -1, zoom = 1;
  long timeout = std::chrono::duration_cast<std::chrono::seconds>(cfg.timeout).count();
  bool want_describe = false, want_xpm = false, want_dark = false;
  bool want_page = false, want_plain = false;
  const char* fname = nullptr;

  const std::array<option, 14> opts = {
    {{"--row", 'r', &row, nullptr, LONG_MIN},
     {"--col", 'c', &col, nullptr, LONG_MIN},
     {"--height", 'h', &height},
     {"--width", 'w', &width},
     {"--node-width", 'W', nullptr, &cfg.default_width},
     {"--max-width", 'x', nullptr, &cfg.max_width},
     {"--label-width", 'l', nullptr, &cfg.max_label_width},
     {"--target-width", 'T', nullptr, &cfg.target_width},
     {"--max-height", 'M', nullptr, &cfg.max_height},
     {"--min-height", 'm', nullptr, &cfg.min_height},
     {"--gap", 'g', nullptr, &cfg.node_gap},
     {"--arrow-gap", 'a', nullptr, &cfg.arrow_gap},
     {"--timeout", 't', &timeout},
     {"--zoom", 'z', &zoom, nullptr, 1}}
  };
  const std::span<char*> args(argv + 1, size_t(std::max(argc - 1, 0)));

  for (size_t i = 0; i < args.size(); ++i) {
    const std::string_view a = args[i];
    // the argument of an option, either the next word or what follows '='
    const auto value = [&](const option& o) -> std::optional<std::string_view> {
      if (a == o.lng || (a.size() == 2 && a[0] == '-' && a[1] == o.shrt)) {
        if (i + 1 >= args.size()) [[unlikely]] {
          std::println(stderr, "flowgraph: missing argument for {}", a);
          std::exit(1);
        }
        return std::string_view(args[++i]);
      }
      if (a.starts_with(o.lng) && a.size() > o.lng.size() && a[o.lng.size()] == '=')
        return a.substr(o.lng.size() + 1);
      return std::nullopt;
    };

    if (a == "--help") {
      std::print("{}", usage);
      return 0;
    }
    if (a == "-d" || a == "--describe") {
      want_describe = true;
      continue;
    }
    if (a == "-P" || a == "--plain") {
      want_plain = true;
      continue;
    }
    if (a == "-p" || a == "--page") {
      want_page = true;
      continue;
    }
    if (a == "-X" || a == "--xpm") {
      want_xpm = true;
      continue;
    }
    if (a == "-D" || a == "--dark") {
      want_dark = true;
      continue;
    }

    std::optional<std::string_view> v;
    const auto o = std::ranges::find_if(opts, [&](const option& o) { return (v = value(o)).has_value(); });
    if (o != opts.end()) {
      const std::optional<long> num = number(*v);
      if (! num || *num < o->least) [[unlikely]] {
        std::println(stderr, "flowgraph: invalid argument '{}' for {}", *v, o->lng);
        return 1;
      }
      if (o->value != nullptr)
        *o->value = *num;
      else
        *o->uvalue = unsigned(*num);
      continue;
    }

    if (a.starts_with('-') && a != "-") [[unlikely]] {
      std::print(stderr, "flowgraph: unknown or malformed option '{}'\n{}", a, usage);
      return 1;
    }
    if (fname != nullptr) [[unlikely]] {
      std::println(stderr, "flowgraph: too many file arguments");
      return 1;
    }
    fname = args[i];
  }

  // A day is longer than anybody waits and keeps the conversion to
  // milliseconds inside what the type holds.
  cfg.timeout = std::chrono::seconds(std::min(timeout, 24L * 60 * 60));

  try {
    name_table names;
    const flowgraph::graph g = [&] {
      if (fname == nullptr || std::strcmp(fname, "-") == 0)
        return read_graph(std::cin, "<stdin>", names);
      std::ifstream in(fname);
      if (! in) [[unlikely]]
        throw std::runtime_error(std::format("{}: cannot open", fname));
      return read_graph(in, fname, names);
    }();

    const flowgraph::layout_result got = flowgraph::layout_graph(g, cfg);
    if (! got) [[unlikely]]
      throw std::runtime_error(std::format("laying the graph out took longer than {} seconds", timeout));
    const flowgraph::layout& l = *got;

    const int h = int(height < 0 ? l.rows : height);
    const int w = int(width < 0 ? l.cols : width);
    const auto paint = [&names](const flowgraph::draw_item& what) { return hashed(names, what); };
    const flowgraph::painter how = want_plain ? flowgraph::painter(flowgraph::plain) : flowgraph::painter(paint);
    if (want_page) {
      if (! page(l, how)) [[unlikely]] {
        std::println(stderr, "flowgraph: --page needs a terminal");
        return 1;
      }
    } else if (want_describe)
      describe(l, names, std::cout);
    else if (want_xpm)
      flowgraph::write_xpm(l, int(row), h, int(col), w, unsigned(zoom), want_dark, std::cout, how);
    else
      flowgraph::draw(l, int(row), h, int(col), w, std::cout, how);
  }
  catch (const std::exception& e) {
    std::println(stderr, "flowgraph: {}", e.what());
    return 1;
  }

  return 0;
}
