# TinkerOS Desktop Simulator
# Usage: make && ./tinkeros_sim

BUILD_DIR := build

.PHONY: all clean run test

all: $(BUILD_DIR)/Makefile
	cmake --build $(BUILD_DIR) -- -j$(shell nproc)
	@echo ""
	@echo "Built: ./tinkeros_sim"
	@echo "Run:   ./tinkeros_sim"

$(BUILD_DIR)/Makefile:
	mkdir -p $(BUILD_DIR)
	cmake -S . -B $(BUILD_DIR) \
	    -DCMAKE_BUILD_TYPE=Debug \
	    -DPKG_CONFIG_PATH=/usr/lib/aarch64-linux-gnu/pkgconfig

run: all
	./tinkeros_sim

test: all
	./tinkeros_sim --test

clean:
	rm -rf $(BUILD_DIR) tinkeros_sim
