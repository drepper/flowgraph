// Showing a layout in the terminal, one screen at a time.
#ifndef _PAGER_HH
# define _PAGER_HH 1

# include "flowgraph.hh"

//! Show the drawing in the alternative screen of the terminal and let the
//! cursor keys move the visible part around.  Returns when the user presses
//! 'q'; the terminal is left exactly as it was found.
//! \param l the layout to show
//! \param how the painter, asked about every node and edge
//! \return true if the pager ran, false if the terminal cannot be used
bool page(const flowgraph::layout& l, flowgraph::painter how = flowgraph::plain);

#endif
