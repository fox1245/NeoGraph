#!/usr/bin/env bash
# Sourced by the wheel and native-archive build scripts: the one curl release that
# every published binary bundles, and how it is configured. Features NeoGraph does
# not use are left out, so their libraries are not shipped (curl decodes no
# content encoding unless an application asks for it, and NeoGraph never does).
wheel_curl_version=8.22.0
wheel_curl_sha256=f7ef3ae8a22e521f289803fe93543eb64c329b58aa73a9e224dfd915a2a5f4f7
wheel_curl_configure_args=(
    --disable-static --with-nghttp2
    --without-libpsl --without-libidn2 --without-librtmp --without-libssh2
    --without-brotli --without-zstd --disable-ldap
    --without-ca-bundle --without-ca-path
)

# Fails when the installed curl reports a feature that the configuration above
# leaves out (curl-config prints one feature per line).
wheel_curl_check_features() {
    local features
    features=$("$1/bin/curl-config" --features)
    printf '%s\n' "$features"
    local unwanted
    for unwanted in brotli zstd; do
        if printf '%s\n' "$features" | grep -Fxqi "$unwanted"; then
            echo "The wheel curl must not be built with $unwanted" >&2
            return 1
        fi
    done
}
