#!/bin/sh
# Run on the target device: sudo sh deploy/install.sh build/edge-gateway
set -eu
if [ "$(id -u)" -ne 0 ]; then
    echo 'Run as root on the target device.' >&2
    exit 1
fi
binary=${1:-build/edge-gateway}
test -f "$binary"
script_dir=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
project_dir=$(dirname -- "$script_dir")
if ! getent group edge-gateway >/dev/null; then groupadd --system edge-gateway; fi
if ! id edge-gateway >/dev/null 2>&1; then
    useradd --system --gid edge-gateway --home-dir /var/lib/edge-gateway --shell /usr/sbin/nologin edge-gateway
fi
if getent group dialout >/dev/null; then usermod -a -G dialout edge-gateway; fi
install -m 0755 "$binary" /usr/local/bin/edge-gateway
install -d -m 0755 /etc/edge-gateway
install -d -m 0750 -o edge-gateway -g edge-gateway /var/lib/edge-gateway
if [ ! -e /etc/edge-gateway/gateway.conf ]; then
    sed 's|^database=.*|database=/var/lib/edge-gateway/gateway.db|' "$project_dir/config/gateway.conf" > /etc/edge-gateway/gateway.conf
    chmod 0644 /etc/edge-gateway/gateway.conf
fi
install -m 0644 "$script_dir/edge-gateway.service" /etc/systemd/system/edge-gateway.service
systemctl daemon-reload
echo 'Installed. Review /etc/edge-gateway/gateway.conf, then: systemctl enable --now edge-gateway'
