# TurboScript Tier 0 interpreter
CXX      ?= g++
CXXFLAGS ?= -O3 -std=c++23 -Wall -Wextra -Wno-unused-parameter -g0
LDFLAGS  ?=

SRCS := src/lexer.cpp src/parser.cpp src/heap.cpp src/object.cpp src/runtime.cpp \
        src/compiler.cpp src/compiler2.cpp src/interp.cpp src/builtins.cpp src/main.cpp
OBJS := $(SRCS:.cpp=.o)
DEPS := $(OBJS:.o=.d)
BIN  := ts

all: $(BIN)

$(BIN): $(OBJS)
	$(CXX) $(CXXFLAGS) -o $@ $(OBJS) $(LDFLAGS)

%.o: %.cpp
	$(CXX) $(CXXFLAGS) -MMD -MP -c -o $@ $<

-include $(DEPS)

clean:
	rm -f $(OBJS) $(DEPS) $(BIN) ts_asan

.PHONY: all clean
