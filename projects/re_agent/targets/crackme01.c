// crackme01 — RE agent self-test target
// Mini license validator. 6 named functions, ~120 lines.
// Ground truth: this file. Strip the binary, hand to Ghidra agent,
// compare recovered names/types/comments back against this source.

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#define LICENSE_LEN 16
#define XOR_KEY 0x5A
#define EXPECTED_SUM 0x1F4A

static void print_help(const char *prog) {
    printf("usage: %s <license>\n", prog);
    printf("  license: %d-character ASCII string\n", LICENSE_LEN);
}

static int parse_args(int argc, char **argv, const char **out_license) {
    if (argc != 2) return -1;
    if (strlen(argv[1]) != LICENSE_LEN) return -1;
    *out_license = argv[1];
    return 0;
}

static void xor_decrypt(const char *in, uint8_t *out, size_t n) {
    for (size_t i = 0; i < n; ++i) {
        out[i] = (uint8_t)in[i] ^ XOR_KEY;
    }
}

static uint32_t compute_checksum(const uint8_t *buf, size_t n) {
    uint32_t s = 0;
    for (size_t i = 0; i < n; ++i) {
        s = (s * 31u) + buf[i];
    }
    return s & 0xFFFFu;
}

static int validate_license(const char *license) {
    uint8_t decrypted[LICENSE_LEN];
    xor_decrypt(license, decrypted, LICENSE_LEN);
    uint32_t sum = compute_checksum(decrypted, LICENSE_LEN);
    return sum == EXPECTED_SUM ? 0 : -1;
}

int main(int argc, char **argv) {
    const char *license = NULL;
    if (parse_args(argc, argv, &license) != 0) {
        print_help(argv[0]);
        return 2;
    }
    if (validate_license(license) != 0) {
        fprintf(stderr, "invalid license\n");
        return 1;
    }
    printf("license accepted\n");
    return 0;
}
