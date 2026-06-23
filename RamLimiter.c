#define WIN32_LEAN_AND_MEAN
#define UNICODE
#define _UNICODE
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <tlhelp32.h>
#include <psapi.h>
#include <shellapi.h>
#include <Commctrl.h>

#pragma comment(lib, "kernel32.lib")
#pragma comment(lib, "user32.lib")
#pragma comment(lib, "psapi.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "comctl32.lib")

// Ativa o visual moderno (botões arredondados e efeitos) no Windows 10/11
#pragma comment(linker,"\"/manifestdependency:type='win32' name='Microsoft.Windows.Common-Controls' version='6.0.0.0' processorArchitecture='*' publicKeyToken='6595b64144ccf1df' language='*'\"")

#define MAX_PROCESSES 128
#define MAX_NAME_LEN 260
#define DEFAULT_INTERVAL_MS 5000
#define DEFAULT_MIN_MEMORY_MB 0
#define ID_TIMER_UPDATE 1
#define ID_LISTBOX 100
#define ID_BTN_ADD 101
#define ID_BTN_REMOVE 102
#define ID_BTN_REFRESH 103
#define ID_BTN_SAVE 104
#define ID_EDIT_PROCESS 200
#define ID_STATIC_STATUS 300
#define ID_STATIC_STATS 301
#define ID_STATIC_FREED 302
#define ID_STATIC_TRIMMED 303
#define ID_STATIC_ERRORS 304
#define ID_STATIC_INTERVAL 400
#define ID_EDIT_INTERVAL 401
#define ID_STATIC_MINMEM 402
#define ID_EDIT_MINMEM 403
#define ID_STATIC_MEMTOTAL 500
#define ID_STATIC_MEMACTIVE 501
#define ID_CHECK_STARTUP 600
#define ID_CHECK_STARTMIN 601
#define ID_EDIT_PROCMIN 602
#define ID_BTN_SAVEPROC 603
#define ID_TRAYICON 1

typedef struct {
    WCHAR name[MAX_NAME_LEN];
    DWORD minMemoryMb;  // 0 = usar global
} PROCESS_ENTRY;

static PROCESS_ENTRY g_procs[MAX_PROCESSES];
static int g_procCount;
static HANDLE g_worker;
static HANDLE g_hShutdownEvent; 
static CRITICAL_SECTION g_cs;
static WCHAR g_exePath[MAX_PATH];

static ULONGLONG g_totalFreed = 0;
static volatile LONG g_totalTrimmed = 0;
static volatile LONG g_totalErrors = 0;
static ULONGLONG g_lastTrimTime = 0;

static DWORD g_intervalMs = DEFAULT_INTERVAL_MS;
static DWORD g_minMemoryMb = DEFAULT_MIN_MEMORY_MB;

static HWND g_hwndList, g_hwndEditProcess, g_hwndBtnAdd, g_hwndBtnRemove;
static HWND g_hwndStatus, g_hwndStats, g_hwndFreed, g_hwndTrimmed, g_hwndErrors;
static HWND g_hwndMemTotal, g_hwndInterval, g_hwndMinMem;
static HWND g_hwndChkStartup, g_hwndChkStartMin;
static HWND g_hwndEditProcMin, g_hwndBtnSaveProc;

static WCHAR g_configPath[MAX_PATH];
static ULONGLONG g_configLastMod = 0;
static NOTIFYICONDATA g_nid;
static HWND g_mainHwnd;

static HBRUSH g_hbrDarkBg, g_hbrDarkEdit, g_hbrDarkList, g_hbrDarkBtn;
static COLORREF g_crDarkBg, g_crDarkText, g_crDarkEditBg;

static BOOL g_startupEnabled;
static BOOL g_startMinimized;

static void show_tray_menu(HWND hwnd);

// Formata os bytes automaticamente para KB ou MB na tela
static void FormatBytes(ULONGLONG bytes, WCHAR* buf, size_t bufSize) {
    if (bytes >= 1048576)
        swprintf_s(buf, bufSize, L"%.1f MB", (double)bytes / 1048576.0);
    else if (bytes >= 1024)
        swprintf_s(buf, bufSize, L"%.1f KB", (double)bytes / 1024.0);
    else
        swprintf_s(buf, bufSize, L"%llu B", (unsigned long long)bytes);
}

static BOOL startup_check(void) {
    wchar_t cmd[512];
    swprintf_s(cmd, ARRAYSIZE(cmd), L"schtasks /Query /TN \"RAMLimiterPro\" >nul 2>&1");
    return _wsystem(cmd) == 0;
}

static void startup_set(BOOL enable) {
    if (enable) {
        wchar_t cmd[1024];
        swprintf_s(cmd, ARRAYSIZE(cmd),
            L"schtasks /Create /TN \"RAMLimiterPro\" /TR \"\\\"%s\\\"\" /SC ONLOGON /RL HIGHEST /F >nul 2>&1",
            g_exePath);
        _wsystem(cmd);
    } else {
        _wsystem(L"schtasks /Delete /TN \"RAMLimiterPro\" /F >nul 2>&1");
    }
    g_startupEnabled = enable;
}

static void config_save(void) {
    char pathA[MAX_PATH];
    WideCharToMultiByte(CP_UTF8, 0, g_configPath, -1, pathA, MAX_PATH, NULL, NULL);

    FILE *f = fopen(pathA, "w");
    if (!f) return;

    fprintf(f, "###############################################################\n");
    fprintf(f, "#               RAM Limiter Pro - Configuracao                #\n");
    fprintf(f, "###############################################################\n\n");
    fprintf(f, "interval=%u\n", g_intervalMs);
    fprintf(f, "min_memory=%u\n", g_minMemoryMb);
    fprintf(f, "start_minimized=%d\n\n", g_startMinimized ? 1 : 0);
    fprintf(f, "###############################################################\n");
    fprintf(f, "#                    LISTA DE PROCESSOS                       #\n");
    fprintf(f, "###############################################################\n");

    EnterCriticalSection(&g_cs);
    for (int i = 0; i < g_procCount; i++) {
        char nameA[MAX_NAME_LEN];
        WideCharToMultiByte(CP_UTF8, 0, g_procs[i].name, -1, nameA, MAX_NAME_LEN, NULL, NULL);
        if (g_procs[i].minMemoryMb > 0)
            fprintf(f, "%s=%u\n", nameA, g_procs[i].minMemoryMb);
        else
            fprintf(f, "%s\n", nameA);
    }
    LeaveCriticalSection(&g_cs);

    fclose(f);
}

static void config_load(void) {
    WCHAR wpath[MAX_PATH];
    wcscpy_s(wpath, MAX_PATH, g_exePath);
    WCHAR *s = wcsrchr(wpath, L'\\');
    if (s) wcscpy_s(s + 1, MAX_PATH - (s - wpath) - 1, L"config.txt");
    else wcscpy_s(wpath, MAX_PATH, L"config.txt");
    wcscpy_s(g_configPath, MAX_PATH, wpath);

    WIN32_FILE_ATTRIBUTE_DATA fad;
    if (!GetFileAttributesExW(wpath, GetFileExInfoStandard, &fad)) {
        g_intervalMs = DEFAULT_INTERVAL_MS;
        g_minMemoryMb = DEFAULT_MIN_MEMORY_MB;
        g_startMinimized = FALSE;
        return;
    }

    ULONGLONG lastMod = ((ULONGLONG)fad.ftLastWriteTime.dwHighDateTime << 32) | fad.ftLastWriteTime.dwLowDateTime;
    if (lastMod == g_configLastMod) return;
    g_configLastMod = lastMod;

    char pathA[MAX_PATH];
    WideCharToMultiByte(CP_UTF8, 0, wpath, -1, pathA, MAX_PATH, NULL, NULL);

    FILE *f = fopen(pathA, "r");
    if (!f) {
        f = fopen(pathA, "w");
        if (f) {
            fprintf(f, "interval=5000\nmin_memory=0\nstart_minimized=0\ndiscord\nchrome\n");
            fclose(f);
            f = fopen(pathA, "r");
        }
        if (!f) return;
    }

    EnterCriticalSection(&g_cs);
    g_procCount = 0;
    char line[MAX_NAME_LEN];

    while (fgets(line, MAX_NAME_LEN, f)) {
        int len = (int)strlen(line);
        while (len > 0 && (line[len-1] == '\r' || line[len-1] == '\n')) line[--len] = '\0';
        while (len > 0 && (line[len-1] == ' ' || line[len-1] == '\t')) line[--len] = '\0';

        if (len == 0 || line[0] == '#') continue;

        if (strncmp(line, "interval=", 9) == 0) {
            g_intervalMs = atoi(line + 9);
            if (g_intervalMs < 1000) g_intervalMs = 1000;
            continue;
        }
        if (strncmp(line, "min_memory=", 11) == 0) {
            g_minMemoryMb = atoi(line + 11);
            continue;
        }
        if (strncmp(line, "start_minimized=", 16) == 0) {
            g_startMinimized = atoi(line + 16) != 0;
            continue;
        }

        char *p = line;
        char *ext = strstr(p, ".exe");
        if (ext) *ext = '\0';

        if (g_procCount < MAX_PROCESSES && strlen(p) > 0) {
            DWORD perProcMb = 0;
            char *eq = strchr(p, '=');
            if (eq) {
                perProcMb = atoi(eq + 1);
                *eq = '\0';
            }
            MultiByteToWideChar(CP_UTF8, 0, p, -1, g_procs[g_procCount].name, MAX_NAME_LEN);
            g_procs[g_procCount].minMemoryMb = perProcMb;
            g_procCount++;
        }
    }
    fclose(f);
    LeaveCriticalSection(&g_cs);
}

static DWORD pid_of_process(const WCHAR *name, SIZE_T *outWs) {
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return 0;
    DWORD bestPid = 0;
    SIZE_T bestWs = 0;
    PROCESSENTRY32W pe = { .dwSize = sizeof(pe) };
    if (Process32FirstW(snap, &pe)) do {
        WCHAR exeName[MAX_NAME_LEN];
        wcscpy_s(exeName, MAX_NAME_LEN, pe.szExeFile);
        WCHAR *dot = wcsrchr(exeName, L'.');
        if (dot && _wcsicmp(dot, L".exe") == 0) *dot = L'\0';
        
        if (_wcsicmp(exeName, name) != 0) continue;
        
        HANDLE hp = OpenProcess(PROCESS_QUERY_INFORMATION | PROCESS_SET_QUOTA, FALSE, pe.th32ProcessID);
        if (!hp) continue;
        PROCESS_MEMORY_COUNTERS pmc;
        if (GetProcessMemoryInfo(hp, &pmc, sizeof(pmc)) && pmc.WorkingSetSize > bestWs) {
            bestWs = pmc.WorkingSetSize;
            bestPid = pe.th32ProcessID;
        }
        CloseHandle(hp);
    } while (Process32NextW(snap, &pe));
    CloseHandle(snap);
    if (outWs) *outWs = bestWs;
    return bestPid;
}

// CORREÇÃO: Utiliza EmptyWorkingSet para otimização forçada e agressiva da RAM
static BOOL trim_pid(DWORD pid, SIZE_T before, LONGLONG *freed) {
    HANDLE hp = OpenProcess(PROCESS_SET_QUOTA | PROCESS_QUERY_INFORMATION, FALSE, pid);
    if (!hp) return FALSE;
    
    SIZE_T wsBefore = before;
    if (wsBefore == 0) {
        PROCESS_MEMORY_COUNTERS pmc;
        if (GetProcessMemoryInfo(hp, &pmc, sizeof(pmc)))
            wsBefore = pmc.WorkingSetSize;
    }
    
    EmptyWorkingSet(hp);

    PROCESS_MEMORY_COUNTERS pmc;
    SIZE_T wsAfter = 0;
    if (GetProcessMemoryInfo(hp, &pmc, sizeof(pmc)))
        wsAfter = pmc.WorkingSetSize;
        
    *freed = (wsBefore > wsAfter) ? (wsBefore - wsAfter) : 0;
    CloseHandle(hp);
    return TRUE;
}

static void trim_all(void) {
    EnterCriticalSection(&g_cs);
    int n = g_procCount;
    WCHAR names[MAX_PROCESSES][MAX_NAME_LEN];
    DWORD perProcMb[MAX_PROCESSES];
    for (int i = 0; i < n; i++) {
        wcscpy_s(names[i], MAX_NAME_LEN, g_procs[i].name);
        perProcMb[i] = g_procs[i].minMemoryMb;
    }
    DWORD globalMinMb = g_minMemoryMb;
    LeaveCriticalSection(&g_cs);

    for (int i = 0; i < n; i++) {
        if (WaitForSingleObject(g_hShutdownEvent, 0) == WAIT_OBJECT_0) break;

        SIZE_T ws = 0;
        DWORD pid = pid_of_process(names[i], &ws);
        if (!pid) continue;
        DWORD effectiveMinMb = (perProcMb[i] > 0) ? perProcMb[i] : globalMinMb;
        if (effectiveMinMb > 0 && ws < (SIZE_T)effectiveMinMb * 1048576) continue;
        
        LONGLONG freed = 0;
        if (trim_pid(pid, ws, &freed)) {
            InterlockedIncrement(&g_totalTrimmed);
            InterlockedAdd64(&g_totalFreed, freed);
            g_lastTrimTime = GetTickCount64();
        } else {
            InterlockedIncrement(&g_totalErrors);
        }
    }
}

static void get_memory_info(int *activeCount, SIZE_T *totalMemory) {
    *activeCount = 0; *totalMemory = 0;
    EnterCriticalSection(&g_cs);
    int n = g_procCount;
    WCHAR names[MAX_PROCESSES][MAX_NAME_LEN];
    for (int i = 0; i < n; i++) wcscpy_s(names[i], MAX_NAME_LEN, g_procs[i].name);
    LeaveCriticalSection(&g_cs);

    for (int i = 0; i < n; i++) {
        SIZE_T ws = 0;
        if (pid_of_process(names[i], &ws)) {
            (*activeCount)++;
            *totalMemory += ws;
        }
    }
}

DWORD WINAPI WorkerThread(LPVOID lp) {
    (void)lp;
    while (WaitForSingleObject(g_hShutdownEvent, g_intervalMs) == WAIT_TIMEOUT) {
        config_load();
        trim_all();
    }
    return 0;
}

static void update_listbox(void) {
    SendMessage(g_hwndList, LB_RESETCONTENT, 0, 0);
    EnterCriticalSection(&g_cs);
    for (int i = 0; i < g_procCount; i++) {
        WCHAR entry[280];
        if (g_procs[i].minMemoryMb > 0)
            swprintf_s(entry, ARRAYSIZE(entry), L"%s (%u MB)", g_procs[i].name, g_procs[i].minMemoryMb);
        else
            wcscpy_s(entry, ARRAYSIZE(entry), g_procs[i].name);
        SendMessage(g_hwndList, LB_ADDSTRING, 0, (LPARAM)entry);
    }
    LeaveCriticalSection(&g_cs);
}

static void update_display(void) {
    int activeCount; SIZE_T totalMem;
    get_memory_info(&activeCount, &totalMem);

    WCHAR buf[256], memStr[64];
    swprintf_s(buf, ARRAYSIZE(buf), L"Ativos: %d / %d", activeCount, g_procCount);
    SetWindowText(g_hwndStatus, buf);

    FormatBytes(totalMem, memStr, ARRAYSIZE(memStr));
    swprintf_s(buf, ARRAYSIZE(buf), L"Memoria: %s", memStr);
    SetWindowText(g_hwndMemTotal, buf);

    swprintf_s(buf, ARRAYSIZE(buf), L"Trimados: %d", g_totalTrimmed);
    SetWindowText(g_hwndTrimmed, buf);

    swprintf_s(buf, ARRAYSIZE(buf), L"Erros: %d", g_totalErrors);
    SetWindowText(g_hwndErrors, buf);

    FormatBytes(g_totalFreed, memStr, ARRAYSIZE(memStr));
    swprintf_s(buf, ARRAYSIZE(buf), L"Liberado: %s", memStr);
    SetWindowText(g_hwndFreed, buf);

    ULONGLONG sinceLast = (GetTickCount64() - g_lastTrimTime) / 1000;
    if (g_lastTrimTime == 0) wcscpy_s(buf, ARRAYSIZE(buf), L"Ultimo trim: Nunca");
    else if (sinceLast < 60) swprintf_s(buf, ARRAYSIZE(buf), L"Ultimo trim: %llu seg", sinceLast);
    else swprintf_s(buf, ARRAYSIZE(buf), L"Ultimo trim: %llu min", sinceLast / 60);
    SetWindowText(g_hwndStats, buf);
}

static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
        case WM_CTLCOLORSTATIC:
            SetBkColor((HDC)wp, g_crDarkBg); SetTextColor((HDC)wp, g_crDarkText);
            return (LRESULT)g_hbrDarkBg;
        case WM_CTLCOLOREDIT:
            SetBkColor((HDC)wp, g_crDarkEditBg); SetTextColor((HDC)wp, g_crDarkText);
            return (LRESULT)g_hbrDarkEdit;
        case WM_CTLCOLORLISTBOX:
            SetBkColor((HDC)wp, RGB(35, 35, 35)); SetTextColor((HDC)wp, g_crDarkText);
            return (LRESULT)g_hbrDarkList;
        case WM_CTLCOLORBTN:
            SetBkColor((HDC)wp, RGB(60, 60, 60)); SetTextColor((HDC)wp, g_crDarkText);
            return (LRESULT)g_hbrDarkBtn;

        case WM_CREATE: {
            HFONT hFont = (HFONT)GetStockObject(DEFAULT_GUI_FONT);

            CreateWindowEx(0, L"STATIC", L"RAM Limiter Pro", WS_CHILD | WS_VISIBLE | SS_CENTER, 10, 5, 530, 25, hwnd, NULL, GetModuleHandle(NULL), NULL);

            CreateWindowEx(0, L"STATIC", L"Processos:", WS_CHILD | WS_VISIBLE, 10, 35, 200, 20, hwnd, NULL, GetModuleHandle(NULL), NULL);
            g_hwndList = CreateWindowEx(WS_EX_CLIENTEDGE, L"LISTBOX", L"", WS_CHILD | WS_VISIBLE | WS_BORDER | WS_VSCROLL | LBS_NOTIFY, 10, 55, 200, 180, hwnd, (HMENU)ID_LISTBOX, GetModuleHandle(NULL), NULL);
            SendMessage(g_hwndList, WM_SETFONT, (WPARAM)hFont, TRUE);

            g_hwndEditProcess = CreateWindowEx(WS_EX_CLIENTEDGE, L"EDIT", L"", WS_CHILD | WS_VISIBLE | WS_BORDER, 10, 240, 200, 22, hwnd, (HMENU)ID_EDIT_PROCESS, GetModuleHandle(NULL), NULL);
            SendMessage(g_hwndEditProcess, WM_SETFONT, (WPARAM)hFont, TRUE);

            g_hwndBtnAdd = CreateWindowEx(0, L"BUTTON", L"+ Add", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON, 10, 268, 65, 24, hwnd, (HMENU)ID_BTN_ADD, GetModuleHandle(NULL), NULL);
            SendMessage(g_hwndBtnAdd, WM_SETFONT, (WPARAM)hFont, TRUE);

            g_hwndBtnRemove = CreateWindowEx(0, L"BUTTON", L"- Remove", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON, 80, 268, 65, 24, hwnd, (HMENU)ID_BTN_REMOVE, GetModuleHandle(NULL), NULL);
            SendMessage(g_hwndBtnRemove, WM_SETFONT, (WPARAM)hFont, TRUE);

            CreateWindowEx(0, L"BUTTON", L"Refresh", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON, 150, 268, 60, 24, hwnd, (HMENU)ID_BTN_REFRESH, GetModuleHandle(NULL), NULL);

            CreateWindowEx(0, L"STATIC", L"Min MB Proc:", WS_CHILD | WS_VISIBLE, 10, 300, 80, 20, hwnd, NULL, GetModuleHandle(NULL), NULL);
            g_hwndEditProcMin = CreateWindowEx(WS_EX_CLIENTEDGE, L"EDIT", L"0", WS_CHILD | WS_VISIBLE | WS_BORDER | ES_NUMBER, 95, 298, 55, 22, hwnd, (HMENU)ID_EDIT_PROCMIN, GetModuleHandle(NULL), NULL);
            SendMessage(g_hwndEditProcMin, WM_SETFONT, (WPARAM)hFont, TRUE);
            g_hwndBtnSaveProc = CreateWindowEx(0, L"BUTTON", L"Salvar Proc", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON, 155, 297, 80, 24, hwnd, (HMENU)ID_BTN_SAVEPROC, GetModuleHandle(NULL), NULL);
            SendMessage(g_hwndBtnSaveProc, WM_SETFONT, (WPARAM)hFont, TRUE);

            CreateWindowEx(0, L"STATIC", L"Configuracoes:", WS_CHILD | WS_VISIBLE, 230, 35, 200, 20, hwnd, NULL, GetModuleHandle(NULL), NULL);

            CreateWindowEx(0, L"STATIC", L"Intervalo:", WS_CHILD | WS_VISIBLE, 230, 60, 70, 20, hwnd, NULL, GetModuleHandle(NULL), NULL);
            g_hwndInterval = CreateWindowEx(WS_EX_CLIENTEDGE, L"EDIT", L"5000", WS_CHILD | WS_VISIBLE | WS_BORDER | ES_NUMBER, 305, 58, 70, 22, hwnd, (HMENU)ID_EDIT_INTERVAL, GetModuleHandle(NULL), NULL);
            SendMessage(g_hwndInterval, WM_SETFONT, (WPARAM)hFont, TRUE);

            CreateWindowEx(0, L"STATIC", L"Min MB:", WS_CHILD | WS_VISIBLE, 385, 60, 60, 20, hwnd, NULL, GetModuleHandle(NULL), NULL);
            g_hwndMinMem = CreateWindowEx(WS_EX_CLIENTEDGE, L"EDIT", L"0", WS_CHILD | WS_VISIBLE | WS_BORDER | ES_NUMBER, 450, 58, 55, 22, hwnd, (HMENU)ID_EDIT_MINMEM, GetModuleHandle(NULL), NULL);
            SendMessage(g_hwndMinMem, WM_SETFONT, (WPARAM)hFont, TRUE);

            CreateWindowEx(0, L"BUTTON", L"Salvar Config", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON, 230, 88, 100, 24, hwnd, (HMENU)ID_BTN_SAVE, GetModuleHandle(NULL), NULL);

            g_hwndChkStartup = CreateWindowEx(0, L"BUTTON", L"Iniciar com Windows", WS_CHILD | WS_VISIBLE | BS_AUTOCHECKBOX, 335, 91, 140, 20, hwnd, (HMENU)ID_CHECK_STARTUP, GetModuleHandle(NULL), NULL);
            SendMessage(g_hwndChkStartup, WM_SETFONT, (WPARAM)hFont, TRUE);
            g_startupEnabled = startup_check();
            SendMessage(g_hwndChkStartup, BM_SETCHECK, g_startupEnabled ? BST_CHECKED : BST_UNCHECKED, 0);

            g_hwndChkStartMin = CreateWindowEx(0, L"BUTTON", L"Iniciar minimizado", WS_CHILD | WS_VISIBLE | BS_AUTOCHECKBOX, 230, 115, 140, 20, hwnd, (HMENU)ID_CHECK_STARTMIN, GetModuleHandle(NULL), NULL);
            SendMessage(g_hwndChkStartMin, WM_SETFONT, (WPARAM)hFont, TRUE);
            SendMessage(g_hwndChkStartMin, BM_SETCHECK, g_startMinimized ? BST_CHECKED : BST_UNCHECKED, 0);

            CreateWindowEx(0, L"STATIC", L"Status:", WS_CHILD | WS_VISIBLE, 230, 145, 300, 20, hwnd, NULL, GetModuleHandle(NULL), NULL);
            g_hwndStatus = CreateWindowEx(0, L"STATIC", L"Ativos: 0 / 0", WS_CHILD | WS_VISIBLE, 230, 165, 140, 20, hwnd, (HMENU)ID_STATIC_STATUS, GetModuleHandle(NULL), NULL);
            g_hwndMemTotal = CreateWindowEx(0, L"STATIC", L"Memoria: 0 B", WS_CHILD | WS_VISIBLE, 375, 165, 140, 20, hwnd, (HMENU)ID_STATIC_MEMTOTAL, GetModuleHandle(NULL), NULL);

            CreateWindowEx(0, L"STATIC", L"Estatisticas:", WS_CHILD | WS_VISIBLE, 230, 195, 300, 20, hwnd, NULL, GetModuleHandle(NULL), NULL);
            g_hwndTrimmed = CreateWindowEx(0, L"STATIC", L"Trimados: 0", WS_CHILD | WS_VISIBLE, 230, 215, 90, 20, hwnd, (HMENU)ID_STATIC_TRIMMED, GetModuleHandle(NULL), NULL);
            g_hwndErrors = CreateWindowEx(0, L"STATIC", L"Erros: 0", WS_CHILD | WS_VISIBLE, 325, 215, 70, 20, hwnd, (HMENU)ID_STATIC_ERRORS, GetModuleHandle(NULL), NULL);
            g_hwndFreed = CreateWindowEx(0, L"STATIC", L"Liberado: 0 B", WS_CHILD | WS_VISIBLE, 230, 235, 140, 20, hwnd, (HMENU)ID_STATIC_FREED, GetModuleHandle(NULL), NULL);
            g_hwndStats = CreateWindowEx(0, L"STATIC", L"Ultimo trim: Nunca", WS_CHILD | WS_VISIBLE, 375, 235, 155, 20, hwnd, (HMENU)ID_STATIC_STATS, GetModuleHandle(NULL), NULL);

            CreateWindowEx(0, L"STATIC", L"Requer privilegios de Administrador", WS_CHILD | WS_VISIBLE | SS_CENTER, 10, 340, 530, 20, hwnd, NULL, GetModuleHandle(NULL), NULL);

            update_listbox();
            SetTimer(hwnd, ID_TIMER_UPDATE, 1000, NULL);
            return 0;
        }

        case WM_TIMER:
            if (wp == ID_TIMER_UPDATE) update_display();
            return 0;

        case WM_COMMAND:
            if (wp == 1) {
                ShowWindow(g_mainHwnd, SW_SHOW); SetForegroundWindow(g_mainHwnd);
            }
            else if (wp == 2) {
                SetEvent(g_hShutdownEvent);
                Shell_NotifyIcon(NIM_DELETE, &g_nid);
                PostMessage(hwnd, WM_QUIT, 0, 0);
            }
            else if (LOWORD(wp) == ID_LISTBOX && HIWORD(wp) == LBN_SELCHANGE) {
                int sel = SendMessage(g_hwndList, LB_GETCURSEL, 0, 0);
                if (sel != LB_ERR) {
                    EnterCriticalSection(&g_cs);
                    if (sel < g_procCount) {
                        WCHAR buf[32];
                        swprintf_s(buf, ARRAYSIZE(buf), L"%u", g_procs[sel].minMemoryMb);
                        SetWindowText(g_hwndEditProcMin, buf);
                    }
                    LeaveCriticalSection(&g_cs);
                }
            }
            else if (wp == ID_BTN_SAVEPROC) {
                int sel = SendMessage(g_hwndList, LB_GETCURSEL, 0, 0);
                if (sel != LB_ERR) {
                    WCHAR buf[32];
                    GetWindowText(g_hwndEditProcMin, buf, 32);
                    DWORD newMinMb = _wtoi(buf);
                    EnterCriticalSection(&g_cs);
                    if (sel < g_procCount) g_procs[sel].minMemoryMb = newMinMb;
                    LeaveCriticalSection(&g_cs);
                    config_save();
                    update_listbox();
                    SendMessage(g_hwndList, LB_SETCURSEL, sel, 0);
                }
            }
            else if (wp == ID_BTN_ADD) {
                WCHAR name[MAX_NAME_LEN];
                GetWindowText(g_hwndEditProcess, name, MAX_NAME_LEN);
                if (wcslen(name) > 0) {
                    // CORREÇÃO: Remove o .exe de forma case-insensitive estável
                    WCHAR *ext = wcsrchr(name, L'.');
                    if (ext && _wcsicmp(ext, L".exe") == 0) *ext = L'\0';
                    
                    EnterCriticalSection(&g_cs);
                    if (g_procCount < MAX_PROCESSES) {
                        wcsncpy_s(g_procs[g_procCount].name, MAX_NAME_LEN, name, _TRUNCATE);
                        g_procCount++;
                    }
                    LeaveCriticalSection(&g_cs);
                    update_listbox(); SetWindowText(g_hwndEditProcess, L"");
                }
            }
            else if (wp == ID_BTN_REMOVE) {
                int sel = SendMessage(g_hwndList, LB_GETCURSEL, 0, 0);
                if (sel != LB_ERR) {
                    EnterCriticalSection(&g_cs);
                    if (g_procCount > 0) {
                        for (int i = sel; i < g_procCount - 1; i++) wcscpy_s(g_procs[i].name, MAX_NAME_LEN, g_procs[i + 1].name);
                        g_procCount--;
                    }
                    LeaveCriticalSection(&g_cs);
                    update_listbox();
                }
            }
            else if (wp == ID_BTN_REFRESH) {
                config_load(); update_listbox();
            }
            else if (wp == ID_BTN_SAVE) {
                WCHAR buf[32];
                GetWindowText(g_hwndInterval, buf, 32);
                DWORD newInterval = _wtoi(buf);
                if (newInterval >= 1000 && newInterval <= 600000) g_intervalMs = newInterval;

                GetWindowText(g_hwndMinMem, buf, 32);
                DWORD newMinMem = _wtoi(buf);
                if (newMinMem <= 10000) g_minMemoryMb = newMinMem;

                config_save();
                MessageBox(hwnd, L"Configuracoes salvas!", L"RAM Limiter Pro", MB_OK | MB_ICONINFORMATION);
                // CORREÇÃO: Linhas do SetEvent removidas daqui para evitar o travamento do motor.
            }
            else if (wp == ID_CHECK_STARTUP) {
                BOOL checked = SendMessage(GetDlgItem(hwnd, ID_CHECK_STARTUP), BM_GETCHECK, 0, 0) == BST_CHECKED;
                startup_set(checked);
            }
            else if (wp == ID_CHECK_STARTMIN) {
                g_startMinimized = SendMessage(GetDlgItem(hwnd, ID_CHECK_STARTMIN), BM_GETCHECK, 0, 0) == BST_CHECKED;
                config_save();
            }
            return 0;

        case WM_CLOSE:
            ShowWindow(hwnd, SW_HIDE);
            return 0;

        case WM_USER:
            if (lp == WM_RBUTTONDOWN) show_tray_menu(hwnd);
            else if (lp == WM_LBUTTONDOWN) {
                ShowWindow(hwnd, SW_SHOW); SetForegroundWindow(hwnd);
            }
            return 0;
    }
    return DefWindowProc(hwnd, msg, wp, lp);
}

static void create_tray_icon(HWND hwnd) {
    g_nid.cbSize = sizeof(NOTIFYICONDATA);
    g_nid.hWnd = hwnd; g_nid.uID = ID_TRAYICON;
    g_nid.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP;
    g_nid.uCallbackMessage = WM_USER;
    g_nid.hIcon = LoadIcon(NULL, IDI_APPLICATION);
    wcscpy_s(g_nid.szTip, ARRAYSIZE(g_nid.szTip), L"RAM Limiter Pro");
    Shell_NotifyIcon(NIM_ADD, &g_nid);
}

static void show_tray_menu(HWND hwnd) {
    POINT pt; GetCursorPos(&pt); SetForegroundWindow(hwnd);
    HMENU hMenu = CreatePopupMenu();
    AppendMenu(hMenu, MF_STRING, 1, L"Abrir");
    AppendMenu(hMenu, MF_SEPARATOR, 0, NULL);
    AppendMenu(hMenu, MF_STRING, 2, L"Sair");
    TrackPopupMenu(hMenu, TPM_LEFTALIGN | TPM_RIGHTBUTTON, pt.x, pt.y, 0, hwnd, NULL);
    DestroyMenu(hMenu);
}

static BOOL is_admin(void) {
    BOOL ok = FALSE; PSID group = NULL;
    SID_IDENTIFIER_AUTHORITY auth = SECURITY_NT_AUTHORITY;
    if (AllocateAndInitializeSid(&auth, 2, SECURITY_BUILTIN_DOMAIN_RID, DOMAIN_ALIAS_RID_ADMINS, 0, 0, 0, 0, 0, 0, &group)) {
        CheckTokenMembership(NULL, group, &ok); FreeSid(group);
    }
    return ok;
}

int WINAPI WinMain(HINSTANCE hInst, HINSTANCE hPrev, LPSTR lpCmd, int nShow) {
    (void)hPrev; (void)lpCmd;
    GetModuleFileNameW(NULL, g_exePath, MAX_PATH);

    if (!is_admin()) {
        MessageBox(NULL, L"Este programa requer privilegios de Administrador!\n\nExecute como Administrador para funcionar.", L"RAM Limiter Pro - Erro", MB_OK | MB_ICONERROR | MB_TOPMOST);
        return 1;
    }

    HANDLE hMutex = CreateMutexW(NULL, TRUE, L"Local\\RAMLimiterPro_SingleInstance");
    if (GetLastError() == ERROR_ALREADY_EXISTS) return 1;

    g_crDarkBg = RGB(30, 30, 30); g_crDarkText = RGB(220, 220, 220); g_crDarkEditBg = RGB(45, 45, 45);
    g_hbrDarkBg = CreateSolidBrush(g_crDarkBg); g_hbrDarkEdit = CreateSolidBrush(g_crDarkEditBg);
    g_hbrDarkList = CreateSolidBrush(RGB(35, 35, 35)); g_hbrDarkBtn = CreateSolidBrush(RGB(60, 60, 60));

    InitializeCriticalSection(&g_cs);
    g_hShutdownEvent = CreateEvent(NULL, TRUE, FALSE, NULL); 
    config_load();

    WNDCLASSEX wc = {0};
    wc.cbSize = sizeof(WNDCLASSEX); wc.lpfnWndProc = WndProc; wc.hInstance = hInst;
    wc.hCursor = LoadCursor(NULL, IDC_ARROW); wc.hbrBackground = g_hbrDarkBg;
    wc.lpszClassName = L"RamLimiterApp";
    RegisterClassEx(&wc);

    HWND hwnd = CreateWindowEx(0, L"RamLimiterApp", L"RAM Limiter Pro", WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT, 560, 380, NULL, NULL, hInst, NULL);
    g_mainHwnd = hwnd;
    if (g_startMinimized) ShowWindow(hwnd, SW_HIDE);
    else ShowWindow(hwnd, nShow);

    create_tray_icon(hwnd);
    g_worker = CreateThread(NULL, 0, WorkerThread, NULL, 0, NULL);

    MSG msg;
    while (GetMessage(&msg, NULL, 0, 0)) {
        TranslateMessage(&msg); DispatchMessage(&msg);
    }

    SetEvent(g_hShutdownEvent);
    if (g_worker) { WaitForSingleObject(g_worker, 3000); CloseHandle(g_worker); }
    
    Shell_NotifyIcon(NIM_DELETE, &g_nid);
    DeleteCriticalSection(&g_cs);
    CloseHandle(g_hShutdownEvent);
    DeleteObject(g_hbrDarkBg); DeleteObject(g_hbrDarkEdit); DeleteObject(g_hbrDarkList); DeleteObject(g_hbrDarkBtn);
    if (hMutex) CloseHandle(hMutex);
    return 0;
}