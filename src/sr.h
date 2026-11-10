// GZPro-Registry (Godzilla Destroy All Monsters Melee Remastered), all inside version.dll. Shared declarations.
// Derived from MMS Registry (Midnight Suns, MIT, agentbenom00): the same launch hook, pak reader/writer and helpers,
// with the asset registry code rewritten for this game's UE 5.8 format.
#pragma once
#define WIN32_LEAN_AND_MEAN
#define UNICODE
#include <windows.h>
#include <setjmp.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define GZPRO_REGISTRY_VERSION "1.0.0"                      // the release; also in the marker and the log
#define GAME_EXE L"GZProSteam-Win64-Shipping.exe"
#define GAME_NAME "Godzilla Destroy All Monsters Melee Remastered"
// The registry pak lives in its own subfolder of Paks (the game mounts subfolders too, and the GZPro Mod Manager only
// lists files at the top level, so it does not show up there as an unknown mod). Its path starts with Paks/GZPro-, which
// UE gives base order 4 (a prefix test, so subfolders count), and "_99_P" adds 100 * 100: pak order 10004, above every
// normal mod pak (GZPro-Name_P.pak = 104), so its AssetRegistry.bin is the one the game reads. The game only mounts a pak that has a .utoc/.ucas pair next to it: the pair is an empty container.
#define REG_DIR L"GZPro-Registry"
#define REG_STEM L"GZPro-Registry_99_P"
#define REG_PAK REG_DIR L"\\" REG_STEM L".pak"
#define REG_UTOC REG_DIR L"\\" REG_STEM L".utoc"
#define REG_UCAS REG_DIR L"\\" REG_STEM L".ucas"
#define REGISTRY_PATH "GZPro/AssetRegistry.bin"
#define STATE_PATH "GZPro/GZPro-Registry/state.json"
#define MANIFEST_DIR "GZPro/RegistryMods/"
#define DATA_DIR_NAME L"GZPro-Registry"
#define LOG_NAME L"GZPro-Registry.log"

// ---- errors: sr_fail() jumps back to the innermost sr_try
extern jmp_buf *sr_jmp;
extern char sr_error[1024];
__attribute__((noreturn)) void sr_fail(const char *fmt, ...);

// ---- memory / growable buffer
void *xmalloc(size_t n);
void *xcalloc(size_t n, size_t m);
void *xrealloc(void *p, size_t n);
char *xstrdup(const char *s);

typedef struct { uint8_t *p; size_t n, cap; } buf;
void buf_reserve(buf *b, size_t extra);
void buf_put(buf *b, const void *d, size_t n);
void buf_u8(buf *b, uint8_t v);
void buf_u16be(buf *b, uint16_t v);
void buf_u32(buf *b, uint32_t v);
void buf_u64(buf *b, uint64_t v);
void buf_fstring(buf *b, const char *s);                    // int32 length (with the 0) + ASCII + 0
void buf_free(buf *b);

static inline uint16_t rd16(const uint8_t *p) { uint16_t v; memcpy(&v, p, 2); return v; }
static inline uint32_t rd32(const uint8_t *p) { uint32_t v; memcpy(&v, p, 4); return v; }
static inline uint64_t rd64(const uint8_t *p) { uint64_t v; memcpy(&v, p, 8); return v; }

// bounds-checked little-endian reader over a byte array
typedef struct { const uint8_t *p; size_t n, at; } rd;
void rd_need(rd *r, size_t n);
uint8_t rd_u8(rd *r);
uint32_t rd_u32(rd *r);
int32_t rd_i32(rd *r);
uint64_t rd_u64(rd *r);
void rd_skip(rd *r, size_t n);
char *rd_fstring(rd *r);                                    // malloc'd UTF-8; UTF-16 strings are converted
void rd_skip_fstring(rd *r);

// ---- files (wide paths)
uint8_t *file_read_all(const wchar_t *path, size_t *n);    // NULL if missing/unreadable
wchar_t *wpath(const wchar_t *a, const wchar_t *b);        // a\b, malloc'd
wchar_t *utf8_to_w(const char *s);
char *w_to_utf8(const wchar_t *s);
const wchar_t *data_dir(void);                              // %LOCALAPPDATA%\GZPro-Registry (created)

// ---- log (file in data_dir + the console when one is open)
void log_open(void);
// Console colours: a message may contain colour markers (they are removed from the log file, the console shows them).
// C_OR orange = what the mods add (costumes, entries, pak names...); C_YE is the same orange; C_BL light blue = the
// banner; C_RD light red = errors; C_WH = normal text.
#define C_OR "\x01O"
#define C_YE "\x01Y"
#define C_BL "\x01" "B"
#define C_RD "\x01R"
#define C_WH "\x01W"
void log_msg(const char *fmt, ...);                         // log file + console (the console shows what was logged before it opened)
void log_file_only(const char *fmt, ...);                   // log file only (details that would flood the console)
void console_show(void);
int console_is_open(void);

// ---- options (%LOCALAPPDATA%\GZPro-Registry\options.txt, written with its defaults on the first run)
typedef struct {
    int show_always;                                            // 1: open the report window on every launch (0: only when the registry is rebuilt)
    int hold_seconds;                                           // how long the window stays after the report (the game waits for it)
    int font_percent;                                           // console text size in percent of the console's own (125 = 25% bigger)
    int full_list;                                              // 1: list every added entry on the console (0: only the totals; the log file always has everything)
} sr_options;
extern sr_options sr_opt;
void options_load(void);

// ---- hashing
uint64_t city_hash64(const uint8_t *s, size_t len);
uint64_t fnv1a64(const void *p, size_t n);
void sha1(const void *p, size_t n, uint8_t out[20]);        // bcrypt, loaded on demand
typedef struct sha1_ctx sha1_ctx;
sha1_ctx *sha1_begin(void);
void sha1_add(sha1_ctx *c, const void *p, size_t n);
void sha1_end(sha1_ctx *c, uint8_t out[20]);

// ---- compression
// zlib stream (header + deflate + adler) or raw deflate; 0 on success
int inflate_raw(const uint8_t *in, size_t inlen, uint8_t *out, size_t outlen, size_t *consumed);
int inflate_zlib(const uint8_t *in, size_t inlen, uint8_t *out, size_t outlen);
size_t deflate_zlib(const uint8_t *in, size_t n, uint8_t *out, size_t outcap);   // returns size, 0 if it didn't fit
size_t deflate_bound(size_t n);
int oodle_load(int allow_download);                         // 1 if Oodle is usable
int oodle_decompress(const uint8_t *in, size_t inlen, uint8_t *out, size_t outlen);

// ---- paks
typedef struct {
    char *path;                                             // relative to the game folder, e.g. GZPro/AssetRegistry.bin
    int64_t offset, size, usize;
    int method;                                             // index into pak.methods (0 = none)
    int encrypted;
    uint32_t block_size;
    int nblocks;
    int64_t *blocks;                                        // start, end pairs (absolute file offsets)
} pak_entry;

typedef struct {
    HANDLE h;
    int64_t length;
    int version;
    int encrypted_index;
    char methods[6][33];
    char *mount;                                            // mount point relative to the game folder ("" or "GZPro/")
    uint8_t *index; size_t index_n;
    int64_t fdi_offset, fdi_size;
    uint8_t *encoded; size_t encoded_n;
    uint8_t *legacy; size_t legacy_n; int legacy_count; size_t legacy_at;
} pak;

// filter: return 1 to keep (dir is relative to the game folder with a trailing '/', or "" at the root)
typedef int (*pak_filter)(const char *dir, const char *name, void *ctx);
int pak_open(pak *pk, const wchar_t *path);                 // 0 = ok, -1 not a pak, -2 encrypted (not readable)
void pak_close(pak *pk);
pak_entry *pak_list(pak *pk, pak_filter f, void *ctx, int *count);
void pak_entries_free(pak_entry *e, int count);
uint8_t *pak_read(pak *pk, pak_entry *e, int allow_oodle);  // NULL + sr_error if it can't (e.g. Oodle not loaded)

typedef struct pak_writer pak_writer;
pak_writer *pak_write_begin(const wchar_t *tmp_path);       // NULL: could not create (GetLastError)
void pak_write_file(pak_writer *w, const char *path, const uint8_t *data, size_t n, int compress);
void pak_write_end(pak_writer *w);

// ---- IoStore (utoc.c)
uint64_t package_id(const char *package_name);              // FPackageId of "/Game/..." (CityHash64 of the lowercase UTF-16 name)
// the ExportBundleData chunk ids (= package ids) of a .utoc, sorted; NULL if it can't be read
uint64_t *utoc_packages(const wchar_t *path, int *count);
int has_package(const uint64_t *sorted, int n, uint64_t id);
int write_empty_container(const wchar_t *utoc, const wchar_t *ucas);   // the registry pak's companion pair; 0 = ok

// ---- JSON (just enough for manifests)
typedef enum { J_NULL, J_BOOL, J_NUM, J_STR, J_ARR, J_OBJ } jtype;
typedef struct json { jtype t; char *s; double num; int n; struct json **items; char **keys; } json;
json *json_parse(const char *s, size_t n);                  // NULL on error
json *json_get(json *o, const char *key);
void json_free(json *j);

// ---- asset registry (registry.c)
typedef struct reg reg;
reg *reg_parse(uint8_t *data, size_t n);                    // keeps data (frees it in reg_free); sr_fail if not a GZPro registry
void reg_free(reg *r);
int reg_count(const reg *r);                                // entries (assets)
int reg_find(reg *r, const char *object_path);              // "/Game/Dir/Pkg.Obj" -> entry index, -1 if not registered
char *reg_object_path(reg *r, int i);                       // malloc'd "/Game/Dir/Pkg.Obj"
char *reg_package(reg *r, int i);                           // malloc'd "/Game/Dir/Pkg"
char *reg_class(reg *r, int i);                             // malloc'd class name, e.g. "GZProCharacterSkinDataAsset"
typedef char *(*rename_fn)(const char *s, void *ctx);       // malloc'd new name, or NULL = unchanged
// add a renamed copy of entry i (rules applied to every name and export path in it): 0 added, 1 the new name is
// already registered (nothing added), 2 the rules leave its name unchanged (nothing added)
int reg_clone(reg *r, int i, rename_fn rn, void *ctx, char **new_path);
// add entry i of another registry: 0 added, 1 already registered
int reg_copy(reg *dst, reg *src, int i);
uint8_t *reg_build(reg *r, size_t *n);                      // the new AssetRegistry.bin (checked by parsing it again)

// ---- the sync
int sync_quick_check(const wchar_t *paks);                  // in DllMain: 1 = something to do (or unsure)
int sync_run(const wchar_t *paks, int interactive);         // the full sync, in its own process
