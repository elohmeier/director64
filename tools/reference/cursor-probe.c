// Load unchanged original projector resources through Windows APIs under Wine.
// Writes only our BMP observations; never modifies the module or cursor data.
typedef unsigned long DWORD;
typedef long LONG;
typedef unsigned short WORD;
typedef int BOOL;
typedef void *HANDLE;
#define API __declspec(dllimport) __stdcall
API HANDLE LoadLibraryExA(const char *, HANDLE, DWORD);
API HANDLE LoadCursorA(HANDLE, const char *);
API DWORD GetLastError(void);
API BOOL FreeLibrary(HANDLE);
API HANDLE CreateFileA(const char *, DWORD, DWORD, void *, DWORD, DWORD, HANDLE);
API BOOL WriteFile(HANDLE, const void *, DWORD, DWORD *, void *);
API BOOL CloseHandle(HANDLE);
API void ExitProcess(unsigned);
API HANDLE CreateCompatibleDC(HANDLE);
API HANDLE SelectObject(HANDLE, HANDLE);
API BOOL DeleteObject(HANDLE);
API BOOL DeleteDC(HANDLE);
typedef struct {
  DWORD size; LONG width, height; WORD planes, bits;
  DWORD compression, image_size; LONG xppm, yppm; DWORD colors, important;
} INFO;
API HANDLE CreateDIBSection(HANDLE, const INFO *, unsigned, void **, HANDLE, DWORD);
typedef struct { BOOL icon; DWORD x, y; HANDLE mask, color; } ICON;
API BOOL GetIconInfo(HANDLE, ICON *);
API BOOL DrawIconEx(HANDLE, int, int, HANDLE, int, int, unsigned, HANDLE, unsigned);
static char log_data[2048];
static unsigned char bitmap_file[54+4096];
static unsigned log_length;
void *memset(void *memory,int value,unsigned n) {
  unsigned char *p=memory;for(unsigned i=0;i<n;i++) p[i]=(unsigned char)value;
  return memory;
}
static void text(const char *s) { while (*s && log_length<sizeof(log_data)) log_data[log_length++]=*s++; }
static void number(DWORD n) {
  char digits[10];unsigned count=0;
  do { digits[count++]=(char)('0'+n%10);n/=10; } while(n);
  while(count) { char digit[2]={digits[--count],0};text(digit); }
}
static BOOL write_file(const char *path,const void *bytes,DWORD n) {
  HANDLE f=CreateFileA(path,0x40000000,1,(void *)0,2,0x80,(HANDLE)0);
  DWORD wrote=0;
  BOOL ok=f!=(HANDLE)-1 && WriteFile(f,bytes,n,&wrote,(void *)0) && wrote==n;
  if(f!=(HANDLE)-1) CloseHandle(f);
  return ok;
}
static void put32(unsigned char *p,DWORD n) {
  for(unsigned i=0;i<4;i++) { p[i]=(unsigned char)n;n>>=8; }
}
static BOOL observe(HANDLE owner,const char *name,const char *path) {
  HANDLE cursor=LoadCursorA(owner,name);
  text(name);text(" LoadCursor=");number((DWORD)cursor);
  if(!cursor) { text(" error=");number(GetLastError());text("\r\n");return 0; }
  ICON icon={0};BOOL info_ok=GetIconInfo(cursor,&icon);
  text(" GetIconInfo=");number((DWORD)info_ok);text(" hotspot=");number(icon.x);text(",");number(icon.y);
  INFO info={40,32,-32,1,32,0,4096,0,0,0,0};
  HANDLE dc=CreateCompatibleDC((HANDLE)0);void *raw=(void *)0;
  HANDLE bitmap=CreateDIBSection(dc,&info,0,&raw,(HANDLE)0,0);
  BOOL ok=info_ok && dc && bitmap && raw;
  HANDLE previous=(HANDLE)0;
  if(ok) {
    previous=SelectObject(dc,bitmap);
    DWORD *pixels=(DWORD *)raw;for(unsigned i=0;i<1024;i++) pixels[i]=0x00123456;
    ok=DrawIconEx(dc,0,0,cursor,32,32,0,(HANDLE)0,3);
    text(" DrawIconEx=");number((DWORD)ok);text(" error=");number(ok?0:GetLastError());
    unsigned char *file=bitmap_file;
    file[0]='B';file[1]='M';put32(file+2,sizeof(bitmap_file));put32(file+10,54);
    put32(file+14,40);put32(file+18,32);put32(file+22,(DWORD)-32);file[26]=1;file[28]=32;
    put32(file+34,4096);
    for(unsigned i=0;i<4096;i++) file[54+i]=((unsigned char *)raw)[i];
    ok=ok && write_file(path,file,sizeof(bitmap_file));
  }
  text(" saved=");number((DWORD)ok);text("\r\n");
  if(previous) SelectObject(dc,previous);
  if(bitmap) DeleteObject(bitmap);
  if(dc) DeleteDC(dc);
  if(icon.mask) DeleteObject(icon.mask);
  if(icon.color) DeleteObject(icon.color);
  return ok;
}
void mainCRTStartup(void) {
  HANDLE owner=LoadLibraryExA("C:\\Original\\STARTW95.EXE",(HANDLE)0,2);
  text("LoadLibraryExA_datafile=");number((DWORD)owner);text(" error=");number(owner?0:GetLastError());text("\r\n");
  BOOL cross=owner && observe(owner,"CrossCursor","C:\\cursor-cross.bmp");
  BOOL plus=owner && observe(owner,"PlusCursor","C:\\cursor-plus.bmp");
  if(owner) FreeLibrary(owner);
  text("DIRECTOR64_CURSOR_PROBE_COMPLETE\r\n");
  BOOL saved=write_file("C:\\cursor-probe.txt",log_data,log_length);
  ExitProcess(!(cross && plus && saved));
}
