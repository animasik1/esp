#include <windows.h>
#include <tlhelp32.h>
#include <cstdio>
#include <cstdint>
#include <string>

HANDLE g_hProc = nullptr;
DWORD  g_pid   = 0;
uintptr_t g_clientBase = 0;

bool findPidByModule(const std::wstring& modName, DWORD& pid) {
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return false;
    PROCESSENTRY32W pe{}; pe.dwSize = sizeof(pe);
    bool found = false;
    if (Process32FirstW(snap, &pe)) {
        do {
            if (pe.th32ProcessID == 0 || pe.th32ProcessID == 4) continue;
            HANDLE msnap = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32, pe.th32ProcessID);
            if (msnap != INVALID_HANDLE_VALUE) {
                MODULEENTRY32W me{}; me.dwSize = sizeof(me);
                if (Module32FirstW(msnap, &me)) {
                    do {
                        if (_wcsicmp(me.szModule, modName.c_str()) == 0) {
                            pid = pe.th32ProcessID; found = true; break;
                        }
                    } while (Module32NextW(msnap, &me));
                }
                CloseHandle(msnap);
            }
            if (found) break;
        } while (Process32NextW(snap, &pe));
    }
    CloseHandle(snap);
    return found;
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
    
    // Делаем окно полностью прозрачным по ключу цвета (черный = прозрачный)
    SetLayeredWindowAttributes(hwnd, RGB(0,0,0), 255, LWA_COLORKEY);
    ShowWindow(hwnd, SW_SHOW);

    HDC screenDC = GetDC(nullptr);
    HDC memDC = CreateCompatibleDC(screenDC);
    HBITMAP bmp = CreateCompatibleBitmap(screenDC, screenW, screenH);
    HBITMAP oldBmp = (HBITMAP)SelectObject(memDC, bmp);

    // Ждем, пока найдем процесс с client.dll
    bool found = false;
    std::wstring procName = L"???";
    
    while (!found) {
        HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
        if (snap != INVALID_HANDLE_VALUE) {
            PROCESSENTRY32W pe{}; pe.dwSize = sizeof(pe);
            if (Process32FirstW(snap, &pe)) {
                do {
                    if (pe.th32ProcessID == 0 || pe.th32ProcessID == 4) continue;
                    HANDLE msnap = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32, pe.th32ProcessID);
                    if (msnap != INVALID_HANDLE_VALUE) {
                        MODULEENTRY32W me{}; me.dwSize = sizeof(me);
                        if (Module32FirstW(msnap, &me)) {
                            do {
                                if (_wcsicmp(me.szModule, L"client.dll") == 0) {
                                    g_pid = pe.th32ProcessID;
                                    g_hProc = OpenProcess(PROCESS_VM_READ | PROCESS_QUERY_INFORMATION, FALSE, g_pid);
                                    g_clientBase = reinterpret_cast<uintptr_t>(me.modBaseAddr);
                                    procName = pe.szExeFile;
                                    found = true;
                                    break;
                                }
                            } while (Module32NextW(msnap, &me));
                        }
                        CloseHandle(msnap);
                    }
                    if (found) break;
                } while (Process32NextW(snap, &pe));
            }
            CloseHandle(snap);
        }
        Sleep(1000);
    }

    // Ищем ViewMatrix
    uintptr_t viewMatrixOffset = 0;
    if (g_hProc) {
        IMAGE_DOS_HEADER dos;
        IMAGE_NT_HEADERS nt;
        SIZE_T got = 0;
        if (ReadProcessMemory(g_hProc, (LPCVOID)g_clientBase, &dos, sizeof(dos), &got) && dos.e_magic == IMAGE_DOS_SIGNATURE) {
            if (ReadProcessMemory(g_hProc, (LPCVOID)(g_clientBase + dos.e_lfanew), &nt, sizeof(nt), &got) && nt.Signature == IMAGE_NT_SIGNATURE) {
                DWORD size = nt.OptionalHeader.SizeOfImage;
                BYTE* buf = new BYTE[size];
                if (ReadProcessMemory(g_hProc, (LPCVOID)g_clientBase, buf, size, &got)) {
                    for (DWORD i = 0; i < size - 64; i += 4) {
                        float* f = (float*)(buf + i);
                        if (f[0] == 1.0f && f[1] == 0.0f && f[2] == 0.0f && f[3] == 0.0f &&
                            f[4] == 0.0f && f[5] == 1.0f && f[6] == 0.0f && f[7] == 0.0f) {
                            viewMatrixOffset = i;
                            break;
                        }
                    }
                }
                delete[] buf;
            }
        }
    }

    while (true) {
        // Заливаем черным (прозрачным)
        RECT r{0,0,screenW,screenH};
        HBRUSH clear = CreateSolidBrush(RGB(0,0,0));
        FillRect(memDC, &r, clear);
        DeleteObject(clear);

        // Рисуем текст
        SetBkMode(memDC, TRANSPARENT);
        SetTextColor(memDC, RGB(255, 255, 0)); // Желтый
        char text[512];
        sprintf_s(text, 
            "=== СКАНЕР РАБОТАЕТ ===\n"
            "Процесс: %ws\n"
            "client.dll Base: 0x%p\n"
            "ViewMatrix Offset: 0x%X\n"
            "(Сделай скриншот и скинь мне)",
            procName.c_str(), (void*)g_clientBase, (unsigned int)viewMatrixOffset);
        
        TextOutA(memDC, 50, 50, text, (int)strlen(text));

        POINT src{0,0}; SIZE size{screenW, screenH}; POINT dst{0,0};
        BLENDFUNCTION bf{AC_SRC_OVER, 0, 255, 0};
        UpdateLayeredWindow(hwnd, screenDC, &dst, &size, memDC, &src, RGB(0,0,0), &bf, ULW_COLORKEY);

        Sleep(100);
    }

    return 0;
}
