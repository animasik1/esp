#include <windows.h>
#include <tlhelp32.h>
#include <cstdio>
#include <cstdint>
#include <string>

HANDLE g_hProc = nullptr;
DWORD  g_pid   = 0;
uintptr_t g_clientBase = 0;

bool findPidByName(const std::wstring& name, DWORD& pid) {
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return false;
    PROCESSENTRY32W pe{}; pe.dwSize = sizeof(pe);
    bool found = false;
    if (Process32FirstW(snap, &pe)) {
        do {
            if (_wcsicmp(pe.szExeFile, name.c_str()) == 0) { pid = pe.th32ProcessID; found = true; break; }
        } while (Process32NextW(snap, &pe));
    }
    CloseHandle(snap);
    return found;
}

bool attachProcess() {
    if (g_hProc) return true;
    const wchar_t* candidates[] = { L"CMLauncher.exe", L"hl2.exe" };
    DWORD pid = 0;
    for (auto name : candidates) {
        if (findPidByName(name, pid)) break;
    }
    if (!pid) return false;
    g_hProc = OpenProcess(PROCESS_VM_READ | PROCESS_QUERY_INFORMATION, FALSE, pid);
    g_pid = pid;
    return g_hProc != nullptr;
}

uintptr_t getModuleBase(const std::wstring& modName) {
    if (!g_pid) return 0;
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32, g_pid);
    if (snap == INVALID_HANDLE_VALUE) return 0;
    MODULEENTRY32W me{}; me.dwSize = sizeof(me);
    uintptr_t base = 0;
    if (Module32FirstW(snap, &me)) {
        do {
            if (_wcsicmp(me.szModule, modName.c_str()) == 0) { base = reinterpret_cast<uintptr_t>(me.modBaseAddr); break; }
        } while (Module32NextW(snap, &me));
    }
    CloseHandle(snap);
    return base;
}

template <typename T>
bool readMem(uintptr_t addr, T& out) {
    if (!g_hProc || !addr) return false;
    SIZE_T got = 0;
    return ReadProcessMemory(g_hProc, reinterpret_cast<LPCVOID>(addr), &out, sizeof(T), &got) && got == sizeof(T);
}

// Простой оверлей для вывода текста
LRESULT CALLBACK WndProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    if (m == WM_DESTROY) { PostQuitMessage(0); return 0; }
    return DefWindowProcW(h, m, w, l);
}

int WINAPI wWinMain(HINSTANCE inst, HINSTANCE, PWSTR, int) {
    int screenW = GetSystemMetrics(SM_CXSCREEN);
    int screenH = GetSystemMetrics(SM_CYSCREEN);

    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = WndProc;
    wc.hInstance = inst;
    wc.lpszClassName = L"scanOverlay";
    RegisterClassExW(&wc);

    HWND hwnd = CreateWindowExW(WS_EX_LAYERED | WS_EX_TRANSPARENT | WS_EX_TOPMOST,
        wc.lpszClassName, L"scan", WS_POPUP, 0, 0, screenW, screenH, nullptr, nullptr, inst, nullptr);
    SetLayeredWindowAttributes(hwnd, RGB(0,0,0), 255, LWA_COLORKEY);
    ShowWindow(hwnd, SW_SHOW);

    while (!attachProcess()) Sleep(1000);
    while (!(g_clientBase = getModuleBase(L"client.dll"))) Sleep(1000);

    HDC screenDC = GetDC(nullptr);
    HDC memDC = CreateCompatibleDC(screenDC);
    HBITMAP bmp = CreateCompatibleBitmap(screenDC, screenW, screenH);
    HBITMAP oldBmp = (HBITMAP)SelectObject(memDC, bmp);

    while (true) {
        // Рисуем черный фон (прозрачный)
        RECT r{0,0,screenW,screenH};
        HBRUSH clear = CreateSolidBrush(RGB(0,0,0));
        FillRect(memDC, &r, clear);
        DeleteObject(clear);

        // Ищем матрицу вида
        uintptr_t viewMatrixAddr = 0;
        IMAGE_DOS_HEADER dos;
        IMAGE_NT_HEADERS nt;
        if (readMem(g_clientBase, dos) && readMem(g_clientBase + dos.e_lfanew, nt)) {
            DWORD size = nt.OptionalHeader.SizeOfImage;
            BYTE* buf = new BYTE[size];
            SIZE_T got = 0;
            if (ReadProcessMemory(g_hProc, (LPCVOID)g_clientBase, buf, size, &got)) {
                for (DWORD i = 0; i < size - 64; i += 4) {
                    float* f = (float*)(buf + i);
                    if (f[0] == 1.0f && f[1] == 0.0f && f[2] == 0.0f && f[3] == 0.0f &&
                        f[4] == 0.0f && f[5] == 1.0f && f[6] == 0.0f && f[7] == 0.0f) {
                        viewMatrixAddr = g_clientBase + i;
                        break;
                    }
                }
            }
            delete[] buf;
        }

        // Выводим адреса
        SetBkMode(memDC, TRANSPARENT);
        SetTextColor(memDC, RGB(255, 255, 0)); // Желтый текст
        char text[256];
        sprintf_s(text, "client.dll Base: 0x%p\nViewMatrix Offset: 0x%p\n(Если ESP не работает, скинь этот скрин)",
            (void*)g_clientBase, (void*)(viewMatrixAddr ? (viewMatrixAddr - g_clientBase) : 0));
        
        TextOutA(memDC, 20, 20, text, (int)strlen(text));

        POINT src{0,0}; SIZE size{screenW, screenH}; POINT dst{0,0};
        BLENDFUNCTION bf{AC_SRC_OVER, 0, 255, 0};
        UpdateLayeredWindow(hwnd, screenDC, &dst, &size, memDC, &src, RGB(0,0,0), &bf, ULW_COLORKEY);

        Sleep(100);
    }

    return 0;
}
