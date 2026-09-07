#include "pager.hh"

#include <algorithm>
#include <array>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <format>
#include <optional>
#include <print>
#include <ranges>
#include <sstream>
#include <string>
#include <string_view>
#include <vector> // IWYU pragma: keep

#include <poll.h>
#include <termios.h>
#include <unistd.h>
#include <sys/ioctl.h>

namespace {

  volatile ::sig_atomic_t resized = 0;

  //! Note that the terminal changed its size; the poll below is interrupted
  //! by the signal and the main loop redraws.
  void winch(int)
  {
    resized = 1;
  }

  //! Ask the terminal for its size.
  //! \return rows and columns, 24x80 if the size cannot be determined
  std::pair<int, int> screen_size() post(r : r.first > 0 && r.second > 0)
  {
    ::winsize ws{};
    if (::ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) != 0 || ws.ws_row == 0 || ws.ws_col == 0) [[unlikely]]
      return {24, 80};
    return {ws.ws_row, ws.ws_col};
  }

  //! Read a single byte from the terminal.
  //! \param timeout milliseconds to wait, -1 to block
  //! \return the byte, -1 on timeout or interruption, -2 at end of input
  int readbyte(int timeout) post(r : r >= -2 && r < 256)
  {
    ::pollfd p{STDIN_FILENO, POLLIN, 0};
    if (::poll(&p, 1, timeout) <= 0)
      return -1;
    unsigned char c;
    const ::ssize_t n = ::read(STDIN_FILENO, &c, 1);
    if (n == 0) [[unlikely]]
      return -2;
    if (n < 0) [[unlikely]]
      return -1;
    return c;
  }

  //! Encode bytes the way the kitty graphics protocol wants its payload.
  //! \param d the bytes
  //! \return the base64 text
  std::string base64(const std::vector<unsigned char>& d) post(r : r.size() == (d.size() + 2) / 3 * 4)
  {
    static constexpr std::string_view tbl = "ABCDEFGHIJKLMNOPQRSTUVWXYZ"
                                            "abcdefghijklmnopqrstuvwxyz0123456789+/";
    return d | std::views::chunk(3) | std::views::transform([](auto three) {
             const size_t k = size_t(std::ranges::distance(three));
             const unsigned n = unsigned(three[0]) << 16 | (k > 1 ? unsigned(three[1]) << 8 : 0u) | (k > 2 ? unsigned(three[2]) : 0u);
             return std::array{tbl[n >> 18 & 63], tbl[n >> 12 & 63], k > 1 ? tbl[n >> 6 & 63] : '=', k > 2 ? tbl[n & 63] : '='};
           }) |
           std::views::join | std::ranges::to<std::string>();
  }

  // The colours of the picture, red, green, blue and how opaque.  The palette
  // is the dark one: the picture is readable whatever ground the terminal
  // has.  The last one marks the ground of the part that the text beside the
  // picture is showing; it is see-through, so what is behind stays visible.
  using pixel = std::array<unsigned char, 4>;
  constexpr std::array<pixel, 4> ink = {{
    {0x14, 0x14, 0x14, 0xff}, // ground
    {0xe8, 0xe8, 0xe8, 0xff}, // boxes
    {0x6e, 0xa8, 0xfe, 0xff}, // edges
    {0xe8, 0xff, 0x00, 0x20}  // visible part
  }};

  //! The whole drawing as RGBA pixels, one cell becoming mag x mag of them.
  //! What the painter asked for takes the place of the palette; the rest of
  //! what it says has no meaning in a picture this small.
  //! \param l the layout
  //! \param mag pixels per cell in each direction
  //! \param top first row the text beside the picture shows
  //! \param left first column it shows
  //! \param rows how many rows it shows
  //! \param cols how many columns it shows
  //! \param how the painter
  //! \return the pixels, row by row, four bytes each
  std::vector<unsigned char> pixels(const flowgraph::layout& l, const unsigned mag, int top, int left, int rows, int cols, flowgraph::painter how) pre(mag > 0) post(r : r.size() == size_t(l.rows) * size_t(l.cols) * mag * mag * 4)
  {
    const std::vector<flowgraph::cell_paint> kd = flowgraph::classify(l, 0, l.rows, 0, l.cols, how);
    const auto colour = [&](int r, int c) {
      const flowgraph::cell_paint& k = kd[size_t(r) * size_t(l.cols) + size_t(c)];
      const bool shown = r >= top && r < top + rows && c >= left && c < left + cols;
      if (k.kind == flowgraph::cell_kind::empty)
        return ink[shown ? 3 : 0];
      if (! k.color)
        return ink[std::to_underlying(k.kind)];
      return pixel{static_cast<unsigned char>(k.color->red >> 8), static_cast<unsigned char>(k.color->green >> 8), static_cast<unsigned char>(k.color->blue >> 8), 0xff};
    };
    // every cell mag pixels wide, every row of cells mag rows of pixels high
    return std::views::iota(0, l.rows) | std::views::transform([&](int r) {
             const std::vector<unsigned char> row = std::views::iota(0, l.cols) | std::views::transform([&, r](int c) { return std::views::repeat(colour(r, c), mag) | std::views::join; }) | std::views::join | std::ranges::to<std::vector>();
             return std::views::repeat(row, mag) | std::views::join | std::ranges::to<std::vector>();
           }) |
           std::views::join | std::ranges::to<std::vector>();
  }

  //! Hand the image over to the terminal under id 1, in chunks of the size the
  //! protocol allows.
  //! \param l the layout
  //! \param mag pixels per cell
  //! \param top first row the text beside the picture shows
  //! \param left first column it shows
  //! \param rows how many rows it shows
  //! \param cols how many columns it shows
  //! \param how the painter
  //! \return the escape sequences to write
  std::string transmit(const flowgraph::layout& l, unsigned mag, int top, int left, int rows, int cols, flowgraph::painter how) pre(mag > 0)
  {
    const std::string b64 = base64(pixels(l, mag, top, left, rows, cols, how));
    constexpr size_t piece = 4096;
    const size_t pieces = (b64.size() + piece - 1) / piece;
    return b64 | std::views::chunk(piece) | std::views::enumerate | std::views::transform([&](auto ic) {
             const auto [i, chunk] = ic;
             return std::format("\033_G{}m={};{}\033\\", i == 0 ? std::format("a=t,i=1,f=32,s={},v={},q=2,", size_t(l.cols) * mag, size_t(l.rows) * mag) : std::string(), size_t(i) + 1 == pieces ? 0 : 1, std::string_view(chunk));
           }) |
           std::views::join | std::ranges::to<std::string>();
  }

  //! Find out whether the terminal understands the kitty graphics protocol.
  //! A query for a one pixel image is followed by a request for the device
  //! attributes: every terminal answers the second, so it bounds the wait.
  //! \return true if the graphics query was answered
  bool graphics()
  {
    std::print("\033_Gi=31,s=1,v=1,a=q,t=d,f=24;AAAA\033\\\033[c");
    std::fflush(stdout);
    std::string reply;
    for (int c; (c = readbyte(300)) >= 0;) {
      reply += char(c);
      if (c == 'c' && reply.contains("\033[?"))
        break;
    }
    return reply.contains("\033_G");
  }

  //! The three numbers of a mouse report, "button;column;row".
  //! \param rep the text between the introducer and the final letter
  //! \return the numbers, nothing if the text is not of that form
  std::optional<std::array<int, 3>> mouse_report(std::string_view rep)
  {
    const std::vector<std::optional<int>> nums = rep | std::views::split(';') | std::views::transform([](auto part) -> std::optional<int> {
                                                   const std::string_view t(part);
                                                   int v = 0;
                                                   const auto [end, ec] = std::from_chars(t.data(), t.data() + t.size(), v);
                                                   if (ec != std::errc{} || end != t.data() + t.size()) [[unlikely]]
                                                     return std::nullopt;
                                                   return v;
                                                 }) |
                                                 std::ranges::to<std::vector>();
    if (nums.size() != 3 || ! std::ranges::all_of(nums, &std::optional<int>::has_value)) [[unlikely]]
      return std::nullopt;
    return std::array{*nums[0], *nums[1], *nums[2]};
  }

  //! Put the terminal into raw mode and onto the alternative screen, and undo
  //! both again however the pager is left.
  struct terminal {
    ::termios saved{};
    bool ok = false;

    terminal()
    {
      if (::tcgetattr(STDIN_FILENO, &saved) != 0) [[unlikely]]
        return;
      ::termios raw = saved;
      ::cfmakeraw(&raw);
      raw.c_cc[VMIN] = 1;
      raw.c_cc[VTIME] = 0;
      if (::tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw) != 0) [[unlikely]]
        return;
      ok = true;
      // alternative screen, no cursor, and reports for the buttons and for
      // the movement while one of them is held down
      std::print("\033[?1049h\033[?25l\033[?1002h\033[?1006h");
      std::fflush(stdout);
    }

    ~terminal()
    {
      if (! ok)
        return;
      std::print(
          "\033_Ga=d,d=A,q=2\033\\"
          "\033[?1006l\033[?1002l\033[?25h\033[?1049l"
      );
      std::fflush(stdout);
      ::tcsetattr(STDIN_FILENO, TCSAFLUSH, &saved);
    }

    terminal(const terminal&) = delete ("the terminal is restored exactly once");
    terminal& operator=(const terminal&) = delete ("the terminal is restored exactly once");
  };

} // namespace

bool page(const flowgraph::layout& l, flowgraph::painter how)
{
  if (! ::isatty(STDIN_FILENO) || ! ::isatty(STDOUT_FILENO)) [[unlikely]]
    return false;

  const terminal term;
  if (! term.ok) [[unlikely]]
    return false;

  struct ::sigaction sa{};
  sa.sa_handler = winch;
  ::sigaction(SIGWINCH, &sa, nullptr);

  const bool image = graphics();
  unsigned mag = 2, sent = 0;
  int top = 0, left = 0;
  int sent_top = -1, sent_left = -1, sent_rows = -1, sent_cols = -1;
  bool dragging = false;
  int grab_x = 0, grab_y = 0, grab_top = 0, grab_left = 0;
  for (;;) {
    const auto [srows, scols] = screen_size();
    const int vh = std::max(1, srows - 1); // one row for the status line

    // The picture goes into the upper right corner and takes as many columns
    // away from the text as it needs.  Its magnification is capped by what is
    // left over, which is also what makes '+' stop.
    ::winsize ws{};
    const bool px_known = image && ::ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == 0 && ws.ws_xpixel > 0 && ws.ws_ypixel > 0;
    const int cw = px_known ? ws.ws_xpixel / ws.ws_col : 0;
    const int ch = px_known ? ws.ws_ypixel / ws.ws_row : 0;
    const auto fits = [&](unsigned m) { return l.cols * int(m) <= scols / 2 * cw && l.rows * int(m) <= vh * ch; };
    const auto sizes = std::views::iota(1u, std::min(mag, 32u) + 1) | std::views::reverse;
    const auto largest = cw > 0 && ch > 0 ? std::ranges::find_if(sizes, fits) : sizes.end();
    const unsigned show = largest != sizes.end() ? *largest : 0;
    const int icols = show != 0 ? (l.cols * int(show) + cw - 1) / cw : 0;
    mag = show != 0 ? show : mag;
    const int vw = std::max(1, scols - (icols != 0 ? icols + 1 : 0));

    // The corner may never leave the drawing, which is also what makes the
    // cursor keys stop at the edges.
    top = std::clamp(top, 0, std::max(0, l.rows - vh));
    left = std::clamp(left, 0, std::max(0, l.cols - vw));

    std::ostringstream os;
    flowgraph::draw(l, top, vh, left, vw, os, how);
    const std::string body = os.str();

    // In raw mode every line needs its own carriage return, and the rest of
    // the line has to be cleared: draw() suppresses trailing blanks.
    std::string_view text(body);
    if (text.ends_with('\n'))
      text.remove_suffix(1);
    std::string frame = "\033[H";
    frame += text | std::views::split('\n') | std::views::transform([](auto line) { return std::format("{}\033[K\r\n", std::string_view(line)); }) | std::views::join | std::ranges::to<std::string>();

    // The picture is sent once per magnification and only placed again on
    // every redraw.
    if (show != 0) {
      // The marked part of the picture follows the text, so the pixels go
      // over again whenever either the magnification or the visible part of
      // the drawing changed.
      if (show != sent || top != sent_top || left != sent_left || vh != sent_rows || vw != sent_cols) {
        frame += transmit(l, show, top, left, vh, vw, how);
        sent = show;
        sent_top = top;
        sent_left = left;
        sent_rows = vh;
        sent_cols = vw;
      }
      frame += std::format(
          "\033_Ga=d,d=i,i=1,q=2\033\\\033[1;{}H"
          "\033_Ga=p,i=1,C=1,q=2\033\\",
          scols - icols + 1
      );
    }

    std::string status = std::format(
        "{},{} - {},{}  of {}x{}{}   arrows scroll"
        "  q quits",
        top, left, std::min(top + vh, l.rows) - 1, std::min(left + vw, l.cols) - 1, l.rows, l.cols, show != 0 ? std::format("  x{}", show) : ""
    );
    status.resize(size_t(std::max(0, scols - 1)), ' ');
    frame += std::format("\033[{};1H\033[7m{}\033[0m", srows, status);
    std::print("{}", frame);
    std::fflush(stdout);

    const int k = readbyte(-1);
    if (k == -2) [[unlikely]]
      break;
    if (k < 0) { // interrupted, redraw
      resized = 0;
      continue;
    }
    if (k == 'q' || k == 'Q' || k == 3) // 3 is control-C
      break;
    if (k == '+' || k == '=') {
      ++mag; // capped again on redraw
      continue;
    }
    if (k == '-' && mag > 1) {
      --mag;
      continue;
    }
    if (k != 0x1b)
      continue;

    const int kind = readbyte(50);
    if (kind != '[' && kind != 'O')
      continue;

    int c = readbyte(50);
    if (c != '<') {
      switch (c) {
      case 'A':
        --top;
        break;
      case 'B':
        ++top;
        break;
      case 'C':
        ++left;
        break;
      case 'D':
        --left;
        break;
      default:
        break;
      }
      continue;
    }

    // A report of the mouse: the button and the cell it is over, ended by 'M'
    // for a press or a movement and by 'm' for a release.
    std::string rep;
    while ((c = readbyte(50)) >= 0 && c != 'M' && c != 'm')
      rep += char(c);
    if (c < 0) [[unlikely]]
      continue;
    const std::optional<std::array<int, 3>> report = mouse_report(rep);
    if (! report) [[unlikely]]
      continue;
    const auto [button, mx, my] = *report;

    if (c == 'm') { // the button came up again
      dragging = false;
      continue;
    }
    if (button == 0 && show != 0 && cw > 0 && ch > 0) {
      // Taking hold of the marked part of the picture: the cell under the
      // pointer has to be inside it.
      const int ix = mx - (scols - icols + 1);
      const int iy = my - 1;
      const int gr = (iy * ch + ch / 2) / int(show);
      const int gc = (ix * cw + cw / 2) / int(show);
      if (ix >= 0 && ix < icols && iy >= 0 && gr >= top && gr < top + vh && gc >= left && gc < left + vw) {
        dragging = true;
        grab_x = mx;
        grab_y = my;
        grab_top = top;
        grab_left = left;
      }
      continue;
    }
    if (dragging && button == 32) { // moved with the button down
      top = grab_top + (my - grab_y) * ch / int(show);
      left = grab_left + (mx - grab_x) * cw / int(show);
    }
  }

  return true;
}
