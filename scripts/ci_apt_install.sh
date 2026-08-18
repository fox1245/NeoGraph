#!/usr/bin/env bash
set -euo pipefail

if (( $# == 0 )); then
  echo "usage: ci_apt_install.sh <package>..." >&2
  exit 2
fi

apt_options=(
  -o Acquire::Retries=2
  -o Acquire::http::Timeout=20
  -o Acquire::https::Timeout=20
  -o Dpkg::Lock::Timeout=60
)

run_apt() {
  sudo env DEBIAN_FRONTEND=noninteractive \
    timeout --kill-after=30s 3m apt-get "${apt_options[@]}" update
  sudo env DEBIAN_FRONTEND=noninteractive \
    timeout --kill-after=30s 7m apt-get "${apt_options[@]}" install \
      -y --no-install-recommends "$@"
}

if run_apt "$@"; then
  exit 0
fi

echo "::warning::Ubuntu package mirror stalled; retrying with the official mirror"
for source in /etc/apt/sources.list /etc/apt/sources.list.d/ubuntu.sources; do
  if [[ -f "$source" ]]; then
    sudo sed -i \
      -e 's|https\?://azure\.archive\.ubuntu\.com/ubuntu|http://archive.ubuntu.com/ubuntu|g' \
      -e 's|https\?://azure\.ports\.ubuntu\.com/ubuntu-ports|http://ports.ubuntu.com/ubuntu-ports|g' \
      "$source"
  fi
done

sudo env DEBIAN_FRONTEND=noninteractive \
  timeout --kill-after=30s 2m dpkg --configure -a || true
run_apt "$@"
