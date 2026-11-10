// Keep Paks\GZPro-Registry\GZPro-Registry_99_P.pak in sync with the installed mods.
//
// Godzilla DAMM Remastered finds its costumes (and every other primary asset) only through the premade
// GZPro/AssetRegistry.bin, read from the highest-priority pak that has one. So a costume mod has to put its entry into
// that file, and two mods that each ship a whole copy of it conflict: only one copy wins, the other mod's costume
// disappears. The registry pak holds ONE copy: the game's own registry plus the entries of every installed mod. A mod
// adds entries in either of two ways:
//   1. It ships its own GZPro/AssetRegistry.bin (as costume mods made without this tool do). Every entry that the
//      game's registry lacks and whose package is in the mod's own container (its .utoc) is taken over.
//   2. It ships a manifest GZPro/RegistryMods/<id>.json (the game ignores it), as in MMS Registry:
//        {"format": 1, "id": ..., "title": ..., "sources": [object paths of the game's entries to copy],
//         "rules": [[old, new], ...] (applied in order to every name), "keep": [substrings whose names stay unchanged]}
//      A source may also be {"source": path, "rules": [[old, new], ...]}: those rules replace the global ones for
//      that source, so one donor can be copied more than once.
// The registry pak also carries GZPro/GZPro-Registry/state.json: what it was built from (the game paks and the mod paks,
// with their sizes and dates), and added.json: what it added. It is rebuilt only when state.json no longer matches.
#include "sr.h"

#define STATE_FORMAT 1
#define ADDED_PATH "GZPro/GZPro-Registry/added.json"
#define SYNC_NEEDS_ADMIN 5

typedef struct { char **from, **to; int n; } ruleset;

typedef struct {
    char *id, *title, *pak, hash[17];
    char **sources; int nsrc;
    ruleset *src_rules;                                         // per source; n < 0 = use the global rules
    ruleset rules;
    char **keep; int nkeep;
} manifest;

typedef struct {
    wchar_t *rel;                                               // relative to the Paks folder
    char *name;                                                 // the same in UTF-8 with '/' (log, state)
    int order;                                                  // the game's pak order (higher wins)
    int registry, manifests;                                    // ships GZPro/AssetRegistry.bin / how many manifests
    uint64_t size, mtime, utoc_size, utoc_mtime;
    int has_utoc;
} modpak;

typedef struct {
    modpak *p; int np;                                          // the mod paks that add registry entries
    manifest *m; int n;                                         // their manifests (full scan only)
    wchar_t **game; int ngame;                                  // the game's own paks (top level)
    buf fp_game;                                                // fingerprint of the game paks (JSON)
    char **warn; int nwarn;
    int unsure;                                                 // quick mode: could not decide here
} scan_result;

static void warn(scan_result *s, const char *fmt, ...)
{
    char t[1024];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(t, sizeof t, fmt, ap);
    va_end(ap);
    s->warn = xrealloc(s->warn, sizeof(char *) * (s->nwarn + 1));
    s->warn[s->nwarn++] = xstrdup(t);
}

static void json_str(buf *b, const char *s)
{
    buf_u8(b, '"');
    for (; *s; s++) {
        uint8_t c = (uint8_t)*s;
        if (c == '"' || c == '\\') { buf_u8(b, '\\'); buf_u8(b, c); }
        else if (c < 0x20) { char t[8]; snprintf(t, sizeof t, "\\u%04x", c); buf_put(b, t, 6); }
        else buf_u8(b, c);
    }
    buf_u8(b, '"');
}

static void buf_str(buf *b, const char *s) { buf_put(b, s, strlen(s)); }

static int cmp_wstr(const void *a, const void *b) { return _wcsicmp(*(wchar_t *const *)a, *(wchar_t *const *)b); }

static const wchar_t *file_name(const wchar_t *path)
{
    const wchar_t *last = path;
    for (const wchar_t *p = path; *p; p++)
        if (*p == L'\\' || *p == L'/') last = p + 1;
    return last;
}

// the game's own paks: GZPro-Windows*.pak, pakchunk*.pak (only at the top level; copies elsewhere are backups)
static int is_game_pak_name(const wchar_t *n)
{
    return !_wcsnicmp(n, L"GZPro-Windows", 13) || !_wcsnicmp(n, L"pakchunk", 8);
}

// UE's pak order (FPakPlatformFile::GetPakOrderFromPakFilePath + the _P patch bonus): a path that starts with
// Paks/GZPro- 4 (a prefix test, so Paks/GZPro-Folder/Any.pak counts too), any other pak under Content 3; "_P.pak" adds
// 100 x the chunk version ("Name_N_P.pak": N + 1, otherwise 1)
static int pak_order(const wchar_t *rel)
{
    int order = !_wcsnicmp(rel, L"GZPro-", 6) ? 4 : 3;
    size_t n = wcslen(rel);
    if (n >= 6 && !_wcsicmp(rel + n - 6, L"_P.pak")) {
        int version = 1;
        const wchar_t *end = rel + n - 6, *start = end;
        while (start > rel && start[-1] != L'_') start--;
        if (start > rel && start < end) {
            int ok = 1, v = 0;
            for (const wchar_t *c = start; c < end; c++) {
                if (*c < L'0' || *c > L'9' || v > 1000000) { ok = 0; break; }
                v = v * 10 + (*c - L'0');
            }
            if (ok && v >= 1) version = v + 1;
        }
        order += 100 * version;
    }
    return order;
}

// "X.pak" -> "X.utoc" (malloc'd)
static wchar_t *utoc_of(const wchar_t *pak_path)
{
    size_t n = wcslen(pak_path);
    wchar_t *u = xmalloc((n + 2) * sizeof(wchar_t));
    wcscpy(u, pak_path);
    wcscpy(u + (n >= 4 ? n - 4 : n), L".utoc");
    return u;
}

static int file_info(const wchar_t *path, uint64_t *size, uint64_t *mtime)
{
    WIN32_FILE_ATTRIBUTE_DATA a;
    if (!GetFileAttributesExW(path, GetFileExInfoStandard, &a)) return 0;
    *size = ((uint64_t)a.nFileSizeHigh << 32) | a.nFileSizeLow;
    uint64_t ft = ((uint64_t)a.ftLastWriteTime.dwHighDateTime << 32) | a.ftLastWriteTime.dwLowDateTime;
    *mtime = ft / 10000000ull - 11644473600ull;
    return 1;
}

// ---------------------------------------------------------------- manifests

static char **str_array(json *a, int *n, const char *what, const char *pak)
{
    *n = 0;
    if (!a) return NULL;
    if (a->t != J_ARR) sr_fail("%s: \"%s\" in its manifest must be a list", pak, what);
    char **v = xmalloc(sizeof(char *) * (a->n + 1));
    for (int i = 0; i < a->n; i++) {
        if (a->items[i]->t != J_STR) sr_fail("%s: \"%s\" in its manifest must hold text", pak, what);
        v[i] = xstrdup(a->items[i]->s);
    }
    *n = a->n;
    return v;
}

static void parse_rules(json *rules, ruleset *rs, const char *pak)
{
    if (!rules || rules->t != J_ARR) sr_fail("%s: its manifest has no \"rules\" list", pak);
    rs->from = xmalloc(sizeof(char *) * (rules->n + 1));
    rs->to = xmalloc(sizeof(char *) * (rules->n + 1));
    for (int i = 0; i < rules->n; i++) {
        json *r = rules->items[i];
        if (r->t != J_ARR || r->n != 2 || r->items[0]->t != J_STR || r->items[1]->t != J_STR || !r->items[0]->s[0])
            sr_fail("%s: each rule in its manifest must be [\"old\", \"new\"]", pak);
        rs->from[i] = xstrdup(r->items[0]->s);
        rs->to[i] = xstrdup(r->items[1]->s);
    }
    rs->n = rules->n;
}

static void parse_manifest(manifest *m, const uint8_t *data, size_t n, const char *pak)
{
    memset(m, 0, sizeof *m);
    json *j = json_parse((const char *)data, n);
    if (!j || j->t != J_OBJ) sr_fail("%s: its manifest is not valid JSON", pak);
    json *id = json_get(j, "id"), *title = json_get(j, "title"), *src = json_get(j, "sources");
    if (!id || id->t != J_STR || !id->s[0]) sr_fail("%s: its manifest has no \"id\"", pak);
    m->id = xstrdup(id->s);
    m->title = xstrdup(title && title->t == J_STR ? title->s : id->s);
    m->pak = xstrdup(pak);
    snprintf(m->hash, sizeof m->hash, "%016llx", (unsigned long long)fnv1a64(data, n));
    if (!src || src->t != J_ARR || !src->n) sr_fail("%s: its manifest has no \"sources\"", pak);
    m->nsrc = src->n;
    m->sources = xmalloc(sizeof(char *) * (src->n + 1));
    m->src_rules = xcalloc(src->n + 1, sizeof(ruleset));
    for (int i = 0; i < src->n; i++) {
        json *e = src->items[i];
        m->src_rules[i].n = -1;
        if (e->t == J_STR) { m->sources[i] = xstrdup(e->s); continue; }
        json *path = json_get(e, "source");
        if (e->t != J_OBJ || !path || path->t != J_STR)
            sr_fail("%s: each entry of \"sources\" must be a path or {\"source\": path, \"rules\": [...]}", pak);
        m->sources[i] = xstrdup(path->s);
        parse_rules(json_get(e, "rules"), &m->src_rules[i], pak);
    }
    m->keep = str_array(json_get(j, "keep"), &m->nkeep, "keep", pak);
    parse_rules(json_get(j, "rules"), &m->rules, pak);
    json_free(j);
}

// rename for reg_clone (ctx: the manifest + which source): NULL = unchanged
typedef struct { manifest *m; int src; } rename_ctx;

static char *rename_name(const char *s, void *ctx)
{
    rename_ctx *rc = ctx;
    manifest *m = rc->m;
    const ruleset *rs = m->src_rules[rc->src].n >= 0 ? &m->src_rules[rc->src] : &m->rules;
    for (int i = 0; i < m->nkeep; i++)
        if (strstr(s, m->keep[i])) return NULL;
    char *cur = xstrdup(s);
    for (int i = 0; i < rs->n; i++) {
        const char *a = rs->from[i], *b = rs->to[i];
        size_t la = strlen(a), lb = strlen(b), cnt = 0;
        for (const char *p = strstr(cur, a); p; p = strstr(p + la, a)) cnt++;
        if (!cnt) continue;
        char *nw = xmalloc(strlen(cur) + cnt * lb + 1), *o = nw;
        const char *p = cur;
        for (const char *q = strstr(p, a); q; q = strstr(p, a)) {
            memcpy(o, p, q - p); o += q - p;
            memcpy(o, b, lb); o += lb;
            p = q + la;
        }
        strcpy(o, p);
        free(cur);
        cur = nw;
    }
    if (!strcmp(cur, s)) { free(cur); return NULL; }
    return cur;
}

// ---------------------------------------------------------------- scanning the Paks folder

typedef struct { int registry, manifests; } mod_filter_ctx;

static int mod_filter(const char *dir, const char *name, void *ctx)
{
    mod_filter_ctx *c = ctx;
    if (!_stricmp(dir, "GZPro/")) {
        if (!name) return 1;
        if (_stricmp(name, "AssetRegistry.bin")) return 0;
        c->registry = 1;
        return 1;
    }
    if (_stricmp(dir, MANIFEST_DIR)) return 0;
    if (!name) return 1;
    size_t n = strlen(name);
    if (n > 5 && !_stricmp(name + n - 5, ".json")) { c->manifests++; return 1; }
    return 0;
}

// Pre-release builds of this tool kept their registry pak under other names. A sync deletes those: left there, such a
// pak would load at almost the same pak order as this one, and its stale registry would come back if this tool were
// removed.
static const wchar_t *const legacy_paks[][2] = {
    {L"GZProRegistry", L"GZPro-ModRegistry_99_P"},              // as "GZPro Registry"
    {L"DAMM-Registry", L"DAMM-Registry_99_P"},                  // as "DAMM-Registry"
};
#define NLEGACY ((int)(sizeof legacy_paks / sizeof legacy_paks[0]))

// the registry's own folder in Paks, or one of its old ones
static int is_registry_dir(const wchar_t *n)
{
    if (!_wcsicmp(n, REG_DIR)) return 1;
    for (int k = 0; k < NLEGACY; k++)
        if (!_wcsicmp(n, legacy_paks[k][0])) return 1;
    return 0;
}

// *.pak in the Paks folder and its subfolders (the game mounts those too), relative to it; the registry's own folder
// (and its old ones) is left out
static void collect_paks(const wchar_t *paks, const wchar_t *rel, int depth, wchar_t ***names, int *nn)
{
    wchar_t *dir = *rel ? wpath(paks, rel) : _wcsdup(paks), *pat = wpath(dir, L"*");
    WIN32_FIND_DATAW fd;
    HANDLE f = FindFirstFileW(pat, &fd);
    free(pat);
    free(dir);
    if (f == INVALID_HANDLE_VALUE) return;
    do {
        const wchar_t *n = fd.cFileName;
        wchar_t *r = *rel ? wpath(rel, n) : _wcsdup(n);
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
            if (wcscmp(n, L".") && wcscmp(n, L"..") && depth < 4 && !(fd.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) &&
                !(depth == 0 && is_registry_dir(n)))
                collect_paks(paks, r, depth + 1, names, nn);
        } else {
            size_t l = wcslen(n);
            if (l > 4 && !_wcsicmp(n + l - 4, L".pak")) {
                *names = xrealloc(*names, sizeof(wchar_t *) * (*nn + 1));
                (*names)[(*nn)++] = r;
                r = NULL;
            }
        }
        free(r);
    } while (FindNextFileW(f, &fd));
    FindClose(f);
}

static char *rel_utf8(const wchar_t *rel)
{
    char *s = w_to_utf8(rel);
    for (char *c = s; *c; c++) if (*c == '\\') *c = '/';
    return s;
}

static int cmp_modpak(const void *a, const void *b)
{
    const modpak *x = a, *y = b;
    if (x->order != y->order) return y->order - x->order;      // the pak that wins in the game first
    return _wcsicmp(x->rel, y->rel);
}

// Scan the Paks folder. full = 0: in DllMain (pak indexes only, no Oodle, no threads); anything unusual sets unsure.
static void scan(const wchar_t *paks, int full, scan_result *s)
{
    memset(s, 0, sizeof *s);
    wchar_t **names = NULL;
    int nn = 0;
    collect_paks(paks, L"", 0, &names, &nn);
    qsort(names, nn, sizeof *names, cmp_wstr);
    buf_u8(&s->fp_game, '[');
    for (int i = 0; i < nn; i++) {
        wchar_t *full_path = wpath(paks, names[i]);
        char *name8 = rel_utf8(names[i]);
        int top = !wcschr(names[i], L'\\');
        if (is_game_pak_name(file_name(names[i]))) {
            uint64_t size, mtime;
            if (top && file_info(full_path, &size, &mtime)) {
                char t[64];
                if (s->ngame) buf_u8(&s->fp_game, ',');
                buf_u8(&s->fp_game, '[');
                json_str(&s->fp_game, name8);
                snprintf(t, sizeof t, ",%llu,%llu]", (unsigned long long)size, (unsigned long long)mtime);
                buf_str(&s->fp_game, t);
                s->game = xrealloc(s->game, sizeof(wchar_t *) * (s->ngame + 1));
                s->game[s->ngame++] = _wcsdup(names[i]);
            }
        } else {
            jmp_buf j, *outer = sr_jmp;
            static pak pk;
            pak_entry *volatile e = NULL;
            volatile int ne = 0;
            uint8_t *volatile data = NULL;
            memset(&pk, 0, sizeof pk);
            sr_jmp = &j;
            if (!setjmp(j)) {
                int r = pak_open(&pk, full_path);
                if (r == -2) {
                    if (full) warn(s, "%s: its index is encrypted, skipped", name8);
                    else s->unsure = 1;
                } else if (r < 0) {
                    if (full) warn(s, "%s: not a pak file, skipped", name8);
                } else {
                    mod_filter_ctx c = {0};
                    int cnt;
                    e = pak_list(&pk, mod_filter, &c, &cnt);
                    ne = cnt;
                    if (c.registry || c.manifests) {
                        modpak mp = {0};
                        mp.rel = _wcsdup(names[i]);
                        mp.name = xstrdup(name8);
                        mp.order = pak_order(names[i]);
                        mp.registry = c.registry;
                        mp.manifests = c.manifests;
                        file_info(full_path, &mp.size, &mp.mtime);
                        wchar_t *utoc = utoc_of(full_path);
                        mp.has_utoc = file_info(utoc, &mp.utoc_size, &mp.utoc_mtime);
                        free(utoc);
                        s->p = xrealloc(s->p, sizeof(modpak) * (s->np + 1));
                        s->p[s->np++] = mp;
                        for (int k = 0; full && k < ne; k++) {
                            if (_strnicmp(e[k].path, MANIFEST_DIR, strlen(MANIFEST_DIR))) continue;
                            size_t n = (size_t)e[k].usize;
                            jmp_buf j2;
                            sr_jmp = &j2;
                            if (!setjmp(j2)) {
                                if (e[k].method && !strcmp(pk.methods[e[k].method], "Oodle")) oodle_load(1);
                                data = pak_read(&pk, &e[k], 1);
                                manifest m;
                                parse_manifest(&m, data, n, name8);
                                s->m = xrealloc(s->m, sizeof(manifest) * (s->n + 1));
                                s->m[s->n++] = m;
                            } else {
                                warn(s, "manifest %s skipped: %s", e[k].path + strlen(MANIFEST_DIR), sr_error);
                            }
                            sr_jmp = &j;
                            free(data);
                            data = NULL;
                        }
                    }
                }
            } else {
                if (!full) s->unsure = 1;
                else warn(s, "%s: could not be read (%s), skipped", name8, sr_error);
            }
            sr_jmp = outer;
            free(data);
            if (e) pak_entries_free(e, ne);
            if (pk.h) pak_close(&pk);
        }
        free(name8);
        free(full_path);
        free(names[i]);
    }
    free(names);
    buf_u8(&s->fp_game, ']');
    buf_u8(&s->fp_game, 0);
    qsort(s->p, s->np, sizeof *s->p, cmp_modpak);
}

static int cmp_manifest_id(const void *a, const void *b)
{
    return strcmp((*(manifest *const *)a)->id, (*(manifest *const *)b)->id);
}

static const char *pak_kinds(const modpak *p)
{
    return p->registry && p->manifests ? "registry+manifest" : p->registry ? "registry" : "manifest";
}

// what the registry pak must have been built from (compared as text with its state.json)
static char *expected_state(scan_result *s)
{
    buf b = {0};
    char t[160];
    snprintf(t, sizeof t, "{\"format\":%d,\"game\":", STATE_FORMAT);
    buf_str(&b, t);
    buf_str(&b, (char *)s->fp_game.p);
    buf_str(&b, ",\"mods\":[");
    for (int i = 0; i < s->np; i++) {
        modpak *p = &s->p[i];
        if (i) buf_u8(&b, ',');
        buf_u8(&b, '[');
        json_str(&b, p->name);
        snprintf(t, sizeof t, ",%llu,%llu,\"%s\",%llu,%llu]", (unsigned long long)p->size, (unsigned long long)p->mtime,
                 pak_kinds(p), (unsigned long long)p->utoc_size, (unsigned long long)p->utoc_mtime);
        buf_str(&b, t);
    }
    buf_str(&b, "]}\n");
    buf_u8(&b, 0);
    return (char *)b.p;
}

static int state_filter(const char *dir, const char *name, void *ctx)
{
    (void)ctx;
    if (_stricmp(dir, "GZPro/GZPro-Registry/")) return 0;
    return !name || !_stricmp(name, "state.json") || !_stricmp(name, "added.json");
}

// state.json and added.json of the installed registry pak (NULL if none / unreadable)
static void installed_state(const wchar_t *paks, int *exists, char **state, char **added)
{
    wchar_t *p = wpath(paks, REG_PAK);
    *exists = GetFileAttributesW(p) != INVALID_FILE_ATTRIBUTES;
    *state = NULL;
    if (added) *added = NULL;
    jmp_buf j, *outer = sr_jmp;
    static pak pk;
    pak_entry *volatile e = NULL;
    volatile int ne = 0;
    memset(&pk, 0, sizeof pk);
    sr_jmp = &j;
    if (*exists && !setjmp(j) && pak_open(&pk, p) == 0) {
        int cnt;
        e = pak_list(&pk, state_filter, NULL, &cnt);
        ne = cnt;
        for (int k = 0; k < ne; k++) {
            int is_state = !_stricmp(e[k].path, STATE_PATH);
            if (!is_state && !added) continue;
            uint8_t *d = pak_read(&pk, &e[k], 0);
            d[e[k].usize] = 0;
            if (is_state) *state = (char *)d; else *added = (char *)d;
        }
    }
    sr_jmp = outer;
    if (e) pak_entries_free(e, ne);
    if (pk.h) pak_close(&pk);
    free(p);
}

// an old registry pak of a pre-release build (or its .utoc/.ucas) is still there
static int legacy_present(const wchar_t *paks)
{
    static const wchar_t *const ext[] = {L".pak", L".utoc", L".ucas"};
    int found = 0;
    for (int k = 0; k < NLEGACY && !found; k++) {
        wchar_t *d = wpath(paks, legacy_paks[k][0]);
        for (int i = 0; i < 3 && !found; i++) {
            wchar_t name[MAX_PATH];
            swprintf(name, MAX_PATH, L"%ls%ls", legacy_paks[k][1], ext[i]);
            wchar_t *p = wpath(d, name);
            found = GetFileAttributesW(p) != INVALID_FILE_ATTRIBUTES;
            free(p);
        }
        free(d);
    }
    return found;
}

int sync_quick_check(const wchar_t *paks)
{
    if (legacy_present(paks)) return 1;                         // the sync deletes it
    scan_result s;
    scan(paks, 0, &s);
    if (s.unsure) return 1;
    int exists;
    char *have;
    installed_state(paks, &exists, &have, NULL);
    if (s.np == 0) {
        if (exists) return 1;                                   // the mods were all removed: remove the registry pak
        log_open();                                             // (file writes only: fine under the loader lock)
        log_msg("Paks folder: %ls", paks);
        log_msg("no mod with registry entries found (searched the subfolders too) - nothing to do");
        return 0;
    }
    char *want = expected_state(&s);
    int differ = !have || strcmp(have, want);
    free(have);
    free(want);
    return differ;                                              // (the small leaks of scan_result: once per launch)
}

// ---------------------------------------------------------------- the report

// a friendly word for an entry's class
static const char *class_label(const char *cls)
{
    static const struct { const char *cls, *label; } L[] = {
        {"GZProCharacterSkinDataAsset", "costume"}, {"GZProCharacterDataAsset", "monster"},
        {"GZProEnvironmentDataAsset", "stage"}, {"GZProLevelVariantDataAsset", "stage variant"},
        {"GZProMusicDataAsset", "music"}, {"GZProGameTypeDataAsset", "game mode"},
        {"GZProGalleryImageDataAsset", "gallery image"}, {"GZProMoveListDataAsset", "move list"},
        {"GZProAnimSet", "anim set"}, {"World", "map"},
    };
    for (size_t i = 0; i < sizeof L / sizeof *L; i++) if (!strcmp(cls, L[i].cls)) return L[i].label;
    return cls;
}

// "DA_GZ1_Skin_KiryuPS4" from "/Game/.../DA_GZ1_Skin_KiryuPS4.DA_GZ1_Skin_KiryuPS4"
static const char *short_name(const char *path)
{
    const char *dot = strrchr(path, '.');
    return dot ? dot + 1 : path;
}

// added.json: [[pak, object path, class, how], ...] -> the report, pak by pak
static void report_added(const char *added_json)
{
    json *j = added_json ? json_parse(added_json, strlen(added_json)) : NULL;
    json *a = j ? json_get(j, "added") : NULL;
    if (!a || a->t != J_ARR) { json_free(j); return; }
    int costumes = 0, other = 0;
    for (int i = 0; i < a->n; i++) {
        json *e = a->items[i];
        if (e->t != J_ARR || e->n < 4) continue;
        if (!strcmp(e->items[2]->s ? e->items[2]->s : "", "GZProCharacterSkinDataAsset")) costumes++; else other++;
    }
    if (other) log_msg(C_WH "Registered by the mods: " C_OR "%d" C_WH " costume%s and " C_OR "%d" C_WH " other entr%s",
                       costumes, costumes == 1 ? "" : "s", other, other == 1 ? "y" : "ies");
    else log_msg(C_WH "Registered by the mods: " C_OR "%d" C_WH " costume%s", costumes, costumes == 1 ? "" : "s");
    const char *last = "";
    for (int i = 0; i < a->n; i++) {
        json *e = a->items[i];
        if (e->t != J_ARR || e->n < 4 || !e->items[0]->s || !e->items[1]->s || !e->items[2]->s || !e->items[3]->s) continue;
        if (strcmp(last, e->items[0]->s)) {
            last = e->items[0]->s;
            log_msg(C_WH "  Pak " C_OR "%s", last);
        }
        if (sr_opt.full_list)
            log_msg("    " C_OR "%-14s %-44s" C_WH " (%s)", class_label(e->items[2]->s), short_name(e->items[1]->s), e->items[3]->s);
        else log_file_only("    %-14s %-44s (%s)", class_label(e->items[2]->s), short_name(e->items[1]->s), e->items[3]->s);
    }
    if (!sr_opt.full_list) log_msg(C_WH "  (every entry is in " C_OR "GZPro-Registry.log" C_WH "; full_list=1 in options.txt shows them here)");
    json_free(j);
}

// ---------------------------------------------------------------- the build

static int registry_filter(const char *dir, const char *name, void *ctx)
{
    (void)ctx;
    if (_stricmp(dir, "GZPro/")) return 0;
    return !name || !_stricmp(name, "AssetRegistry.bin");
}

// GZPro/AssetRegistry.bin of a pak (NULL + sr_error / warning text if it can't be read)
static uint8_t *read_registry(const wchar_t *path, size_t *n)
{
    pak pk;
    if (pak_open(&pk, path)) sr_fail("not a readable pak");
    int ne;
    pak_entry *e = pak_list(&pk, registry_filter, NULL, &ne);
    if (ne < 1) { pak_entries_free(e, ne); pak_close(&pk); sr_fail("it has no GZPro/AssetRegistry.bin"); }
    uint8_t *d = pak_read(&pk, &e[ne - 1], 1);
    *n = (size_t)e[ne - 1].usize;
    pak_entries_free(e, ne);
    pak_close(&pk);
    return d;
}

typedef struct { buf b; int n; } added_list;

static void added_put(added_list *l, const char *pak, const char *path, const char *cls, const char *how)
{
    buf_str(&l->b, l->n++ ? ",\n  [" : "\n  [");
    json_str(&l->b, pak);
    buf_u8(&l->b, ',');
    json_str(&l->b, path);
    buf_u8(&l->b, ',');
    json_str(&l->b, cls);
    buf_u8(&l->b, ',');
    json_str(&l->b, how);
    buf_u8(&l->b, ']');
}

// the shared registry: the game's registry + the manifests' copies + the entries the mods' own registries add
static int build(const wchar_t *paks, scan_result *s, const char *state)
{
    if (!oodle_load(1)) sr_fail("Oodle is not available");
    log_msg("");
    log_msg(C_BL "Building the shared registry" C_WH);
    // the game's registry: from the game pak with the highest order that has one
    int best = -1, best_order = -1;
    for (int i = 0; i < s->ngame; i++) {
        wchar_t *p = wpath(paks, s->game[i]);
        pak pk;
        if (pak_open(&pk, p)) sr_fail("the game's pak %ls could not be read", s->game[i]);
        int ne;
        pak_entry *e = pak_list(&pk, registry_filter, NULL, &ne);
        pak_entries_free(e, ne);
        pak_close(&pk);
        free(p);
        int o = pak_order(s->game[i]);
        if (ne > 0 && o >= best_order) { best = i; best_order = o; }
    }
    if (best < 0) sr_fail(REGISTRY_PATH " was not found in the game's paks (is the game installed completely?)");
    size_t gn;
    wchar_t *gp = wpath(paks, s->game[best]);
    uint8_t *gd = read_registry(gp, &gn);
    free(gp);
    reg *r = reg_parse(gd, gn);
    int base = reg_count(r);
    log_msg(C_WH "  The game's registry: " C_OR "%d" C_WH " entries (from %ls)", base, s->game[best]);
    added_list al = {0};
    volatile int ncostume = 0, nother = 0;                      // (volatile: changed between setjmp and a longjmp)

    // 1. manifests (sorted by id)
    manifest **v = xmalloc(sizeof(manifest *) * (s->n + 1));
    for (int i = 0; i < s->n; i++) v[i] = &s->m[i];
    qsort(v, s->n, sizeof *v, cmp_manifest_id);
    for (int i = 0; i < s->n; i++) {
        manifest *m = v[i];
        if (i && !strcmp(v[i - 1]->id, m->id)) {
            log_msg(C_RD "  warning: mod id %s is in two paks (%s and %s): only the first is used" C_WH, m->id, v[i - 1]->pak, m->pak);
            continue;
        }
        log_msg(C_WH "  Mod " C_OR "%s" C_WH " (manifest %s in " C_OR "%s" C_WH ")", m->title, m->id, m->pak);
        for (int k = 0; k < m->nsrc; k++) {
            int at = reg_find(r, m->sources[k]);
            if (at < 0 || at >= base) {
                log_msg(C_RD "    warning: %s is not in the game's registry - skipped" C_WH, m->sources[k]);
                continue;
            }
            rename_ctx rc = {m, k};
            char *np = NULL;
            int rc2 = reg_clone(r, at, rename_name, &rc, &np);
            char *cls = reg_class(r, at);
            if (rc2 == 2) log_msg(C_RD "    warning: the rules leave %s unchanged - skipped" C_WH, short_name(m->sources[k]));
            else if (rc2 == 1) log_msg(C_WH "    %s is already registered - skipped", short_name(np));
            else {
                added_put(&al, m->pak, np, cls, "manifest");
                if (!strcmp(cls, "GZProCharacterSkinDataAsset")) ncostume++; else nother++;
                if (sr_opt.full_list) log_msg("    " C_OR "+ %-14s %-44s" C_WH " <- %s", class_label(cls), short_name(np), short_name(m->sources[k]));
                else log_file_only("    + %-14s %-44s <- %s", class_label(cls), short_name(np), short_name(m->sources[k]));
            }
            free(cls);
            free(np);
        }
    }
    free(v);

    // 2. the registries the mods ship, the pak that wins in the game first
    for (int i = 0; i < s->np; i++) {
        modpak *p = &s->p[i];
        if (!p->registry) continue;
        wchar_t *path = wpath(paks, p->rel);
        jmp_buf j, *outer = sr_jmp;
        sr_jmp = &j;
        reg *volatile src = NULL;
        uint64_t *volatile pkgs = NULL;
        if (!setjmp(j)) {
            if (!p->has_utoc) sr_fail("there is no .utoc next to it, so the game does not load it");
            size_t n;
            uint8_t *d = read_registry(path, &n);
            src = reg_parse(d, n);
            wchar_t *utoc = utoc_of(path);
            int npk;
            pkgs = utoc_packages(utoc, &npk);
            free(utoc);
            if (!pkgs) sr_fail("its .utoc could not be read");
            int added = 0, in_game = 0, stale = 0, dup = 0;
            log_msg(C_WH "  Pak " C_OR "%s" C_WH " (its own registry, %d entries)", p->name, reg_count(src));
            for (int k = 0; k < reg_count(src); k++) {
                char *op = reg_object_path(src, k);
                int at = reg_find(r, op);
                if (at >= 0) {
                    if (at < base) in_game++;
                    else { dup++; log_file_only("    %s is registered by another mod already - kept that one", op); }
                    free(op);
                    continue;
                }
                char *pkg = reg_package(src, k);
                int mine = has_package(pkgs, npk, package_id(pkg));
                free(pkg);
                if (!mine) { stale++; log_file_only("    %s: its package is not in this mod - skipped", op); free(op); continue; }
                if (reg_copy(r, src, k) == 0) {
                    char *cls = reg_class(src, k);
                    added++;
                    added_put(&al, p->name, op, cls, "registry");
                    if (!strcmp(cls, "GZProCharacterSkinDataAsset")) ncostume++; else nother++;
                    if (sr_opt.full_list) log_msg("    " C_OR "+ %-14s %s" C_WH, class_label(cls), short_name(op));
                    else log_file_only("    + %-14s %s", class_label(cls), short_name(op));
                    free(cls);
                }
                free(op);
            }
            if (!added) log_msg(C_WH "    its registry adds nothing new");
            if (dup) log_msg(C_RD "    note: %d of its entries %s registered by a mod that comes first (that one was kept)" C_WH, dup,
                             dup == 1 ? "is" : "are");
            if (stale) log_msg(C_WH "    (%d entr%s of its registry %s neither in the game nor in this mod - skipped)", stale,
                               stale == 1 ? "y" : "ies", stale == 1 ? "is" : "are");
            log_file_only("    (%d entries of its registry are the game's own)", in_game);
        } else {
            log_msg(C_RD "  warning: %s: its registry was skipped: %s" C_WH, p->name, sr_error);
        }
        sr_jmp = outer;
        if (src) reg_free(src);
        free(pkgs);
        free(path);
    }

    size_t on;
    uint8_t *out = reg_build(r, &on);
    int total = reg_count(r);
    reg_free(r);
    log_msg(C_WH "  Shared registry: " C_OR "%d" C_WH " entries (%d from the game + " C_OR "%d" C_WH " added: %d costume%s, %d other)",
            total, base, total - base, ncostume, ncostume == 1 ? "" : "s", nother);
    buf added = {0};
    buf_str(&added, "{\"format\":1,\"added\":[");
    buf_put(&added, al.b.p, al.b.n);
    buf_str(&added, "\n]}\n");
    buf_free(&al.b);

    // write: the folder, the empty .utoc/.ucas, then the pak (to a temporary name first)
    wchar_t *dir = wpath(paks, REG_DIR);
    if (!CreateDirectoryW(dir, NULL) && GetLastError() != ERROR_ALREADY_EXISTS) {
        DWORD e = GetLastError();
        free(dir);
        if (e == ERROR_ACCESS_DENIED) return SYNC_NEEDS_ADMIN;
        sr_fail("could not create the folder %ls\\%ls (error %lu)", paks, REG_DIR, e);
    }
    free(dir);
    wchar_t *utoc = wpath(paks, REG_UTOC), *ucas = wpath(paks, REG_UCAS);
    int ce = write_empty_container(utoc, ucas);
    free(utoc);
    free(ucas);
    if (ce == ERROR_ACCESS_DENIED) return SYNC_NEEDS_ADMIN;
    if (ce) sr_fail("could not write the registry's .utoc/.ucas (error %d)", ce);
    wchar_t *final_path = wpath(paks, REG_PAK), *tmp = wpath(paks, REG_PAK L".tmp");
    pak_writer *w = pak_write_begin(tmp);
    if (!w) {
        DWORD e = GetLastError();
        if (e == ERROR_ACCESS_DENIED) return SYNC_NEEDS_ADMIN;
        sr_fail("could not create %ls (error %lu)", tmp, e);
    }
    jmp_buf j, *outer = sr_jmp;
    sr_jmp = &j;
    if (setjmp(j)) { sr_jmp = outer; DeleteFileW(tmp); longjmp(*outer, 1); }
    pak_write_file(w, REGISTRY_PATH, out, on, 0);              // uncompressed, like the costume mods' own registries
    pak_write_file(w, ADDED_PATH, added.p, added.n, 0);
    pak_write_file(w, STATE_PATH, (const uint8_t *)state, strlen(state), 0);
    pak_write_end(w);
    sr_jmp = outer;
    free(out);
    buf_free(&added);
    if (!MoveFileExW(tmp, final_path, MOVEFILE_REPLACE_EXISTING)) {
        DWORD e = GetLastError();
        DeleteFileW(tmp);
        if (e == ERROR_ACCESS_DENIED) return SYNC_NEEDS_ADMIN;
        sr_fail("could not replace %ls (error %lu)", final_path, e);
    }
    free(final_path);
    free(tmp);
    return 0;
}

// delete Paks\<dir>\<stem>.pak/.utoc/.ucas (and a .pak.tmp an interrupted build left), then the folder when it is
// empty; 0 or the Windows error. *found = how many of those files there were.
static DWORD remove_pak_set(const wchar_t *paks, const wchar_t *dir, const wchar_t *stem, int *found)
{
    static const wchar_t *const ext[] = {L".pak", L".utoc", L".ucas", L".pak.tmp"};
    wchar_t *d = wpath(paks, dir);
    DWORD err = 0;
    *found = 0;
    for (int i = 0; i < 4 && !err; i++) {
        wchar_t name[MAX_PATH];
        swprintf(name, MAX_PATH, L"%ls%ls", stem, ext[i]);
        wchar_t *p = wpath(d, name);
        if (DeleteFileW(p)) ++*found;
        else if (GetLastError() != ERROR_FILE_NOT_FOUND && GetLastError() != ERROR_PATH_NOT_FOUND) err = GetLastError();
        free(p);
    }
    if (!err) RemoveDirectoryW(d);                              // (only when empty)
    free(d);
    return err;
}

// what changed since the installed registry pak was built (its state.json), in words
static void log_changes(const char *have, scan_result *s, const char *want)
{
    json *old = json_parse(have, strlen(have)), *nw = json_parse(want, strlen(want));
    json *fmt = old ? json_get(old, "format") : NULL;
    if (!old || !fmt || fmt->t != J_NUM || (int)fmt->num != STATE_FORMAT || !nw) {
        log_msg(C_OR "Reason: the registry pak was built by another version of GZPro-Registry" C_WH);
        json_free(old); json_free(nw);
        return;
    }
    int any = 0;
    json *og = json_get(old, "game"), *ng = json_get(nw, "game");
    // the game part: compare the entries one by one
    int same_game = og && ng && og->t == J_ARR && ng->t == J_ARR && og->n == ng->n;
    for (int i = 0; same_game && i < og->n; i++) {
        json *a = og->items[i], *b = ng->items[i];
        same_game = a->t == J_ARR && b->t == J_ARR && a->n == 3 && b->n == 3 && a->items[0]->s && b->items[0]->s &&
                    !strcmp(a->items[0]->s, b->items[0]->s) && a->items[1]->num == b->items[1]->num && a->items[2]->num == b->items[2]->num;
    }
    if (!same_game) { log_msg(C_OR "Reason: the game files changed (game update?)" C_WH); any = 1; }
    json *om = json_get(old, "mods");
    for (int i = 0; i < s->np; i++) {
        json *hit = NULL;
        for (int k = 0; om && om->t == J_ARR && k < om->n; k++) {
            json *e = om->items[k];
            if (e->t == J_ARR && e->n >= 6 && e->items[0]->s && !strcmp(e->items[0]->s, s->p[i].name)) hit = e;
        }
        if (!hit) { log_msg(C_OR "Reason: " C_WH "new mod pak " C_OR "%s" C_WH, s->p[i].name); any = 1; }
        else if ((uint64_t)hit->items[1]->num != s->p[i].size || (uint64_t)hit->items[2]->num != s->p[i].mtime ||
                 (uint64_t)hit->items[4]->num != s->p[i].utoc_size || (uint64_t)hit->items[5]->num != s->p[i].utoc_mtime ||
                 !hit->items[3]->s || strcmp(hit->items[3]->s, pak_kinds(&s->p[i]))) {
            log_msg(C_OR "Reason: " C_WH "changed mod pak " C_OR "%s" C_WH, s->p[i].name);
            any = 1;
        }
    }
    for (int k = 0; om && om->t == J_ARR && k < om->n; k++) {
        json *e = om->items[k];
        if (e->t != J_ARR || e->n < 1 || !e->items[0]->s) continue;
        int still = 0;
        for (int i = 0; i < s->np; i++) still |= !strcmp(s->p[i].name, e->items[0]->s);
        if (!still) { log_msg(C_OR "Reason: " C_WH "removed mod pak " C_OR "%s" C_WH, e->items[0]->s); any = 1; }
    }
    if (!any) log_msg(C_OR "Reason: the registry pak's record does not match" C_WH);
    json_free(old);
    json_free(nw);
}

// 0 = up to date / updated, SYNC_NEEDS_ADMIN = the Paks folder is not writable; errors via sr_fail
int sync_run(const wchar_t *paks, int interactive)
{
    (void)interactive;
    log_msg(C_WH "Paks folder: " C_OR "%ls", paks);
    if (sr_opt.show_always) console_show();                     // the report window on every launch (options.txt: show_always)
    for (int k = 0; k < NLEGACY; k++) {
        int old;
        DWORD le = remove_pak_set(paks, legacy_paks[k][0], legacy_paks[k][1], &old);
        if (le == ERROR_ACCESS_DENIED) return SYNC_NEEDS_ADMIN;
        if (le) sr_fail("could not delete an old registry pak in Paks\\%ls (error %lu)", legacy_paks[k][0], le);
        if (old) {
            console_show();
            log_msg(C_WH "Deleted an old registry pak of this tool in " C_OR "Paks\\%ls" C_WH " (from a pre-release build)",
                    legacy_paks[k][0]);
        }
    }
    scan_result s;
    scan(paks, 1, &s);
    for (int i = 0; i < s.nwarn; i++) log_msg(C_RD "warning: %s" C_WH, s.warn[i]);
    int nreg = 0, nman = 0;
    for (int i = 0; i < s.np; i++) { nreg += s.p[i].registry; nman += s.p[i].manifests > 0; }
    int exists;
    char *have, *added;
    installed_state(paks, &exists, &have, &added);
    if (s.np == 0) {
        log_msg("No mod with registry entries found (no costume mods installed)");
        if (!exists) { log_msg(C_WH "Registry is up to date - nothing to do"); return 0; }
        console_show();
        int n;
        DWORD e = remove_pak_set(paks, REG_DIR, REG_STEM, &n);
        if (e == ERROR_ACCESS_DENIED) return SYNC_NEEDS_ADMIN;
        if (e) sr_fail("could not remove the registry pak (error %lu)", e);
        log_msg(C_WH "Removed the registry pak (no mods need it any more): the game uses its own registry again");
        return 0;
    }
    log_msg(C_WH "Found " C_OR "%d" C_WH " mod pak%s with registry entries (%d with their own registry, %d with manifests)",
            s.np, s.np == 1 ? "" : "s", nreg, nman);
    for (int i = 0; i < s.np; i++)
        log_msg(C_WH "  " C_OR "%s" C_WH "  (%s, pak order %d)%s", s.p[i].name, pak_kinds(&s.p[i]), s.p[i].order,
                s.p[i].has_utoc ? "" : C_RD "  - no .utoc next to it: the game does not load this pak" C_WH);
    char *want = expected_state(&s);
    if (have && !strcmp(have, want)) {
        log_msg(C_WH "Registry is up to date - nothing to do");
        if (console_is_open()) { log_msg(""); report_added(added); }
        return 0;
    }
    if (!exists) log_msg(C_OR "Reason: the registry pak does not exist yet" C_WH);
    else if (!have) log_msg(C_OR "Reason: the registry pak was not built by this tool" C_WH);
    else log_changes(have, &s, want);
    console_show();
    int r = build(paks, &s, want);
    if (r == 0) log_msg("\n" C_BL "Done." C_WH " Registry pak installed: " C_OR "Paks\\%ls" C_WH " - the game starts now.", REG_PAK);
    return r;
}
