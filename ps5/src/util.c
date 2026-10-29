#include "util.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

int mkdirs(const char *path) {
    char buf[1024];
    size_t n = strlen(path);
    if (n == 0 || n >= sizeof(buf)) return -1;
    memcpy(buf, path, n + 1);
    for (size_t i = 1; i <= n; i++) {
        if (buf[i] == '/' || buf[i] == '\0') {
            char c = buf[i];
            buf[i] = '\0';
            if (mkdir(buf, 0777) != 0 && errno != EEXIST) return -1;
            buf[i] = c;
        }
    }
    return is_dir(path) ? 0 : -1;
}

int rmtree(const char *path) {
    struct stat st;
    if (lstat(path, &st) != 0) return errno == ENOENT ? 0 : -1;
    if (!S_ISDIR(st.st_mode)) return unlink(path);
    DIR *d = opendir(path);
    if (!d) return -1;
    struct dirent *e;
    int rc = 0;
    while ((e = readdir(d)) != NULL) {
        if (strcmp(e->d_name, ".") == 0 || strcmp(e->d_name, "..") == 0) continue;
        char child[1024];
        if (path_join(child, sizeof(child), path, e->d_name) != 0 || rmtree(child) != 0) rc = -1;
    }
    closedir(d);
    if (rmdir(path) != 0) rc = -1;
    return rc;
}

int path_exists(const char *path) {
    struct stat st;
    return stat(path, &st) == 0;
}

int is_dir(const char *path) {
    struct stat st;
    return stat(path, &st) == 0 && S_ISDIR(st.st_mode);
}

int64_t file_size(const char *path) {
    struct stat st;
    if (stat(path, &st) != 0 || !S_ISREG(st.st_mode)) return -1;
    return (int64_t)st.st_size;
}

int path_join(char *out, size_t cap, const char *a, const char *b) {
    size_t la = strlen(a);
    int slash = la > 0 && a[la - 1] == '/';
    int n = snprintf(out, cap, slash ? "%s%s" : "%s/%s", a, b);
    return (n < 0 || (size_t)n >= cap) ? -1 : 0;
}

int safe_relpath(const char *p) {
    if (!p || !*p || p[0] == '/') return 0;
    if (strchr(p, '\\')) return 0;
    const char *s = p;
    while (1) {
        const char *slash = strchr(s, '/');
        size_t len = slash ? (size_t)(slash - s) : strlen(s);
        if (len == 0) return 0;
        if (len == 1 && s[0] == '.') return 0;
        if (len == 2 && s[0] == '.' && s[1] == '.') return 0;
        if (!slash) break;
        s = slash + 1;
    }
    return 1;
}

void safe_name(char *out, size_t cap, const char *in) {
    size_t j = 0;
    for (size_t i = 0; in[i] && j + 1 < cap; i++) {
        unsigned char c = (unsigned char)in[i];
        int ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
                 strchr(" ._-[]()", c) != NULL || c >= 0x80;
        out[j++] = ok ? (char)c : '_';
    }
    out[j] = '\0';
    /* sem espaços ou pontos nas pontas */
    while (j > 0 && (out[j - 1] == ' ' || out[j - 1] == '.')) out[--j] = '\0';
    size_t k = 0;
    while (out[k] == ' ' || out[k] == '.') k++;
    if (k) memmove(out, out + k, j - k + 1);
    if (!out[0]) snprintf(out, cap, "item");
}

char *read_file(const char *path, size_t max, size_t *len_out) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    char *buf = malloc(max + 1);
    if (!buf) {
        fclose(f);
        return NULL;
    }
    size_t n = fread(buf, 1, max, f);
    fclose(f);
    buf[n] = '\0';
    if (len_out) *len_out = n;
    return buf;
}

int write_file_atomic(const char *path, const char *data, size_t len) {
    char tmp[1024];
    if (snprintf(tmp, sizeof(tmp), "%s.tmp", path) >= (int)sizeof(tmp)) return -1;
    FILE *f = fopen(tmp, "wb");
    if (!f) return -1;
    size_t w = fwrite(data, 1, len, f);
    /* fsync antes do rename: desligar o console logo depois não deixa um arquivo vazio no lugar */
    int synced = fflush(f) == 0 && fsync(fileno(f)) == 0;
    if (fclose(f) != 0 || w != len || !synced) {
        unlink(tmp);
        return -1;
    }
    return rename(tmp, path);
}

uint64_t now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000u + (uint64_t)ts.tv_nsec / 1000000u;
}

uint64_t wall_ms(void) {
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return (uint64_t)tv.tv_sec * 1000u + (uint64_t)tv.tv_usec / 1000u;
}

int valid_id(const char *id) {
    if (!id) return 0;
    size_t n = 0;
    for (; id[n]; n++) {
        char c = id[n];
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' || c == '-'))
            return 0;
        if (n >= 64) return 0;
    }
    return n > 0;
}

uint64_t now_us(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000u + (uint64_t)ts.tv_nsec / 1000u;
}

void rate_reset(rate_t *r, uint64_t now, int64_t v) {
    memset(r, 0, sizeof(*r));
    r->t[0] = now;
    r->v[0] = v;
    r->n = 1;
    r->last_change_ms = now;
    r->last_v = v;
}

void rate_add(rate_t *r, uint64_t now, int64_t v) {
    if (r->n == 0 || v < r->last_v) {
        rate_reset(r, now, v); /* recomeçou (arquivo baixado de novo, etc.) */
        return;
    }
    if (v != r->last_v) {
        r->last_change_ms = now;
        r->last_v = v;
    }
    if (now - r->t[r->n - 1] < 250) {
        r->v[r->n - 1] = v; /* atualiza a amostra mais nova */
        return;
    }
    if (r->n == RATE_SAMPLES) {
        memmove(r->t, r->t + 1, sizeof(r->t[0]) * (RATE_SAMPLES - 1));
        memmove(r->v, r->v + 1, sizeof(r->v[0]) * (RATE_SAMPLES - 1));
        r->n--;
    }
    r->t[r->n] = now;
    r->v[r->n] = v;
    r->n++;
}

double rate_get(const rate_t *r, uint64_t now) {
    if (r->n < 2 || now - r->last_change_ms > 2000) return 0;
    int k = 0;
    while (k < r->n - 2 && now - r->t[k] > 5000) k++;
    uint64_t dt = r->t[r->n - 1] - r->t[k];
    if (dt < 200) return 0;
    return (double)(r->v[r->n - 1] - r->v[k]) * 1000.0 / (double)dt;
}

int find_ps_title_id(const char *text, char out[10]) {
    size_t n = strlen(text);
    for (size_t i = 0; i + 9 <= n; i++) {
        if (i > 0) {
            char p = text[i - 1];
            if ((p >= 'A' && p <= 'Z') || (p >= '0' && p <= '9')) continue;
        }
        int ok = 1;
        for (int k = 0; k < 4 && ok; k++) ok = text[i + k] >= 'A' && text[i + k] <= 'Z';
        for (int k = 4; k < 9 && ok; k++) ok = text[i + k] >= '0' && text[i + k] <= '9';
        if (!ok) continue;
        if (i + 9 < n && text[i + 9] >= '0' && text[i + 9] <= '9') continue;
        memcpy(out, text + i, 9);
        out[9] = '\0';
        return 1;
    }
    return 0;
}

/* ---------- SHA-256 (FIPS 180-4) ---------- */

static const uint32_t K[64] = {
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
    0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
    0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
    0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
    0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
    0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
    0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
    0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2,
};

#define ROTR(x, n) (((x) >> (n)) | ((x) << (32 - (n))))

static void sha256_blocks_c(uint32_t st[8], const uint8_t *p, size_t nblocks) {
    for (; nblocks > 0; nblocks--, p += 64) {
    uint32_t w[64];
    for (int i = 0; i < 16; i++)
        w[i] = (uint32_t)p[4 * i] << 24 | (uint32_t)p[4 * i + 1] << 16 | (uint32_t)p[4 * i + 2] << 8 | p[4 * i + 3];
    for (int i = 16; i < 64; i++) {
        uint32_t s0 = ROTR(w[i - 15], 7) ^ ROTR(w[i - 15], 18) ^ (w[i - 15] >> 3);
        uint32_t s1 = ROTR(w[i - 2], 17) ^ ROTR(w[i - 2], 19) ^ (w[i - 2] >> 10);
        w[i] = w[i - 16] + s0 + w[i - 7] + s1;
    }
    uint32_t a = st[0], b = st[1], cc = st[2], d = st[3];
    uint32_t e = st[4], f = st[5], g = st[6], h = st[7];
    for (int i = 0; i < 64; i++) {
        uint32_t t1 = h + (ROTR(e, 6) ^ ROTR(e, 11) ^ ROTR(e, 25)) + ((e & f) ^ (~e & g)) + K[i] + w[i];
        uint32_t t2 = (ROTR(a, 2) ^ ROTR(a, 13) ^ ROTR(a, 22)) + ((a & b) ^ (a & cc) ^ (b & cc));
        h = g; g = f; f = e; e = d + t1; d = cc; cc = b; b = a; a = t1 + t2;
    }
    st[0] += a; st[1] += b; st[2] += cc; st[3] += d;
    st[4] += e; st[5] += f; st[6] += g; st[7] += h;
    }
}

/* SHA-NI: o PS5 (Zen 2) tem as instruções SHA do x86. Só esta função é compilada para elas
 * (atributo target), o resto do payload continua com as opções normais; quem decide usar é
 * a checagem de CPUID em tempo de execução. Mesmo algoritmo, ~5-10x mais rápido. */
#if defined(__x86_64__) && (defined(__GNUC__) || defined(__clang__))
#include <cpuid.h>
#include <immintrin.h>
#define PONTE_HAVE_SHANI 1

__attribute__((target("sha,sse4.1"))) static void sha256_blocks_ni(uint32_t st[8], const uint8_t *p, size_t nblocks) {
    const __m128i MASK = _mm_set_epi64x(0x0c0d0e0f08090a0bULL, 0x0405060700010203ULL);
    __m128i tmp = _mm_loadu_si128((const __m128i *)&st[0]);
    __m128i s1 = _mm_loadu_si128((const __m128i *)&st[4]);
    tmp = _mm_shuffle_epi32(tmp, 0xB1);       /* CDAB */
    s1 = _mm_shuffle_epi32(s1, 0x1B);         /* EFGH */
    __m128i s0 = _mm_alignr_epi8(tmp, s1, 8); /* ABEF */
    s1 = _mm_blend_epi16(s1, tmp, 0xF0);      /* CDGH */

    for (; nblocks > 0; nblocks--, p += 64) {
        __m128i abef = s0, cdgh = s1;
        __m128i m[4];
        for (int i = 0; i < 4; i++) m[i] = _mm_shuffle_epi8(_mm_loadu_si128((const __m128i *)(p + 16 * i)), MASK);
        for (int g = 0; g < 16; g++) {
            __m128i w = m[g & 3];
            __m128i msg = _mm_add_epi32(w, _mm_loadu_si128((const __m128i *)&K[4 * g]));
            s1 = _mm_sha256rnds2_epu32(s1, s0, msg);
            msg = _mm_shuffle_epi32(msg, 0x0E);
            s0 = _mm_sha256rnds2_epu32(s0, s1, msg);
            if (g < 12) {
                /* palavras do grupo g+4: W[t-16] + s0(W[t-15]) + W[t-7] + s1(W[t-2]) */
                __m128i x = _mm_sha256msg1_epu32(w, m[(g + 1) & 3]);
                x = _mm_add_epi32(x, _mm_alignr_epi8(m[(g + 3) & 3], m[(g + 2) & 3], 4));
                m[g & 3] = _mm_sha256msg2_epu32(x, m[(g + 3) & 3]);
            }
        }
        s0 = _mm_add_epi32(s0, abef);
        s1 = _mm_add_epi32(s1, cdgh);
    }

    tmp = _mm_shuffle_epi32(s0, 0x1B);   /* FEBA */
    s1 = _mm_shuffle_epi32(s1, 0xB1);    /* DCHG */
    s0 = _mm_blend_epi16(tmp, s1, 0xF0); /* DCBA */
    s1 = _mm_alignr_epi8(s1, tmp, 8);    /* ABEF -> HGFE */
    _mm_storeu_si128((__m128i *)&st[0], s0);
    _mm_storeu_si128((__m128i *)&st[4], s1);
}

static int cpu_has_shani(void) {
    unsigned a = 0, b = 0, c = 0, d = 0;
    if (!__get_cpuid_count(7, 0, &a, &b, &c, &d)) return 0;
    if (!(b & (1u << 29))) return 0; /* SHA */
    if (!__get_cpuid(1, &a, &b, &c, &d)) return 0;
    return (c & (1u << 19)) != 0;    /* SSE4.1 (e, com ele, SSSE3) */
}
#endif

typedef void (*sha_blocks_fn)(uint32_t st[8], const uint8_t *p, size_t nblocks);
static sha_blocks_fn g_blocks;
static int g_impl = -1;

int sha256_set_impl(int ni) {
#ifdef PONTE_HAVE_SHANI
    if (ni && cpu_has_shani()) {
        g_blocks = sha256_blocks_ni;
        g_impl = 1;
        return 1;
    }
#endif
    (void)ni;
    g_blocks = sha256_blocks_c;
    g_impl = 0;
    return 0;
}

int sha256_impl(void) {
    if (g_impl < 0) sha256_set_impl(1);
    return g_impl;
}

static void sha256_block(sha256_ctx *c, const uint8_t *p, size_t nblocks) {
    if (g_impl < 0) sha256_set_impl(1);
    g_blocks(c->h, p, nblocks);
}

void sha256_init(sha256_ctx *c) {
    static const uint32_t H0[8] = {0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a,
                                   0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19};
    memcpy(c->h, H0, sizeof(H0));
    c->len = 0;
    c->used = 0;
}

void sha256_update(sha256_ctx *c, const void *data, size_t n) {
    const uint8_t *p = data;
    c->len += n;
    if (c->used) {
        size_t take = 64 - c->used < n ? 64 - c->used : n;
        memcpy(c->buf + c->used, p, take);
        c->used += take;
        p += take;
        n -= take;
        if (c->used < 64) return;
        sha256_block(c, c->buf, 1);
        c->used = 0;
    }
    if (n >= 64) {
        size_t nb = n / 64;
        sha256_block(c, p, nb);
        p += nb * 64;
        n -= nb * 64;
    }
    memcpy(c->buf, p, n);
    c->used = n;
}

static void sha256_final(sha256_ctx *c, uint8_t out[32]) {
    uint64_t bits = c->len * 8;
    uint8_t pad = 0x80;
    sha256_update(c, &pad, 1);
    uint8_t zero = 0;
    while (c->used != 56) sha256_update(c, &zero, 1);
    uint8_t lenb[8];
    for (int i = 0; i < 8; i++) lenb[i] = (uint8_t)(bits >> (56 - 8 * i));
    sha256_update(c, lenb, 8);
    for (int i = 0; i < 8; i++) {
        out[4 * i] = (uint8_t)(c->h[i] >> 24);
        out[4 * i + 1] = (uint8_t)(c->h[i] >> 16);
        out[4 * i + 2] = (uint8_t)(c->h[i] >> 8);
        out[4 * i + 3] = (uint8_t)c->h[i];
    }
}

void sha256_final_hex(sha256_ctx *c, char out_hex[65]) {
    uint8_t d[32];
    sha256_final(c, d);
    static const char hx[] = "0123456789abcdef";
    for (int i = 0; i < 32; i++) {
        out_hex[2 * i] = hx[d[i] >> 4];
        out_hex[2 * i + 1] = hx[d[i] & 15];
    }
    out_hex[64] = '\0';
}

int sha256_add_file_cb(sha256_ctx *c, const char *path, int64_t n, progress_cb cb, void *arg) {
    int fd = open(path, O_RDONLY);
    if (fd < 0) return -1;
    size_t cap = 4 << 20;
    uint8_t *buf = malloc(cap);
    if (!buf) {
        cap = 256 << 10;
        buf = malloc(cap);
    }
    if (!buf) {
        close(fd);
        return -1;
    }
    int64_t left = n, done = 0;
    int rc = 0;
    while (left > 0) {
        ssize_t r = read(fd, buf, left < (int64_t)cap ? (size_t)left : cap);
        if (r < 0 && errno == EINTR) continue;
        if (r <= 0) break;
        sha256_update(c, buf, (size_t)r);
        left -= r;
        done += r;
        if (cb && cb(arg, done) != 0) {
            rc = -2;
            break;
        }
    }
    free(buf);
    close(fd);
    if (rc) return rc;
    return left == 0 ? 0 : -1;
}

int sha256_add_file(sha256_ctx *c, const char *path, int64_t n) { return sha256_add_file_cb(c, path, n, NULL, NULL); }

int sha256_file_cb(const char *path, char out_hex[65], progress_cb cb, void *arg) {
    sha256_ctx c;
    sha256_init(&c);
    int64_t size = file_size(path);
    if (size < 0) return -1;
    int rc = sha256_add_file_cb(&c, path, size, cb, arg);
    if (rc != 0) return rc;
    sha256_final_hex(&c, out_hex);
    return 0;
}

int sha256_file(const char *path, char out_hex[65]) { return sha256_file_cb(path, out_hex, NULL, NULL); }
