/* Confere o sha256 com SHA-NI contra o C portátil (e contra vetores conhecidos), e mede os dois.
 *
 *   make -C ps5 selftest            compila e roda
 *   build/sha256-selftest --bench   só a medição (MB/s de cada implementação)
 *
 * Sem SHA-NI no processador (ou fora de x86-64), confere só o C portátil e avisa. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "util.h"

static unsigned long long g_rng = 0x9e3779b97f4a7c15ULL;
static unsigned rnd(void) {
    g_rng ^= g_rng << 13;
    g_rng ^= g_rng >> 7;
    g_rng ^= g_rng << 17;
    return (unsigned)(g_rng >> 11);
}

/* Hash de `buf` com a implementação `ni`, dividido em pedaços aleatórios se `split`. */
static void hash_with(int ni, const unsigned char *buf, size_t n, int split, char out[65]) {
    sha256_set_impl(ni);
    sha256_ctx c;
    sha256_init(&c);
    size_t off = 0;
    while (off < n) {
        size_t take = n - off;
        if (split) {
            size_t lim = (rnd() % 4 == 0) ? 3 : (rnd() % 300) + 1;
            if (rnd() % 8 == 0) lim = 64 * (1 + rnd() % 40);
            if (take > lim) take = lim;
        }
        sha256_update(&c, buf + off, take);
        off += take;
    }
    sha256_final_hex(&c, out);
}

static int known_vectors(int ni) {
    static const struct {
        const char *msg;
        const char *hex;
    } v[] = {
        {"", "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855"},
        {"abc", "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad"},
        {"abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq",
         "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1"},
    };
    int bad = 0;
    for (size_t i = 0; i < sizeof(v) / sizeof(v[0]); i++) {
        char hex[65];
        hash_with(ni, (const unsigned char *)v[i].msg, strlen(v[i].msg), 0, hex);
        if (strcmp(hex, v[i].hex) != 0) {
            fprintf(stderr, "FALHA vetor %zu (%s): %s\n", i, ni ? "SHA-NI" : "C", hex);
            bad++;
        }
    }
    /* um milhão de 'a' */
    size_t n = 1000000;
    unsigned char *a = malloc(n);
    memset(a, 'a', n);
    char hex[65];
    hash_with(ni, a, n, 1, hex);
    free(a);
    if (strcmp(hex, "cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0") != 0) {
        fprintf(stderr, "FALHA milhão de a (%s): %s\n", ni ? "SHA-NI" : "C", hex);
        bad++;
    }
    return bad;
}

static double bench(int ni, const unsigned char *buf, size_t n) {
    sha256_set_impl(ni);
    double best = 0;
    for (int r = 0; r < 3; r++) {
        sha256_ctx c;
        char hex[65];
        uint64_t t = now_us();
        sha256_init(&c);
        sha256_update(&c, buf, n);
        sha256_final_hex(&c, hex);
        double mbs = (double)n / (double)(now_us() - t); /* bytes/µs = MB/s */
        if (mbs > best) best = mbs;
    }
    return best;
}

int main(int argc, char **argv) {
    int only_bench = argc > 1 && strcmp(argv[1], "--bench") == 0;
    int has_ni = sha256_set_impl(1);
    int bad = 0;
    if (!only_bench) {
        bad += known_vectors(0);
        if (has_ni) bad += known_vectors(1);
        /* buffers aleatórios de vários tamanhos, inteiros e em pedaços */
        size_t maxn = 70000;
        unsigned char *buf = malloc(maxn);
        int cases = 0;
        for (size_t n = 0; n < maxn; n = n < 300 ? n + 1 : n * 5 / 4 + rnd() % 64) {
            for (size_t i = 0; i < n; i++) buf[i] = (unsigned char)rnd();
            char ref[65], got[65];
            hash_with(0, buf, n, 0, ref);
            hash_with(0, buf, n, 1, got);
            if (strcmp(ref, got) != 0) {
                fprintf(stderr, "FALHA C em pedaços, %zu bytes\n", n);
                bad++;
            }
            if (has_ni) {
                hash_with(1, buf, n, 0, got);
                if (strcmp(ref, got) != 0) {
                    fprintf(stderr, "FALHA SHA-NI inteiro, %zu bytes\n", n);
                    bad++;
                }
                hash_with(1, buf, n, 1, got);
                if (strcmp(ref, got) != 0) {
                    fprintf(stderr, "FALHA SHA-NI em pedaços, %zu bytes\n", n);
                    bad++;
                }
            }
            cases++;
        }
        free(buf);
        printf("sha256: %d tamanhos conferidos (C portátil%s)\n", cases, has_ni ? " e SHA-NI" : "");
        if (!has_ni) printf("aviso: este processador não tem SHA-NI; só o C portátil foi conferido\n");
    }
    size_t n = 256u << 20;
    unsigned char *big = malloc(n);
    if (big) {
        for (size_t i = 0; i < n; i++) big[i] = (unsigned char)(i * 131 + 7);
        printf("sha256 C portátil: %.0f MB/s\n", bench(0, big, n));
        if (has_ni) printf("sha256 SHA-NI:     %.0f MB/s\n", bench(1, big, n));
        free(big);
    }
    sha256_set_impl(1);
    if (bad) {
        fprintf(stderr, "%d falha(s)\n", bad);
        return 1;
    }
    printf("ok\n");
    return 0;
}
