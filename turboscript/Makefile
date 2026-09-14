# TurboScript T0 interpreter — standalone build (Rule 50: fast, isolated).
# Laws: -fno-exceptions (Rule 6), -fno-rtti (Rule 8), no warnings tolerated.
#
#   make            build tsrun
#   make test       build + run corpus (both dispatch paths)
#   make clean

CXX ?= g++
CXXFLAGS ?= -std=c++26 -O2 -g -Wall -Wextra -Wpedantic \
	    -fno-exceptions -fno-rtti -fno-strict-aliasing
SRC := compiler/interp/src
BUILD := build-ts
CORPUS := tests/interp/corpus

SRCS := $(SRC)/ts_value.cpp \
	$(SRC)/ts_object.cpp \
	$(SRC)/ts_builtins.cpp \
	$(SRC)/ts_module.cpp \
	$(SRC)/ts_assembler.cpp \
	$(SRC)/ts_verifier.cpp \
	$(SRC)/ts_interpreter.cpp \
	$(SRC)/ts_dispatch.cpp \
	$(SRC)/ts_proxy.cpp \
	$(SRC)/tsrun_main.cpp

HDRS := $(SRC)/ts_core.h $(SRC)/ts_value.h $(SRC)/ts_object.h \
	$(SRC)/ts_bytecode.h $(SRC)/ts_opcodes.inc $(SRC)/ts_module.h \
	$(SRC)/ts_assembler.h $(SRC)/ts_verifier.h $(SRC)/ts_interpreter.h

.PHONY: all test clean

all: $(BUILD)/tsrun

$(BUILD)/tsrun: $(SRCS) $(HDRS) | $(BUILD)
	$(CXX) $(CXXFLAGS) -o $@ $(SRCS)

$(BUILD):
	mkdir -p $(BUILD)

# Switch-dispatch fallback build (bytecode_spec.md / interp_contract.md 1).
$(BUILD)/tsrun_switch: $(SRCS) $(HDRS) | $(BUILD)
	$(CXX) $(CXXFLAGS) -DTS_NO_COMPUTED_GOTO -o $@ $(SRCS)

# Unit tests (interp_contract.md 8): enterAt reconstruction, both dispatch paths.
UNIT_SRC := tests/interp/unit_enter_at.cpp
# Driver (tsrun_main) owns main(); unit tests link the library sources only.
UNIT_LIB_SRCS := $(filter-out $(SRC)/tsrun_main.cpp,$(SRCS))
$(BUILD)/unit_enter_at: $(UNIT_SRC) $(UNIT_LIB_SRCS) $(HDRS) | $(BUILD)
	$(CXX) $(CXXFLAGS) -I$(SRC) -o $@ $(UNIT_SRC) $(UNIT_LIB_SRCS)

$(BUILD)/unit_enter_at_switch: $(UNIT_SRC) $(UNIT_LIB_SRCS) $(HDRS) | $(BUILD)
	$(CXX) $(CXXFLAGS) -I$(SRC) -DTS_NO_COMPUTED_GOTO -o $@ $(UNIT_SRC) $(UNIT_LIB_SRCS)

.PHONY: unit
unit: $(BUILD)/unit_enter_at $(BUILD)/unit_enter_at_switch
	$(BUILD)/unit_enter_at
	$(BUILD)/unit_enter_at_switch

POSITIVE := $(wildcard $(CORPUS)/*.tsbc)
NEGATIVE := $(wildcard $(CORPUS)/*.verr)

.PHONY: run-positive run-negative

test: all $(BUILD)/tsrun_switch unit
	@echo "== positive corpus (computed goto) =="
	@pass=0; fail=0; \
	for f in $(POSITIVE); do \
	        exp="$${f%.tsbc}.out"; \
	        if [ ! -f "$$exp" ]; then continue; fi; \
	        if $(BUILD)/tsrun "$$f" --check "$$exp" >/dev/null 2>$(BUILD)/last_err.txt; then \
	                pass=$$((pass+1)); \
	        else \
	                fail=$$((fail+1)); echo "FAIL: $$f"; cat $(BUILD)/last_err.txt; \
	        fi; \
	done; \
	echo "== positive corpus (switch fallback) =="; \
	for f in $(POSITIVE); do \
	        exp="$${f%.tsbc}.out"; \
	        if [ ! -f "$$exp" ]; then continue; fi; \
	        if $(BUILD)/tsrun_switch "$$f" --check "$$exp" >/dev/null 2>$(BUILD)/last_err.txt; then \
	                pass=$$((pass+1)); \
	        else \
	                fail=$$((fail+1)); echo "FAIL: $$f"; cat $(BUILD)/last_err.txt; \
	        fi; \
	done; \
	echo "== negative corpus (verifier/assembler rejections) =="; \
	for f in $(NEGATIVE); do \
	        tsbc="$${f%.verr}"; \
	        want="$$(cat $$f)"; \
	        if $(BUILD)/tsrun "$$tsbc" --verify-only >/dev/null 2>$(BUILD)/last_err.txt; then \
	                fail=$$((fail+1)); echo "FAIL(expected rejection): $$tsbc"; \
	        elif grep -q "$$want" $(BUILD)/last_err.txt; then \
	                pass=$$((pass+1)); \
	        else \
	                fail=$$((fail+1)); echo "FAIL: $$tsbc (diagnostic mismatch, wanted: $$want)"; cat $(BUILD)/last_err.txt; \
	        fi; \
	done; \
	echo "corpus: $$pass passed, $$fail failed"; \
	[ $$fail -eq 0 ]

clean:
	rm -rf $(BUILD)
