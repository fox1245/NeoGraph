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
version=8.22.0
sha256=f7ef3ae8a22e521f289803fe93543eb64c329b58aa73a9e224dfd915a2a5f4f7
prefix=/opt/neograph-wheel-deps
work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT
curl --fail --location \
    "https://curl.se/download/curl-${version}.tar.xz" \
    --output "$work/curl.tar.xz"
printf '%s  %s\n' "$sha256" "$work/curl.tar.xz" | sha256sum --check --strict
tar -xf "$work/curl.tar.xz" -C "$work"
cd "$work/curl-${version}"
./configure --prefix="$prefix" --libdir="$prefix/lib" \
    --disable-static --with-openssl --with-nghttp2 \
    --without-libpsl --without-libidn2 --without-librtmp \
    --without-brotli --without-zstd \
    --without-ca-bundle --without-ca-path
make -j2
make install
"$prefix/bin/curl-config" --version
"$prefix/bin/curl-config" --features
