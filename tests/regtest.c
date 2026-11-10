// Test driver for registry.c / utoc.c (a Windows console exe, run under Wine by tests/run_tests.sh).
//   regtest roundtrip <AssetRegistry.bin>                       parse + write back unchanged: must be identical
//   regtest clone <AssetRegistry.bin> <object path> <out> [old new]...   one manifest-style copy
//   regtest copy <AssetRegistry.bin> <mod AssetRegistry.bin> <out> [mod .utoc]   the entries the mod adds
//   regtest find <AssetRegistry.bin> <object path>
//   regtest pkgid <package name>
//   regtest container <out .utoc> <out .ucas>
#include "../src/sr.h"

static uint8_t *readf(const char *p, size_t *n)
{
    wchar_t *w = utf8_to_w(p);
    uint8_t *d = file_read_all(w, n);
    free(w);
    if (!d) sr_fail("cannot read %s", p);
    return d;
}

static void writef(const char *p, const uint8_t *d, size_t n)
{
    FILE *f = fopen(p, "wb");
    if (!f || fwrite(d, 1, n, f) != n) sr_fail("cannot write %s", p);
    fclose(f);
}

typedef struct { int n; char **from, **to; } rules;

static char *rn(const char *s, void *ctx)
{
    rules *r = ctx;
    char *cur = xstrdup(s);
    for (int i = 0; i < r->n; i++) {
        const char *a = r->from[i], *b = r->to[i];
        char *q;
        while ((q = strstr(cur, a))) {
            size_t la = strlen(a), lb = strlen(b), lc = strlen(cur);
            char *nw = xmalloc(lc - la + lb + 1);
            memcpy(nw, cur, q - cur);
            memcpy(nw + (q - cur), b, lb);
            strcpy(nw + (q - cur) + lb, q + la);
            free(cur);
            cur = nw;
            if (strstr(b, a)) break;
        }
    }
    if (!strcmp(cur, s)) { free(cur); return NULL; }
    return cur;
}

int main(int argc, char **argv)
{
    jmp_buf j;
    sr_jmp = &j;
    if (setjmp(j)) { printf("error: %s\n", sr_error); return 1; }
    if (argc < 3) { printf("usage: see regtest.c\n"); return 2; }
    const char *cmd = argv[1];
    if (!strcmp(cmd, "pkgid")) { printf("%016llx\n", (unsigned long long)package_id(argv[2])); return 0; }
    if (!strcmp(cmd, "container")) {
        wchar_t *u = utf8_to_w(argv[2]), *c = utf8_to_w(argv[3]);
        int e = write_empty_container(u, c);
        printf("container: %d\n", e);
        return e != 0;
    }
    size_t n;
    uint8_t *d = readf(argv[2], &n);
    uint8_t *orig = xmalloc(n);
    memcpy(orig, d, n);
    reg *r = reg_parse(d, n);
    printf("parsed: %d entries\n", reg_count(r));
    if (!strcmp(cmd, "roundtrip")) {
        size_t on;
        uint8_t *o = reg_build(r, &on);
        int same = on == n && !memcmp(o, orig, n);
        printf("roundtrip: %s (%zu -> %zu bytes)\n", same ? "identical" : "DIFFERENT", n, on);
        return !same;
    }
    if (!strcmp(cmd, "find")) {
        int i = reg_find(r, argv[3]);
        if (i < 0) { printf("not found\n"); return 1; }
        char *p = reg_object_path(r, i), *c = reg_class(r, i), *k = reg_package(r, i);
        printf("found %d: %s class %s package %s\n", i, p, c, k);
        return 0;
    }
    if (!strcmp(cmd, "clone")) {
        rules rl = {0};
        rl.n = (argc - 5) / 2;
        rl.from = argv + 5;
        rl.to = xmalloc(sizeof(char *) * (rl.n + 1));
        for (int i = 0; i < rl.n; i++) { rl.from[i] = argv[5 + 2 * i]; rl.to[i] = argv[6 + 2 * i]; }
        char **from = xmalloc(sizeof(char *) * (rl.n + 1));
        for (int i = 0; i < rl.n; i++) from[i] = argv[5 + 2 * i];
        rl.from = from;
        int i = reg_find(r, argv[3]);
        if (i < 0) sr_fail("source not found: %s", argv[3]);
        char *np = NULL;
        int rc = reg_clone(r, i, rn, &rl, &np);
        printf("clone: %d -> %s\n", rc, np);
        rc = reg_clone(r, i, rn, &rl, &np);
        printf("clone again: %d (1 = already registered)\n", rc);
        size_t on;
        uint8_t *o = reg_build(r, &on);
        writef(argv[4], o, on);
        printf("wrote %s: %zu bytes, %d entries\n", argv[4], on, reg_count(r));
        return 0;
    }
    if (!strcmp(cmd, "copy")) {
        size_t mn;
        uint8_t *md = readf(argv[3], &mn);
        reg *m = reg_parse(md, mn);
        int npk = 0;
        uint64_t *pk = NULL;
        if (argc > 5) {
            wchar_t *w = utf8_to_w(argv[5]);
            pk = utoc_packages(w, &npk);
            printf("utoc: %d packages\n", npk);
        }
        int added = 0, in = 0, skipped = 0;
        for (int k = 0; k < reg_count(m); k++) {
            char *op = reg_object_path(m, k);
            if (reg_find(r, op) >= 0) { in++; free(op); continue; }
            char *pkg = reg_package(m, k);
            if (pk && !has_package(pk, npk, package_id(pkg))) { skipped++; printf("  skip (not in utoc) %s\n", op); free(pkg); free(op); continue; }
            int rc = reg_copy(r, m, k);
            printf("  copy %s -> %d\n", op, rc);
            added += rc == 0;
            free(pkg);
            free(op);
        }
        printf("copy: %d added, %d already there, %d skipped\n", added, in, skipped);
        size_t on;
        uint8_t *o = reg_build(r, &on);
        writef(argv[4], o, on);
        printf("wrote %s: %zu bytes, %d entries\n", argv[4], on, reg_count(r));
        return 0;
    }
    printf("unknown command\n");
    return 2;
}
