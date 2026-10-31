// Observe the original OPTGRAPH export inside an isolated 32-bit Wine prefix.
// This probe never modifies the DLL, the disc, or a protection result.
// Build with clang --target=i686-pc-windows-msvc -ffreestanding -fno-builtin
// and link /nodefaultlib against an import library from optgraph-probe.def.
typedef unsigned long DWORD;
typedef void *HANDLE;
#define API __declspec(dllimport) __stdcall
API HANDLE LoadLibraryA(const char *);
API void *GetProcAddress(HANDLE, const char *);
API DWORD GetLastError(void);
API int FreeLibrary(HANDLE);
API HANDLE CreateFileA(const char *, DWORD, DWORD, void *, DWORD, DWORD, HANDLE);
API int WriteFile(HANDLE, const void *, DWORD, DWORD *, void *);
API int CloseHandle(HANDLE);
API void ExitProcess(unsigned);

static char output[1024];
static unsigned length;
static void text(const char *s) {
  while (*s && length < sizeof(output)) output[length++] = *s++;
}
static void hex(DWORD n) {
  for (int shift = 28; shift >= 0; shift -= 4) {
    char digit[2] = {"0123456789abcdef"[(n >> shift) & 15], 0};
    text(digit);
  }
}
static void decimal(DWORD n) {
  char digits[10];
  unsigned count = 0;
  do {
    digits[count++] = '0' + n % 10;
    n /= 10;
  } while (n);
  while (count) {
    char digit[2] = {digits[--count], 0};
    text(digit);
  }
}

void mainCRTStartup(void) {
  const char *path = "C:\\Original\\SETUP\\WIN95\\XTRAS\\OPTGRAPH.DLL";
  text("library_path="); text(path); text("\r\n");
  HANDLE library = LoadLibraryA(path);
  DWORD load_error = library ? 0 : GetLastError();
  text("LoadLibraryA_handle="); hex((DWORD)library);
  text(" error="); hex(load_error); text("\r\n");
  void *address = (void *)0;
  if (library) {
    address = GetProcAddress(library, "initdisplay");
    DWORD export_error = address ? 0 : GetLastError();
    text("GetProcAddress_address="); hex((DWORD)address);
    text(" error="); hex(export_error); text("\r\n");
    if (address) {
      DWORD value = ((DWORD (__cdecl *)(void))address)();
      text("initdisplay_return_hex="); hex(value);
      text(" decimal="); decimal(value); text("\r\n");
    }
    FreeLibrary(library);
  }
  text("DIRECTOR64_PROBE_COMPLETE\r\n");
  HANDLE file = CreateFileA("C:\\director64-optgraph-probe.txt", 0x40000000, 1,
                            (void *)0, 2, 0x80, (void *)0);
  DWORD written = 0;
  int ok = file != (HANDLE)-1 && WriteFile(file, output, length, &written, (void *)0);
  if (file != (HANDLE)-1) CloseHandle(file);
  ExitProcess(!ok || written != length || !address);
}
