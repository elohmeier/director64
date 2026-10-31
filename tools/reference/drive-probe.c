// Synthetic Win32 drive observations in a private Wine prefix. No game code.
// Build with clang --target=i686-pc-windows-msvc -ffreestanding -fno-builtin
// and link /nodefaultlib against an import library from drive-probe.def.
typedef unsigned long DWORD;
typedef void *HANDLE;
#define API __declspec(dllimport) __stdcall
API unsigned GetDriveTypeA(const char *);
API int GetVolumeInformationA(const char *, char *, DWORD, DWORD *, DWORD *, DWORD *, char *, DWORD);
API DWORD GetFileAttributesA(const char *);
API DWORD GetLastError(void);
API HANDLE CreateFileA(const char *, DWORD, DWORD, void *, DWORD, DWORD, HANDLE);
API int ReadFile(HANDLE, void *, DWORD, DWORD *, void *);
API int WriteFile(HANDLE, const void *, DWORD, DWORD *, void *);
API int CloseHandle(HANDLE);
API void ExitProcess(unsigned);
static char output[16384], label[256], filesystem[64], block[2048];
static unsigned length;
static void text(const char *s) { while (*s && length < sizeof(output)) output[length++] = *s++; }
static void number(DWORD n) {
  for (int shift = 28; shift >= 0; shift -= 4) {
    char digit[2] = {"0123456789abcdef"[(n >> shift) & 15], 0};
    text(digit);
  }
}
void mainCRTStartup(void) {
  for (char drive = 'A'; drive <= 'Z'; drive++) {
    char root[4] = {drive, ':', '\\', 0};
    DWORD serial = 0, maximum = 0, flags = 0;
    label[0] = filesystem[0] = 0;
    text(root); text(" type="); number(GetDriveTypeA(root));
    int ok = GetVolumeInformationA(root, label, sizeof(label), &serial, &maximum, &flags,
                                  filesystem, sizeof(filesystem));
    text(" volume_ok="); number((DWORD)ok); text(" error="); number(ok ? 0 : GetLastError());
    text(" serial="); number(serial); text(" label="); text(label);
    text(" fs="); text(filesystem); text("\r\n");
  }
  HANDLE raw = CreateFileA("\\\\.\\D:", 0x80000000, 3, (void *)0, 3, 0, (void *)0);
  text("raw_D_open="); number(raw != (HANDLE)-1); text(" error=");
  number(raw == (HANDLE)-1 ? GetLastError() : 0);
  if (raw != (HANDLE)-1) {
    DWORD received = 0;
    text(" read="); number(ReadFile(raw, block, sizeof(block), &received, (void *)0));
    text(" bytes="); number(received); CloseHandle(raw);
  }
  text("\r\nD_DATA_attributes="); number(GetFileAttributesA("D:\\DATA")); text("\r\n");
  HANDLE file = CreateFileA("C:\\director64-drive-probe.txt", 0x40000000, 1, (void *)0,
                            2, 0x80, (void *)0);
  DWORD written = 0;
  int ok = file != (HANDLE)-1 && WriteFile(file, output, length, &written, (void *)0);
  if (file != (HANDLE)-1) CloseHandle(file);
  ExitProcess(!ok || written != length);
}
