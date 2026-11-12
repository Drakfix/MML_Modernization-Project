#pragma once
/* The part of <windows.h> our code uses, for platforms that aren't Windows (Android, later Linux
 * and Apple): types, structures (including the PE headers our loader reads in the game's exe),
 * the constants (values as the Windows SDK has them: the game compares some of them itself), and
 * the functions, implemented over POSIX in src/compat_posix.c. Only the non-Windows builds put
 * this folder on the include path. Handles are small table indices, never pointers: the game
 * keeps some (files, events) in 32 bits. */
#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <alloca.h>

/* calling conventions and MSVC keywords */
#define WINAPI
#define CALLBACK
#define APIENTRY
#define __stdcall
#define __cdecl
#define __fastcall
#define __inline inline
#define __forceinline inline __attribute__((always_inline))
#define _ReturnAddress() __builtin_return_address(0)

typedef int BOOL;
typedef unsigned char BYTE;
typedef unsigned short WORD;
typedef uint32_t DWORD;
typedef int32_t LONG;
typedef uint32_t ULONG;
typedef uint32_t UINT;
typedef int32_t INT;
typedef int16_t SHORT;
typedef uint16_t USHORT;
typedef char CHAR;
typedef uint16_t WCHAR;
typedef int64_t LONGLONG;
typedef uint64_t ULONGLONG;
typedef uint64_t DWORD64;
typedef uintptr_t ULONG_PTR, DWORD_PTR, SIZE_T, UINT_PTR, WPARAM;
typedef intptr_t LONG_PTR, INT_PTR, LPARAM, LRESULT;
typedef LONG HRESULT;
typedef float FLOAT;
typedef void VOID, *PVOID, *LPVOID;
typedef const void *LPCVOID;
typedef char *LPSTR;
typedef const char *LPCSTR;
typedef WCHAR *LPWSTR;
typedef const WCHAR *LPCWSTR;
typedef BYTE *LPBYTE;
typedef DWORD *LPDWORD;
typedef LONG *PLONG, *LPLONG;
typedef BOOL *LPBOOL;
typedef void *HANDLE;
typedef HANDLE *LPHANDLE;
typedef struct HWND__ *HWND;
typedef struct HINSTANCE__ *HINSTANCE, *HMODULE;
typedef struct HDC__ *HDC;
typedef struct HICON__ *HICON, *HCURSOR;
typedef struct HBRUSH__ *HBRUSH;
typedef struct HMENU__ *HMENU;
typedef struct HMONITOR__ *HMONITOR;
typedef struct HGDIOBJ__ *HGDIOBJ;
typedef LONG(WINAPI *FARPROC)(void);
typedef LRESULT(CALLBACK *WNDPROC)(HWND, UINT, WPARAM, LPARAM);

#define TRUE 1
#define FALSE 0
#define INVALID_HANDLE_VALUE ((HANDLE)(intptr_t)-1)
#define SUCCEEDED(hr) (((HRESULT)(hr)) >= 0)
#define FAILED(hr) (((HRESULT)(hr)) < 0)
#define LOWORD(l) ((WORD)((DWORD_PTR)(l) & 0xFFFF))
#define HIWORD(l) ((WORD)(((DWORD_PTR)(l) >> 16) & 0xFFFF))
#define MAKELONG(a, b) ((LONG)(((WORD)(a)) | ((DWORD)((WORD)(b))) << 16))
#define RGB(r, g, b) ((DWORD)(((BYTE)(r) | ((WORD)((BYTE)(g)) << 8)) | (((DWORD)(BYTE)(b)) << 16)))

typedef struct { LONG left, top, right, bottom; } RECT, *LPRECT;
typedef const RECT *LPCRECT;
typedef struct { LONG x, y; } POINT, *LPPOINT;
typedef struct { LONG cx, cy; } SIZE;
typedef union {
    struct { DWORD LowPart; LONG HighPart; } u;
    LONGLONG QuadPart;
} LARGE_INTEGER;

typedef struct {
    DWORD Data1;
    WORD Data2, Data3;
    BYTE Data4[8];
} GUID, IID, CLSID;
typedef const GUID *REFGUID, *REFIID, *REFCLSID;
#define IsEqualGUID(a, b) (memcmp((a), (b), sizeof(GUID)) == 0)
#define IsEqualIID IsEqualGUID
#ifdef INITGUID
#define DEFINE_GUID(n, l, w1, w2, b1, b2, b3, b4, b5, b6, b7, b8) \
    const GUID n = { l, w1, w2, { b1, b2, b3, b4, b5, b6, b7, b8 } }
#else
#define DEFINE_GUID(n, l, w1, w2, b1, b2, b3, b4, b5, b6, b7, b8) extern const GUID n
#endif

/* COM: an interface is a pointer to its vtable */
typedef struct IUnknown { void **lpVtbl; } IUnknown, *LPUNKNOWN;


/* values as the Windows SDK defines them */
#define BITSPIXEL                            0x0000000Cu
#define CP_ACP                               0x00000000u
#define CREATE_ALWAYS                        0x00000002u
#define CREATE_NEW                           0x00000001u
#define DLL_PROCESS_ATTACH                   0x00000001u
#define DRIVE_FIXED                          0x00000003u
#define ERROR_ACCESS_DENIED                  0x00000005u
#define ERROR_ALREADY_EXISTS                 0x000000B7u
#define ERROR_CALL_NOT_IMPLEMENTED           0x00000078u
#define ERROR_CANNOT_FIND_WND_CLASS          0x0000057Fu
#define ERROR_FILE_EXISTS                    0x00000050u
#define ERROR_FILE_NOT_FOUND                 0x00000002u
#define ERROR_GEN_FAILURE                    0x0000001Fu
#define ERROR_INSUFFICIENT_BUFFER            0x0000007Au
#define ERROR_INVALID_HANDLE                 0x00000006u
#define ERROR_INVALID_PARAMETER              0x00000057u
#define ERROR_MOD_NOT_FOUND                  0x0000007Eu
#define ERROR_NEGATIVE_SEEK                  0x00000083u
#define ERROR_NOT_ENOUGH_MEMORY              0x00000008u
#define ERROR_PATH_NOT_FOUND                 0x00000003u
#define ERROR_PROC_NOT_FOUND                 0x0000007Fu
#define ERROR_READ_FAULT                     0x0000001Eu
#define ERROR_TOO_MANY_OPEN_FILES            0x00000004u
#define ERROR_WRITE_FAULT                    0x0000001Du
#define EXCEPTION_ACCESS_VIOLATION           0xC0000005u
#define EXCEPTION_CONTINUE_SEARCH            0x00000000u
#define EXCEPTION_ILLEGAL_INSTRUCTION        0xC000001Du
#define EXCEPTION_PRIV_INSTRUCTION           0xC0000096u
#define EXCEPTION_STACK_OVERFLOW             0xC00000FDu
#define E_FAIL                               ((HRESULT)0x80004005u)
#define E_NOINTERFACE                        ((HRESULT)0x80004002u)
#define E_POINTER                            ((HRESULT)0x80004003u)
#define FILE_ATTRIBUTE_ARCHIVE               0x00000020u
#define FILE_ATTRIBUTE_DIRECTORY             0x00000010u
#define FILE_ATTRIBUTE_NORMAL                0x00000080u
#define FILE_CURRENT                         0x00000001u
#define FILE_END                             0x00000002u
#define FILE_MAP_ALL_ACCESS                  0x000F001Fu
#define FILE_SHARE_READ                      0x00000001u
#define FILE_TYPE_DISK                       0x00000001u
#define GENERIC_ALL                          0x10000000u
#define GENERIC_READ                         0x80000000u
#define GENERIC_WRITE                        0x40000000u
#define GWL_WNDPROC                          0xFFFFFFFCu
#define HORZRES                              0x00000008u
#define IDOK                                 0x00000001u
#define IMAGE_DIRECTORY_ENTRY_IMPORT         0x00000001u
#define IMAGE_SCN_CNT_CODE                   0x00000020u
#define INFINITE                             0xFFFFFFFFu
#define INVALID_FILE_ATTRIBUTES              0xFFFFFFFFu
#define INVALID_SET_FILE_POINTER             0xFFFFFFFFu
#define MAX_PATH                             260
#define MCIERR_DEVICE_NOT_INSTALLED          0x00000132u
#define MEM_COMMIT                           0x00001000u
#define MEM_DECOMMIT                         0x00004000u
#define MEM_FREE                             0x00010000u
#define MEM_PRIVATE                          0x00020000u
#define MEM_RELEASE                          0x00008000u
#define MEM_RESERVE                          0x00002000u
#define OPEN_ALWAYS                          0x00000004u
#define OPEN_EXISTING                        0x00000003u
#define PAGE_EXECUTE_READWRITE               0x00000040u
#define PAGE_NOACCESS                        0x00000001u
#define PAGE_READWRITE                       0x00000004u
#define PLANES                               0x0000000Eu
#define PM_REMOVE                            0x00000001u
#define SM_CXSCREEN                          0x00000000u
#define SM_CYSCREEN                          0x00000001u
#define S_OK                                 ((HRESULT)0x00000000u)
#define THREAD_PRIORITY_HIGHEST              0x00000002u
#define THREAD_PRIORITY_ABOVE_NORMAL         0x00000001u
#define TIMER_ALL_ACCESS                     0x001F0003u
#define TRUNCATE_EXISTING                    0x00000005u
#define VERTRES                              0x0000000Au
#define VK_BACK                              0x00000008u
/* (more keys: the names kbm_names.c knows) */
#define VK_MBUTTON 0x04u
#define VK_XBUTTON1 0x05u
#define VK_XBUTTON2 0x06u
#define VK_CAPITAL 0x14u
#define VK_END 0x23u
#define VK_HOME 0x24u
#define VK_INSERT 0x2Du
#define VK_DELETE 0x2Eu
#define VK_NUMPAD0 0x60u
#define VK_NUMPAD1 0x61u
#define VK_NUMPAD2 0x62u
#define VK_NUMPAD3 0x63u
#define VK_NUMPAD4 0x64u
#define VK_NUMPAD5 0x65u
#define VK_NUMPAD6 0x66u
#define VK_NUMPAD7 0x67u
#define VK_NUMPAD8 0x68u
#define VK_NUMPAD9 0x69u
#define VK_RSHIFT 0xA1u
#define VK_RCONTROL 0xA3u
#define VK_LMENU 0xA4u
#define VK_RMENU 0xA5u
#define VK_OEM_1 0xBAu
#define VK_OEM_PLUS 0xBBu
#define VK_OEM_COMMA 0xBCu
#define VK_OEM_MINUS 0xBDu
#define VK_OEM_PERIOD 0xBEu
#define VK_OEM_2 0xBFu
#define VK_OEM_3 0xC0u
#define VK_OEM_4 0xDBu
#define VK_OEM_5 0xDCu
#define VK_OEM_6 0xDDu
#define VK_OEM_7 0xDEu
#define VK_DOWN                              0x00000028u
#define VK_ESCAPE                            0x0000001Bu
#define VK_F1                                0x00000070u
#define VK_F5                                0x00000074u
#define VK_F6                                0x00000075u
#define VK_LBUTTON                           0x00000001u
#define VK_LCONTROL                          0x000000A2u
#define VK_LEFT                              0x00000025u
#define VK_LSHIFT                            0x000000A0u
#define VK_NEXT                              0x00000022u
#define VK_PRIOR                             0x00000021u
#define VK_RBUTTON                           0x00000002u
#define VK_RETURN                            0x0000000Du
#define VK_RIGHT                             0x00000027u
#define VK_SPACE                             0x00000020u
#define VK_TAB                               0x00000009u
#define VK_UP                                0x00000026u
#define WAIT_FAILED                          0xFFFFFFFFu
#define WAIT_OBJECT_0                        0x00000000u
#define WAIT_TIMEOUT                         0x00000102u
#define WM_ACTIVATEAPP                       0x0000001Cu
#define WM_CHAR                              0x00000102u
#define WM_DESTROY                           0x00000002u
#define WM_KEYDOWN                           0x00000100u
#define WM_LBUTTONDOWN                       0x00000201u
#define WM_MOVE                              0x00000003u
#define WM_QUIT                              0x00000012u
#define WM_RBUTTONDOWN                       0x00000204u
#define WM_SYSKEYDOWN                        0x00000104u

typedef struct {
    HWND hwnd;
    UINT message;
    WPARAM wParam;
    LPARAM lParam;
    DWORD time;
    POINT pt;
} MSG, *LPMSG;

typedef struct {
    DWORD nLength;
    LPVOID lpSecurityDescriptor;
    BOOL bInheritHandle;
} SECURITY_ATTRIBUTES, *LPSECURITY_ATTRIBUTES;

typedef struct { DWORD dwLowDateTime, dwHighDateTime; } FILETIME;
typedef struct {
    DWORD dwFileAttributes;
    FILETIME ftCreationTime, ftLastAccessTime, ftLastWriteTime;
    DWORD nFileSizeHigh, nFileSizeLow, dwReserved0, dwReserved1;
    CHAR cFileName[MAX_PATH];
    CHAR cAlternateFileName[14];
} WIN32_FIND_DATAA, *LPWIN32_FIND_DATAA;

typedef struct {
    PVOID BaseAddress, AllocationBase;
    DWORD AllocationProtect;
    SIZE_T RegionSize;
    DWORD State, Protect, Type;
} MEMORY_BASIC_INFORMATION;

typedef struct {
    UINT MaxCharSize;
    BYTE DefaultChar[2];
    BYTE LeadByte[12];
} CPINFO, *LPCPINFO;

/* locks: recursive mutexes (compat_posix.c) */
typedef struct { void *impl; } CRITICAL_SECTION, *LPCRITICAL_SECTION;
typedef struct { void *impl; } SRWLOCK, *PSRWLOCK;
#define SRWLOCK_INIT { 0 }

/* the PE format (the game's exe, which our loader and shims read) */
typedef struct {
    WORD e_magic, e_cblp, e_cp, e_crlc, e_cparhdr, e_minalloc, e_maxalloc, e_ss, e_sp, e_csum, e_ip, e_cs, e_lfarlc,
        e_ovno, e_res[4], e_oemid, e_oeminfo, e_res2[10];
    LONG e_lfanew;
} IMAGE_DOS_HEADER;
typedef struct {
    WORD Machine, NumberOfSections;
    DWORD TimeDateStamp, PointerToSymbolTable, NumberOfSymbols;
    WORD SizeOfOptionalHeader, Characteristics;
} IMAGE_FILE_HEADER;
typedef struct { DWORD VirtualAddress, Size; } IMAGE_DATA_DIRECTORY;
typedef struct {
    WORD Magic;
    BYTE MajorLinkerVersion, MinorLinkerVersion;
    DWORD SizeOfCode, SizeOfInitializedData, SizeOfUninitializedData, AddressOfEntryPoint, BaseOfCode, BaseOfData,
        ImageBase, SectionAlignment, FileAlignment;
    WORD MajorOperatingSystemVersion, MinorOperatingSystemVersion, MajorImageVersion, MinorImageVersion,
        MajorSubsystemVersion, MinorSubsystemVersion;
    DWORD Win32VersionValue, SizeOfImage, SizeOfHeaders, CheckSum;
    WORD Subsystem, DllCharacteristics;
    DWORD SizeOfStackReserve, SizeOfStackCommit, SizeOfHeapReserve, SizeOfHeapCommit, LoaderFlags,
        NumberOfRvaAndSizes;
    IMAGE_DATA_DIRECTORY DataDirectory[16];
} IMAGE_OPTIONAL_HEADER32;
typedef struct {
    DWORD Signature;
    IMAGE_FILE_HEADER FileHeader;
    IMAGE_OPTIONAL_HEADER32 OptionalHeader;
} IMAGE_NT_HEADERS32;
typedef struct {
    BYTE Name[8];
    union { DWORD PhysicalAddress, VirtualSize; } Misc;
    DWORD VirtualAddress, SizeOfRawData, PointerToRawData, PointerToRelocations, PointerToLinenumbers;
    WORD NumberOfRelocations, NumberOfLinenumbers;
    DWORD Characteristics;
} IMAGE_SECTION_HEADER;
typedef struct {
    union { DWORD Characteristics, OriginalFirstThunk; };
    DWORD TimeDateStamp, ForwarderChain, Name, FirstThunk;
} IMAGE_IMPORT_DESCRIPTOR;
typedef struct { WORD Hint; CHAR Name[1]; } IMAGE_IMPORT_BY_NAME;
typedef struct { union { DWORD ForwarderString, Function, Ordinal, AddressOfData; } u1; } IMAGE_THUNK_DATA32;
#define IMAGE_DOS_SIGNATURE 0x5A4D
#define IMAGE_NT_SIGNATURE 0x00004550
#define IMAGE_FILE_MACHINE_I386 0x014C
#define IMAGE_SCN_CNT_UNINITIALIZED_DATA 0x00000080
#define IMAGE_ORDINAL_FLAG32 0x80000000u
#define IMAGE_SNAP_BY_ORDINAL32(o) (((o) & IMAGE_ORDINAL_FLAG32) != 0)
#define IMAGE_ORDINAL32(o) ((o) & 0xFFFF)
#define IMAGE_FIRST_SECTION(nt) ((IMAGE_SECTION_HEADER *)((uint8_t *)&(nt)->OptionalHeader + \
                                                         (nt)->FileHeader.SizeOfOptionalHeader))

/* the MSVC C runtime's "secure" functions and others, as their standard forms */
#define _TRUNCATE ((size_t)-1)
/* as MSVC's with _TRUNCATE: -1 when the text was cut (vsnprintf returns the length it needed) */
static inline int _vsnprintf_s(char *buf, size_t size, size_t count, const char *fmt, va_list ap)
{
    int n;
    (void)count;
    if (!size)
        return -1;
    n = vsnprintf(buf, size, fmt, ap);
    return n < 0 || (size_t)n >= size ? -1 : n;
}
static inline int _snprintf_s(char *buf, size_t size, size_t count, const char *fmt, ...)
{
    va_list ap;
    int n;
    va_start(ap, fmt);
    n = _vsnprintf_s(buf, size, count, fmt, ap);
    va_end(ap);
    return n;
}
#define sprintf_s(buf, size, ...) _snprintf_s((buf), (size), _TRUNCATE, __VA_ARGS__)
#define sscanf_s sscanf      /* (our uses read numbers only: no size arguments) */
#define fscanf_s fscanf
#define _stricmp strcasecmp
#define _strnicmp strncasecmp
#define _strdup strdup
#define _alloca alloca
static inline int strcpy_s(char *d, size_t n, const char *s)
{
    if (!n)
        return 22;
    snprintf(d, n, "%s", s);
    return 0;
}
static inline int strncpy_s(char *d, size_t n, const char *s, size_t count)
{
    size_t k;
    if (!n)
        return 22;
    k = strnlen(s, count == _TRUNCATE ? n - 1 : count);
    if (k >= n)
        k = n - 1;
    memcpy(d, s, k);
    d[k] = 0;
    return 0;
}
static inline int strcat_s(char *d, size_t n, const char *s)
{
    size_t l = strnlen(d, n);
    return l < n ? strcpy_s(d + l, n - l, s) : 22;
}
static inline int _strlwr_s(char *s, size_t n)
{
    size_t i;
    for (i = 0; i < n && s[i]; i++)
        if (s[i] >= 'A' && s[i] <= 'Z')
            s[i] += 32;
    return 0;
}
const char *compat_path(const char *win_path, char *out, size_t n);
/* a Windows path (backslashes, any letter case), as CreateFileA takes it */
static inline int fopen_s(FILE **f, const char *path, const char *mode)
{
    char real[1024];
    *f = fopen(compat_path(path, real, sizeof real), mode);
    return *f ? 0 : 2;
}

/* ---- compat_posix.c ---- */
/* settings files */
DWORD GetPrivateProfileStringA(LPCSTR sec, LPCSTR key, LPCSTR dflt, LPSTR out, DWORD n, LPCSTR file);
UINT GetPrivateProfileIntA(LPCSTR sec, LPCSTR key, INT dflt, LPCSTR file);
BOOL WritePrivateProfileStringA(LPCSTR sec, LPCSTR key, LPCSTR value, LPCSTR file);
/* files: paths may use \ and any letter case (looked up case-insensitively) */
HANDLE CreateFileA(LPCSTR path, DWORD access, DWORD share, LPSECURITY_ATTRIBUTES sa, DWORD disposition,
                   DWORD flags, HANDLE tmpl);
BOOL ReadFile(HANDLE h, LPVOID buf, DWORD n, LPDWORD done, void *overlapped);
BOOL WriteFile(HANDLE h, LPCVOID buf, DWORD n, LPDWORD done, void *overlapped);
DWORD SetFilePointer(HANDLE h, LONG dist, PLONG dist_hi, DWORD method);
DWORD GetFileSize(HANDLE h, LPDWORD hi);
DWORD GetFileType(HANDLE h);
BOOL FlushFileBuffers(HANDLE h);
BOOL CloseHandle(HANDLE h);
DWORD GetFileAttributesA(LPCSTR path);
BOOL DeleteFileA(LPCSTR path);
BOOL MoveFileA(LPCSTR from, LPCSTR to);
BOOL CreateDirectoryA(LPCSTR path, LPSECURITY_ATTRIBUTES sa);
HANDLE FindFirstFileA(LPCSTR pattern, LPWIN32_FIND_DATAA fd);
BOOL FindNextFileA(HANDLE h, LPWIN32_FIND_DATAA fd);
BOOL FindClose(HANDLE h);
DWORD GetCurrentDirectoryA(DWORD n, LPSTR buf);
BOOL SetCurrentDirectoryA(LPCSTR dir);
DWORD GetModuleFileNameA(HMODULE m, LPSTR buf, DWORD n);   /* our "exe": mml_run.exe in the game folder */
const char *compat_path(const char *win_path, char *out, size_t n);   /* a Windows path as the real one */
void compat_set_game_dir(const char *dir);
/* a folder in the game folder that holds the version being played (its program, data and saves):
   their paths go there from now on (gamefiles.h GAMEFILES_JP_DIR) */
void compat_set_edition_dir(const char *sub);
/* time */
BOOL QueryPerformanceCounter(LARGE_INTEGER *c);
BOOL QueryPerformanceFrequency(LARGE_INTEGER *f);
DWORD GetTickCount(void);
void Sleep(DWORD ms);
/* threads, events, timers, locks */
typedef DWORD(WINAPI *LPTHREAD_START_ROUTINE)(LPVOID);
HANDLE CreateThread(LPSECURITY_ATTRIBUTES sa, SIZE_T stack, LPTHREAD_START_ROUTINE fn, LPVOID arg, DWORD flags,
                    LPDWORD id);
BOOL SetThreadPriority(HANDLE h, int prio);
DWORD GetCurrentThreadId(void);
HANDLE CreateEventA(LPSECURITY_ATTRIBUTES sa, BOOL manual, BOOL initial, LPCSTR name);
#define CreateEventW(sa, manual, initial, name) CreateEventA((sa), (manual), (initial), NULL)
BOOL SetEvent(HANDLE h);
BOOL ResetEvent(HANDLE h);
HANDLE CreateWaitableTimerExW(LPSECURITY_ATTRIBUTES sa, LPCWSTR name, DWORD flags, DWORD access);
BOOL SetWaitableTimer(HANDLE h, const LARGE_INTEGER *due, LONG period, void *fn, LPVOID arg, BOOL resume);
DWORD WaitForSingleObject(HANDLE h, DWORD ms);
void InitializeCriticalSection(LPCRITICAL_SECTION cs);
void DeleteCriticalSection(LPCRITICAL_SECTION cs);
void EnterCriticalSection(LPCRITICAL_SECTION cs);
void LeaveCriticalSection(LPCRITICAL_SECTION cs);
void AcquireSRWLockExclusive(PSRWLOCK l);
void ReleaseSRWLockExclusive(PSRWLOCK l);
#define AcquireSRWLockShared AcquireSRWLockExclusive
#define ReleaseSRWLockShared ReleaseSRWLockExclusive
#define InterlockedExchange(p, v) __atomic_exchange_n((p), (v), __ATOMIC_SEQ_CST)
#define InterlockedExchangeAdd(p, v) __atomic_fetch_add((p), (v), __ATOMIC_SEQ_CST)
#define InterlockedIncrement(p) __atomic_add_fetch((p), 1, __ATOMIC_SEQ_CST)
#define InterlockedDecrement(p) __atomic_sub_fetch((p), 1, __ATOMIC_SEQ_CST)
#define InterlockedCompareExchange(p, v, cmp) __sync_val_compare_and_swap((p), (cmp), (v))
#if defined(__aarch64__)
#define YieldProcessor() __asm__ __volatile__("yield")
#else
#define YieldProcessor() __builtin_ia32_pause()
#endif
/* memory */
LPVOID VirtualAlloc(LPVOID at, SIZE_T n, DWORD type, DWORD protect);
BOOL VirtualFree(LPVOID p, SIZE_T n, DWORD type);
BOOL VirtualProtect(LPVOID p, SIZE_T n, DWORD protect, LPDWORD old);
SIZE_T VirtualQuery(LPCVOID p, MEMORY_BASIC_INFORMATION *mi, SIZE_T n);
#define GetCurrentProcess() ((HANDLE)(intptr_t)-1)
#define FlushInstructionCache(proc, p, n) (__builtin___clear_cache((char *)(p), (char *)(p) + (n)), TRUE)
/* modules */
HMODULE LoadLibraryA(LPCSTR name);
FARPROC GetProcAddress(HMODULE m, LPCSTR name);
HMODULE GetModuleHandleA(LPCSTR name);
/* the process */
DWORD GetLastError(void);
void SetLastError(DWORD e);
DWORD GetEnvironmentVariableA(LPCSTR name, LPSTR buf, DWORD n);
__attribute__((noreturn)) void ExitProcess(UINT code);
int MessageBoxA(HWND w, LPCSTR text, LPCSTR caption, UINT type);   /* (logged) */
int GetSystemMetrics(int index);   /* the display's size */
HWND GetForegroundWindow(void);   /* the game's window (compat_set_window) */
BOOL GetClientRect(HWND w, LPRECT r);   /* its size */
void compat_set_window(HWND w, int width, int height);
#define CreateWaitableTimerW(sa, manual, name) CreateWaitableTimerExW((sa), (name), 0, 0)
#define IS_INTRESOURCE(p) ((((uintptr_t)(p)) >> 16) == 0)
/* x87 precision control: none off x86 */
#define _PC_24 0x00020000u
#define _MCW_PC 0x00030000u
static inline int _controlfp_s(unsigned *cur, unsigned value, unsigned mask)
{
    (void)value; (void)mask;
    if (cur)
        *cur = 0;
    return 0;
}
/* nothing on other platforms: the Windows-only parts check for NULL / FALSE */
#define PostQuitMessage(code) ((void)(code))
#define CreateFileMappingA(h, sa, prot, hi, lo, name) ((HANDLE)NULL)
#define MapViewOfFile(h, access, hi, lo, n) ((LPVOID)NULL)
#define PeekMessageA(msg, w, lo, hi, flags) (FALSE)
#define TranslateMessage(msg) (FALSE)
#define DispatchMessageA(msg) ((LRESULT)0)
SHORT GetAsyncKeyState(int vk);   /* plat_sdl.c: from SDL's keyboard state */
#define MB_OK 0
#define MB_ICONERROR 0x10
#define MB_ICONWARNING 0x30
