// Stand-in for GZProSteam-Win64-Shipping.exe: imports version.dll like the game (GetFileVersionInfoSizeW,
// GetFileVersionInfoW, VerQueryValueW) and calls them, so the proxy's DllMain and forwarders get exercised.
#include <windows.h>
#include <stdio.h>
int main(void)
{
    wchar_t path[MAX_PATH];
    GetSystemDirectoryW(path, MAX_PATH);
    lstrcatW(path, L"\\kernel32.dll");
    DWORD h = 0, n = GetFileVersionInfoSizeW(path, &h);
    void *buf = malloc(n ? n : 1);
    BOOL ok = n && GetFileVersionInfoW(path, 0, n, buf);
    VS_FIXEDFILEINFO *fi = NULL;
    UINT len = 0;
    BOOL q = ok && VerQueryValueW(buf, L"\\", (void **)&fi, &len);
    HMODULE v = GetModuleHandleW(L"version.dll");
    wchar_t mod[MAX_PATH] = L"?";
    if (v) GetModuleFileNameW(v, mod, MAX_PATH);
    // Wine reports its own builtin version.dll under the path of a version.dll it found next to the exe, so the path
    // alone does not tell which one is loaded: only GZPro-Registry's has the GZProRegistrySync export
    printf("host: version.dll = %ls\nhost: GZProRegistrySync export: %s\n", mod,
           v && GetProcAddress(v, "GZProRegistrySync") ? "yes" : "no");
    printf("host: GetFileVersionInfoSizeW=%lu GetFileVersionInfoW=%d VerQueryValueW=%d version %lu.%lu\n",
           n, ok, q, fi ? (unsigned long)HIWORD(fi->dwFileVersionMS) : 0ul, fi ? (unsigned long)LOWORD(fi->dwFileVersionMS) : 0ul);
    return 0;
}
