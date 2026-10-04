# Lock-Free Concurrency & Custom Allocators - build file
CXX      ?= g++
STD       = -std=c++17
WARN      = -Wall -Wextra -Wpedantic
INC       = -Iinclude -Itests
LIBS      = -pthread
OPT      ?= -O3
BUILD     = build
TSAN_DIR  = $(BUILD)/tsan

HEADERS   = $(wildcard include/*.hpp)
TESTS     = test_arena test_spsc test_mpmc

.PHONY: all test bench run tsan clean
all: $(BUILD)/pipeline_bench $(addprefix $(BUILD)/,$(TESTS))

$(BUILD)/pipeline_bench: src/pipeline_bench.cpp $(HEADERS) | $(BUILD)
	$(CXX) $(STD) $(WARN) $(OPT) $(INC) $< -o $@ $(LIBS)

$(BUILD)/test_%: tests/test_%.cpp $(HEADERS) tests/test_util.hpp | $(BUILD)
	$(CXX) $(STD) $(WARN) -O2 -g $(INC) $< -o $@ $(LIBS)

test: $(addprefix $(BUILD)/,$(TESTS))
	@for t in $(TESTS); do ./$(BUILD)/$$t || exit 1; done

bench run: $(BUILD)/pipeline_bench
	./$(BUILD)/pipeline_bench

# Bonus: build everything with ThreadSanitizer and run tests + a short pipeline run.
tsan: | $(TSAN_DIR)
	for t in $(TESTS); do \
	  $(CXX) $(STD) $(WARN) -O1 -g -fsanitize=thread $(INC) tests/$$t.cpp -o $(TSAN_DIR)/$$t $(LIBS) || exit 1; \
	  ./$(TSAN_DIR)/$$t || exit 1; \
	done
	$(CXX) $(STD) $(WARN) -O1 -g -fsanitize=thread $(INC) src/pipeline_bench.cpp -o $(TSAN_DIR)/pipeline_bench $(LIBS)
	./$(TSAN_DIR)/pipeline_bench --messages 200000 --runs 1
	@echo "TSan: no data races reported"

$(BUILD) $(TSAN_DIR):
	mkdir -p $@

clean:
	rm -rf $(BUILD)
