# Host-side simulator for src/power_boards.cpp (see sim.cpp / README.md).
FW        := ../..
CXX       ?= c++
CXXFLAGS  := -std=gnu++17 -O0 -g -Wall -Wno-unused-function -Wno-unused-variable -Istubs -I$(FW)/include
SCENARIOS := rapid rapid_snappy rapid_failoff grace short_repeat case1_gap manual_refire rearticulate long_note orphan \
             inhibit verified_off verified_off_pending

sim: sim.cpp $(FW)/src/power_boards.cpp $(FW)/include/power_boards.h $(FW)/include/pca9685.h $(FW)/include/config.h $(wildcard stubs/*.h stubs/freertos/*.h)
	$(CXX) $(CXXFLAGS) sim.cpp $(FW)/src/power_boards.cpp -o $@

check: sim
	@fail=0; for s in $(SCENARIOS); do ./sim $$s || fail=1; done; \
	if [ $$fail -ne 0 ]; then echo "DISPATCH SIM: FAILED"; exit 1; else echo "DISPATCH SIM: all scenarios OK"; fi

clean:
	rm -f sim

.PHONY: check clean
