// TZShowDesktop.cpp : アプリケーションのエントリ ポイントを定義します。
//

#include "framework.h"
#include <strsafe.h>

// プロトタイプ
void MinimizeWindowsOnPrimary();

// 状態ファイルの形式識別子と、保存するウィンドウ数の上限。
static constexpr DWORD TOGGLE_STATE_SIGNATURE = 0x545A5344;
static constexpr DWORD MAX_TRACKED_WINDOWS = 2048;

// 復元対象のウィンドウと、ハンドル再利用を検出するプロセス ID。
struct WindowRecord {
    UINT64 window;
    DWORD processId;
};

// 状態ファイルの先頭に書き込むヘッダー。
struct ToggleStateHeader {
    DWORD signature;
    DWORD count;
};

int APIENTRY wWinMain(_In_ HINSTANCE hInstance,
                      _In_opt_ HINSTANCE hPrevInstance,
                      _In_ LPWSTR lpCmdLine,
                      _In_ int nCmdShow) {
    UNREFERENCED_PARAMETER(hInstance);
    UNREFERENCED_PARAMETER(hPrevInstance);
    UNREFERENCED_PARAMETER(lpCmdLine);
    UNREFERENCED_PARAMETER(nCmdShow);

    // UI は表示せず、起動処理だけを実行して終了する。
    MinimizeWindowsOnPrimary();
    return 0;
}

// 状態ファイルを実行ファイルと同じフォルダーに配置する。
static BOOL GetToggleStatePath(WCHAR* path, size_t pathLength) {
    DWORD length = GetModuleFileNameW(nullptr, path, (DWORD)pathLength);
    if(length == 0 || length >= pathLength) {
        return FALSE;
    }

    DWORD separator = length;
    while(separator > 0 && path[separator - 1] != L'\\') {
        --separator;
    }
    if(separator == 0) {
        return FALSE;
    }

    path[separator] = L'\0';
    return SUCCEEDED(StringCchCatW(path, pathLength, L"TZShowDesktop.toggle"));
}

static BOOL SaveToggleState(const WCHAR* path, const WindowRecord* records, DWORD count) {
    WCHAR tempPath[MAX_PATH];
    if(FAILED(StringCchCopyW(tempPath, ARRAYSIZE(tempPath), path)) ||
        FAILED(StringCchCatW(tempPath, ARRAYSIZE(tempPath), L".tmp"))) {
        return FALSE;
    }

    // 一時ファイルへ書き込み、完了後に置き換えて不完全な状態ファイルを避ける。
    HANDLE file = CreateFileW(
        tempPath,
        GENERIC_WRITE,
        0,
        nullptr,
        CREATE_ALWAYS,
        FILE_ATTRIBUTE_HIDDEN | FILE_ATTRIBUTE_TEMPORARY,
        nullptr);
    if(file == INVALID_HANDLE_VALUE) {
        return FALSE;
    }

    ToggleStateHeader header = {TOGGLE_STATE_SIGNATURE, count};
    DWORD written = 0;
    BOOL success = WriteFile(file, &header, sizeof(header), &written, nullptr) &&
        written == sizeof(header);
    if(success && count > 0) {
        DWORD bytes = count * sizeof(WindowRecord);
        success = WriteFile(file, records, bytes, &written, nullptr) && written == bytes;
    }
    if(success) {
        success = FlushFileBuffers(file);
    }
    CloseHandle(file);

    if(!success || !MoveFileExW(tempPath, path, MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        DeleteFileW(tempPath);
        return FALSE;
    }
    return TRUE;
}

static BOOL ReadToggleState(const WCHAR* path, WindowRecord* records, DWORD* count) {
    HANDLE file = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
        FILE_ATTRIBUTE_NORMAL, nullptr);
    if(file == INVALID_HANDLE_VALUE) {
        return FALSE;
    }

    LARGE_INTEGER fileSize = {};
    ToggleStateHeader header = {};
    DWORD bytesRead = 0;
    // ヘッダー、件数、ファイルサイズを検証してからレコードを読み込む。
    BOOL valid = GetFileSizeEx(file, &fileSize) &&
        ReadFile(file, &header, sizeof(header), &bytesRead, nullptr) &&
        bytesRead == sizeof(header) &&
        header.signature == TOGGLE_STATE_SIGNATURE &&
        header.count <= MAX_TRACKED_WINDOWS &&
        fileSize.QuadPart == sizeof(header) + (LONGLONG)header.count * sizeof(WindowRecord);

    if(valid && header.count > 0) {
        DWORD bytes = header.count * sizeof(WindowRecord);
        valid = ReadFile(file, records, bytes, &bytesRead, nullptr) && bytesRead == bytes;
    }
    CloseHandle(file);

    if(!valid) {
        DeleteFileW(path);
        return FALSE;
    }

    *count = header.count;
    return TRUE;
}

// EnumWindows のコールバックに渡す検索条件と結果。
struct EnumData {
    HMONITOR primaryMonitor;
    HWND shell;
    HWND desktop;
    WindowRecord* records;
    DWORD count;
};

static BOOL CALLBACK EnumWindowsProc_Minimize(HWND hWnd, LPARAM lParam) {
    EnumData* p = reinterpret_cast<EnumData*>(lParam);

    if(hWnd == p->shell || hWnd == p->desktop)
        return TRUE;
    if(!IsWindowVisible(hWnd) || IsIconic(hWnd))
        return TRUE;

    int len = GetWindowTextLengthW(hWnd);
    if(len == 0)
        return TRUE; // タイトルが空のウィンドウはスキップ

    DWORD pid = 0;
    GetWindowThreadProcessId(hWnd, &pid);
    if(pid == GetCurrentProcessId())
        return TRUE; // 自分自身のウィンドウは除外

    if(MonitorFromWindow(hWnd, MONITOR_DEFAULTTONEAREST) == p->primaryMonitor) {
        if(p->count < MAX_TRACKED_WINDOWS) {
            p->records[p->count].window = (UINT64)(UINT_PTR)hWnd;
            p->records[p->count].processId = pid;
            ++p->count;
        }
    }

    return TRUE;
}

void MinimizeWindowsOnPrimary() {
    WCHAR statePath[MAX_PATH];
    if(!GetToggleStatePath(statePath, ARRAYSIZE(statePath)))
        return;

    HANDLE mutex = CreateMutexW(nullptr, FALSE, L"Local\\TZShowDesktop.Toggle");
    if(!mutex)
        return;

    DWORD waitResult = WaitForSingleObject(mutex, INFINITE);
    if(waitResult != WAIT_OBJECT_0 && waitResult != WAIT_ABANDONED) {
        CloseHandle(mutex);
        return;
    }

    WindowRecord records[MAX_TRACKED_WINDOWS] = {};
    // 保存済みのウィンドウがあれば、ハンドルと PID を照合して復元する。
    DWORD savedCount = 0;
    BOOL restoredAny = FALSE;
    if(ReadToggleState(statePath, records, &savedCount)) {
        for(DWORD i = 0; i < savedCount; ++i) {
            HWND window = (HWND)(UINT_PTR)records[i].window;
            DWORD processId = 0;
            if(IsWindow(window)) {
                GetWindowThreadProcessId(window, &processId);
                if(processId == records[i].processId) {
                    ShowWindow(window, SW_RESTORE);
                    restoredAny = TRUE;
                }
            }
        }
        DeleteFileW(statePath);
    }

    if(!restoredAny) {
        HMONITOR primaryMonitor = nullptr;
        EnumDisplayMonitors(
            nullptr,
            nullptr,
            [](HMONITOR monitor, HDC, LPRECT, LPARAM parameter) -> BOOL {
                MONITORINFO monitorInfo = {};
                monitorInfo.cbSize = sizeof(monitorInfo);
                if(!GetMonitorInfoW(monitor, &monitorInfo))
                    return TRUE;

                if(monitorInfo.dwFlags & MONITORINFOF_PRIMARY) {
                    *reinterpret_cast<HMONITOR*>(parameter) = monitor;
                    return FALSE;
                }
                return TRUE;
            },
            reinterpret_cast<LPARAM>(&primaryMonitor));

        if(primaryMonitor) {
            EnumData data = {};
            data.primaryMonitor = primaryMonitor;
            data.shell = GetShellWindow();
            data.desktop = GetDesktopWindow();
            data.records = records;
            EnumWindows(EnumWindowsProc_Minimize, reinterpret_cast<LPARAM>(&data));

            if(data.count > 0) {
                if(SaveToggleState(statePath, records, data.count)) {
                    for(DWORD i = 0; i < data.count; ++i) {
                        HWND window = (HWND)(UINT_PTR)records[i].window;
                        DWORD processId = 0;
                        if(!IsWindow(window))
                            continue;

                        GetWindowThreadProcessId(window, &processId);
                        if(processId == records[i].processId &&
                            MonitorFromWindow(window, MONITOR_DEFAULTTONEAREST) == primaryMonitor) {
                            ShowWindow(window, SW_MINIMIZE);
                        }
                    }
                } else {
                    DeleteFileW(statePath);
                }
            }
        }
    }

    ReleaseMutex(mutex);
    CloseHandle(mutex);
}
