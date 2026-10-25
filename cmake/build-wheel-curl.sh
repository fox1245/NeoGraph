#!/usr/bin/env bash
set -euo pipefail

# AlmaLinux 9's curl 7.76 cannot satisfy SchemaProvider's curl >=7.88 contract.
# Keep the manylinux_2_34 baseline and build a pinned OpenSSL/HTTP2 library.
# The installed-wheel gate bootstraps the bundled libxcrypt source, including
# Perl core modules absent from EL9's minimal perl package. The gate also checks
# that patchelf is available in the manylinux/auditwheel environment.
dnf install -y openssl-devel libpq-devel sqlite-devel libnghttp2-devel zlib-devel \
    gcc make binutils glibc-devel kernel-headers patch autoconf automake libtool \
    pkgconf-pkg-config perl-core openssl
source "$(dirname "${BASH_SOURCE[0]}")/wheel-curl.sh"
version=$wheel_curl_version
sha256=$wheel_curl_sha256
prefix=/opt/neograph-wheel-deps
work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT
curl --fail --location \
    "https://curl.se/download/curl-${version}.tar.xz" \
    --output "$work/curl.tar.xz"
printf '%s  %s\n' "$sha256" "$work/curl.tar.xz" | sha256sum --check --strict
tar -xf "$work/curl.tar.xz" -C "$work"
cd "$work/curl-${version}"
./configure --prefix="$prefix" --libdir="$prefix/lib" --with-openssl \
    "${wheel_curl_configure_args[@]}"
make -j2
make install
"$prefix/bin/curl-config" --version
wheel_curl_check_features "$prefix"
