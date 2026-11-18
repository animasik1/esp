#include <windows.h>
#include <tlhelp32.h>
#include <cmath>
#include <cstdio>
#include <cstdint>
#include <string>
#include <vector>

// ================= БАЗОВЫЕ СМЕЩЕНИЯ =================
namespace off {
    // Эти три мы найдем автоматически при запуске
    uintptr_t dwLocalPlayer = 0; 
    uintptr_t dwEntityList  = 0; 
    uintptr_t dwViewMatrix  = 0; 

    // А эти (здоровье, команда и т.д.) в Source-движке везде одинаковые
    constexpr uintptr_t m_iTeamNum      = 0x000000F0; 
    constexpr uintptr_t m_iHealth       = 0x000000FC; 
    constexpr uintptr_t m_lifeState     = 0x000000FF; 
    constexpr uintptr_t m_vecOrigin     = 0x00000134; 
    constexpr uintptr_t entityStride    = 0x00000010; 
    constexpr int       maxEntities     = 64;         
}

HANDLE g_hProc = nullptr;
DWORD  g_pid   = 0;
uintptr_t g_clientBase = 0;
bool g_initialized = false;

// ================= ПОИСК ПРОЦЕССА =================
bool attachProcess() {
    if (g_hProc) return true;
    const wchar_t* candidates[] = { L"CMLauncher.exe", L"hl2.exe", L"cssmod.exe" };
    DWORD pid = 0;
    
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return false;
    PROCESSENTRY32W pe{}; pe.dwSize = sizeof(pe);
    
    if (Process32FirstW(snap, &pe)) {
        do {
            for (auto name : candidates) {
                if (_wcsicmp(pe.szExeFile, name) == 0) { pid = pe.th32ProcessID; break; }
            }
            if (pid) break;
        } while (Process32NextW(snap, &pe));
    }
    CloseHandle(snap);
    
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
bool readMemRaw(uintptr_t addr, void* buf, size_t sz) {
    if (!g_hProc || !addr) return false;
    SIZE_T got = 0;
    return ReadProcessMemory(g_hProc, reinterpret_cast<LPCVOID>(addr), buf, sz, &got) && got == sz;
}

// ================= АВТОПОИСК ОФСЕТОВ =================
void initOffsets() {
    if (!g_clientBase) return;
    
    IMAGE_DOS_HEADER dos;
    IMAGE_NT_HEADERS nt;
    if (!readMemRaw(g_clientBase, &dos, sizeof(dos))) return;
    if (!readMemRaw(g_clientBase + dos.e_lfanew, &nt, sizeof(nt))) return;

    DWORD size = nt.OptionalHeader.SizeOfImage;
    BYTE* buf = new BYTE[size];
    if (!readMemRaw(g_clientBase, buf, size)) { delete[] buf; return; }

    // 1. Ищем ViewMatrix (16 float подряд, начинающиеся с 1.0, 0.0...)
    for (DWORD i = 0; i < size - 64; i += 4) {
        float* f = (float*)(buf + i);
        if (f[0] == 1.0f && f[1] == 0.0f && f[2] == 0.0f && f[3] == 0.0f &&
            f[4] == 0.0f && f[5] == 1.0f && f[6] == 0.0f && f[7] == 0.0f) {
            off::dwViewMatrix = i;
            break;
        }
    }

    // 2. Ищем EntityList (куча указателей на игроков)
    // Обычно это массив, где лежат валидные адреса. Ищем по паттерну.
    for (DWORD i = 0; i < size - 2048; i += 4) {
        uintptr_t first = *(uintptr_t*)(buf + i + 4); // Пропускаем 0-й элемент
        uintptr_t second = *(uintptr_t*)(buf + i + 20); // + 16 байт (stride)
        if (first > 0x10000000 && first < 0x7FFFFFFF && 
            second > 0x10000000 && second < 0x7FFFFFFF) {
            // Проверяем, похоже ли это на список сущностей
            bool valid = true;
            for (int k = 1; k < 5; k++) {
                uintptr_t ptr = *(uintptr_t*)(buf + i + k * 16);
                if (ptr < 0x10000000) { valid = false; break; }
            }
            if (valid) {
                off::dwEntityList = i;
                break;
            }
        }
    }
    
    // 3. LocalPlayer ищем в куче (это сложно, поэтому берем стандартный или ищем по паттерну)
    // В CSS v34 он обычно не сильно сдвигается, но если что - добавим стандартный фолбэк.
    off::dwLocalPlayer = 0x00A4A2C4; // Стандартный. Если не сработает - будем искать точнее.

    delete[] buf;
    g_initialized = true;
}

// ================= MATH =================
struct Vec3 { float x, y, z; };
struct Vec2 { float x, y; };
int g_screenW = 0;
int g_screenH = 0;

bool worldToScreen(const Vec3& w, const float m[16], Vec2& out) {
    const float wv = m[3]*w.x + m[7]*w.y + m[11]*w.z + m[15];
    if (wv < 0.001f) return false;
    out.x = m[0]*w.x + m[4]*w.y + m[8]*w.z + m[12];
    out.y = m[1]*w.x + m[5]*w.y + m[9]*w.z + m[13];
    out.x /= wv; out.y /= wv;
    out.x = (g_screenW * 0.5f) * (out.x + 1.0f);
    out.y = (g_screenH * 0.5f) * (1.0f - out.y);
    return true;
}

// ================= ОВЕРЛЕЙ =================
HWND g_overlay = nullptr;
bool g_running = true;
bool g_espOn = true;
#define HOTKEY_TOGGLE 1
#define VK_TOGGLE     VK_F6

LRESULT CALLBACK WndProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    if (m == WM_HOTKEY && w == HOTKEY_TOGGLE) {
        g_espOn = !g_espOn;
        ShowWindow(g_overlay, g_espOn ? SW_SHOW : SW_HIDE);
        return 0;
    }
    if (m == WM_DESTROY) { PostQuitMessage(0); return 0; }
    return DefWindowProcW(h, m, w, l);
}

void drawESP(HDC hdc) {
    if (!g_initialized) return;
    
    uintptr_t lp = 0;
    if (!readMem(g_clientBase + off::dwLocalPlayer, lp) || !lp) return;
    int myTeam = 0; readMem(lp + off::m_iTeamNum, myTeam);

    float matrix[16];
    if (!readMemRaw(g_clientBase + off::dwViewMatrix, matrix, sizeof(matrix))) return;

    uintptr_t listBase = 0;
    if (!readMem(g_clientBase + off::dwEntityList, listBase) || !listBase) return;
    // Если EntityList найден как массив, а не указатель, читаем напрямую
    if (off::dwEntityList != 0) listBase = g_clientBase + off::dwEntityList;

    HPEN penEnemy = CreatePen(PS_SOLID, 1, RGB(255, 0, 0));
    HPEN oldPen = (HPEN)SelectObject(hdc, penEnemy);
    HBRUSH nullBrush = (HBRUSH)GetStockObject(NULL_BRUSH);
    HBRUSH oldBrush = (HBRUSH)SelectObject(hdc, nullBrush);
    SetBkMode(hdc, TRANSPARENT);

    for (int i = 1; i <= off::maxEntities; ++i) {
        uintptr_t ent = 0;
        readMem(listBase + off::entityStride * i, ent); // Читаем указатель на игрока
        if (!ent || ent == lp) continue;

        Vec3 origin;
        int team, health;
        uint8_t life = 1;
        readMem(ent + off::m_vecOrigin, origin);
        readMem(ent + off::m_iTeamNum, team);
        readMem(ent + off::m_iHealth, health);
        readMem(ent + off::m_lifeState, life);

        if (life != 0 || health <= 0 || team == myTeam) continue;

        Vec3 head = origin; head.z += 72.0f;
        Vec2 feet2d, head2d;
        if (!worldToScreen(origin, matrix, feet2d)) continue;
        if (!worldToScreen(head, matrix, head2d)) continue;

        float h = feet2d.y - head2d.y;
        if (h < 4.0f || h > 2000.0f) continue;
        float w = h * 0.55f;
        float x = head2d.x - w * 0.5f;
        float y = head2d.y;

        Rectangle(hdc, (int)x, (int)y, (int)(x + w), (int)(y + h));

        int hpColor = 255 - (health * 255 / 100);
        HPEN penHp = CreatePen(PS_SOLID, 3, RGB(255 - hpColor, hpColor, 0));
        SelectObject(hdc, penHp);
        MoveToEx(hdc, (int)(x - 6), (int)(y + h), nullptr);
        LineTo(hdc, (int)(x - 6), (int)(y + h - h * (health / 100.0f)));
        DeleteObject(penHp);

        char buf[64];
        std::snprintf(buf, sizeof(buf), "%d HP", health);
        SetTextColor(hdc, RGB(255, 255, 255));
        TextOutA(hdc, (int)x, (int)(y + h + 2), buf, (int)strlen(buf));
    }

    SelectObject(hdc, oldBrush);
    SelectObject(hdc, oldPen);
    DeleteObject(penEnemy);
}

int WINAPI wWinMain(HINSTANCE inst, HINSTANCE, PWSTR, int) {
    g_screenW = GetSystemMetrics(SM_CXSCREEN);
    g_screenH = GetSystemMetrics(SM_CYSCREEN);

    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = WndProc;
    wc.hInstance = inst;
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wc.lpszClassName = L"cssEspOverlay";
    RegisterClassExW(&wc);

    g_overlay = CreateWindowExW(WS_EX_LAYERED | WS_EX_TRANSPARENT | WS_EX_TOPMOST | WS_EX_TOOLWINDOW,
        wc.lpszClassName, L"css_esp", WS_POPUP, 0, 0, g_screenW, g_screenH, nullptr, nullptr, inst, nullptr);
    
    SetLayeredWindowAttributes(g_overlay, RGB(0,0,0), 255, LWA_COLORKEY);
    ShowWindow(g_overlay, SW_SHOW);
    RegisterHotKey(g_overlay, HOTKEY_TOGGLE, MOD_NOREPEAT, VK_TOGGLE);

    // Ждем игру
    while (g_running && !attachProcess()) Sleep(1000);
    while (g_running && !(g_clientBase = getModuleBase(L"client.dll"))) Sleep(1000);

    // Ищем офсеты
    initOffsets();

    HDC screenDC = GetDC(nullptr);
    HDC memDC = CreateCompatibleDC(screenDC);
    HBITMAP bmp = CreateCompatibleBitmap(screenDC, g_screenW, g_screenH);
    HBITMAP oldBmp = (HBITMAP)SelectObject(memDC, bmp);
    HBRUSH clear = CreateSolidBrush(RGB(0,0,0));

    MSG msg;
    while (g_running) {
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            if (msg.message == WM_QUIT) { g_running = false; break; }
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
        if (!g_running) break;

        RECT r{0,0,g_screenW,g_screenH};
        FillRect(memDC, &r, clear);
        if (g_espOn && g_hProc && g_clientBase && g_initialized) drawESP(memDC);

        POINT src{0,0}; SIZE size{g_screenW, g_screenH}; POINT dst{0,0};
        BLENDFUNCTION bf{AC_SRC_OVER, 0, 255, 0};
        UpdateLayeredWindow(g_overlay, screenDC, &dst, &size, memDC, &src, RGB(0,0,0), &bf, ULW_COLORKEY);
        Sleep(1);
    }

    SelectObject(memDC, oldBmp); DeleteObject(bmp); DeleteObject(clear);
    DeleteDC(memDC); ReleaseDC(nullptr, screenDC);
    return 0;
}
