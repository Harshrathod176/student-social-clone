#!/usr/bin/env bash
# Sets up the site on a fresh Ubuntu VM: builds the app, runs it under systemd
# so it survives reboots, and puts Caddy in front for HTTPS.
#
# Safe to run more than once - every step checks before it acts.
#
#   sudo bash setup-server.sh
set -euo pipefail

APP_DIR=/opt/student-profiles
REPO=https://github.com/Harshrathod176/student-social-clone.git
RUN_USER=ubuntu

if [ "$(id -u)" -ne 0 ]; then
    echo "Run this with sudo." >&2
    exit 1
fi

id "$RUN_USER" > /dev/null 2>&1 || { echo "No '$RUN_USER' user on this box." >&2; exit 1; }

# The public IP doubles as the hostname: sslip.io resolves <ip>.sslip.io to
# that ip, so Let's Encrypt can issue a certificate without registering a
# domain. Swap DOMAIN for a real one later and re-run.
PUBLIC_IP="$(curl -fsS --max-time 10 https://api.ipify.org)"
DOMAIN="${DOMAIN:-${PUBLIC_IP}.sslip.io}"
echo "=== serving as https://${DOMAIN} ==="

# vcpkg compiles libsodium and Crow from source, which a 1 GB free instance
# cannot do without help. Swap costs nothing when it is not being used.
if [ "$(free -m | awk '/^Mem:/{print $2}')" -lt 3000 ] && [ ! -f /swapfile ]; then
    echo "=== adding 2G swap (small instance) ==="
    fallocate -l 2G /swapfile
    chmod 600 /swapfile
    mkswap /swapfile
    swapon /swapfile
    echo '/swapfile none swap sw 0 0' >> /etc/fstab
fi

echo "=== packages ==="
export DEBIAN_FRONTEND=noninteractive
apt-get update -qq
# The same set setup.sh installs in the codespace. autoconf/automake/libtool
# are required: vcpkg builds libsodium with autotools and stops without them.
apt-get install -y -qq --no-install-recommends \
    build-essential cmake ninja-build git curl zip unzip tar ca-certificates \
    pkg-config libcurl4-openssl-dev \
    autoconf automake libtool autoconf-archive \
    debian-keyring debian-archive-keyring apt-transport-https

echo "=== source ==="
if [ -d "$APP_DIR/.git" ]; then
    git -C "$APP_DIR" fetch --depth 1 origin main
    git -C "$APP_DIR" reset --hard origin/main
else
    git clone --depth 1 "$REPO" "$APP_DIR"
fi
mkdir -p "$APP_DIR/data" "$APP_DIR/static/uploads"
chown -R "$RUN_USER:$RUN_USER" "$APP_DIR"

echo "=== libraries and build (slow the first time, ~10-20 min) ==="
VCPKG_ROOT=/opt/vcpkg
if [ ! -x "$VCPKG_ROOT/vcpkg" ]; then
    [ -d "$VCPKG_ROOT" ] || git clone --depth 1 https://github.com/microsoft/vcpkg "$VCPKG_ROOT"
    "$VCPKG_ROOT/bootstrap-vcpkg.sh" -disableMetrics
fi
chown -R "$RUN_USER:$RUN_USER" "$VCPKG_ROOT"

sudo -u "$RUN_USER" bash -c "
set -e
cd '$APP_DIR'
'$VCPKG_ROOT/vcpkg' install crow sqlite3 libsodium
cmake -B build -DCMAKE_TOOLCHAIN_FILE='$VCPKG_ROOT/scripts/buildsystems/vcpkg.cmake'
cmake --build build
"

echo "=== service ==="
install -m 644 "$APP_DIR/deploy/student-profiles.service" /etc/systemd/system/
systemctl daemon-reload
systemctl enable --now student-profiles
systemctl restart student-profiles

echo "=== caddy (https) ==="
if ! command -v caddy > /dev/null; then
    curl -fsSL https://dl.cloudsmith.io/public/caddy/stable/gpg.key \
        | gpg --dearmor -o /usr/share/keyrings/caddy-stable-archive-keyring.gpg
    echo "deb [signed-by=/usr/share/keyrings/caddy-stable-archive-keyring.gpg] https://dl.cloudsmith.io/public/caddy/stable/deb/debian any-version main" \
        > /etc/apt/sources.list.d/caddy-stable.list
    apt-get update -qq
    apt-get install -y -qq caddy
fi
sed "s|DOMAIN_PLACEHOLDER|${DOMAIN}|" "$APP_DIR/deploy/Caddyfile.template" > /etc/caddy/Caddyfile
systemctl restart caddy

# Oracle's Ubuntu image ships iptables rules that drop everything but SSH, and
# that is separate from the ingress rules set in the cloud console. Both have
# to allow 80 and 443 or the site is unreachable with no error to show for it.
echo "=== firewall ==="
for port in 80 443; do
    iptables -C INPUT -p tcp --dport "$port" -j ACCEPT 2>/dev/null \
        || iptables -I INPUT 1 -p tcp --dport "$port" -j ACCEPT
done
if command -v netfilter-persistent > /dev/null; then
    netfilter-persistent save
else
    apt-get install -y -qq iptables-persistent && netfilter-persistent save
fi

echo
echo "=== status ==="
systemctl is-active student-profiles | sed 's/^/  app:   /'
systemctl is-active caddy            | sed 's/^/  caddy: /'
sleep 3
echo "  local:  $(curl -s -o /dev/null -w '%{http_code}' --max-time 10 http://127.0.0.1:8090/)"
echo "  public: $(curl -s -o /dev/null -w '%{http_code}' --max-time 30 "https://${DOMAIN}/" || echo 'not answering yet - certificate can take a minute')"
echo
echo "Site: https://${DOMAIN}"
