# flowgraph — terminal layout for directed graphs

Reads a description of a directed graph, computes a layout suitable for a
character terminal and draws any rectangular part of it with box drawing
characters.

    make                              # needs GCC 16 or later, see below
    ./flowgraph G1                    # draw the whole graph
    ./flowgraph G1 -r 12 -h 12 -c 4 -w 26    # draw one viewport
    ./flowgraph G1 -p                 # page through it in the terminal
    ./flowgraph G1 -d                 # print the layout description
    ./flowgraph G1 -X -z 4 >G1.xpm    # the same drawing as a bitmap
    ./flowgraph G1 -X -D >G1.xpm      # ... on a black ground
    ./flowgraph G1 -P                 # no colours, plain solid lines
    ./browse                          # page through random graphs

## Input format

The library itself reads no files: it is handed a graph one node and one edge
at a time (see the API below), and the format below is what the driver in
`main.cc` happens to hand it.  Anything else that can name nodes and edges
does just as well.

One record per line; empty lines and lines starting with `#` are ignored.
The first character selects the record type:

| Type | Fields                    | Meaning                          |
|------|---------------------------|----------------------------------|
| `B`  | *name* *weight* *label*   | a beginning node (may occur more than once) |
| `N`  | *name* *weight* *label*   | an inner node                    |
| `R`  | *name* *weight* *label*   | a return node                    |
| `E`  | *from* *to* [*type*]      | a directed edge of that type, 0 if none is given |
| `C`  | *name* *text*             | one line of what a node carries  |
| `P`  | *name* *successor*        | the successor the node prefers   |

*name* is the internal identifier used in the edge records -- the driver gives
every one of them a number, which is all the library goes by -- *weight* a
non-negative integer and *label* the rest of the line.  A label is UTF-8 and is
measured in the columns it really takes on a terminal, so a double wide
character such as `❌` counts for two and a combining accent for none; a box
is made wide enough for that and the drawing keeps its shape.  A label that
would make the box wider than `max_width` is broken across the lines of the
box and, if it still does not fit, cut short with an ellipsis (see the layout
rules below).  Begin nodes are laid out like any other node, so they usually
do not end up in the same rows.

A node may carry a block of text besides its name.  Every `C` record adds one
line of it, in the order the records come; a `C` record may stand before or
after the line that introduces its node, which is why the driver reads the
whole file before it hands anything over.  The *text* is everything after the
single blank that follows *name*, taken exactly as it is written -- what it is
indented by belongs to it -- except that `\n` in it stands for a line break
and `\\` for a backslash, so one record can carry several lines.  A tab
becomes the blanks up to the next eighth column, since how wide a tab is
depends on where it lands and a box has to know.

    B f 4 fetch
    C f   mov    (%rdi),%eax
    C f   test   %eax,%eax
    C f   je     .L4

A node that carries anything is made big enough to show all of it, whatever
`max_width` and `max_height` say, and its label becomes a heading over it,
cut to the width the content asks for.  A node whose label is empty gets no
heading and no line for one -- and, unlike a node without content, does not
fall back to being labelled with its name.

A `P` record says which of its successors a node prefers -- the block control
falls through to, say.  There has to be an `E` record for that edge, a node
prefers one successor at most, and like `C` the record may stand anywhere in
the file.  The edge to the preferred successor is drawn as straight and as short
as the drawing allows, ahead of everything else the layout weighs (see the
layout rules below).

    B t 3 test
    N y 3 then
    N n 3 else
    E t y
    E t n
    P t n

## Building

The code is C++26 and uses contracts, so it needs GCC 16 or later with
`-std=c++26 -fcontracts`; the Makefile turns the contracts on and enforces
them (`-fcontract-evaluation-semantic=enforce`), so a broken precondition
stops the program with a message naming it rather than carrying on.  Other
It links against **libunistring**, which is what knows how wide a piece of
text is on a terminal and where one character ends and the next begins
(`u8_width`, `u8_grapheme_next`, `u8_uctomb`).  Other
`std::flat_map`, `std::mdspan`, `std::inplace_vector`, `std::optional<T&>`
and the C++26 range adaptors (`concat`, `cache_latest`, `enumerate`,
`cartesian_product`, ...), all of which libstdc++ 16 provides.  GCC 16 warns
`-Wmismatched-new-delete` about the promise of a `std::generator`; that is
about `<generator>` rather than about anything here, and is switched off
around the coroutines here with a note saying so.  A `graph_source` written
by a caller draws the same warning for the same reason.  Clang, and
therefore clangd, does not parse contracts yet: the editor may flag the
`pre`/`post` clauses, the compiler does not.

## API

### Handing over a graph

Where a graph comes from is no business of this library.  It is described to
it one piece at a time by a coroutine, and never sees more than one piece at
a time:

```c++
struct node_record {
  unsigned long id = 0;             // unique, nothing more
  std::string_view label;           // shown in the drawing
  std::string_view content;         // ... above this, when there is any
  unsigned long weight = 0;
  node_kind kind = node_kind::inner;
  std::optional<unsigned long> preferred{}; // the successor it prefers
};

struct edge_record { unsigned long from = 0, to = 0; int type = 0; };

using graph_item = std::variant<node_record, edge_record>;
using graph_source = std::generator<graph_item>;

explicit graph::graph(graph_source items);
```

A node is known by a number the caller picks.  It has to be unique and that
is all: nothing here takes it for an index, so the numbers need be neither
small nor consecutive -- an address, a hash, a database key all do.  Names
are the caller's own business and stay there; the label is the one piece of
text this library needs, because it draws it, and it is borrowed only for the
call that hands the node over.  An edge may name nodes the source only gets
to later.  A source is therefore just a coroutine over whatever the caller
already has:

```c++
flowgraph::graph_source walk()
{
  for (const auto& [name, b] : program)
    co_yield flowgraph::node_record{ number(b), b.name, b.cycles, kind_of(b) };
  for (const auto& [name, b] : program)
    for (const std::string& to : b.goes_to)
      co_yield flowgraph::edge_record{ number(b), number(program.at(to)) };
}

const flowgraph::graph g{ walk() };
```

The painter is asked about those same numbers, so whatever the caller makes
of them it can make of them there too.

What the graph cannot make sense of -- a number handed over twice, an edge
naming a node that never comes, a preference for a node there is no edge to,
no nodes at all, no node to begin at -- it
throws as a `graph_error`.  That says what is wrong in the terms this library
has: a `reason`, the number of the piece of the source it is about, and the
node in question.  It names neither a place nor a name, because it knows
neither.  The reader in `main.cc` keeps the line each record came from and
what the file called each node, and says the whole thing again in those
terms: `G1:2: unknown node 'b'`.

### The rest

```c++
layout_result layout_graph(const graph& g, const config& cfg = {});
void   draw(const layout& l, int start_row, int height, int start_col,
            int width, std::ostream& out, painter how = plain);
std::vector<cell_paint> classify(const layout& l, int start_row, int height,
                                 int start_col, int width, painter how = plain);
void   write_xpm(const layout& l, int start_row, int height, int start_col,
                 int width, unsigned magnify, bool dark, std::ostream& out,
                 painter how = plain);
```

Every function states what it needs and what it delivers as a contract:
the `graph` constructor promises a graph with nodes and with somewhere to
begin, 
`layout_graph` wants at least one node and promises that a layout it does
deliver has a node and an edge for each of the graph's and a positive size;
`draw`, `classify` and
`write_xpm` want a viewport of non-negative size and a magnification of at
least one, and `classify` promises exactly `height * width` cells.  The
driver checks its arguments so that it never runs into one of them.

`layout_graph` returns a complete, self-contained description of the drawing:
the position and size of every node box together with its label both as it was
handed over (`label`) and as it is drawn (`lines`, one entry per line of the
box, with `truncated` saying whether that is all of it), an orthogonal
polyline plus an arrow head for every edge, the entry/exit
markers, and the total size (`rows` x `cols`) of the drawing.  All coordinates are 0 based and relative to the upper
left corner of the drawing.

It searches, though, and how long that takes grows fast with the graph, so it
is on a clock: `config::timeout` says how much of it the search may have and
the answer comes back as a `std::expected`.  When the time is up the search
stops where it is and the result holds a `layout_problem` instead -- so far
`timeout` is the only thing that can go wrong.  A timeout of zero lets it run
as long as it likes.  The clock is read wherever the work could take a while,
which for a large graph means the answer comes a few tens of milliseconds
after the moment asked for.  The driver turns the empty result into
`flowgraph: laying the graph out took longer than 20 seconds` and `-t` sets
another limit.

`draw` renders the viewport
`[start_row, start_row+height) x [start_col, start_col+width)`.  It writes
exactly `height` lines, each terminated by a newline, with trailing blanks
suppressed; the cursor is assumed to be at the start of the first line.  Parts
of the graph outside the viewport are clipped, so a viewport is always an exact
slice of the full drawing.

### The painter

How a node or an edge looks is not the layout's business.  Whoever draws may
pass a *painter*, which is asked once about every node and every edge before
anything is put on the canvas:

```c++
struct rgb { std::uint16_t red = 0, green = 0, blue = 0; };
enum struct line_style { solid, blink, dense_dash, dash, sparse_dash };

struct attributes {
  std::optional<rgb> colour;                    // unset: the terminal's own
  line_style style = line_style::solid;         // edges only
};

struct node_item { std::string_view name; node_kind kind; };
struct edge_item { std::string_view from, to; bool backward; };
using draw_item = std::variant<node_item, edge_item>;
using painter = std::function_ref<attributes(const draw_item&)>;

attributes plain(const draw_item&) noexcept;    // the default: nothing at all
```

The painter is a `std::function_ref`: it is borrowed for the duration of the
call and never stored, so any callable will do and nothing is copied.  `plain`
is what is used when none is given.

A node is named by its internal name and whether it begins, ends or sits
inside the graph; an edge by the names of its two ends and whether it runs
against the flow.  The colour applies to the frame of a node and to the whole
of an edge, and it is the colour of that node resp. edge in the bitmap as
well.  An arrow head belongs to the line it ends rather than to the border it
is drawn on, so it carries the colour of an edge into the frame of the node it
points at.  Where several edges merge, that is the colour of the trunk they
share, not of any one of them: a head never differs from the line leading into
it.  Two boxes may also touch, and then the head ends no line at all and takes
the colour of its own edge.  Labels are never coloured -- they keep whatever the terminal uses,
so they stay readable inside a frame of any colour.  `style` only concerns edges:
`blink` sets the blink attribute, the three dash weights choose `╌ ┄ ┈` resp.
`╎ ┆ ┊` for the straight pieces.  Nothing but the colour reaches the bitmap.

Where two lines share a cell the box wins over an edge and a solid line wins
over a dashed one, so no piece of a solid edge is ever left out.  With the
default painter every edge is solid, nothing blinks and nothing is coloured:
the output is plain text, byte for byte what it was before painters existed.

The driver hands in a painter that hashes the names it is given, so a graph
always comes out looking the same; `-P` leaves the painter out.

## Paging

`./flowgraph FILE -p` shows the drawing on the alternative screen of the
terminal, starting at its upper left corner and sized to fit, with the last
row left for a status line.  The cursor keys move the visible part around and
stop at the edges of the drawing: right and down do nothing once the far side
is reached, left and up do nothing at 0.  `q` or `Q` returns to the normal
screen.  The status line names the corners of the visible part and the size of
the whole drawing:

    0,0 - 18,33  of 47x34   arrows scroll  q quits

On a terminal that understands the kitty graphics protocol the whole drawing
is shown as a picture in the upper right corner, and the text gets the columns
that are left.  `+` and `-` change its magnification, two pixels per cell to
begin with; `+` stops once the picture would no longer fit beside the text.
The part of the drawing that the text is showing is marked in the picture: its
background pixels are drawn in a see-through neon yellow, so it says where in
the whole one is without hiding anything.  The mark follows the cursor keys.

The mark can also be taken hold of with the mouse: pressing the left button
over it and moving with the button held down drags the visible part along, one
cell of the terminal counting for as many rows resp. columns of the drawing as
the magnification hides in it.  Letting go of the button ends it, and the
corner is kept inside the drawing exactly as the cursor keys keep it.  This
needs a terminal that reports the buttons and the movement while one is held
down; the pager asks for those reports on the way in and turns them off again
on the way out.

The pixels are built straight from the layout, not by way of the XPM writer,
and sent again whenever the magnification or the visible part changes.  Their
four colours -- ground, boxes, edges and the mark, each with its own opacity --
are the table at the top of `pager.cc`, changed the same way as any of the
others.  Terminals that do not answer the graphics query, or do not report
their cell size, simply get the full width for text.

The terminal is put into raw mode for this and restored on the way out,
whichever way the pager is left.  A change of the terminal size redraws.

`classify` says what every cell of a viewport carries -- background, box or
edge, and the colour the painter gave it -- which is what both the XPM writer
and the picture in the pager are built from.

`write_xpm` writes the same viewport as an XPM image instead of characters.
One cell becomes `magnify` x `magnify` pixels, so the image is
`width * magnify` by `height * magnify`.  It carries the boxes and the edges
only, no labels, in three colours: black boxes and blue edges on white, or
with `dark` set light grey boxes and a lighter blue on black.  The boxes are
filled rather than outlined -- without their labels an outline alone is easy
to lose among the edges.  Dashes and arrow heads are not distinguished at this size; what the
image shows is the shape of the graph.  A painter's colours replace the box
and edge colours cell by cell; the palette of the image is built from whatever
colours actually occur, so it stays small when few are used.

### Configuration (`struct config`)

| Field           | Default | Meaning                                        |
|-----------------|---------|------------------------------------------------|
| `default_width` | 11      | minimum node width, forced odd                 |
| `max_width`     | 20      | hard upper bound on the columns of a node, rounded down to odd |
| `max_label_width` | 16    | a label wider than this is broken across lines |
| `target_width`  | 0       | how wide the drawing should come out, zero when it does not matter |
| `max_height`    | 20      | hard upper bound on the lines of a node        |
| `min_height`    | 3       | desired lower bound, never less than 3         |
| `node_gap`      | 3       | free columns between two boxes                 |
| `arrow_gap`     | 5       | rows that make a second arrow head on one vertical worth having |
| `timeout`       | 20 s    | how long the search may take, zero for no limit |
| `name_color`    | white   | the heading of a node that carries content, unset for none |
| `name_ground`   | dark blue | the ground behind that heading, unset for none |

The magnification of the bitmap is not part of `config`, it is an argument of
`write_xpm` (`-z` on the command line).

## Layout rules

* **Width** — `max(default_width, length(label) + 4)`, rounded up to an odd
  number so that every box has a center column, and never beyond `max_width`.
  Getting a long label to stay inside that bound is what the two rules below
  are for.
* **Edge types** — edges of different types are never drawn as the same line.
  A node gets an exit on its bottom border for every type it is left by and an
  entry on its top border for every type it is entered by, two columns apart
  and side by side around the middle, in the order of where their edges are
  headed so that they need not cross right at the node.  That asks for a
  node at least `2k + 3` columns wide for `k` types.  The points are settled
  before anything is lined up, and lining up aims at them rather than at the
  middles of the nodes: a node is pulled into the column that puts the point
  of an edge right under the point it comes from, and a long edge is
  straightened through the points at its two ends, so that a second type costs
  hardly a bend.  Between the nodes, the
  trunks that edges share are kept apart by type, and two trunks only share a
  row -- which joins their lines -- when everything landing in it is of one
  type.  Edges of the same type share lines exactly as before, so a graph whose
  edges are all of one type, whichever it is, is drawn as it always was.
* **Long edges** — the dummies of a long edge are brought into one column
  wherever every layer it runs through has room for it there, and this is
  done over and over while it still changes anything: of two long edges running
  side by side, the one in the way of the other may be out of the way once it is
  straight itself.
* **Merging early** — edges of the same type running into the same node share
  a single line from as high up as they can: in every layer more than one of
  them passes they use one dummy between them, so each joins that line right
  below its own source instead of running down beside the others and merging
  only just above the node.  That saves the parallel lines, most of the
  crossings they make, and a good deal of ink.  The longest of the edges
  carries the shared line when it is straightened.  An edge to a preferred
  successor keeps a line of its own, so that straightening it disturbs nothing
  else.
* **Pulling together** — a wide layer pushes what stands in it far out, and a
  long edge running straight down from there stays out as far long after the
  layers around it have room again.  So the drawing is also built with
  everything pulled towards its middle, the blocks nearest the middle first.
  A vertex moves along with whatever it stands in one column with through an
  edge, so no straight edge bends, and every layer keeps its order and at least
  its spacing.  Lines between two layers can still come to cross where they
  did not, so whether the pulled-together drawing is kept is up to the rating,
  as with every other choice.
* **Preferred successors** — the edge from a node to the successor it prefers
  comes before everything else.  The depth first search that decides which
  edges run backwards follows it first, so it is the other edges that close
  the cycles.  A node is pulled down from the layer its longest way in would
  give it until it stands right above its topmost successor -- the preferred
  one when nothing else is higher, which makes the edge span a single layer --
  and a whole run of preferences closes up from the bottom.  When the nodes
  of a layer are ordered and placed, a neighbour a preferred edge leads to
  weighs more than all the others together; Brandes/Köpf tries to align with
  it before the medians, and a node pulled into a neighbour's column picks it
  before any other.  Last, every preferred edge is straightened, pushing aside
  whatever stands in its way even where that makes the drawing wider, from the
  top down so that a chain of them lines up in one column.  Of all the layouts built, a bend on a
  preferred edge counts as ten bends anywhere else, and the length of preferred
  edges comes right after the crossings, before the width: a preferred edge
  runs straight unless a layout that is far better everywhere else buys it a
  jog.
  Without any preferences every one of these steps does nothing, and the
  layout is exactly the one made without them.
* **Content** — a node that carries content is sized by that content and by
  nothing else: a line for every line of it plus one for the heading, and
  columns for the widest of them, borders and a blank on either side included.
  Neither `max_width` nor `max_height` applies, because the point of content is
  that it is shown as it stands.  The weight still sets a floor, so such a box
  is never smaller than it would otherwise have been.  The heading sits on the
  topmost line inside the box, the content in the middle of what is left, and
  every line of the content starts in the same column so that whatever it
  lines up in stays lined up.  `layout_node` keeps the two apart: `lines` is
  the heading, `body` the content.
* **Breaking a label** — a label wider than `max_label_width` is broken across
  the lines of its box, if the box has more than one; a box of three lines has
  room for a single one and nothing can be done there.  The break goes between
  words, and inside a word only when the word alone is too long for a line.
  Since `max_width` leaves the label `max_width - 4` columns, `max_label_width`
  is capped at that: with the defaults both come to 15 and a label is broken
  exactly when it would otherwise make the box too wide.  Setting
  `max_label_width` lower breaks labels earlier and keeps the boxes narrower
  than they would have to be.
* **Cutting a label** — what is still too long once the box has run out of
  lines is dropped, and the last line that is drawn ends in `…` to say so.
  `layout_node::truncated` says that the name in the box is not the whole
  name.
* **Aiming for a width** — `target_width`, when the caller gives one, says how
  wide the drawing should come out.  A drawing that stays narrower than that
  while labels are still being cut short has room to spare, so the layout is
  made again with a larger `max_label_width` (and `max_width` with it, since a
  label may only have what its box may have).  That repeats -- taking bigger
  steps while the drawing is still far too narrow, smaller ones once a step
  overshoots -- until the drawing is wide enough, or a step would take it more
  than five percent past the target, or no name is cut short any more.
  Anything within five percent of the target counts as wide enough.  It costs
  one whole layout per step, a handful of them in practice, and the timeout
  covers all of them together.  While the drawing is still too narrow the
  bound on how wide a node may be does not apply: `max_width` grows with
  `max_label_width`, so a name is shown in full whenever the target has the
  room for it.  The library asks for no target of its own; the driver takes
  `$COLUMNS` when the shell exports it and otherwise asks the terminal the
  drawing goes to how wide it is, so a drawing made for a terminal fills the
  width that terminal has.  `-T` says otherwise, and a redirected drawing has
  no terminal and so no target.
* **Taking what is left over** — once everything has its place, a name that is
  still cut short takes whatever room is free beside it.  Nothing moves: the
  box grows by one column on each side at a time, so its center column, where
  the edges attach, stays where it is.  A column is only taken when it is free
  over the whole height of the box, lies inside the drawing, and does not
  bring the box up against another one, and the box stops growing the moment
  its name fits.  Where the room runs out first the name is still cut, only
  less of it: a box hemmed in on both sides gains nothing, and one that
  already spans the whole width of the drawing has nowhere to go.
* **Height** — strictly proportional to the weight, borders included.  The
  scale is the smallest one that gives the lightest node `min_height` lines; if
  that would push the heaviest node past `max_height` the spread of the weights
  is too large and the upper bound wins, so light nodes may end up below
  `min_height` (never below 3).
* **Layers** — edges that close a cycle in a depth first search from the
  beginning node become backward edges; the rest form a DAG in which every node
  is placed on the longest path from a source.  Forward edges therefore always
  point down, backward edges up.
* **Order** — the nodes of a layer are ordered by a depth first walk, then by
  barycenter sweeps, each of them followed by exchanging two neighbours
  wherever that removes more crossings than it makes.  The exchanges find
  orders the sweeps walk past — a chain of dummies on the wrong side of a box,
  for instance.
* **Columns** — the columns are then assigned in two ways and the better result
  is kept: by repeatedly pulling every node towards the average position of
  its neighbours, or by the method of Brandes and Köpf, whose four passes
  align every vertex with the median of its neighbours — from above and from
  below, packed to the left and to the right — and take the median of the four
  results.  Conflicts with the chain of a long edge are resolved in the
  chain's favour, so long edges come out as straight verticals.
* **Straightening** — afterwards every node is pulled into the column of one
  of its neighbours wherever the gap to its own neighbours allows it: the edge
  between the two then runs straight down and costs neither a bend nor a
  crossing.  Nodes standing above each other need not be centered on each
  other for that.  Preferring a parent over the nearest neighbour makes the
  alignment carry on down a chain and straightens whole runs of edges at once,
  but not in every graph, so both are tried.
* **Making room** — a long edge sometimes still bends only because something
  stands where it wants to run, and that something has room to spare.  So the
  whole of such an edge -- its dummies and the node it ends in -- is offered
  the column of either of its two ends, and whatever is in the way slides
  aside as far as its own neighbours leave room.  Everything keeps its order
  and its distances, and nothing is pushed past the columns the drawing
  already occupies, so this cannot widen it.  What it costs the edges of what
  was moved is not weighed here: the layout is built both ways and the better
  one kept.
* **Rows** — rows are handed out in the order in which the drawing is built
  (the rows of a band, then the nodes of the layer below it) and everything is
  put as high up as it can go.  Two things constrain each other only if their
  columns overlap, so a tall node no longer pushes down what merely stands
  beside it.  Nodes that one node reaches directly and that share a layer
  start in the same row; a return node without successors drops to the end of
  the drawing when nothing is in its way -- but only if it reaches all the way
  down, since stopping short of it would just lengthen the edge leading into
  the node for nothing.  Reaching the end is also the one thing worth giving
  up the common starting row with its siblings for.  Begin nodes end up in the
  top row.  An edge that runs straight down needs no row
  at all.
* **Long edges** — a forward edge spanning more than one layer is broken up by
  invisible dummy vertices, so it never runs through a node box.
* **Backward edges** — leave through the exit point of their source, run out
  to a private channel, up beside the layers they span and back in through the
  entry point of their target.  The channel only has to keep clear of what
  stands in *those* layers, not of the whole drawing, so it can sit between
  the boxes, on either side of its source, and even to the left of everything
  — the drawing then moves over to make room.  Of the free columns it takes
  the one that cuts through the fewest of the other edges of its two nodes; a
  channel on the other side of a node often avoids those crossings
  altogether.  Should it still end up between the dummies of one of those
  edges and the nodes themselves, it cuts that chain twice for nothing — the
  two columns are then simply exchanged, which costs neither a bend nor a
  column.

Several of these decisions help in one graph and hurt in another: which of the
two column assignments is used, which neighbour a node lines itself up with,
how near the channels sit, on which side, in which order they are handed out,
whether such an exchange is made, and whether an edge between two busy trunks
is kept in the row of the trunk it leaves or of the one it runs into, and
whether the drawing is pulled together towards its middle.  The whole layout is
therefore built five hundred and twelve times, once per combination, and the
best result is kept: fewest bends first, where a bend on an edge to a preferred
successor counts as ten -- such an edge should run straight, but a layout that
is far better everywhere else may buy it a jog -- then fewest crossings, where a
crossing between two edges that meet in a node counts twice — those are the
ones the reader trips over, but not so much that a layout may buy one of them
with several others — then the shortest edges to preferred successors, then
the narrowest, then the lowest, and last of all the shortest edges.  All of it
together takes about half a second for a graph of forty nodes, a second for the
fifty-six nodes of `G6` and three for a hundred and twenty, which is what
`config::timeout` is there to cut short.

## Edge attachment

Every node has **one** entry point on its top border for every type of edge
that enters it and **one** exit point on its bottom border for every type of
edge that leaves it — backward edges included.  With a single type that is the
middle of the border; with several they stand two columns apart around the
middle (see *Edge types* above).  All edges of one type leaving a node share
one vertical trunk that fans out in a single row, all edges of one type
entering a node run together in a single row and share one vertical trunk into
their entry point.  A node therefore has at most one divergence and at most one
merge point per type, and every edge changes direction as rarely as the
attachment rule allows: two bends for a forward edge (none when its two
attachment points share a column), four for a backward edge.

Everything that meets in one row has to meet in one trunk, because otherwise
the row would show connections that do not exist.  Every edge therefore lies
in the row of one of its two trunks and the other trunk simply continues its
vertical up resp. down to that row — the fan point of both stays single and
the edge keeps the two bends the attachment rule already costs.  A trunk can
do that for one edge only; a second one would give it a second fan point, and
so would stretching a trunk whose column already carries an edge straight
through — unless that is the trunk's only edge needing a row, because then it
has no fan point of its own for the stretch to conflict with.  An edge running
straight down needs no row, and so does not make its trunk busy either.  The edges that are left over get a free column of their own to
change rows in, which costs two more bends; on the examples here none are.

Trunks arriving from above are placed in the upper rows of a band, those
continuing downwards in the lower ones, so a stretched trunk always continues
in the direction it already runs in.  Rows whose column ranges do not overlap
are reused.  Should two unrelated trunks still end up in the same column, the
band is redone with every trunk in a row of its own.

Every row costs one cell where its trunk fans out and every edge one where it
turns off again, so the corners are fewest when as few trunks as possible own
a row.  Edges between two busy trunks are therefore handed to the trunk that
already carries runs, and two trunks share one row outright when the runs that
land in it do not overlap and are all of one type -- a shared row joins its
lines, and lines of different types never join.  Sharing saves a row and turns a corner and a
junction into a single junction:

    ┌─────┴───────┬┄┄┄┄┄┘

Rows whose runs would overlap are still kept apart; there the loser gets a
free column of its own to change rows in, which costs two more bends.

## Drawing conventions

* A label is placed one grapheme cluster at a time, so an accent stays with
  the character it belongs to.  Its lines sit around the middle of the box,
  each of them centered on its own.  A double wide character takes two cells and
  the terminal moves on by two columns of its own accord.  Where a viewport
  cuts such a character in half -- its other half being outside -- a blank is
  drawn instead, since half of it cannot be: everything else in the line stays
  in the column it belongs in.
* The heading of a node that carries content is drawn on its own ground --
  `name_color` on `name_ground`, white on dark blue unless the caller says
  otherwise -- the whole width of the box, so that it reads as a heading and
  is not taken for the first line of what stands under it.  Unset either color
  and it is drawn like any other text; `-P` unsets both, since it asks for no
  colors at all.
* Boxes and edges use the light box drawing characters; junctions
  (`├ ┤ ┬ ┴ ┼`) are derived from the set of directions actually meeting in a
  cell.  A cell that is not a corner or an end point of any edge is a
  *crossing*: the vertical line is drawn through and the horizontal one is
  interrupted, so a crossing can be told apart from a junction.  What is drawn
  there is the vertical line, so the cell takes its colour and its dashes from
  the edge that owns it, not from the one that is interrupted.
* Begin nodes have doubled top and bottom sides (`╒═╕ ╘═╛`), return nodes
  doubled left and right sides (`╓─╖ ╙─╜`), inner nodes plain light ones.
* An edge the painter asked to be dashed is drawn with `╌ ┄ ┈` resp. `╎ ┆ ┊`,
  by weight; corners and junctions of a dashed edge keep the solid glyph, and
  a trunk shared with a solid edge is solid.
* Where an edge *leaves* a box the border becomes a `┬`, where it *enters* one
  the border carries the arrow head `▼`, in the colour of the line it ends
  rather than of the box.  A backward edge carries a `▲` in the
  middle of its channel, the part that runs against the flow — unless one of
  the heads below already sits within `arrow_gap` rows of it on the same
  vertical and says so.
* Everything running into a junction carries an arrow head just before it, so
  it is plain which way the lines meet and where a line ends rather than
  carries on:

        └──────────────────────────────▶┼◀───────────┘

  A junction is a cell where three or more directions really join.  A crossing
  is not one and keeps its glyph, and a border is not one either -- it has its
  own head already.
* Neither end of the graph is marked with an edge of its own: the top border
  of a begin node and the bottom border of a return node stay closed unless a
  backward edge really enters resp. leaves there.  The doubled sides of the
  frame say which node is which.

## Files

    flowgraph.hh   interface
    flowgraph.cc   layout and renderer
    main.cc        command line driver, and the reader for the file format
    browse         show randomly generated graphs, space for the next one
    pager.hh       showing a layout one screen at a time
    pager.cc
    G1             the example graph
    G2             a second example (skip edges, self loop, two returns)
    G3             two begin nodes, doubled frames
