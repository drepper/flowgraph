CXX = g++
CXXFLAGS = -std=c++26 -fcontracts -fcontract-evaluation-semantic=enforce -Wall -Wextra -O2 -g -Weffc++
LDLIBS = -lunistring

OBJS = flowgraph.o pager.o main.o

flowgraph: $(OBJS)
	$(CXX) $(CXXFLAGS) -o $@ $(OBJS) $(LDLIBS)

flowgraph.o: flowgraph.cc flowgraph.hh
pager.o: pager.cc pager.hh flowgraph.hh
main.o: main.cc flowgraph.hh pager.hh

clean:
	rm -f flowgraph $(OBJS)

.PHONY: clean
