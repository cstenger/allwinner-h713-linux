#!/bin/bash
# Install hy310-hdmird on a running mainline hy310.
# Run AFTER flashing back the mainline boot.img and ssh-ing as root.
set -euo pipefail

cd "$(dirname "$0")"

echo "=== installing hy310-hdmird ==="

install -m 0755 hy310-hdmird /usr/local/sbin/
install -m 0755 hy310-hdmi   /usr/local/bin/
install -m 0644 hdcp_v22.bin /lib/firmware/
install -m 0644 hy310-hdmird.service /etc/systemd/system/

systemctl daemon-reload

echo ""
echo "Files installed:"
ls -la /usr/local/sbin/hy310-hdmird /usr/local/bin/hy310-hdmi \
       /lib/firmware/hdcp_v22.bin /etc/systemd/system/hy310-hdmird.service

echo ""
echo "Next steps:"
echo "  systemctl start hy310-hdmird"
echo "  journalctl -fu hy310-hdmird   # watch init sequence + callback events"
echo "  hy310-hdmi src 3              # switch to HDMI source"
echo ""
echo "First-test run (manual, with stderr visible):"
echo "  /usr/local/sbin/hy310-hdmird --no-socket"
echo ""
echo "If init looks good but want to keep daemon running for source-switch:"
echo "  systemctl enable --now hy310-hdmird"
