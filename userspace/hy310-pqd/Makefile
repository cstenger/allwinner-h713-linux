# hy310-pqd — Picture Quality daemon for HY310 projector
#
# Cross-compile from openclaw (armhf target) or native on hy310:
#   make          cross-compile (default)
#   make NATIVE=1 native build
#   make install  copy binaries + systemd unit to /usr/sbin + /etc

ifeq ($(NATIVE),1)
    CXX    := g++
else
    CXX    := arm-linux-gnueabihf-g++
endif

CXXFLAGS := -std=c++20 -Wall -Wextra -O2 -fPIC -Iinclude
LDFLAGS  := -pthread

# Library sources vs. binary-specific sources
LIB_SRC  := src/cpucomm.cpp src/name2id.cpp src/pqgamma.cpp \
            src/pq_bezier.cpp src/pq_calculate_gamma.cpp src/pqconfig.cpp src/pqservice.cpp
LIB_OBJ  := $(patsubst src/%.cpp,build/%.o,$(LIB_SRC))

TEST_SRC := $(wildcard tests/test_*.cpp)
TEST_BIN := $(patsubst tests/%.cpp,build/%,$(TEST_SRC))

BIN_DAEMON := build/hy310-pqd
BIN_CTL    := build/hy310-pqctl

.PHONY: all clean test install deploy

all: build/libhy310pqd.a $(BIN_DAEMON) $(BIN_CTL) $(TEST_BIN)

build:
	@mkdir -p build

build/%.o: src/%.cpp | build
	$(CXX) $(CXXFLAGS) -c $< -o $@

build/libhy310pqd.a: $(LIB_OBJ)
	$(AR) rcs $@ $^

$(BIN_DAEMON): src/main.cpp build/libhy310pqd.a | build
	$(CXX) $(CXXFLAGS) $< -Lbuild -lhy310pqd $(LDFLAGS) -lsqlite3 -o $@

$(BIN_CTL): src/pqctl.cpp | build
	$(CXX) $(CXXFLAGS) $< $(LDFLAGS) -o $@

build/%: tests/%.cpp build/libhy310pqd.a | build
	$(CXX) $(CXXFLAGS) $< -Lbuild -lhy310pqd $(LDFLAGS) -lsqlite3 -o $@

test: $(TEST_BIN)
ifeq ($(NATIVE),1)
	@for t in $(TEST_BIN); do echo "=== $$t ==="; $$t; done
else
	@echo "Cross-compiled; copy $(TEST_BIN) to target and run there."
endif

clean:
	rm -rf build

# Deployment targets (run on hy310 as root)
install:
	install -D -m 0755 $(BIN_DAEMON) $(DESTDIR)/usr/sbin/hy310-pqd
	install -D -m 0755 $(BIN_CTL)    $(DESTDIR)/usr/bin/hy310-pqctl
	install -D -m 0644 etc/hy310-pqd.service $(DESTDIR)/etc/systemd/system/hy310-pqd.service

deploy: all
	scp $(BIN_DAEMON) $(BIN_CTL) hy310:/tmp/
	scp etc/hy310-pqd.service hy310:/tmp/
	@echo "On hy310 as root:"
	@echo "  install -m 0755 /tmp/hy310-pqd   /usr/sbin/"
	@echo "  install -m 0755 /tmp/hy310-pqctl /usr/bin/"
	@echo "  install -m 0644 /tmp/hy310-pqd.service /etc/systemd/system/"
	@echo "  systemctl daemon-reload"
