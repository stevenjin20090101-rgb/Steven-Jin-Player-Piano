# Host-side simulator for src/ota_session.cpp (see sim.cpp / README.md).
# The session is compiled with the device build's language settings: C++11, no RTTI.
FW        := ../..
CXX       ?= c++
CXXFLAGS  := -std=gnu++11 -fno-rtti -O2 -g -Wall -Wextra -I$(FW)/include

sim: sim.cpp $(FW)/src/ota_session.cpp $(FW)/include/ota_session.h
	$(CXX) $(CXXFLAGS) sim.cpp $(FW)/src/ota_session.cpp -o $@

check: sim
	@./sim

clean:
	rm -f sim

.PHONY: check clean
