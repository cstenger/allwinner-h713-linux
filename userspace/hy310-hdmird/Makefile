# hy310-hdmird — HDMI-RX bringup daemon for HY310 mainline
#
# Cross-compile from openclaw (armhf target) or native on hy310:
#   make           cross-compile (default)
#   make NATIVE=1  native build
#   make install   copy binaries + systemd unit + firmware to rootfs
#   make deploy    scp binaries to hy310 (after stock-flash → mainline)

ifeq ($(NATIVE),1)
    CXX  := g++
else
    CXX  := arm-linux-gnueabihf-g++
endif

CXXFLAGS := -std=c++20 -Wall -Wextra -O2 -Iinclude
LDFLAGS  := -pthread

LIB_SRC := src/cpucomm.cpp src/name2id.cpp src/receiver.cpp
LIB_OBJ := $(patsubst src/%.cpp,build/%.o,$(LIB_SRC))

BIN_DAEMON := build/hy310-hdmird
BIN_CTL    := build/hy310-hdmi

# HDCP22 key (copied from stock vendor partition)
HDCP22_KEY := /opt/hy310/stock-re/super_extracted/vendor/etc/firmware/hdcp_v22.bin

.PHONY: all clean install deploy

all: $(BIN_DAEMON) $(BIN_CTL)

build:
	@mkdir -p build

build/%.o: src/%.cpp | build
	$(CXX) $(CXXFLAGS) -c $< -o $@

$(BIN_DAEMON): src/main.cpp $(LIB_OBJ) | build
	$(CXX) $(CXXFLAGS) $< $(LIB_OBJ) $(LDFLAGS) -o $@

$(BIN_CTL): src/hdmi_ctl.cpp | build
	$(CXX) $(CXXFLAGS) $< $(LDFLAGS) -o $@

clean:
	rm -rf build

# Install to a rootfs (DESTDIR for staging; default = on-target paths)
install: all
	install -D -m 0755 $(BIN_DAEMON) $(DESTDIR)/usr/sbin/hy310-hdmird
	install -D -m 0755 $(BIN_CTL)    $(DESTDIR)/usr/bin/hy310-hdmi
	install -D -m 0644 $(HDCP22_KEY) $(DESTDIR)/lib/firmware/hdcp_v22.bin
	install -D -m 0644 etc/hy310-hdmird.service \
	    $(DESTDIR)/etc/systemd/system/hy310-hdmird.service

# Direct push to hy310 via scp (run AFTER mainline boot.img is flashed back)
deploy: all
	scp $(BIN_DAEMON) $(BIN_CTL) hy310:/tmp/
	scp $(HDCP22_KEY) hy310:/tmp/hdcp_v22.bin
	scp etc/hy310-hdmird.service hy310:/tmp/
	@echo ""
	@echo "On hy310 as root:"
	@echo "  install -m 0755 /tmp/hy310-hdmird /usr/local/sbin/"
	@echo "  install -m 0755 /tmp/hy310-hdmi   /usr/local/bin/"
	@echo "  install -m 0644 /tmp/hdcp_v22.bin /lib/firmware/"
	@echo "  install -m 0644 /tmp/hy310-hdmird.service /etc/systemd/system/"
	@echo "  systemctl daemon-reload"
	@echo "  systemctl enable --now hy310-hdmird"
