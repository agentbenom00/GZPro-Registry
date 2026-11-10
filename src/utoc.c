// IoStore bits: which packages a .utoc holds (a registry entry of a mod is only taken when the mod's own container
// has its package), and the empty .utoc/.ucas pair next to the registry pak.
//   .utoc: "-==--==--==--==-" | uint8 version | ... | uint32 header size (at 0x14) | uint32 chunk count (at 0x18) |
//          ... | chunk ids (12 bytes each) from the header size on: uint64 id, uint16 big-endian index, 0, uint8 type
//   a package's export data is chunk (package id, 0, type 1 = ExportBundleData)
#include "sr.h"
#include "container.h"

static const char TOC_MAGIC[16] = "-==--==--==--==-";

uint64_t package_id(const char *name)
{
    wchar_t *w = utf8_to_w(name);
    size_t n = wcslen(w);
    for (size_t i = 0; i < n; i++) w[i] = (wchar_t)towlower(w[i]);
    uint64_t h = city_hash64((const uint8_t *)w, 2 * n);
    free(w);
    return h;
}

static int cmp_u64(const void *a, const void *b)
{
    uint64_t x = *(const uint64_t *)a, y = *(const uint64_t *)b;
    return x < y ? -1 : x > y;
}

uint64_t *utoc_packages(const wchar_t *path, int *count)
{
    *count = 0;
    HANDLE h = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL, OPEN_EXISTING,
                           FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) return NULL;
    uint8_t hdr[0x20];
    DWORD got = 0;
    uint64_t *ids = NULL;
    if (ReadFile(h, hdr, sizeof hdr, &got, NULL) && got == sizeof hdr && !memcmp(hdr, TOC_MAGIC, 16)) {
        uint32_t hsize = rd32(hdr + 0x14), n = rd32(hdr + 0x18);
        LARGE_INTEGER sz, at;
        at.QuadPart = hsize;
        if (hdr[16] >= 2 && hsize >= 0x20 && n < (1u << 26) && GetFileSizeEx(h, &sz) && (int64_t)hsize + 12ll * n <= sz.QuadPart &&
            SetFilePointerEx(h, at, NULL, FILE_BEGIN)) {
            uint8_t *raw = xmalloc(12ull * n + 1);
            if (ReadFile(h, raw, 12 * n, &got, NULL) && got == 12 * n) {
                ids = xmalloc(sizeof(uint64_t) * (n + 1));
                for (uint32_t i = 0; i < n; i++) {
                    const uint8_t *c = raw + 12ull * i;
                    if (c[11] == 1 && c[8] == 0 && c[9] == 0) ids[(*count)++] = rd64(c);
                }
                qsort(ids, *count, sizeof *ids, cmp_u64);
            }
            free(raw);
        }
    }
    CloseHandle(h);
    return ids;
}

int has_package(const uint64_t *sorted, int n, uint64_t id)
{
    int lo = 0, hi = n - 1;
    while (lo <= hi) {
        int mid = lo + (hi - lo) / 2;
        if (sorted[mid] == id) return 1;
        if (sorted[mid] < id) lo = mid + 1; else hi = mid - 1;
    }
    return 0;
}

// write `data` unless the file already holds exactly that; 0 = ok, else the Windows error
static DWORD put_file(const wchar_t *path, const uint8_t *data, size_t n)
{
    size_t have_n = 0;
    uint8_t *have = file_read_all(path, &have_n);
    int same = have && have_n == n && !memcmp(have, data, n);
    free(have);
    if (same) return 0;
    HANDLE h = CreateFileW(path, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) return GetLastError();
    DWORD put = 0;
    BOOL ok = WriteFile(h, data, (DWORD)n, &put, NULL) && put == n;
    DWORD e = ok ? 0 : GetLastError();
    if (!CloseHandle(h) && !e) e = GetLastError();
    return ok ? e : (e ? e : ERROR_WRITE_FAULT);
}

int write_empty_container(const wchar_t *utoc, const wchar_t *ucas)
{
    DWORD e = put_file(ucas, EMPTY_UCAS, sizeof EMPTY_UCAS);
    if (!e) e = put_file(utoc, EMPTY_UTOC, sizeof EMPTY_UTOC);
    return (int)e;
}
