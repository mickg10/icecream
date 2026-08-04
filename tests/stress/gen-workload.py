#!/usr/bin/env python3
"""Generate the 400-TU template-heavy stress workload (~1.2s/TU with g++ -O2)."""
import os, sys
out = sys.argv[1] if len(sys.argv) > 1 else 'work'
os.makedirs(f'{out}/src', exist_ok=True)
tpl = '''#include <vector>
#include <map>
#include <string>
#include <algorithm>
#include <memory>
template<int N, int M> struct Grid {{ static constexpr unsigned long v = Grid<N-1,M>::v + Grid<N,M-1>::v; }};
template<int M> struct Grid<0,M> {{ static constexpr unsigned long v = 1; }};
template<int N> struct Grid<N,0> {{ static constexpr unsigned long v = 1; }};
namespace tu{i} {{
  unsigned long grid() {{ return Grid<{d},{d}>::v; }}
  unsigned long stl() {{
    std::map<std::string, std::vector<std::unique_ptr<std::string>>> m;
    m["k"].push_back(std::make_unique<std::string>("v{i}"));
    return (unsigned long)m.size() + (unsigned long)sizeof(m);
  }}
}}
unsigned long entry_{i}() {{ return tu{i}::grid() + tu{i}::stl(); }}
'''
for i in range(1, 401):
    open(f'{out}/src/tu{i:03d}.cpp', 'w').write(tpl.format(i=i, d=150 + (i % 40)))
open(f'{out}/Makefile', 'w').write('''SRCS := $(wildcard src/*.cpp)
OBJS := $(SRCS:.cpp=.o)
all: $(OBJS)
%.o: %.cpp
\t$(CXX) -O2 -c $< -o $@
clean:
\trm -f src/*.o
''')
print(f'{out}: 400 TUs')
