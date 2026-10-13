# Host-side check of the Bluetooth lock's cryptography (src/lock_crypto.cpp)
# against the app's side written in Python (check.py). The firmware links the
# SDK's libsodium; libsodium is not installed on this Mac, so shim/ maps the
# six libsodium calls onto Homebrew's OpenSSL 3. C++11, no RTTI, as on the device.
FW        := ../..
CXX       ?= c++
OPENSSL   ?= $(shell brew --prefix openssl@3 2>/dev/null || echo /opt/homebrew/opt/openssl@3)
PY        ?= $(HOME)/.platformio/penv/bin/python
CXXFLAGS  := -std=gnu++11 -fno-rtti -O2 -Wall -Wextra -I$(FW)/include -Ishim -I$(OPENSSL)/include

lockcheck: host.cpp shim/sodium_shim.cpp shim/sodium.h $(FW)/src/lock_crypto.cpp $(FW)/include/lock_crypto.h
	$(CXX) $(CXXFLAGS) host.cpp shim/sodium_shim.cpp $(FW)/src/lock_crypto.cpp -L$(OPENSSL)/lib -lcrypto -o $@

check: lockcheck
	@$(PY) check.py ./lockcheck

clean:
	rm -f lockcheck

.PHONY: check clean
