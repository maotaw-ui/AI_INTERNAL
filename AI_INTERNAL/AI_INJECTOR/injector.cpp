#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#define WINVER 0x0A00
#define _WIN32_WINNT 0x0A00

#include <windows.h>
#include <windowsx.h>
#include <shellapi.h>
#include <tlhelp32.h>
#include <d3d11.h>
#include <dxgi.h>
#include <d2d1.h>
#include <d2d1helper.h>
#include <dwrite.h>
#include <algorithm>
#include <atomic>
#include <cstddef>
#include <cstring>
#include <cwchar>
#include <fstream>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "d2d1.lib")
#pragma comment(lib, "dwrite.lib")
#pragma comment(lib, "shell32.lib")

typedef BOOL(WINAPI* f_RtlAddFunctionTable)(PIMAGE_RUNTIME_FUNCTION_ENTRY, DWORD, DWORD64);
typedef BOOL(WINAPI* f_DLL_ENTRY_POINT)(HINSTANCE, DWORD, LPVOID);
typedef HMODULE(WINAPI* f_LoadLibraryA)(LPCSTR);
typedef FARPROC(WINAPI* f_GetProcAddress)(HMODULE, LPCSTR);

struct MANUAL_MAPPING_DATA {
    f_LoadLibraryA pLoadLibraryA;
    f_GetProcAddress pGetProcAddress;
#ifdef _WIN64
    f_RtlAddFunctionTable pRtlAddFunctionTable;
    void* pCxxThrowStub;
#endif
    BYTE* pbase;
    DWORD fdwReasonParam;
    LPVOID reservedParam;
    bool SEHSupport;
    HINSTANCE hMod;
};

#ifdef _WIN64
#define CURRENT_ARCH IMAGE_FILE_MACHINE_AMD64
#define RELOC_FLAG(R) (((R) >> 0x0C) == IMAGE_REL_BASED_DIR64)
#else
#define CURRENT_ARCH IMAGE_FILE_MACHINE_I386
#define RELOC_FLAG(R) (((R) >> 0x0C) == IMAGE_REL_BASED_HIGHLOW)
#endif

extern "C" void __stdcall Shellcode(MANUAL_MAPPING_DATA* pData);

#pragma runtime_checks("", off)
#pragma optimize("", off)
void __stdcall Shellcode(MANUAL_MAPPING_DATA* pData) {
    if (!pData) {
        return;
    }

    BYTE* pBase = pData->pbase;
    auto* pDos = reinterpret_cast<IMAGE_DOS_HEADER*>(pBase);
    auto* pNt = reinterpret_cast<IMAGE_NT_HEADERS*>(pBase + pDos->e_lfanew);
    IMAGE_OPTIONAL_HEADER* pOpt = &pNt->OptionalHeader;

    auto LoadLib = pData->pLoadLibraryA;
    auto GetProc = pData->pGetProcAddress;
#ifdef _WIN64
    auto RtlAdd = pData->pRtlAddFunctionTable;
#endif

    auto DllMain = reinterpret_cast<f_DLL_ENTRY_POINT>(pBase + pOpt->AddressOfEntryPoint);

    const ptrdiff_t delta = pBase - reinterpret_cast<BYTE*>(pOpt->ImageBase);
    if (delta != 0 && pOpt->DataDirectory[IMAGE_DIRECTORY_ENTRY_BASERELOC].Size) {
        auto* rel = reinterpret_cast<IMAGE_BASE_RELOCATION*>(
            pBase + pOpt->DataDirectory[IMAGE_DIRECTORY_ENTRY_BASERELOC].VirtualAddress);
        auto* end = reinterpret_cast<IMAGE_BASE_RELOCATION*>(
            reinterpret_cast<uintptr_t>(rel) + pOpt->DataDirectory[IMAGE_DIRECTORY_ENTRY_BASERELOC].Size);

        while (rel < end && rel->SizeOfBlock) {
            UINT cnt = (rel->SizeOfBlock - sizeof(IMAGE_BASE_RELOCATION)) / sizeof(WORD);
            WORD* info = reinterpret_cast<WORD*>(rel + 1);
            for (UINT i = 0; i < cnt; ++i, ++info) {
                if (RELOC_FLAG(*info)) {
                    auto* patch = reinterpret_cast<UINT_PTR*>(
                        pBase + rel->VirtualAddress + (*info & 0xFFF));
                    *patch = static_cast<UINT_PTR>(static_cast<ptrdiff_t>(*patch) + delta);
                }
            }
            rel = reinterpret_cast<IMAGE_BASE_RELOCATION*>(reinterpret_cast<BYTE*>(rel) + rel->SizeOfBlock);
        }
    }

    if (pOpt->DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT].Size) {
        auto* imp = reinterpret_cast<IMAGE_IMPORT_DESCRIPTOR*>(
            pBase + pOpt->DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT].VirtualAddress);
        while (imp->Name) {
            char* mod = reinterpret_cast<char*>(pBase + imp->Name);
            HINSTANCE h = LoadLib(mod);
            auto* thunk = reinterpret_cast<ULONG_PTR*>(pBase + imp->OriginalFirstThunk);
            auto* func = reinterpret_cast<ULONG_PTR*>(pBase + imp->FirstThunk);
            if (!imp->OriginalFirstThunk) {
                thunk = func;
            }

            for (; *thunk; ++thunk, ++func) {
                if (IMAGE_SNAP_BY_ORDINAL(*thunk)) {
                    *func = reinterpret_cast<ULONG_PTR>(GetProc(h, reinterpret_cast<char*>(*thunk & 0xFFFF)));
                }
                else {
                    auto* byName = reinterpret_cast<IMAGE_IMPORT_BY_NAME*>(pBase + *thunk);
#ifdef _WIN64
                    const char* n = byName->Name;
                    bool isThrow = pData->pCxxThrowStub &&
                        n[0] == '_' && n[1] == 'C' && n[2] == 'x' && n[3] == 'x' && n[4] == 'T' && n[5] == 'h' &&
                        n[6] == 'r' && n[7] == 'o' && n[8] == 'w' && n[9] == 'E' && n[10] == 'x' && n[11] == 'c' &&
                        n[12] == 'e' && n[13] == 'p' && n[14] == 't' && n[15] == 'i' && n[16] == 'o' && n[17] == 'n' && n[18] == '\0';
                    if (isThrow) {
                        *func = reinterpret_cast<ULONG_PTR>(pData->pCxxThrowStub);
                    }
                    else {
                        *func = reinterpret_cast<ULONG_PTR>(GetProc(h, byName->Name));
                    }
#else
                    * func = reinterpret_cast<ULONG_PTR>(GetProc(h, byName->Name));
#endif
                }
            }
            ++imp;
        }
    }

    if (pOpt->DataDirectory[IMAGE_DIRECTORY_ENTRY_TLS].Size) {
        auto* tls = reinterpret_cast<IMAGE_TLS_DIRECTORY*>(
            pBase + pOpt->DataDirectory[IMAGE_DIRECTORY_ENTRY_TLS].VirtualAddress);
        auto* cb = reinterpret_cast<PIMAGE_TLS_CALLBACK*>(tls->AddressOfCallBacks);
        for (; cb && *cb; ++cb) {
            (*cb)(pBase, DLL_PROCESS_ATTACH, nullptr);
        }
    }

    bool sehFail = false;
#ifdef _WIN64
    if (pData->SEHSupport) {
        auto ex = pOpt->DataDirectory[IMAGE_DIRECTORY_ENTRY_EXCEPTION];
        if (ex.Size) {
            if (!RtlAdd(reinterpret_cast<IMAGE_RUNTIME_FUNCTION_ENTRY*>(pBase + ex.VirtualAddress),
                ex.Size / sizeof(IMAGE_RUNTIME_FUNCTION_ENTRY), reinterpret_cast<DWORD64>(pBase))) {
                sehFail = true;
            }
        }
        if (pData->pCxxThrowStub) {
            auto* sb = static_cast<BYTE*>(pData->pCxxThrowStub);
            RtlAdd(reinterpret_cast<IMAGE_RUNTIME_FUNCTION_ENTRY*>(sb + 0x30), 1, reinterpret_cast<DWORD64>(sb));
        }
    }
#endif

    DllMain(reinterpret_cast<HINSTANCE>(pBase), pData->fdwReasonParam, pData->reservedParam);
    pData->hMod = sehFail ? reinterpret_cast<HINSTANCE>(0x505050) : reinterpret_cast<HINSTANCE>(pBase);
}
#pragma runtime_checks("", restore)
#pragma optimize("", on)

bool ManualMapDll(HANDLE hProc, BYTE* pSrcData, SIZE_T /*FileSize*/) {
    auto* pDos = reinterpret_cast<IMAGE_DOS_HEADER*>(pSrcData);
    if (pDos->e_magic != 0x5A4D) {
        return false;
    }

    auto* pNt = reinterpret_cast<IMAGE_NT_HEADERS*>(pSrcData + pDos->e_lfanew);
    IMAGE_OPTIONAL_HEADER* pOpt = &pNt->OptionalHeader;
    IMAGE_FILE_HEADER* pFile = &pNt->FileHeader;
    if (pFile->Machine != CURRENT_ARCH) {
        return false;
    }

    auto* pTargetBase = static_cast<BYTE*>(
        VirtualAllocEx(hProc, nullptr, pOpt->SizeOfImage, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE));
    if (!pTargetBase) {
        return false;
    }

    DWORD oldProt = 0;
    VirtualProtectEx(hProc, pTargetBase, pOpt->SizeOfImage, PAGE_EXECUTE_READWRITE, &oldProt);

    MANUAL_MAPPING_DATA data{};
    data.pLoadLibraryA = reinterpret_cast<f_LoadLibraryA>(LoadLibraryA);
    data.pGetProcAddress = reinterpret_cast<f_GetProcAddress>(GetProcAddress);
    data.pbase = pTargetBase;
    data.fdwReasonParam = DLL_PROCESS_ATTACH;
    data.reservedParam = nullptr;
    data.SEHSupport = true;

#ifdef _WIN64
    data.pRtlAddFunctionTable = reinterpret_cast<f_RtlAddFunctionTable>(RtlAddFunctionTable);
    data.pCxxThrowStub = nullptr;
    if (data.SEHSupport) {
        HMODULE hK32 = GetModuleHandleW(L"kernel32.dll");
        FARPROC pRaise = hK32 ? GetProcAddress(hK32, "RaiseException") : nullptr;
        if (pRaise) {
            void* stub = VirtualAllocEx(hProc, nullptr, 0x1000, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
            if (stub) {
                BYTE code[] = {
                    0x48, 0x89, 0x4C, 0x24, 0x08, 0x48, 0x89, 0x54, 0x24, 0x10,
                    0x48, 0x83, 0xEC, 0x20, 0x48, 0x8B, 0x4C, 0x24, 0x28,
                    0x48, 0x8B, 0x54, 0x24, 0x30, 0xBA, 0x04, 0x00, 0x00, 0x00,
                    0x49, 0xB8, 0, 0, 0, 0, 0, 0, 0, 0
                };
                WriteProcessMemory(hProc, stub, code, sizeof(code), nullptr);
                auto base = reinterpret_cast<DWORD64>(pTargetBase);
                WriteProcessMemory(hProc, static_cast<BYTE*>(stub) + 0x1B, &base, sizeof(base), nullptr);
                data.pCxxThrowStub = stub;
            }
        }
    }
#endif

    if (!WriteProcessMemory(hProc, pTargetBase, pSrcData, pOpt->SizeOfHeaders, nullptr)) {
        VirtualFreeEx(hProc, pTargetBase, 0, MEM_RELEASE);
        return false;
    }

    IMAGE_SECTION_HEADER* sec = IMAGE_FIRST_SECTION(pNt);
    for (UINT i = 0; i < pFile->NumberOfSections; ++i, ++sec) {
        if (sec->SizeOfRawData) {
            if (!WriteProcessMemory(hProc, pTargetBase + sec->VirtualAddress,
                pSrcData + sec->PointerToRawData, sec->SizeOfRawData, nullptr)) {
                VirtualFreeEx(hProc, pTargetBase, 0, MEM_RELEASE);
                return false;
            }
        }
    }

    LPVOID mapAlloc = VirtualAllocEx(hProc, nullptr, sizeof(MANUAL_MAPPING_DATA),
        MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
    if (!mapAlloc) {
        VirtualFreeEx(hProc, pTargetBase, 0, MEM_RELEASE);
        return false;
    }

    std::vector<BYTE> zeroData(sizeof(MANUAL_MAPPING_DATA), 0);
    WriteProcessMemory(hProc, mapAlloc, zeroData.data(), zeroData.size(), nullptr);

    if (!WriteProcessMemory(hProc, mapAlloc, &data, sizeof(data), nullptr)) {
        VirtualFreeEx(hProc, pTargetBase, 0, MEM_RELEASE);
        VirtualFreeEx(hProc, mapAlloc, 0, MEM_RELEASE);
        return false;
    }

    void* pShell = VirtualAllocEx(hProc, nullptr, 0x1000, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
    if (!pShell) {
        VirtualFreeEx(hProc, pTargetBase, 0, MEM_RELEASE);
        VirtualFreeEx(hProc, mapAlloc, 0, MEM_RELEASE);
        return false;
    }

    if (!WriteProcessMemory(hProc, pShell, reinterpret_cast<const void*>(&Shellcode), 0x1000, nullptr)) {
        VirtualFreeEx(hProc, pTargetBase, 0, MEM_RELEASE);
        VirtualFreeEx(hProc, mapAlloc, 0, MEM_RELEASE);
        VirtualFreeEx(hProc, pShell, 0, MEM_RELEASE);
        return false;
    }

    HANDLE hThread = CreateRemoteThread(hProc, nullptr, 0,
        reinterpret_cast<LPTHREAD_START_ROUTINE>(pShell), mapAlloc, 0, nullptr);
    if (!hThread) {
        VirtualFreeEx(hProc, pTargetBase, 0, MEM_RELEASE);
        VirtualFreeEx(hProc, mapAlloc, 0, MEM_RELEASE);
        VirtualFreeEx(hProc, pShell, 0, MEM_RELEASE);
        return false;
    }
    CloseHandle(hThread);

    HINSTANCE hCheck = nullptr;
    DWORD t0 = GetTickCount();
    while (!hCheck) {
        DWORD ec = 0;
        GetExitCodeProcess(hProc, &ec);
        if (ec != STILL_ACTIVE) {
            return false;
        }
        if (GetTickCount() - t0 > 15000) {
            return false;
        }

        MANUAL_MAPPING_DATA chk{};
        ReadProcessMemory(hProc, mapAlloc, &chk, sizeof(chk), nullptr);
        hCheck = chk.hMod;
        if (hCheck == reinterpret_cast<HINSTANCE>(0x404040)) {
            return false;
        }
        Sleep(10);
    }

    constexpr SIZE_T kZeroSize = 4 * 1024 * 1024;
    auto* zero = static_cast<BYTE*>(malloc(kZeroSize));
    if (zero) {
        memset(zero, 0, kZeroSize);
        WriteProcessMemory(hProc, pTargetBase, zero, 0x1000, nullptr);

        sec = IMAGE_FIRST_SECTION(pNt);
        for (UINT i = 0; i < pFile->NumberOfSections; ++i, ++sec) {
            if (!sec->Misc.VirtualSize) {
                continue;
            }
            const char* n = reinterpret_cast<const char*>(sec->Name);
            if (strcmp(n, ".rsrc") == 0 || strcmp(n, ".reloc") == 0) {
                WriteProcessMemory(hProc, pTargetBase + sec->VirtualAddress,
                    zero, std::min<SIZE_T>(sec->Misc.VirtualSize, kZeroSize), nullptr);
            }
        }

        DWORD old = 0;
        sec = IMAGE_FIRST_SECTION(pNt);
        for (UINT i = 0; i < pFile->NumberOfSections; ++i, ++sec) {
            if (!sec->Misc.VirtualSize) {
                continue;
            }
            DWORD np = PAGE_READONLY;
            if (sec->Characteristics & IMAGE_SCN_MEM_WRITE) {
                np = PAGE_READWRITE;
            }
            else if (sec->Characteristics & IMAGE_SCN_MEM_EXECUTE) {
                np = PAGE_EXECUTE_READ;
            }
            VirtualProtectEx(hProc, pTargetBase + sec->VirtualAddress,
                sec->Misc.VirtualSize, np, &old);
        }

        VirtualProtectEx(hProc, pTargetBase,
            IMAGE_FIRST_SECTION(pNt)->VirtualAddress, PAGE_READONLY, &old);

        WriteProcessMemory(hProc, pShell, zero, 0x1000, nullptr);
        free(zero);
    }

    VirtualFreeEx(hProc, pShell, 0, MEM_RELEASE);
    VirtualFreeEx(hProc, mapAlloc, 0, MEM_RELEASE);
    return true;
}

namespace {
    constexpr wchar_t kWindowClass[] = L"MaotawDX11Launcher";
    constexpr UINT_PTR kProcessTimer = 2;
    constexpr UINT kWmLogMessage = WM_APP + 1;
    constexpr int kWindowWidth = 400;
    constexpr int kWindowHeight = 170;
    constexpr wchar_t kTargetDllName[] = L"INTERNAL_DLL.dll";

    constexpr D2D1_COLOR_F C(float r, float g, float b, float a = 1.0f) {
        return D2D1_COLOR_F{ r / 255.0f, g / 255.0f, b / 255.0f, a };
    }

    HWND g_window = nullptr;

    ID3D11Device* g_d3dDevice = nullptr;
    ID3D11DeviceContext* g_d3dContext = nullptr;
    IDXGISwapChain* g_swapChain = nullptr;
    ID2D1Factory* g_d2dFactory = nullptr;
    ID2D1RenderTarget* g_d2dTarget = nullptr;
    IDWriteFactory* g_writeFactory = nullptr;

    ID2D1SolidColorBrush* g_bgBrush = nullptr;
    ID2D1SolidColorBrush* g_panelBrush = nullptr;
    ID2D1SolidColorBrush* g_panelBorderBrush = nullptr;
    ID2D1SolidColorBrush* g_borderBrush = nullptr;
    ID2D1SolidColorBrush* g_textBrush = nullptr;
    ID2D1SolidColorBrush* g_mutedBrush = nullptr;
    ID2D1SolidColorBrush* g_accentBrush = nullptr;
    ID2D1SolidColorBrush* g_accentHoverBrush = nullptr;
    ID2D1SolidColorBrush* g_buttonBrush = nullptr;
    ID2D1SolidColorBrush* g_buttonHoverBrush = nullptr;
    ID2D1SolidColorBrush* g_buttonPressedBrush = nullptr;
    ID2D1SolidColorBrush* g_greenBrush = nullptr;
    ID2D1SolidColorBrush* g_iconDarkBrush = nullptr;

    IDWriteTextFormat* g_brandFormat = nullptr;
    IDWriteTextFormat* g_bodyFormat = nullptr;
    IDWriteTextFormat* g_buttonFormat = nullptr;

    bool g_graphicsReady = false;
    bool g_cs2Running = false;
    bool g_launchHover = false;
    bool g_launchPressed = false;
    bool g_closeHover = false;
    bool g_minimizeHover = false;
    DWORD g_cs2Pid = 0;
    std::atomic<bool> g_injectInProgress{ false };
    wchar_t g_graphicsError[256] = L"Unknown graphics initialization error.";

    struct LogEntry {
        SYSTEMTIME time{};
        wchar_t message[72]{};
        bool highlighted = false;
    };

    struct QueuedLog {
        wchar_t message[72]{};
        bool highlighted = false;
    };

    std::mutex g_logMutex;
    std::vector<QueuedLog> g_pendingLogs;
    LogEntry g_logs[3]{};
    int g_logCount = 0;

    const D2D1_RECT_F kLaunchButton = D2D1::RectF(278.0f, 138.0f, 382.0f, 162.0f);
    const D2D1_RECT_F kMinimizeButton = D2D1::RectF(336.0f, 0.0f, 368.0f, 34.0f);
    const D2D1_RECT_F kCloseButton = D2D1::RectF(368.0f, 0.0f, 400.0f, 34.0f);

    template <typename T>
    void SafeRelease(T*& object) {
        if (object) {
            object->Release();
            object = nullptr;
        }
    }

    bool PointInRectF(POINT point, const D2D1_RECT_F& rect) {
        return point.x >= rect.left && point.x < rect.right &&
            point.y >= rect.top && point.y < rect.bottom;
    }

    std::wstring GetSameFolderDllPath() {
        wchar_t exePath[MAX_PATH]{};
        const DWORD len = GetModuleFileNameW(nullptr, exePath, MAX_PATH);
        if (len == 0 || len >= MAX_PATH) {
            return std::wstring(kTargetDllName);
        }

        std::wstring dir(exePath, len);
        const size_t slash = dir.find_last_of(L"\\/");
        if (slash != std::wstring::npos) {
            dir.resize(slash + 1);
            return dir + kTargetDllName;
        }
        return std::wstring(kTargetDllName);
    }

    void AddLog(const wchar_t* message, bool highlighted = false) {
        for (int i = 2; i > 0; --i) {
            g_logs[i] = g_logs[i - 1];
        }

        LogEntry& entry = g_logs[0];
        GetLocalTime(&entry.time);
        wcsncpy_s(entry.message, _countof(entry.message), message, _TRUNCATE);
        entry.highlighted = highlighted;
        if (g_logCount < 3) {
            ++g_logCount;
        }
    }

    void PostLogAsync(const wchar_t* message, bool highlighted = false) {
        {
            std::lock_guard<std::mutex> lock(g_logMutex);
            QueuedLog item{};
            wcsncpy_s(item.message, _countof(item.message), message, _TRUNCATE);
            item.highlighted = highlighted;
            g_pendingLogs.push_back(item);
        }
        if (g_window) {
            PostMessageW(g_window, kWmLogMessage, 0, 0);
        }
    }

    void FlushPendingLogs() {
        std::vector<QueuedLog> batch;
        {
            std::lock_guard<std::mutex> lock(g_logMutex);
            batch.swap(g_pendingLogs);
        }
        for (const auto& item : batch) {
            AddLog(item.message, item.highlighted);
        }
    }

    DWORD FindCS2Pid() {
        HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
        if (snapshot == INVALID_HANDLE_VALUE) {
            return 0;
        }

        DWORD foundPid = 0;
        PROCESSENTRY32W entry{};
        entry.dwSize = sizeof(entry);
        if (Process32FirstW(snapshot, &entry)) {
            do {
                if (_wcsicmp(entry.szExeFile, L"cs2.exe") == 0) {
                    foundPid = entry.th32ProcessID;
                    break;
                }
            } while (Process32NextW(snapshot, &entry));
        }

        CloseHandle(snapshot);
        return foundPid;
    }

    bool IsCS2Running() {
        g_cs2Pid = FindCS2Pid();
        return g_cs2Pid != 0;
    }

    bool ReadDllBytes(const std::wstring& path, std::vector<BYTE>& outBytes) {
        std::ifstream file(path, std::ios::binary | std::ios::ate);
        if (!file.is_open()) {
            return false;
        }

        const auto size = file.tellg();
        if (size <= 0) {
            return false;
        }

        outBytes.resize(static_cast<size_t>(size));
        file.seekg(0, std::ios::beg);
        file.read(reinterpret_cast<char*>(outBytes.data()), size);
        return static_cast<bool>(file);
    }

    bool InjectIntoPid(DWORD pid) {
        Sleep(3000);
        const std::wstring dllPath = GetSameFolderDllPath();
        std::vector<BYTE> dllBytes;
        if (!ReadDllBytes(dllPath, dllBytes)) {
            PostLogAsync(L"maotaw.dll not found in folder", false);
            return false;
        }

        HANDLE hProc = OpenProcess(PROCESS_ALL_ACCESS, FALSE, pid);
        if (!hProc) {
            PostLogAsync(L"OpenProcess failed (run as Admin)", false);
            return false;
        }

        BOOL bWow64Proc = FALSE;
        BOOL bWow64Self = FALSE;
        IsWow64Process(hProc, &bWow64Proc);
        IsWow64Process(GetCurrentProcess(), &bWow64Self);
        if (bWow64Proc != bWow64Self) {
            CloseHandle(hProc);
            PostLogAsync(L"Arch mismatch (x86/x64)", false);
            return false;
        }

        const bool mapped = ManualMapDll(hProc, dllBytes.data(), static_cast<SIZE_T>(dllBytes.size()));
        CloseHandle(hProc);

        if (mapped) {
            PostLogAsync(L"maotaw.dll manual mapped", true);
            return true;
        }

        PostLogAsync(L"maotaw.dll map failed", false);
        return false;
    }

    void LaunchAndInjectWorker(bool alreadyRunning, DWORD existingPid) {
        g_injectInProgress.store(true);

        DWORD targetPid = existingPid;
        if (!alreadyRunning) {
            const DWORD startTick = GetTickCount();
            while (targetPid == 0 && (GetTickCount() - startTick) < 30000) {
                Sleep(250);
                targetPid = FindCS2Pid();
            }

            if (targetPid == 0) {
                PostLogAsync(L"Timed out waiting for CS2", false);
                g_injectInProgress.store(false);
                return;
            }

            PostLogAsync(L"CS2 started, preparing map...", true);
            Sleep(1500);
        }

        InjectIntoPid(targetPid);
        g_injectInProgress.store(false);
    }

    HRESULT CreateTextFormat(float size, DWRITE_FONT_WEIGHT weight, IDWriteTextFormat** outFormat) {
        HRESULT hr = g_writeFactory->CreateTextFormat(
            L"Segoe UI", nullptr, weight, DWRITE_FONT_STYLE_NORMAL,
            DWRITE_FONT_STRETCH_NORMAL, size, L"en-us", outFormat);
        if (SUCCEEDED(hr)) {
            (*outFormat)->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
            (*outFormat)->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
        }
        return hr;
    }

    void ReleaseD2DResources() {
        SafeRelease(g_bgBrush);
        SafeRelease(g_panelBrush);
        SafeRelease(g_panelBorderBrush);
        SafeRelease(g_borderBrush);
        SafeRelease(g_textBrush);
        SafeRelease(g_mutedBrush);
        SafeRelease(g_accentBrush);
        SafeRelease(g_accentHoverBrush);
        SafeRelease(g_buttonBrush);
        SafeRelease(g_buttonHoverBrush);
        SafeRelease(g_buttonPressedBrush);
        SafeRelease(g_greenBrush);
        SafeRelease(g_iconDarkBrush);
        SafeRelease(g_d2dTarget);
    }

    void RecordGraphicsError(const wchar_t* stage, HRESULT hr) {
        swprintf_s(g_graphicsError, sizeof(g_graphicsError) / sizeof(g_graphicsError[0]),
            L"%s failed (HRESULT 0x%08lX).", stage,
            static_cast<unsigned long>(hr));
    }

    bool CreateD2DTarget() {
        IDXGISurface* surface = nullptr;
        HRESULT hr = g_swapChain->GetBuffer(0, __uuidof(IDXGISurface),
            reinterpret_cast<void**>(&surface));
        if (FAILED(hr)) {
            RecordGraphicsError(L"IDXGISwapChain::GetBuffer", hr);
            return false;
        }

        const auto properties = D2D1::RenderTargetProperties(
            D2D1_RENDER_TARGET_TYPE_DEFAULT,
            D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_IGNORE));
        hr = g_d2dFactory->CreateDxgiSurfaceRenderTarget(surface, &properties, &g_d2dTarget);
        surface->Release();
        if (FAILED(hr)) {
            RecordGraphicsError(L"ID2D1Factory::CreateDxgiSurfaceRenderTarget", hr);
            return false;
        }

        g_d2dTarget->SetTextAntialiasMode(D2D1_TEXT_ANTIALIAS_MODE_CLEARTYPE);

        const auto makeBrush = [](D2D1_COLOR_F color,
            ID2D1SolidColorBrush** brush, const wchar_t* name) -> bool {
                const HRESULT brushHr = g_d2dTarget->CreateSolidColorBrush(color, brush);
                if (FAILED(brushHr)) {
                    RecordGraphicsError(name, brushHr);
                }
                return SUCCEEDED(brushHr);
            };

        return makeBrush(C(10, 10, 10), &g_bgBrush, L"Create background brush") &&
            makeBrush(C(17, 17, 17), &g_panelBrush, L"Create panel brush") &&
            makeBrush(C(35, 35, 35), &g_panelBorderBrush, L"Create panel border brush") &&
            makeBrush(C(43, 43, 43), &g_borderBrush, L"Create border brush") &&
            makeBrush(C(238, 238, 238), &g_textBrush, L"Create text brush") &&
            makeBrush(C(126, 126, 126), &g_mutedBrush, L"Create muted text brush") &&
            makeBrush(C(190, 43, 61), &g_accentBrush, L"Create accent brush") &&
            makeBrush(C(218, 53, 72), &g_accentHoverBrush, L"Create hover brush") &&
            makeBrush(C(200, 201, 208), &g_buttonBrush, L"Create button brush") &&
            makeBrush(C(220, 221, 228), &g_buttonHoverBrush, L"Create button hover brush") &&
            makeBrush(C(174, 175, 182), &g_buttonPressedBrush, L"Create button pressed brush") &&
            makeBrush(C(82, 190, 124), &g_greenBrush, L"Create status brush") &&
            makeBrush(C(10, 10, 10), &g_iconDarkBrush, L"Create icon brush");
    }

    bool CreateGraphics(HWND window) {
        DXGI_SWAP_CHAIN_DESC swapDesc{};
        swapDesc.BufferDesc.Width = kWindowWidth;
        swapDesc.BufferDesc.Height = kWindowHeight;
        swapDesc.BufferDesc.RefreshRate.Numerator = 60;
        swapDesc.BufferDesc.RefreshRate.Denominator = 1;
        swapDesc.BufferDesc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        swapDesc.SampleDesc.Count = 1;
        swapDesc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
        swapDesc.BufferCount = 2;
        swapDesc.OutputWindow = window;
        swapDesc.Windowed = TRUE;
        swapDesc.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;

        const D3D_FEATURE_LEVEL levels[] = {
            D3D_FEATURE_LEVEL_11_0,
            D3D_FEATURE_LEVEL_10_1
        };
        const UINT deviceFlags = D3D11_CREATE_DEVICE_BGRA_SUPPORT;
        D3D_FEATURE_LEVEL selectedLevel{};

        HRESULT hardwareHr = D3D11CreateDeviceAndSwapChain(
            nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, deviceFlags,
            levels, ARRAYSIZE(levels), D3D11_SDK_VERSION,
            &swapDesc, &g_swapChain, &g_d3dDevice, &selectedLevel, &g_d3dContext);

        if (FAILED(hardwareHr)) {
            SafeRelease(g_swapChain);
            SafeRelease(g_d3dContext);
            SafeRelease(g_d3dDevice);
            HRESULT warpHr = D3D11CreateDeviceAndSwapChain(
                nullptr, D3D_DRIVER_TYPE_WARP, nullptr, deviceFlags,
                levels, ARRAYSIZE(levels), D3D11_SDK_VERSION,
                &swapDesc, &g_swapChain, &g_d3dDevice, &selectedLevel, &g_d3dContext);
            if (FAILED(warpHr)) {
                swprintf_s(g_graphicsError, sizeof(g_graphicsError) / sizeof(g_graphicsError[0]),
                    L"D3D11 device creation failed. Hardware=0x%08lX, WARP=0x%08lX.",
                    static_cast<unsigned long>(hardwareHr),
                    static_cast<unsigned long>(warpHr));
                return false;
            }
        }

        HRESULT hr = D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, &g_d2dFactory);
        if (FAILED(hr)) {
            RecordGraphicsError(L"D2D1CreateFactory", hr);
            return false;
        }

        hr = DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory),
            reinterpret_cast<IUnknown**>(&g_writeFactory));
        if (FAILED(hr)) {
            RecordGraphicsError(L"DWriteCreateFactory", hr);
            return false;
        }

        const auto makeFormat = [](float size, DWRITE_FONT_WEIGHT weight,
            IDWriteTextFormat** format, const wchar_t* name) -> bool {
                const HRESULT formatHr = CreateTextFormat(size, weight, format);
                if (FAILED(formatHr)) {
                    RecordGraphicsError(name, formatHr);
                }
                return SUCCEEDED(formatHr);
            };

        if (!makeFormat(13.0f, DWRITE_FONT_WEIGHT_SEMI_BOLD, &g_brandFormat, L"Create brand font") ||
            !makeFormat(10.0f, DWRITE_FONT_WEIGHT_NORMAL, &g_bodyFormat, L"Create body font") ||
            !makeFormat(10.0f, DWRITE_FONT_WEIGHT_BOLD, &g_buttonFormat, L"Create button font")) {
            return false;
        }
        g_buttonFormat->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);

        return CreateD2DTarget();
    }

    void ResizeGraphics(UINT width, UINT height) {
        if (!g_swapChain || width == 0 || height == 0) {
            return;
        }

        ReleaseD2DResources();
        if (SUCCEEDED(g_swapChain->ResizeBuffers(0, width, height, DXGI_FORMAT_UNKNOWN, 0))) {
            CreateD2DTarget();
        }
    }

    void DrawText(const wchar_t* text, IDWriteTextFormat* format,
        const D2D1_RECT_F& rect, ID2D1Brush* brush) {
        if (!text || !format || !brush) {
            return;
        }
        g_d2dTarget->DrawText(text, static_cast<UINT32>(wcslen(text)),
            format, rect, brush, D2D1_DRAW_TEXT_OPTIONS_CLIP,
            DWRITE_MEASURING_MODE_NATURAL);
    }

    void DrawUI() {
        if (!g_d2dTarget) {
            return;
        }

        g_d2dTarget->BeginDraw();
        g_d2dTarget->SetAntialiasMode(D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
        g_d2dTarget->SetTextAntialiasMode(D2D1_TEXT_ANTIALIAS_MODE_CLEARTYPE);
        g_d2dTarget->Clear(C(10, 10, 10));

        g_d2dTarget->DrawRectangle(D2D1::RectF(0.5f, 0.5f, 399.5f, 169.5f), g_borderBrush, 1.0f);
        g_d2dTarget->DrawLine(D2D1::Point2F(1.0f, 34.5f), D2D1::Point2F(399.0f, 34.5f), g_borderBrush, 1.0f);

        const auto logoRect = D2D1::RectF(14.0f, 7.0f, 35.0f, 28.0f);
        g_d2dTarget->FillRectangle(logoRect, g_accentBrush);
        g_d2dTarget->DrawLine(D2D1::Point2F(18.5f, 22.5f), D2D1::Point2F(18.5f, 12.0f), g_textBrush, 1.8f);
        g_d2dTarget->DrawLine(D2D1::Point2F(18.5f, 12.0f), D2D1::Point2F(24.5f, 18.0f), g_textBrush, 1.8f);
        g_d2dTarget->DrawLine(D2D1::Point2F(24.5f, 18.0f), D2D1::Point2F(30.5f, 12.0f), g_textBrush, 1.8f);
        g_d2dTarget->DrawLine(D2D1::Point2F(30.5f, 12.0f), D2D1::Point2F(30.5f, 22.5f), g_textBrush, 1.8f);
        g_d2dTarget->FillEllipse(D2D1::Ellipse(D2D1::Point2F(24.5f, 23.0f), 1.2f, 1.2f), g_textBrush);
        DrawText(L"MAOTAW", g_brandFormat, D2D1::RectF(44.0f, 5.0f, 150.0f, 29.0f), g_textBrush);

        if (g_minimizeHover) {
            g_d2dTarget->FillRectangle(kMinimizeButton, g_panelBrush);
        }
        if (g_closeHover) {
            g_d2dTarget->FillRectangle(kCloseButton, g_accentHoverBrush);
        }
        g_d2dTarget->DrawLine(D2D1::Point2F(347.0f, 17.0f), D2D1::Point2F(357.0f, 17.0f), g_mutedBrush, 1.2f);
        g_d2dTarget->DrawLine(D2D1::Point2F(379.0f, 11.0f), D2D1::Point2F(389.0f, 21.0f), g_closeHover ? g_textBrush : g_mutedBrush, 1.2f);
        g_d2dTarget->DrawLine(D2D1::Point2F(389.0f, 11.0f), D2D1::Point2F(379.0f, 21.0f), g_closeHover ? g_textBrush : g_mutedBrush, 1.2f);

        const auto logCard = D2D1::RectF(18.0f, 44.0f, 382.0f, 132.0f);
        //g_d2dTarget->FillRectangle(logCard, g_panelBrush);
        //g_d2dTarget->DrawRectangle(logCard, g_panelBorderBrush, 1.0f);
        DrawText(L"ACTIVITY", g_bodyFormat,
            D2D1::RectF(29.0f, 47.0f, 112.0f, 65.0f), g_textBrush);
        g_d2dTarget->FillRectangle(D2D1::RectF(350.0f, 55.0f, 354.0f, 59.0f), g_accentBrush);
        g_d2dTarget->FillRectangle(D2D1::RectF(358.0f, 52.0f, 362.0f, 59.0f), g_accentBrush);
        g_d2dTarget->FillRectangle(D2D1::RectF(366.0f, 49.0f, 370.0f, 59.0f), g_accentBrush);
        g_d2dTarget->DrawLine(D2D1::Point2F(29.0f, 67.0f), D2D1::Point2F(371.0f, 67.0f), g_borderBrush, 1.0f);

        for (int row = 0; row < g_logCount; ++row) {
            const LogEntry& entry = g_logs[g_logCount - row - 1];
            wchar_t timeText[8]{};
            swprintf_s(timeText, L"%02u:%02u",
                static_cast<unsigned>(entry.time.wHour),
                static_cast<unsigned>(entry.time.wMinute));
            const float top = 70.0f + static_cast<float>(row) * 18.0f;
            const float centerY = top + 6.0f;
            ID2D1Brush* markerBrush = entry.highlighted
                ? static_cast<ID2D1Brush*>(g_greenBrush)
                : static_cast<ID2D1Brush*>(g_accentBrush);
            ID2D1Brush* messageBrush = entry.highlighted
                ? static_cast<ID2D1Brush*>(g_greenBrush)
                : static_cast<ID2D1Brush*>(g_textBrush);

            g_d2dTarget->FillEllipse(
                D2D1::Ellipse(D2D1::Point2F(32.0f, centerY), 2.0f, 2.0f), markerBrush);
            DrawText(timeText, g_bodyFormat,
                D2D1::RectF(42.0f, top, 80.0f, top + 13.0f), g_mutedBrush);
            DrawText(entry.message, g_bodyFormat,
                D2D1::RectF(92.0f, top, 369.0f, top + 13.0f), messageBrush);
        }

        g_d2dTarget->FillEllipse(D2D1::Ellipse(D2D1::Point2F(25.0f, 150.0f), 2.5f, 2.5f), g_greenBrush);
        DrawText(g_injectInProgress.load() ? L"MAPPING..." : (g_cs2Running ? L"CS2 RUNNING" : L"STEAM READY"),
            g_bodyFormat, D2D1::RectF(34.0f, 138.0f, 180.0f, 162.0f), g_mutedBrush);

        ID2D1Brush* buttonBrush = g_launchPressed
            ? static_cast<ID2D1Brush*>(g_buttonPressedBrush)
            : (g_launchHover ? static_cast<ID2D1Brush*>(g_buttonHoverBrush)
                : static_cast<ID2D1Brush*>(g_buttonBrush));
        g_d2dTarget->FillRectangle(kLaunchButton, buttonBrush);
        DrawText(L"LAUNCH", g_buttonFormat,
            D2D1::RectF(288.0f, 138.0f, 361.0f, 162.0f), g_iconDarkBrush);
        g_d2dTarget->DrawLine(D2D1::Point2F(367.0f, 146.0f), D2D1::Point2F(371.0f, 150.0f), g_iconDarkBrush, 1.2f);
        g_d2dTarget->DrawLine(D2D1::Point2F(371.0f, 150.0f), D2D1::Point2F(367.0f, 154.0f), g_iconDarkBrush, 1.2f);

        const HRESULT drawResult = g_d2dTarget->EndDraw();
        if (drawResult == D2DERR_RECREATE_TARGET) {
            ReleaseD2DResources();
            CreateD2DTarget();
            return;
        }
        if (g_swapChain) {
            g_swapChain->Present(1, 0);
        }
    }

    void ReleaseGraphics() {
        ReleaseD2DResources();
        SafeRelease(g_brandFormat);
        SafeRelease(g_bodyFormat);
        SafeRelease(g_buttonFormat);
        SafeRelease(g_writeFactory);
        SafeRelease(g_d2dFactory);
        SafeRelease(g_swapChain);
        SafeRelease(g_d3dContext);
        SafeRelease(g_d3dDevice);
        g_graphicsReady = false;
    }

    bool LaunchCS2FromSteam(HWND owner) {
        if (g_injectInProgress.load()) {
            AddLog(L"Injection already in progress");
            return false;
        }

        if (IsCS2Running()) {
            g_cs2Running = true;
            AddLog(L"CS2 running, mapping DLL...", true);
            std::thread(LaunchAndInjectWorker, true, g_cs2Pid).detach();
            return true;
        }

        HINSTANCE result = ShellExecuteW(owner, L"open", L"steam://run/730",
            nullptr, nullptr, SW_SHOWNORMAL);
        const INT_PTR code = reinterpret_cast<INT_PTR>(result);
        if (code <= 32) {
            AddLog(L"Steam launch request failed");
            wchar_t errorText[128]{};
            swprintf_s(errorText, L"Could not open the Steam launch link.\nShellExecute code: %lld",
                static_cast<long long>(code));
            MessageBoxW(owner, errorText, L"Steam launch error", MB_OK | MB_ICONERROR);
            return false;
        }

        AddLog(L"Launch sent, waiting for CS2", true);
        std::thread(LaunchAndInjectWorker, false, 0).detach();
        return true;
    }

    LRESULT CALLBACK WindowProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam) {
        switch (message) {
        case WM_CREATE:
            g_graphicsReady = CreateGraphics(window);
            if (!g_graphicsReady) {
                const std::wstring messageText = std::wstring(L"Graphics initialization failed.\n\n") + g_graphicsError;
                ReleaseGraphics();
                MessageBoxW(window, messageText.c_str(), L"Maotaw - Graphics Error", MB_ICONERROR | MB_OK);
                return -1;
            }
            IsCS2Running();
            g_cs2Running = (g_cs2Pid != 0);
            AddLog(L"Launcher initialized", true);
            AddLog(L"Steam launch ready");
            if (g_cs2Running) {
                AddLog(L"CS2 process detected", true);
            }
            SetTimer(window, kProcessTimer, 1000, nullptr);
            DrawUI();
            return 0;

        case kWmLogMessage:
            FlushPendingLogs();
            DrawUI();
            return 0;

        case WM_NCHITTEST: {
            POINT point{ GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };
            ScreenToClient(window, &point);
            if (point.y >= 0 && point.y < 34 && point.x < 336) {
                return HTCAPTION;
            }
            return HTCLIENT;
        }

        case WM_MOUSEMOVE: {
            POINT point{ GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };
            const bool hover = PointInRectF(point, kLaunchButton);
            const bool closeHover = PointInRectF(point, kCloseButton);
            const bool minimizeHover = PointInRectF(point, kMinimizeButton);
            if (hover != g_launchHover || closeHover != g_closeHover || minimizeHover != g_minimizeHover) {
                g_launchHover = hover;
                g_closeHover = closeHover;
                g_minimizeHover = minimizeHover;
                DrawUI();
            }
            TRACKMOUSEEVENT track{ sizeof(track), TME_LEAVE, window, 0 };
            TrackMouseEvent(&track);
            return 0;
        }

        case WM_MOUSELEAVE:
            g_launchHover = false;
            g_closeHover = false;
            g_minimizeHover = false;
            DrawUI();
            return 0;

        case WM_LBUTTONDOWN: {
            POINT point{ GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };
            if (PointInRectF(point, kLaunchButton)) {
                g_launchPressed = true;
                SetCapture(window);
                DrawUI();
                return 0;
            }
            return 0;
        }

        case WM_LBUTTONUP: {
            POINT point{ GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };
            if (g_launchPressed) {
                g_launchPressed = false;
                if (GetCapture() == window) {
                    ReleaseCapture();
                }
                if (PointInRectF(point, kLaunchButton)) {
                    LaunchCS2FromSteam(window);
                }
                DrawUI();
                return 0;
            }
            if (PointInRectF(point, kCloseButton)) {
                DestroyWindow(window);
                return 0;
            }
            if (PointInRectF(point, kMinimizeButton)) {
                ShowWindow(window, SW_MINIMIZE);
                return 0;
            }
            return 0;
        }

        case WM_CAPTURECHANGED:
            if (g_launchPressed) {
                g_launchPressed = false;
                DrawUI();
            }
            return 0;

        case WM_TIMER:
            if (wParam == kProcessTimer) {
                const bool wasRunning = g_cs2Running;
                const DWORD oldPid = g_cs2Pid;
                g_cs2Running = IsCS2Running();
                if (!wasRunning && g_cs2Running) {
                    AddLog(L"CS2 process detected", true);
                }
                else if (wasRunning && !g_cs2Running) {
                    AddLog(L"CS2 process closed");
                }
                else if (g_cs2Running && oldPid != g_cs2Pid) {
                    AddLog(L"CS2 process ID changed", true);
                }
                if (wasRunning != g_cs2Running || oldPid != g_cs2Pid) {
                    DrawUI();
                }
                return 0;
            }
            break;

        case WM_SIZE:
            if (g_graphicsReady && wParam != SIZE_MINIMIZED) {
                ResizeGraphics(LOWORD(lParam), HIWORD(lParam));
                DrawUI();
            }
            return 0;

        case WM_ERASEBKGND:
            return 1;

        case WM_PAINT: {
            PAINTSTRUCT paint{};
            BeginPaint(window, &paint);
            EndPaint(window, &paint);
            DrawUI();
            return 0;
        }

        case WM_DESTROY:
            KillTimer(window, kProcessTimer);
            ReleaseGraphics();
            PostQuitMessage(0);
            return 0;
        }

        return DefWindowProcW(window, message, wParam, lParam);
    }
}

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int showCommand) {
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);

    WNDCLASSEXW windowClass{};
    windowClass.cbSize = sizeof(windowClass);
    windowClass.lpfnWndProc = WindowProc;
    windowClass.hInstance = instance;
    windowClass.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    windowClass.hbrBackground = nullptr;
    windowClass.lpszClassName = kWindowClass;
    windowClass.hIcon = LoadIconW(nullptr, IDI_APPLICATION);
    if (!RegisterClassExW(&windowClass)) {
        return 1;
    }

    const int x = (GetSystemMetrics(SM_CXSCREEN) - kWindowWidth) / 2;
    const int y = (GetSystemMetrics(SM_CYSCREEN) - kWindowHeight) / 2;
    g_window = CreateWindowExW(
        WS_EX_APPWINDOW, kWindowClass, L"Maotaw CS2 Utility", WS_POPUP,
        x, y, kWindowWidth, kWindowHeight, nullptr, nullptr, instance, nullptr);
    if (!g_window) {
        return 1;
    }

    ShowWindow(g_window, showCommand);
    UpdateWindow(g_window);

    MSG message{};
    while (GetMessageW(&message, nullptr, 0, 0) > 0) {
        TranslateMessage(&message);
        DispatchMessageW(&message);
    }
    return static_cast<int>(message.wParam);
}