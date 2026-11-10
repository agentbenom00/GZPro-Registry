// Read, extend and write the AssetRegistry.bin of Godzilla DAMM Remastered (UE 5.8 cooked registry, version 24).
// A C port of the reader/writer of the Kiryu costume (Kiryu/tools/assetregistry.py), plus two ways to add entries:
//   reg_clone: a renamed copy of one of its own entries (the manifests: "sources" + "rules", as in MMS Registry)
//   reg_copy:  an entry of another registry (the registries that costume mods ship)
//
// Layout
//   header: FGuid (16) | int32 version (24) | uint32 bFilterEditorOnlyData (1)
//   name batch: int32 Num | uint32 string bytes | uint64 hash version | uint64 CityHash64(lowercase name)[Num] |
//               2-byte big-endian headers[Num] (bit 15 = UTF-16, low 15 bits = length) | the strings back to back
//   tag store, each section 16-byte aligned at its absolute file offset:
//     uint32 0x12345679 ("text first") | int32 nums[11] | uint32 text bytes |
//     Texts FString[n4] | NumberlessNames u32[n0] | Names FName[n1] | NumberlessExportPaths (4 x u32)[n2] |
//     ExportPaths (4 x FName)[n3] | AnsiStringOffsets u32[n5] | WideStringOffsets u32[n6] | AnsiStrings u8[n7] |
//     WideStrings u16[n8] | NumberlessPairs (u32 key, u32 value)[n9] | Pairs (FName key, u32 value)[n10] | 0x87654321
//     value id = index << 3 | type (0 ansi, 1 wide, 2 numberless name, 3 name, 4 numberless export path,
//                                   5 export path, 6 text); string offsets count bytes (ansi) or characters (wide)
//     tag map handle (uint64) = bit 63 numberless keys | Num << 32 | first pair (0 = no tags)
//   int32 Num | FAssetData[Num]: PackagePath, ClassPath (package, object), PackageName, AssetName (FNames) |
//     uint64 tag map handle | bundles (int32 n; each FName + int32 m x (FName, FName, FString)) |
//     int32 n + int32 chunk ids[n] | uint32 package flags
//   then the dependency section and package data (16 bytes in a cooked registry), kept as they are
//   FName = uint32 name index (bit 31 set: a uint32 number follows; number = suffix + 1, "Rock_2" = "Rock", 3)
// New entries go at the END of every array, so every index the game's own entries use stays valid.
#include "sr.h"

#define BEGIN_MAGIC 0x12345679u
#define END_MAGIC 0x87654321u
#define NUMBERED 0x80000000u
#define REG_VERSION 24
static const uint8_t REG_GUID[16] = {0xe7, 0x9e, 0x7f, 0x71, 0x3a, 0x49, 0xb0, 0xe9,
                                     0x32, 0x91, 0xb3, 0x88, 0x07, 0x81, 0x38, 0x1b};

// sections, in the order of nums[]; ORDER is the order in the file
enum { S_NLNAMES, S_NAMES, S_NLEXPORTS, S_EXPORTS, S_TEXTS, S_ANSIOFF, S_WIDEOFF, S_ANSI, S_WIDE, S_NLPAIRS, S_PAIRS, NSEC };
static const int ORDER[NSEC] = {S_TEXTS, S_NLNAMES, S_NAMES, S_NLEXPORTS, S_EXPORTS, S_ANSIOFF, S_WIDEOFF, S_ANSI,
                                S_WIDE, S_NLPAIRS, S_PAIRS};
enum { T_ANSI, T_WIDE, T_NLNAME, T_NAME, T_NLEXPORT, T_EXPORT, T_TEXT };

typedef struct { uint32_t i, num; } fname;                    // num 0 = no number

typedef struct {
    const uint8_t *s;                                          // the characters (into r->b, or owned)
    uint32_t len;                                              // in characters
    uint8_t wide, owned;
    uint64_t hash;                                             // as in the name batch
} name_t;

typedef struct {
    fname f[5];                                                // package path, class package, class, package, object
    uint64_t tags;
    int added;                                                 // 0: bytes in r->b; 1: bytes in r->added
    size_t start, bundles_at, end;                             // its bytes; bundles_at = after the tag handle
} asset_t;

typedef struct { uint64_t h; uint32_t idx; } slot;              // idx UINT32_MAX = empty

struct reg {
    uint8_t *b;
    size_t n;
    int32_t version;
    uint64_t hash_version;
    size_t hash_at, hdr_at, str_at, str_end;                    // the game's name batch
    uint32_t nbase_names;
    name_t *names; uint32_t nnames, cap_names;
    slot *ntab; uint64_t nmask; uint32_t nfill;
    size_t sec_at[NSEC], sec_len[NSEC];
    int32_t nums[NSEC];
    uint32_t *var_off[NSEC];                                   // texts / names / export paths / pairs: where each entry starts
    buf add[NSEC];                                             // appended entries
    uint32_t addn[NSEC];                                       // appended counts (bytes for S_ANSI, characters for S_WIDE)
    asset_t *assets; int32_t nassets, nbase_assets, cap_assets;
    size_t assets_at, assets_end;
    slot *atab; uint64_t amask; uint32_t afill;
    buf added;                                                 // the appended assets' bytes
};

// ---------------------------------------------------------------- names

static uint64_t name_key(const uint8_t *s, size_t bytes, int wide)
{
    return fnv1a64(s, bytes) ^ (wide ? 0x9e3779b97f4a7c15ull : 0);
}

static size_t name_bytes(const name_t *n) { return n->wide ? 2ull * n->len : n->len; }

static void ntab_grow(reg *r)
{
    uint64_t cap = r->ntab ? 2 * (r->nmask + 1) : 1024;
    while (cap < 2ull * r->nnames + 64) cap <<= 1;
    free(r->ntab);
    r->ntab = xmalloc(sizeof(slot) * cap);
    memset(r->ntab, 0xff, sizeof(slot) * cap);
    r->nmask = cap - 1;
    r->nfill = 0;
    for (uint32_t i = 0; i < r->nnames; i++) {                 // later duplicates win, like a dict built in order
        name_t *n = &r->names[i];
        uint64_t h = name_key(n->s, name_bytes(n), n->wide);
        for (uint64_t k = h & r->nmask;; k = (k + 1) & r->nmask) {
            slot *sl = &r->ntab[k];
            if (sl->idx == UINT32_MAX) { sl->h = h; sl->idx = i; r->nfill++; break; }
            name_t *m = &r->names[sl->idx];
            if (sl->h == h && m->wide == n->wide && m->len == n->len && !memcmp(m->s, n->s, name_bytes(n))) { sl->idx = i; break; }
        }
    }
}

static slot *name_slot(reg *r, const uint8_t *s, uint32_t len, int wide, uint64_t h)
{
    size_t bytes = wide ? 2ull * len : len;
    for (uint64_t k = h & r->nmask;; k = (k + 1) & r->nmask) {
        slot *sl = &r->ntab[k];
        if (sl->idx == UINT32_MAX) return sl;
        name_t *m = &r->names[sl->idx];
        if (sl->h == h && m->wide == wide && m->len == len && !memcmp(m->s, s, bytes)) return sl;
    }
}

// index of a name (-1 if it isn't in the batch)
static int64_t find_raw(reg *r, const uint8_t *s, uint32_t len, int wide)
{
    slot *sl = name_slot(r, s, len, wide, name_key(s, wide ? 2ull * len : len, wide));
    return sl->idx == UINT32_MAX ? -1 : (int64_t)sl->idx;
}

// index of a name, appended to the batch if it isn't there yet
static uint32_t intern_raw(reg *r, const uint8_t *s, uint32_t len, int wide, uint64_t hash)
{
    if (len > 0x7fff) sr_fail("name too long (%u characters)", len);
    uint64_t h = name_key(s, wide ? 2ull * len : len, wide);
    slot *sl = name_slot(r, s, len, wide, h);
    if (sl->idx != UINT32_MAX) return sl->idx;
    if (r->nnames == r->cap_names) {
        r->cap_names = r->cap_names ? 2 * r->cap_names : 1024;
        r->names = xrealloc(r->names, sizeof(name_t) * r->cap_names);
    }
    size_t bytes = wide ? 2ull * len : len;
    uint8_t *copy = xmalloc(bytes + 2);
    memcpy(copy, s, bytes);
    name_t *n = &r->names[r->nnames];
    n->s = copy; n->len = len; n->wide = (uint8_t)wide; n->owned = 1; n->hash = hash;
    sl->h = h;
    sl->idx = r->nnames++;
    if (++r->nfill * 2 > r->nmask) ntab_grow(r);
    return r->nnames - 1;
}

// UE's FName number split (FName::ParseNumber): "Rock_2" -> "Rock" + 3; "Rock_02", "Rock_" and "12" stay as they are
static uint32_t split_number(const char *s, size_t *len)
{
    size_t n = *len, d = 0;
    while (d < n && s[n - 1 - d] >= '0' && s[n - 1 - d] <= '9') d++;
    if (!d || d >= n || d > 10 || s[n - d - 1] != '_') return 0;
    if (d > 1 && s[n - d] == '0') return 0;
    int64_t v = 0;
    for (size_t k = n - d; k < n; k++) v = v * 10 + (s[k] - '0');
    if (v >= 0x7fffffff) return 0;
    *len = n - d - 1;
    return (uint32_t)v + 1;
}

static uint64_t ascii_lower_hash(const char *s, size_t n)
{
    char *lw = xmalloc(n + 1);
    for (size_t k = 0; k < n; k++) lw[k] = (s[k] >= 'A' && s[k] <= 'Z') ? s[k] + 32 : s[k];
    uint64_t h = city_hash64((const uint8_t *)lw, n);
    free(lw);
    return h;
}

// a display name ("Rock_2") as an FName, the name added to the batch if needed (new names must be ASCII)
static fname intern_display(reg *r, const char *s)
{
    size_t n = strlen(s);
    for (size_t k = 0; k < n; k++)
        if ((uint8_t)s[k] > 0x7f || s[k] == 0) sr_fail("new names must be plain ASCII: %s", s);
    fname f;
    f.num = split_number(s, &n);
    f.i = intern_raw(r, (const uint8_t *)s, (uint32_t)n, 0, ascii_lower_hash(s, n));
    return f;
}

// a display name as an FName if its name is in the batch (nothing added): 1 found
static int find_display(reg *r, const char *s, size_t n, fname *f)
{
    f->num = split_number(s, &n);
    int64_t i = find_raw(r, (const uint8_t *)s, (uint32_t)n, 0);
    if (i < 0) return 0;
    f->i = (uint32_t)i;
    return 1;
}

static void check_name(reg *r, uint32_t i)
{
    if (i >= r->nnames) sr_fail("bad name index %u in the registry", i);
}

// "Rock_2" (malloc'd UTF-8)
static char *display(reg *r, fname f)
{
    check_name(r, f.i);
    name_t *n = &r->names[f.i];
    char *s;
    if (n->wide) {
        wchar_t *w = xmalloc(2ull * n->len + 2);
        memcpy(w, n->s, 2ull * n->len);
        w[n->len] = 0;
        s = w_to_utf8(w);
        free(w);
    } else {
        s = xmalloc(n->len + 1);
        memcpy(s, n->s, n->len);
        s[n->len] = 0;
    }
    if (f.num) {
        size_t l = strlen(s);
        s = xrealloc(s, l + 16);
        snprintf(s + l, 16, "_%u", f.num - 1);
    }
    return s;
}

// ---------------------------------------------------------------- assets (lookup by package + object name)

static uint64_t asset_key(fname pkg, fname obj)
{
    uint64_t h = ((uint64_t)pkg.i << 32 | pkg.num) * 0x9e3779b97f4a7c15ull;
    h ^= ((uint64_t)obj.i << 32 | obj.num) + 0x632be59bd9b4e019ull + (h << 6) + (h >> 2);
    return h * 0xff51afd7ed558ccdull;
}

static slot *asset_slot(reg *r, fname pkg, fname obj)
{
    uint64_t h = asset_key(pkg, obj);
    for (uint64_t k = (h >> 17) & r->amask;; k = (k + 1) & r->amask) {
        slot *sl = &r->atab[k];
        if (sl->idx == UINT32_MAX) { sl->h = h; return sl; }
        asset_t *a = &r->assets[sl->idx];
        if (sl->h == h && a->f[3].i == pkg.i && a->f[3].num == pkg.num && a->f[4].i == obj.i && a->f[4].num == obj.num) return sl;
    }
}

static void atab_grow(reg *r)
{
    uint64_t cap = r->atab ? 2 * (r->amask + 1) : 1024;
    while (cap < 2ull * r->nassets + 64) cap <<= 1;
    free(r->atab);
    r->atab = xmalloc(sizeof(slot) * cap);
    memset(r->atab, 0xff, sizeof(slot) * cap);
    r->amask = cap - 1;
    r->afill = 0;
    for (int32_t i = 0; i < r->nassets; i++) {
        slot *sl = asset_slot(r, r->assets[i].f[3], r->assets[i].f[4]);
        if (sl->idx == UINT32_MAX) r->afill++;
        sl->idx = (uint32_t)i;                                 // (a duplicate: the last one wins)
    }
}

static asset_t *new_asset(reg *r)
{
    if (r->nassets == r->cap_assets) {
        r->cap_assets = r->cap_assets ? 2 * r->cap_assets : 1024;
        r->assets = xrealloc(r->assets, sizeof(asset_t) * r->cap_assets);
    }
    asset_t *a = &r->assets[r->nassets++];
    memset(a, 0, sizeof *a);
    return a;
}

// ---------------------------------------------------------------- parse

static fname rd_fname(rd *rr)
{
    uint32_t v = rd_u32(rr);
    fname f = {v & ~NUMBERED, 0};
    if (v & NUMBERED) f.num = rd_u32(rr);
    return f;
}

static void put_fname(buf *o, fname f)
{
    if (!f.num) buf_u32(o, f.i);
    else { buf_u32(o, f.i | NUMBERED); buf_u32(o, f.num); }
}

static void align16(rd *rr)
{
    size_t q = (rr->at + 15) & ~(size_t)15;
    rd_skip(rr, q - rr->at);
}

// one FAssetData at rr (fields into a, bytes [start, end))
static void read_asset(reg *r, rd *rr, asset_t *a)
{
    a->start = rr->at;
    for (int k = 0; k < 5; k++) { a->f[k] = rd_fname(rr); check_name(r, a->f[k].i); }
    a->tags = rd_u64(rr);
    a->bundles_at = rr->at;
    int32_t nb = rd_i32(rr);
    if (nb < 0 || nb > (1 << 20)) sr_fail("bad bundle count");
    for (int32_t i = 0; i < nb; i++) {
        rd_fname(rr);
        int32_t m = rd_i32(rr);
        if (m < 0 || m > (1 << 24)) sr_fail("bad bundle size");
        for (int32_t j = 0; j < m; j++) { rd_fname(rr); rd_fname(rr); rd_skip_fstring(rr); }
    }
    int32_t nc = rd_i32(rr);
    if (nc < 0 || nc > (1 << 20)) sr_fail("bad chunk count");
    rd_skip(rr, 4ull * nc);
    rd_u32(rr);
    a->end = rr->at;
}

reg *reg_parse(uint8_t *data, size_t n)
{
    reg *r = xcalloc(1, sizeof *r);
    r->b = data;
    r->n = n;
    rd rr = {data, n, 0};
    rd_need(&rr, 24);
    if (memcmp(data, REG_GUID, 16)) sr_fail("not an asset registry of this game (unknown version GUID)");
    rr.at = 16;
    r->version = rd_i32(&rr);
    if (r->version != REG_VERSION)
        sr_fail("asset registry version %d is not supported (this build reads version %d - a game update?)", r->version, REG_VERSION);
    if (rd_u32(&rr) != 1) sr_fail("not a cooked asset registry");
    // name batch
    int32_t nn = rd_i32(&rr);
    if (nn <= 0) sr_fail("bad name count");
    uint32_t nbytes = rd_u32(&rr);
    r->hash_version = rd_u64(&rr);
    if (r->hash_version != 0xC1640000ull) sr_fail("unknown name hash version %llx", (unsigned long long)r->hash_version);
    r->hash_at = rr.at; rd_skip(&rr, 8ull * nn);
    r->hdr_at = rr.at; rd_skip(&rr, 2ull * nn);
    r->str_at = rr.at;
    r->cap_names = (uint32_t)nn + 256;
    r->names = xmalloc(sizeof(name_t) * r->cap_names);
    size_t q = r->str_at;
    for (int32_t i = 0; i < nn; i++) {
        const uint8_t *h = data + r->hdr_at + 2ull * i;
        name_t *nm = &r->names[i];
        nm->wide = (h[0] & 0x80) != 0;
        nm->len = ((uint32_t)(h[0] & 0x7f) << 8) | h[1];
        nm->owned = 0;
        nm->hash = rd64(data + r->hash_at + 8ull * i);
        size_t bytes = name_bytes(nm);
        if (q + bytes > n) sr_fail("truncated name batch");
        nm->s = data + q;
        q += bytes;
    }
    if (q - r->str_at != nbytes) sr_fail("name batch size mismatch");
    r->nnames = r->nbase_names = (uint32_t)nn;
    r->str_end = q;
    ntab_grow(r);
    rr.at = q;
    // tag store
    align16(&rr);
    if (rd_u32(&rr) != BEGIN_MAGIC) sr_fail("unexpected tag store (only the text-first layout is known)");
    for (int k = 0; k < NSEC; k++) {
        r->nums[k] = rd_i32(&rr);
        if (r->nums[k] < 0) sr_fail("bad tag store count");
    }
    uint32_t text_bytes = rd_u32(&rr);
    for (int o = 0; o < NSEC; o++) {
        int k = ORDER[o];
        align16(&rr);
        r->sec_at[k] = rr.at;
        int32_t cnt = r->nums[k];
        switch (k) {
        case S_TEXTS:
            r->var_off[k] = xmalloc(4ull * cnt + 4);
            for (int32_t i = 0; i < cnt; i++) { r->var_off[k][i] = (uint32_t)rr.at; rd_skip_fstring(&rr); }
            if (rr.at - r->sec_at[k] != text_bytes) sr_fail("text section size mismatch");
            break;
        case S_NAMES:
            r->var_off[k] = xmalloc(4ull * cnt + 4);
            for (int32_t i = 0; i < cnt; i++) { r->var_off[k][i] = (uint32_t)rr.at; rd_fname(&rr); }
            break;
        case S_EXPORTS:
            r->var_off[k] = xmalloc(4ull * cnt + 4);
            for (int32_t i = 0; i < cnt; i++) { r->var_off[k][i] = (uint32_t)rr.at; for (int f = 0; f < 4; f++) rd_fname(&rr); }
            break;
        case S_PAIRS:
            r->var_off[k] = xmalloc(4ull * cnt + 4);
            for (int32_t i = 0; i < cnt; i++) { r->var_off[k][i] = (uint32_t)rr.at; rd_fname(&rr); rd_u32(&rr); }
            break;
        case S_NLNAMES: case S_ANSIOFF: case S_WIDEOFF: rd_skip(&rr, 4ull * cnt); break;
        case S_NLEXPORTS: rd_skip(&rr, 16ull * cnt); break;
        case S_ANSI: rd_skip(&rr, (size_t)cnt); break;
        case S_WIDE: rd_skip(&rr, 2ull * cnt); break;
        case S_NLPAIRS: rd_skip(&rr, 8ull * cnt); break;
        }
        r->sec_len[k] = rr.at - r->sec_at[k];
    }
    if (rd_u32(&rr) != END_MAGIC) sr_fail("bad tag store end");
    // assets
    int32_t na = rd_i32(&rr);
    if (na < 0) sr_fail("bad asset count");
    r->assets_at = rr.at;
    r->cap_assets = na + 64;
    r->assets = xmalloc(sizeof(asset_t) * r->cap_assets);
    for (int32_t i = 0; i < na; i++) {
        asset_t *a = new_asset(r);
        read_asset(r, &rr, a);
    }
    r->nbase_assets = r->nassets;
    r->assets_end = rr.at;
    atab_grow(r);
    return r;
}

void reg_free(reg *r)
{
    if (!r) return;
    for (uint32_t i = 0; i < r->nnames; i++)
        if (r->names[i].owned) free((void *)r->names[i].s);
    free(r->names);
    free(r->ntab);
    for (int k = 0; k < NSEC; k++) { free(r->var_off[k]); buf_free(&r->add[k]); }
    free(r->assets);
    free(r->atab);
    buf_free(&r->added);
    free(r->b);
    free(r);
}

int reg_count(const reg *r) { return r->nassets; }

int reg_find(reg *r, const char *object_path)
{
    const char *slash = strrchr(object_path, '/'), *dot = strrchr(object_path, '.');
    if (!dot || (slash && dot < slash)) return -1;
    fname pkg, obj;
    if (!find_display(r, object_path, (size_t)(dot - object_path), &pkg) || !find_display(r, dot + 1, strlen(dot + 1), &obj))
        return -1;
    slot *sl = asset_slot(r, pkg, obj);
    return sl->idx == UINT32_MAX ? -1 : (int)sl->idx;
}

char *reg_package(reg *r, int i) { return display(r, r->assets[i].f[3]); }
char *reg_class(reg *r, int i) { return display(r, r->assets[i].f[2]); }

char *reg_object_path(reg *r, int i)
{
    char *p = display(r, r->assets[i].f[3]), *o = display(r, r->assets[i].f[4]);
    size_t lp = strlen(p), lo = strlen(o);
    p = xrealloc(p, lp + lo + 2);
    p[lp] = '.';
    memcpy(p + lp + 1, o, lo + 1);
    free(o);
    return p;
}

// ---------------------------------------------------------------- adding entries

// an FName of src as an FName of dst: renamed (rn), or the same text
typedef struct { reg *dst, *src; rename_fn rn; void *ctx; } xl;

static fname tr_name(xl *x, fname f)
{
    check_name(x->src, f.i);
    name_t *n = &x->src->names[f.i];
    if (x->rn && !n->wide) {                                   // (UTF-16 names are never renamed)
        char *s = display(x->src, f);
        char *t = x->rn(s, x->ctx);
        free(s);
        if (t) {
            fname g = intern_display(x->dst, t);
            free(t);
            return g;
        }
    }
    if (x->dst == x->src) return f;
    fname g = {intern_raw(x->dst, n->s, n->len, n->wide, n->hash), f.num};
    return g;
}

static int same(fname a, fname b) { return a.i == b.i && a.num == b.num; }

static const uint8_t *sec_entry(reg *r, int k, uint32_t idx, size_t size)
{
    if (idx >= (uint32_t)r->nums[k]) sr_fail("bad tag value index %u (section %d)", idx, k);
    return r->b + r->sec_at[k] + (size_t)idx * size;
}

// append a value to dst: returns its value id
static uint32_t add_value(reg *r, int k, int type, const void *p, size_t n, uint32_t count)
{
    uint32_t idx = (uint32_t)r->nums[k] + r->addn[k];
    buf_put(&r->add[k], p, n);
    r->addn[k] += count;
    return idx << 3 | (uint32_t)type;
}

// string bytes of ansi value idx (with the terminating 0)
static size_t ansi_value(reg *r, uint32_t idx, const uint8_t **s)
{
    uint32_t off = rd32(sec_entry(r, S_ANSIOFF, idx, 4));
    if (off >= (uint32_t)r->nums[S_ANSI]) sr_fail("bad string offset");
    const uint8_t *a = r->b + r->sec_at[S_ANSI], *end = memchr(a + off, 0, (size_t)r->nums[S_ANSI] - off);
    if (!end) sr_fail("unterminated string value");
    *s = a + off;
    return (size_t)(end - (a + off)) + 1;
}

static size_t wide_value(reg *r, uint32_t idx, const uint8_t **s)       // in characters, with the 0
{
    uint32_t off = rd32(sec_entry(r, S_WIDEOFF, idx, 4));
    const uint8_t *w = r->b + r->sec_at[S_WIDE];
    for (uint32_t c = off; c < (uint32_t)r->nums[S_WIDE]; c++)
        if (!rd16(w + 2ull * c)) { *s = w + 2ull * off; return c - off + 1; }
    sr_fail("unterminated string value");
}

// a tag value of src as a value of dst (renamed names / export paths when x->rn): its new value id
static uint32_t tr_value(xl *x, uint32_t vid)
{
    reg *s = x->src, *d = x->dst;
    uint32_t t = vid & 7, idx = vid >> 3;
    switch (t) {
    case T_ANSI: {
        if (d == s) return vid;                                // strings stay as they are (as in MMS Registry)
        const uint8_t *p;
        size_t len = ansi_value(s, idx, &p);
        uint32_t off = (uint32_t)d->nums[S_ANSI] + d->addn[S_ANSI];
        add_value(d, S_ANSI, 0, p, len, (uint32_t)len);
        return add_value(d, S_ANSIOFF, T_ANSI, &off, 4, 1);
    }
    case T_WIDE: {
        if (d == s) return vid;
        const uint8_t *p;
        size_t len = wide_value(s, idx, &p);
        uint32_t off = (uint32_t)d->nums[S_WIDE] + d->addn[S_WIDE];
        add_value(d, S_WIDE, 0, p, 2 * len, (uint32_t)len);
        return add_value(d, S_WIDEOFF, T_WIDE, &off, 4, 1);
    }
    case T_TEXT: {
        if (d == s) return vid;
        if (idx >= (uint32_t)s->nums[S_TEXTS]) sr_fail("bad text value");
        size_t a = s->var_off[S_TEXTS][idx];
        rd rr = {s->b, s->n, a};
        rd_skip_fstring(&rr);
        return add_value(d, S_TEXTS, T_TEXT, s->b + a, rr.at - a, 1);
    }
    case T_NLNAME: {
        fname f = {rd32(sec_entry(s, S_NLNAMES, idx, 4)), 0}, g = tr_name(x, f);
        if (d == s && same(f, g)) return vid;
        if (g.num) {                                           // renamed to "Name_2": stored as a name with a number
            buf o = {0};
            put_fname(&o, g);
            uint32_t v = add_value(d, S_NAMES, T_NAME, o.p, o.n, 1);
            buf_free(&o);
            return v;
        }
        return add_value(d, S_NLNAMES, T_NLNAME, &g.i, 4, 1);
    }
    case T_NAME: {
        if (idx >= (uint32_t)s->nums[S_NAMES]) sr_fail("bad name value");
        rd rr = {s->b, s->n, s->var_off[S_NAMES][idx]};
        fname f = rd_fname(&rr), g = tr_name(x, f);
        if (d == s && same(f, g)) return vid;
        buf o = {0};
        put_fname(&o, g);
        uint32_t v = add_value(d, S_NAMES, T_NAME, o.p, o.n, 1);
        buf_free(&o);
        return v;
    }
    case T_NLEXPORT: case T_EXPORT: {
        fname f[4], g[4];
        if (t == T_NLEXPORT) {
            const uint8_t *e = sec_entry(s, S_NLEXPORTS, idx, 16);
            for (int k = 0; k < 4; k++) { f[k].i = rd32(e + 4 * k); f[k].num = 0; }
        } else {
            if (idx >= (uint32_t)s->nums[S_EXPORTS]) sr_fail("bad export path value");
            rd rr = {s->b, s->n, s->var_off[S_EXPORTS][idx]};
            for (int k = 0; k < 4; k++) f[k] = rd_fname(&rr);
        }
        int changed = 0, numbered = 0;
        for (int k = 0; k < 4; k++) {
            g[k] = tr_name(x, f[k]);
            changed |= !same(f[k], g[k]);
            numbered |= g[k].num != 0;
        }
        if (d == s && !changed) return vid;
        buf o = {0};
        uint32_t v;
        if (!numbered) {
            for (int k = 0; k < 4; k++) buf_u32(&o, g[k].i);
            v = add_value(d, S_NLEXPORTS, T_NLEXPORT, o.p, o.n, 1);
        } else {
            for (int k = 0; k < 4; k++) put_fname(&o, g[k]);
            v = add_value(d, S_EXPORTS, T_EXPORT, o.p, o.n, 1);
        }
        buf_free(&o);
        return v;
    }
    }
    sr_fail("tag value type %u is not handled", t);
}

// entry i of x->src, renamed / translated, appended to x->dst
static void add_entry(xl *x, int i, fname path, fname pkg, fname obj)
{
    reg *s = x->src, *d = x->dst;
    asset_t a0 = s->assets[i];                                 // (a copy: d->assets may move)
    const uint8_t *base = a0.added ? s->added.p : s->b;
    fname f[5] = {path, tr_name(x, a0.f[1]), tr_name(x, a0.f[2]), pkg, obj};
    // the tag map
    uint64_t h = a0.tags;
    uint32_t num = (uint32_t)(h >> 32) & 0xffff, begin = (uint32_t)h, numberless = (uint32_t)(h >> 63);
    uint64_t nh = 0;
    if (num) {
        if (a0.added) sr_fail("internal error: an added entry can't be a source");
        int k = numberless ? S_NLPAIRS : S_PAIRS;
        if ((uint64_t)begin + num > (uint64_t)s->nums[k]) sr_fail("bad tag map");
        uint32_t nbegin = (uint32_t)d->nums[k] + d->addn[k];
        for (uint32_t p = 0; p < num; p++) {
            buf o = {0};
            if (numberless) {
                const uint8_t *e = s->b + s->sec_at[k] + 8ull * (begin + p);
                fname key = {rd32(e), 0};
                check_name(s, key.i);
                name_t *kn = &s->names[key.i];
                uint32_t dk = d == s ? key.i : intern_raw(d, kn->s, kn->len, kn->wide, kn->hash);   // keys are never renamed
                buf_u32(&o, dk);
                buf_u32(&o, tr_value(x, rd32(e + 4)));
            } else {
                rd rr = {s->b, s->n, s->var_off[k][begin + p]};
                fname key = rd_fname(&rr);
                uint32_t vid = rd_u32(&rr);
                check_name(s, key.i);
                name_t *kn = &s->names[key.i];
                fname dk = {d == s ? key.i : intern_raw(d, kn->s, kn->len, kn->wide, kn->hash), key.num};
                put_fname(&o, dk);
                buf_u32(&o, tr_value(x, vid));
            }
            add_value(d, k, 0, o.p, o.n, 1);
            buf_free(&o);
        }
        nh = (uint64_t)numberless << 63 | (uint64_t)num << 32 | nbegin;
    }
    // the entry: names, tag map, bundles (names renamed / translated), chunk ids + package flags as they are
    buf o = {0};
    for (int k = 0; k < 5; k++) put_fname(&o, f[k]);
    buf_u64(&o, nh);
    size_t head = o.n;
    rd rr = {base, a0.end, a0.bundles_at};
    int32_t nb = rd_i32(&rr);
    buf_u32(&o, (uint32_t)nb);
    for (int32_t b = 0; b < nb; b++) {
        put_fname(&o, tr_name(x, rd_fname(&rr)));
        int32_t m = rd_i32(&rr);
        buf_u32(&o, (uint32_t)m);
        for (int32_t j = 0; j < m; j++) {
            put_fname(&o, tr_name(x, rd_fname(&rr)));
            put_fname(&o, tr_name(x, rd_fname(&rr)));
            size_t p0 = rr.at;
            rd_skip_fstring(&rr);
            buf_put(&o, base + p0, rr.at - p0);
        }
    }
    buf_put(&o, base + rr.at, a0.end - rr.at);
    // record it
    asset_t *a = new_asset(d);
    memcpy(a->f, f, sizeof f);
    a->tags = nh;
    a->added = 1;
    a->start = d->added.n;
    a->bundles_at = d->added.n + head;
    buf_put(&d->added, o.p, o.n);
    a->end = d->added.n;
    buf_free(&o);
    slot *sl = asset_slot(d, f[3], f[4]);
    if (sl->idx == UINT32_MAX) d->afill++;
    sl->idx = (uint32_t)(d->nassets - 1);
    if (d->afill * 2 > d->amask) atab_grow(d);
}

int reg_clone(reg *r, int i, rename_fn rn, void *ctx, char **new_path)
{
    if (i < 0 || i >= r->nbase_assets) sr_fail("internal error: bad clone source");
    asset_t a0 = r->assets[i];
    xl x = {r, r, rn, ctx};
    // the new package and object names, looked up first (nothing added when the entry is already there)
    char *ps = display(r, a0.f[3]), *os = display(r, a0.f[4]);
    char *pn = r->names[a0.f[3].i].wide ? NULL : rn(ps, ctx), *on = r->names[a0.f[4].i].wide ? NULL : rn(os, ctx);
    char *pkg_s = pn ? pn : ps, *obj_s = on ? on : os;
    if (new_path) {
        size_t lp = strlen(pkg_s), lo = strlen(obj_s);
        *new_path = xmalloc(lp + lo + 2);
        memcpy(*new_path, pkg_s, lp);
        (*new_path)[lp] = '.';
        memcpy(*new_path + lp + 1, obj_s, lo + 1);
    }
    int rc = 0;
    fname pf, of;
    if (!pn && !on) rc = 2;
    else if (find_display(r, pkg_s, strlen(pkg_s), &pf) && find_display(r, obj_s, strlen(obj_s), &of) &&
             asset_slot(r, pf, of)->idx != UINT32_MAX) rc = 1;
    free(ps); free(os); free(pn); free(on);
    if (rc) return rc;
    // the names in the order of the entry (package path first), as the Python tool did
    fname path = tr_name(&x, a0.f[0]);
    fname pkg = tr_name(&x, a0.f[3]), obj = tr_name(&x, a0.f[4]);
    add_entry(&x, i, path, pkg, obj);
    return 0;
}

int reg_copy(reg *dst, reg *src, int i)
{
    if (i < 0 || i >= src->nassets) sr_fail("internal error: bad copy source");
    asset_t a0 = src->assets[i];
    // already in dst? (looked up without adding names)
    int found = 1;
    fname pf = {0, a0.f[3].num}, of = {0, a0.f[4].num};
    name_t *pn = &src->names[a0.f[3].i], *on = &src->names[a0.f[4].i];
    int64_t pi = find_raw(dst, pn->s, pn->len, pn->wide), oi = find_raw(dst, on->s, on->len, on->wide);
    if (pi < 0 || oi < 0) found = 0;
    else { pf.i = (uint32_t)pi; of.i = (uint32_t)oi; found = asset_slot(dst, pf, of)->idx != UINT32_MAX; }
    if (found) return 1;
    xl x = {dst, src, NULL, NULL};
    fname path = tr_name(&x, a0.f[0]);
    fname pkg = tr_name(&x, a0.f[3]), obj = tr_name(&x, a0.f[4]);
    add_entry(&x, i, path, pkg, obj);
    return 0;
}

// ---------------------------------------------------------------- write

static void pad16(buf *o)
{
    static const uint8_t z[16] = {0};
    buf_put(o, z, (16 - (o->n & 15)) & 15);
}

uint8_t *reg_build(reg *r, size_t *outn)
{
    const uint8_t *b = r->b;
    size_t extra = r->added.n + 64 * NSEC;
    for (int k = 0; k < NSEC; k++) extra += r->add[k].n;
    for (uint32_t i = r->nbase_names; i < r->nnames; i++) extra += 10 + name_bytes(&r->names[i]);
    buf o = {0};
    buf_reserve(&o, r->n + extra);
    // header + name batch
    buf_put(&o, b, 16);
    buf_u32(&o, (uint32_t)r->version);
    buf_u32(&o, 1);
    size_t str_bytes = r->str_end - r->str_at;
    for (uint32_t i = r->nbase_names; i < r->nnames; i++) str_bytes += name_bytes(&r->names[i]);
    if (str_bytes > 0xffffffffu) sr_fail("name batch too large");
    buf_u32(&o, r->nnames);
    buf_u32(&o, (uint32_t)str_bytes);
    buf_u64(&o, r->hash_version);
    buf_put(&o, b + r->hash_at, r->hdr_at - r->hash_at);
    for (uint32_t i = r->nbase_names; i < r->nnames; i++) buf_u64(&o, r->names[i].hash);
    buf_put(&o, b + r->hdr_at, r->str_at - r->hdr_at);
    for (uint32_t i = r->nbase_names; i < r->nnames; i++)
        buf_u16be(&o, (uint16_t)((r->names[i].wide ? 0x8000 : 0) | r->names[i].len));
    buf_put(&o, b + r->str_at, r->str_end - r->str_at);
    for (uint32_t i = r->nbase_names; i < r->nnames; i++) buf_put(&o, r->names[i].s, name_bytes(&r->names[i]));
    // tag store
    pad16(&o);
    buf_u32(&o, BEGIN_MAGIC);
    for (int k = 0; k < NSEC; k++) buf_u32(&o, (uint32_t)r->nums[k] + r->addn[k]);
    buf_u32(&o, (uint32_t)(r->sec_len[S_TEXTS] + r->add[S_TEXTS].n));
    for (int q = 0; q < NSEC; q++) {
        int k = ORDER[q];
        pad16(&o);
        buf_put(&o, b + r->sec_at[k], r->sec_len[k]);
        buf_put(&o, r->add[k].p, r->add[k].n);
    }
    buf_u32(&o, END_MAGIC);
    // entries, then the rest as it was
    buf_u32(&o, (uint32_t)r->nassets);
    buf_put(&o, b + r->assets_at, r->assets_end - r->assets_at);
    buf_put(&o, r->added.p, r->added.n);
    buf_put(&o, b + r->assets_end, r->n - r->assets_end);

    // self-check: parse what was just built; the counts and every added entry must be there. Any error aborts the sync
    // before anything is written, so the installed registry pak stays as it was.
    jmp_buf j, *outer = sr_jmp;
    sr_jmp = &j;
    uint8_t *copy = xmalloc(o.n + 1);
    memcpy(copy, o.p, o.n);
    reg *volatile chk = NULL;
    int bad = 0;
    if (!setjmp(j)) {
        chk = reg_parse(copy, o.n);
        bad = chk->nassets != r->nassets || chk->nnames != r->nnames;
        for (int k = 0; k < NSEC && !bad; k++) bad = chk->nums[k] != r->nums[k] + (int32_t)r->addn[k];
        for (int32_t i = r->nbase_assets; i < r->nassets && !bad; i++) {
            char *p = reg_object_path(r, i);
            int at = reg_find(chk, p);
            bad = at != i;
            free(p);
        }
    } else {
        bad = 1;
    }
    sr_jmp = outer;
    if (chk) reg_free(chk); else free(copy);
    if (bad) { buf_free(&o); sr_fail("the new registry failed its self-check; nothing was changed"); }
    *outn = o.n;
    return o.p;
}
