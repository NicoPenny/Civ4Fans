
#include <windows.h>
#include <shellapi.h>
#include <gdiplus.h>
#include <objidl.h>
#include <cwchar>
#include <cstring>
#include "C4F_ChatPanel.h"
#include "C4F_IRC.h"
#include <shlobj.h>

#define IDI_APP_ICON 101
#define IDB_LAUNCHER_BACKGROUND 102

#pragma comment(lib, "gdiplus.lib")
#pragma comment(lib, "ole32.lib")

#pragma comment(lib, "Shell32.lib")
#pragma comment(lib, "Ole32.lib")

using namespace Gdiplus;

// ============================================================
// CHANGE THIS NUMBER ONLY WHEN UPGRADING THE GAME VERSION
// ============================================================

const int GAME_VERSION = 48;

// ============================================================
// APPLICATION NAME — CHANGE HERE
// ============================================================

const wchar_t APP_NAME[] = L"Civ4Fans";

const int LARGE_WINDOW_WIDTH  = 1560;
const int LARGE_WINDOW_HEIGHT = 1024;
const int COMPACT_WINDOW_WIDTH  = 1360;
const int COMPACT_WINDOW_HEIGHT = 820;

int g_windowClientWidth = COMPACT_WINDOW_WIDTH;
int g_windowClientHeight = COMPACT_WINDOW_HEIGHT;
wchar_t g_currentIrcNickname[64] = L"";
// Last nickname is persisted locally to pre-populate future connection prompts.
wchar_t g_lastEnteredNickname[64] = L"";

// Fixed-size window: keep the normal title bar, minimise and close buttons,
// but remove the resize frame and maximise button.
const DWORD WINDOW_STYLE =
    (WS_OVERLAPPEDWINDOW & ~(WS_THICKFRAME | WS_MAXIMIZEBOX)) | WS_CLIPCHILDREN;

// Windows 10 does not expose reliable per-window caption text/button colours.
// Use a small owner-drawn title bar there; keep the native styled caption on 11+.
bool g_useCustomTitleBar = false;
const int CUSTOM_TITLEBAR_HEIGHT = 32;
int g_titleBarHoverButton = 0; // 1=minimise, 2=close
int g_titleBarPressedButton = 0;

// ============================================================
// PANEL COLOURS
// ============================================================

const COLORREF BG_COLOR    = RGB(25, 29, 36);
const COLORREF PANEL_COLOR = RGB(43, 48, 57);
const COLORREF PANEL_HOVER = RGB(57, 64, 75);
const COLORREF GOLD_COLOR  = RGB(196, 158, 82);
const COLORREF TEXT_COLOR  = RGB(240, 240, 235);

bool IsWindows10OrEarlier()
{
    typedef LONG (WINAPI *RtlGetVersionFn)(OSVERSIONINFOEXW*);
    HMODULE ntdll = GetModuleHandle(L"ntdll.dll");
    if (!ntdll) return false;
    RtlGetVersionFn rtlGetVersion = (RtlGetVersionFn)GetProcAddress(ntdll, "RtlGetVersion");
    if (!rtlGetVersion) return false;
    OSVERSIONINFOEXW version = {};
    version.dwOSVersionInfoSize = sizeof(version);
    if (rtlGetVersion(&version) != 0) return false;
    // Windows 11 reports build 22000 or later (still version 10.0).
    return version.dwMajorVersion < 10 ||
        (version.dwMajorVersion == 10 && version.dwBuildNumber < 22000);
}

void DrawWindows10CustomTitleBar(HDC hdc, int width)
{
    if (!g_useCustomTitleBar) return;
    RECT bar = { 0, 0, width, CUSTOM_TITLEBAR_HEIGHT };
    HBRUSH background = CreateSolidBrush(RGB(24, 25, 28));
    FillRect(hdc, &bar, background);
    DeleteObject(background);

    HPEN accent = CreatePen(PS_SOLID, 1, RGB(196, 158, 82));
    HPEN oldPen = (HPEN)SelectObject(hdc, accent);
    MoveToEx(hdc, 0, CUSTOM_TITLEBAR_HEIGHT - 1, NULL);
    LineTo(hdc, width, CUSTOM_TITLEBAR_HEIGHT - 1);
    SelectObject(hdc, oldPen);
    DeleteObject(accent);

    // Restore the application's icon in the custom Windows 10 title bar.
    HICON appIcon = (HICON)LoadImage(GetModuleHandle(NULL),
        MAKEINTRESOURCE(IDI_APP_ICON), IMAGE_ICON, 20, 20, 0);
    if (appIcon != NULL)
    {
        DrawIconEx(hdc, 10, 6, appIcon, 20, 20, 0, NULL, DI_NORMAL);
        DestroyIcon(appIcon);
    }

    HFONT font = CreateFont(16, 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE, FALSE,
        DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
        CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Segoe UI");
    HFONT oldFont = (HFONT)SelectObject(hdc, font);
    SetBkMode(hdc, TRANSPARENT);
    SetTextColor(hdc, RGB(245, 211, 143));
    // Align the title with the left edge of the main option panels (x=40).
    RECT title = { 40, 0, width - 96, CUSTOM_TITLEBAR_HEIGHT - 1 };
    DrawText(hdc, L"Civ4Fans", -1, &title, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
    SelectObject(hdc, oldFont);
    DeleteObject(font);

    const int buttonWidth = 44;
    RECT minButton = { width - buttonWidth * 2, 0, width - buttonWidth, CUSTOM_TITLEBAR_HEIGHT - 1 };
    RECT closeButton = { width - buttonWidth, 0, width, CUSTOM_TITLEBAR_HEIGHT - 1 };
    if (g_titleBarHoverButton == 1)
    {
        HBRUSH hover = CreateSolidBrush(RGB(65, 59, 47)); FillRect(hdc, &minButton, hover); DeleteObject(hover);
    }
    else if (g_titleBarHoverButton == 2)
    {
        HBRUSH hover = CreateSolidBrush(RGB(150, 45, 38)); FillRect(hdc, &closeButton, hover); DeleteObject(hover);
    }

    HPEN glyph = CreatePen(PS_SOLID, 2, RGB(245, 211, 143));
    oldPen = (HPEN)SelectObject(hdc, glyph);
    int cx = width - buttonWidth * 3 / 2;
    MoveToEx(hdc, cx - 6, 19, NULL); LineTo(hdc, cx + 6, 19);
    cx = width - buttonWidth / 2;
    MoveToEx(hdc, cx - 5, 11, NULL); LineTo(hdc, cx + 5, 21);
    MoveToEx(hdc, cx + 5, 11, NULL); LineTo(hdc, cx - 5, 21);
    SelectObject(hdc, oldPen);
    DeleteObject(glyph);
}

const int PANEL_LEFT = 40;
const int PANEL_RIGHT_MARGIN = 40;
int GetGamePanelRight(int clientWidth)
{
    // Keep the four game panels the same width as the lobby-chat toggle.
    // The chat panel is positioned to the right with a fixed 20px gap.
    return ChatPanel_IsEnabled()
        ? 650
        : clientWidth - PANEL_RIGHT_MARGIN;
}

// ============================================================
// GENERATED VERSION STRINGS
// ============================================================

wchar_t g_versionDisplay[16];
wchar_t g_versionCompact[16];
wchar_t g_versionFolder[64];
wchar_t g_windowTitle[64];
wchar_t g_heading[64];
wchar_t g_panel1Subtitle[64];
wchar_t g_panel2Subtitle[96];
wchar_t g_panel3Subtitle[64];
wchar_t g_panel4Subtitle[64];

// ============================================================
// GLOBALS
// ============================================================

Image* g_background = NULL;
IStream* g_backgroundStream = NULL; // Must stay alive while GDI+ uses the image stream
ULONG_PTR g_gdiplusToken = 0;

bool g_largeIconChecked = false;
bool g_checkboxHovered = false;
bool g_cacheResetChecked = false;
bool g_cacheCheckboxHovered = false;
bool g_gameDirectoryHovered = false;

// Civ4Fans preferences are stored separately from the game/mod settings.
// This keeps them available across launches without changing any Civ4 files.
wchar_t g_c4fSettingsPath[MAX_PATH] = L"";
wchar_t g_gameRootPath[MAX_PATH] = L"";
wchar_t g_gameInstallPath[MAX_PATH] = L"";
bool g_gameRootManuallySelected = false;
// Older game version selected for the entire launcher.
// Zero means no older version has been selected.
int g_preferredGameVersion = 0;

bool InitialiseC4FSettingsPath()
{
    wchar_t localAppData[MAX_PATH] = L"";
    DWORD length = GetEnvironmentVariableW(L"LOCALAPPDATA", localAppData, MAX_PATH);
    if (length == 0 || length >= MAX_PATH)
    {
        g_c4fSettingsPath[0] = L'\0';
        return false;
    }

    wchar_t settingsDirectory[MAX_PATH] = L"";
    if (swprintf_s(settingsDirectory, MAX_PATH, L"%s\\Civ4Fans", localAppData) < 0)
    {
        g_c4fSettingsPath[0] = L'\0';
        return false;
    }

    if (!CreateDirectoryW(settingsDirectory, NULL))
    {
        DWORD error = GetLastError();
        if (error != ERROR_ALREADY_EXISTS)
        {
            g_c4fSettingsPath[0] = L'\0';
            return false;
        }
    }

    DWORD attributes = GetFileAttributesW(settingsDirectory);
    if (attributes == INVALID_FILE_ATTRIBUTES ||
        (attributes & FILE_ATTRIBUTE_DIRECTORY) == 0)
    {
        g_c4fSettingsPath[0] = L'\0';
        return false;
    }

    if (swprintf_s(g_c4fSettingsPath, MAX_PATH,
        L"%s\\Civ4Fans.ini", settingsDirectory) < 0)
    {
        g_c4fSettingsPath[0] = L'\0';
        return false;
    }

    return true;
}

void LoadC4FSettings()
{
    if (g_c4fSettingsPath[0] == L'\0')
        return;

    g_cacheResetChecked = GetPrivateProfileIntW(
        L"Preferences", L"ForceCacheReset", 0, g_c4fSettingsPath) != 0;

    g_largeIconChecked = GetPrivateProfileIntW(
        L"Preferences", L"LargeResourceIcon", 0, g_c4fSettingsPath) != 0;

    bool lobbyChatEnabled = GetPrivateProfileIntW(
        L"Preferences", L"EnableLobbyChat", 0, g_c4fSettingsPath) != 0;
    ChatPanel_SetEnabled(lobbyChatEnabled);

    GetPrivateProfileStringW(L"Preferences", L"LastNickname", L"",
        g_lastEnteredNickname, 64, g_c4fSettingsPath);

	GetPrivateProfileStringW(L"Preferences", L"GameRoot", L"",
		g_gameRootPath, MAX_PATH, g_c4fSettingsPath);

	GetPrivateProfileStringW(L"Preferences", L"GameInstallPath", L"",
		g_gameInstallPath, MAX_PATH, g_c4fSettingsPath);

	g_gameRootManuallySelected = GetPrivateProfileIntW(
		L"Preferences", L"GameRootManual", 0, g_c4fSettingsPath) != 0;

	g_preferredGameVersion = (int)GetPrivateProfileIntW(
		L"Preferences",
		L"OlderGameVersion",
		0,
		g_c4fSettingsPath);
}

// Write the complete preference file with ordinary Win32 file I/O.
// This avoids the profile API's INI caching/flush behaviour.
bool SaveC4FSetting(const wchar_t* name, const wchar_t* value)
{
    // Callers update the in-memory preference first. Rewrite the complete
    // file so saving one setting cannot discard the others.
    (void)name;
    (void)value;

    if (g_c4fSettingsPath[0] == L'\0')
    {
        OutputDebugStringW(L"Civ4Fans: preferences save skipped; settings path is empty.\n");
        return false;
    }

    wchar_t contents[1024];
    contents[0] = (wchar_t)0xFEFF; // UTF-16 little-endian BOM
    int chars = swprintf_s(contents + 1, 1023,
        L"[Preferences]\r\n"
        L"ForceCacheReset=%d\r\n"
        L"LargeResourceIcon=%d\r\n"
        L"EnableLobbyChat=%d\r\n"
		L"LastNickname=%s\r\n"
		L"GameRoot=%s\r\n"
		L"GameInstallPath=%s\r\n"
		L"GameRootManual=%d\r\n"
		L"OlderGameVersion=%d\r\n",
        g_cacheResetChecked ? 1 : 0,
        g_largeIconChecked ? 1 : 0,
        ChatPanel_IsEnabled() ? 1 : 0,
		g_lastEnteredNickname,
		g_gameRootPath,
		g_gameInstallPath,
		g_gameRootManuallySelected ? 1 : 0,
		g_preferredGameVersion);

    if (chars < 0)
    {
        OutputDebugStringW(L"Civ4Fans: could not format preferences file.\n");
        return false;
    }

    DWORD bytesToWrite = (DWORD)((chars + 1) * sizeof(wchar_t));
    HANDLE file = CreateFileW(g_c4fSettingsPath, GENERIC_WRITE, FILE_SHARE_READ,
        NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (file == INVALID_HANDLE_VALUE)
    {
        wchar_t diagnostic[256];
        swprintf_s(diagnostic, 256,
            L"Civ4Fans: CreateFileW failed saving preferences (error %lu).\n",
            GetLastError());
        OutputDebugStringW(diagnostic);
        return false;
    }

    DWORD bytesWritten = 0;
    BOOL writeOk = WriteFile(file, contents, bytesToWrite, &bytesWritten, NULL);
    DWORD writeError = writeOk ? ERROR_SUCCESS : GetLastError();
    BOOL flushOk = writeOk && bytesWritten == bytesToWrite
        ? FlushFileBuffers(file) : FALSE;
    DWORD flushError = flushOk ? ERROR_SUCCESS : GetLastError();
    CloseHandle(file);

    if (!writeOk || bytesWritten != bytesToWrite || !flushOk)
    {
        wchar_t diagnostic[256];
        swprintf_s(diagnostic, 256,
            L"Civ4Fans: preference file write failed (write error %lu, flush error %lu, %lu/%lu bytes).\n",
            writeError, flushError, bytesWritten, bytesToWrite);
        OutputDebugStringW(diagnostic);
        return false;
    }

    return true;
}

void SaveC4FBooleanSetting(const wchar_t* name, bool value)
{
    // Preference persistence failures are deliberately silent for end users.
    // Diagnostics remain available to developers through the debug output.
    if (!SaveC4FSetting(name, value ? L"1" : L"0"))
        OutputDebugStringW(L"Civ4Fans: preference change could not be saved.\n");
}

void SaveAllC4FSettings()
{
    if (g_c4fSettingsPath[0] == L'\0')
        return;

    if (!SaveC4FSetting(L"", L""))
        OutputDebugStringW(L"Civ4Fans: final preferences save failed.\n");
}

// Apply the Civ4Fans dark-and-gold colours to the native Windows title bar.
// Dynamic loading keeps the project compatible with the older VS2010 SDK.
void ApplyCiv4FansTitleBar(HWND hwnd)
{
    HMODULE dwm = LoadLibrary(L"dwmapi.dll");
    if (!dwm) return;

    typedef HRESULT (WINAPI *DwmSetWindowAttributeFn)(
        HWND, DWORD, LPCVOID, DWORD);
    DwmSetWindowAttributeFn setAttribute =
        (DwmSetWindowAttributeFn)GetProcAddress(dwm, "DwmSetWindowAttribute");

    if (setAttribute)
    {
        // Windows 10 supports immersive dark title bars on recent builds.
        // Attribute 20 is used by current Windows 10; 19 is a compatibility
        // fallback for some earlier builds. Windows 11 additionally supports
        // the custom caption, text and border colours below.
        const DWORD DWMWA_USE_IMMERSIVE_DARK_MODE_LOCAL = 20;
        const DWORD DWMWA_USE_IMMERSIVE_DARK_MODE_OLD_LOCAL = 19;
        BOOL darkMode = TRUE;
        HRESULT darkResult = setAttribute(hwnd,
            DWMWA_USE_IMMERSIVE_DARK_MODE_LOCAL,
            &darkMode, sizeof(darkMode));
        if (FAILED(darkResult))
        {
            setAttribute(hwnd, DWMWA_USE_IMMERSIVE_DARK_MODE_OLD_LOCAL,
                &darkMode, sizeof(darkMode));
        }

        const DWORD DWMWA_BORDER_COLOR_LOCAL = 34;
        const DWORD DWMWA_CAPTION_COLOR_LOCAL = 35;
        const DWORD DWMWA_TEXT_COLOR_LOCAL = 36;
        COLORREF border = RGB(196, 158, 82);
        COLORREF caption = RGB(24, 25, 28);
        COLORREF text = RGB(235, 207, 151);

        // These three colour attributes are supported on Windows 11.
        // Windows 10 ignores them and uses its dark title-bar treatment.
        setAttribute(hwnd, DWMWA_BORDER_COLOR_LOCAL, &border, sizeof(border));
        setAttribute(hwnd, DWMWA_CAPTION_COLOR_LOCAL, &caption, sizeof(caption));
        setAttribute(hwnd, DWMWA_TEXT_COLOR_LOCAL, &text, sizeof(text));
    }

    FreeLibrary(dwm);
}

// Checkbox is positioned dynamically above the bottom-anchored chat toggle.
const int CHECKBOX_HEIGHT = 52;
const int CHECKBOX_BOTTOM_GAP = 8;

RECT GetLargeIconCheckboxRect(int clientWidth, int clientHeight)
{
    RECT r;
    int toggleTop = clientHeight - 10 - 48;
    r.left = 40;
    r.right = 650;
    r.bottom = toggleTop - CHECKBOX_BOTTOM_GAP;
    r.top = r.bottom - CHECKBOX_HEIGHT;
    return r;
}

RECT GetCacheResetCheckboxRect(int clientWidth, int clientHeight)
{
    RECT r = GetLargeIconCheckboxRect(clientWidth, clientHeight);
    r.bottom = r.top - CHECKBOX_BOTTOM_GAP;
    r.top = r.bottom - CHECKBOX_HEIGHT;
    return r;
}

RECT GetGameDirectoryRect(int clientWidth, int clientHeight)
{
    RECT r = GetCacheResetCheckboxRect(clientWidth, clientHeight);
    r.bottom = r.top - CHECKBOX_BOTTOM_GAP;
    r.top = r.bottom - CHECKBOX_HEIGHT;
    return r;
}

// ============================================================
// LOAD EMBEDDED BACKGROUND IMAGE
// ============================================================

Image* LoadBackgroundFromResource(HINSTANCE hInstance)
{
    HRSRC resourceInfo = FindResource(
        hInstance,
        MAKEINTRESOURCE(IDB_LAUNCHER_BACKGROUND),
        RT_RCDATA);

    if (resourceInfo == NULL)
    {
        wchar_t message[256];
        swprintf_s(message, 256,
            L"The embedded background resource was not found.\n\n"
            L"Resource ID: %d\nResource type: RCDATA\n\n"
            L"Check that the RCDATA line is in the .rc file used by this build.",
            IDB_LAUNCHER_BACKGROUND);
        MessageBox(NULL, message, L"BTG Launcher - Background Error",
            MB_OK | MB_ICONERROR);
        return NULL;
    }

    DWORD resourceSize = SizeofResource(hInstance, resourceInfo);
    if (resourceSize == 0)
    {
        MessageBox(NULL,
            L"The embedded background resource was found, but its size is zero.",
            L"BTG Launcher - Background Error", MB_OK | MB_ICONERROR);
        return NULL;
    }

    HGLOBAL loadedResource = LoadResource(hInstance, resourceInfo);
    if (loadedResource == NULL)
    {
        MessageBox(NULL,
            L"Windows found the background resource but could not load it.",
            L"BTG Launcher - Background Error", MB_OK | MB_ICONERROR);
        return NULL;
    }

    const void* resourceData = LockResource(loadedResource);
    if (resourceData == NULL)
    {
        MessageBox(NULL,
            L"Windows loaded the background resource but could not access its data.",
            L"BTG Launcher - Background Error", MB_OK | MB_ICONERROR);
        return NULL;
    }

    HGLOBAL imageMemory = GlobalAlloc(GMEM_MOVEABLE, resourceSize);
    if (imageMemory == NULL)
    {
        MessageBox(NULL,
            L"Not enough memory to load the embedded background.",
            L"BTG Launcher - Background Error", MB_OK | MB_ICONERROR);
        return NULL;
    }

    void* memoryPointer = GlobalLock(imageMemory);
    if (memoryPointer == NULL)
    {
        GlobalFree(imageMemory);
        MessageBox(NULL,
            L"Windows could not allocate a memory buffer for the background.",
            L"BTG Launcher - Background Error", MB_OK | MB_ICONERROR);
        return NULL;
    }

    memcpy(memoryPointer, resourceData, resourceSize);
    GlobalUnlock(imageMemory);

    // CreateStreamOnHGlobal takes ownership of imageMemory on success.
    HRESULT hr = CreateStreamOnHGlobal(imageMemory, TRUE, &g_backgroundStream);
    if (FAILED(hr))
    {
        GlobalFree(imageMemory);
        wchar_t message[128];
        swprintf_s(message, 128,
            L"Could not create the image stream. HRESULT: 0x%08X",
            (unsigned int)hr);
        MessageBox(NULL, message, L"BTG Launcher - Background Error",
            MB_OK | MB_ICONERROR);
        return NULL;
    }

    // Keep g_backgroundStream alive until after the GDI+ Image is deleted.
    Image* image = Image::FromStream(g_backgroundStream, FALSE);
    if (image == NULL || image->GetLastStatus() != Ok)
    {
        Status imageStatus = image == NULL ? GenericError : image->GetLastStatus();
        delete image;
        g_backgroundStream->Release();
        g_backgroundStream = NULL;

        wchar_t message[160];
        swprintf_s(message, 160,
            L"The background resource was found (%lu bytes), but GDI+ could not decode it.\n\nGDI+ status: %d\n\nCheck that the resource contains the actual PNG file.",
            (unsigned long)resourceSize, (int)imageStatus);
        MessageBox(NULL, message, L"BTG Launcher - Background Error",
            MB_OK | MB_ICONERROR);
        return NULL;
    }

    return image;
}

void DestroyBackground()
{
    delete g_background;
    g_background = NULL;

    if (g_backgroundStream != NULL)
    {
        g_backgroundStream->Release();
        g_backgroundStream = NULL;
    }
}

// ============================================================
// CLEAR THE CACHE FOR THE SELECTED GAME / MOD
// The user-data folder names match the mod folders used by the launcher.
// Only the contents of that exact cache directory are removed.
// ============================================================

bool ClearDirectoryContents(const wchar_t* directory)
{
    wchar_t searchPath[MAX_PATH];
    if (wcscpy_s(searchPath, MAX_PATH, directory) != 0 ||
        wcscat_s(searchPath, MAX_PATH, L"\\*") != 0)
        return false;

    WIN32_FIND_DATA data;
    HANDLE find = FindFirstFile(searchPath, &data);
    if (find == INVALID_HANDLE_VALUE)
        return GetLastError() == ERROR_FILE_NOT_FOUND ||
               GetLastError() == ERROR_NO_MORE_FILES;

    bool ok = true;
    do
    {
        if (wcscmp(data.cFileName, L".") == 0 ||
            wcscmp(data.cFileName, L"..") == 0)
            continue;

        wchar_t itemPath[MAX_PATH];
        if (swprintf_s(itemPath, MAX_PATH, L"%s\\%s",
            directory, data.cFileName) < 0)
        {
            ok = false;
            break;
        }

        if ((data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0)
        {
            if (!ClearDirectoryContents(itemPath) || !RemoveDirectory(itemPath))
            {
                ok = false;
                break;
            }
        }
        else
        {
            SetFileAttributes(itemPath, FILE_ATTRIBUTE_NORMAL);
            if (!DeleteFile(itemPath))
            {
                ok = false;
                break;
            }
        }
    }
    while (FindNextFile(find, &data));

    FindClose(find);
    return ok;
}

bool ClearSelectedGameCache(const wchar_t* userDataFolder)
{
    wchar_t localAppData[MAX_PATH];
    DWORD length = GetEnvironmentVariable(L"LOCALAPPDATA",
        localAppData, MAX_PATH);
    if (length == 0 || length >= MAX_PATH)
        return false;

    wchar_t cachePath[MAX_PATH];
    if (swprintf_s(cachePath, MAX_PATH, L"%s\\My Games\\%s\\cache",
        localAppData, userDataFolder) < 0)
        return false;

    DWORD attributes = GetFileAttributes(cachePath);
    if (attributes == INVALID_FILE_ATTRIBUTES)
    {
        // A cache that has not yet been created has nothing to clear.
        return GetLastError() == ERROR_FILE_NOT_FOUND ||
               GetLastError() == ERROR_PATH_NOT_FOUND;
    }
    if ((attributes & FILE_ATTRIBUTE_DIRECTORY) == 0)
        return false;

    return ClearDirectoryContents(cachePath);
}

// ============================================================
// BROWSE FOR CIV4 INSTALLATION ROOT
// ============================================================

static BOOL BuildGamePath(
    const wchar_t* root,
    const wchar_t* folder,
    const wchar_t* exe,
    wchar_t* path,
    size_t pathSize);

static BOOL BrowseForGameRoot(HWND owner, wchar_t* selectedPath,
                              size_t pathSize)
{
    BROWSEINFOW browseInfo;
    LPITEMIDLIST folderID;
    BOOL success = FALSE;

    wchar_t browseTitle[256];

    swprintf_s(
        browseTitle,
        _countof(browseTitle),
		L"Please select a folder named \"Beyond The Game 2.xx\".");

    ZeroMemory(&browseInfo, sizeof(browseInfo));

    browseInfo.hwndOwner = owner;
    browseInfo.lpszTitle = browseTitle;
    browseInfo.ulFlags =
        BIF_RETURNONLYFSDIRS | BIF_NEWDIALOGSTYLE;

    folderID = SHBrowseForFolderW(&browseInfo);

    if (folderID)
    {
        if (SHGetPathFromIDListW(folderID, selectedPath))
        {
            size_t length = wcslen(selectedPath);

			if (length < pathSize)
			{
				wchar_t* lastSlash =
					wcsrchr(selectedPath, L'\\');

				const wchar_t* versionPrefix = L"Beyond The Game 2.";
				const wchar_t* versionName =
					lastSlash ? lastSlash + 1 : selectedPath;

				size_t prefixLength = wcslen(versionPrefix);

				BOOL validVersion =
					lastSlash &&
					_wcsnicmp(versionName, versionPrefix, prefixLength) == 0;

				const wchar_t* versionNumber =
					validVersion ? versionName + prefixLength : L"";

				if (*versionNumber == L'\0')
					validVersion = FALSE;

				for (const wchar_t* p = versionNumber; *p; ++p)
				{
					if (*p < L'0' || *p > L'9')
					{
						validVersion = FALSE;
						break;
					}
				}

				if (validVersion)
				{
					// Preserve the complete selected version-folder path.
					wcscpy_s(g_gameInstallPath, MAX_PATH, selectedPath);

					// Keep the parent root for the existing version-search logic.
					*lastSlash = L'\0';
					success = TRUE;
				}
			}
        }

        CoTaskMemFree(folderID);
    }

    return success;
}

static BOOL BuildSelectedInstallGamePath(
    const wchar_t* root,
    const wchar_t* folder,
    const wchar_t* exe,
    wchar_t* path,
    size_t pathSize)
{
    if (!g_gameInstallPath[0])
        return FALSE;

    const wchar_t* selectedVersionFolder =
        wcsrchr(g_gameInstallPath, L'\\');

    if (!selectedVersionFolder)
        return FALSE;

    ++selectedVersionFolder;

    // Retain the game-specific part of the original relative folder.
    const wchar_t* gameSubfolder =
        folder + wcslen(g_versionFolder);

    wchar_t selectedFolder[MAX_PATH];

    if (swprintf_s(selectedFolder, MAX_PATH, L"%s%s",
        selectedVersionFolder, gameSubfolder) < 0)
        return FALSE;

    return BuildGamePath(root, selectedFolder, exe, path, pathSize);
}


// ============================================================
// CHECK WHETHER A FOLDER EXISTS
// ============================================================

static BOOL GameFolderExists(const wchar_t* path)
{
    DWORD attributes = GetFileAttributesW(path);

    return attributes != INVALID_FILE_ATTRIBUTES &&
           (attributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
}

// ============================================================
// FIND THE NEWEST OLDER INSTALLATION
// ============================================================

static BOOL FindOlderGameVersion(
    const wchar_t* folder,
    const wchar_t* exe,
    int requestedVersion,
    wchar_t* olderFolder,
    wchar_t* olderExe,
    wchar_t* olderVersion)
{
    wchar_t searchPath[MAX_PATH];
    wchar_t relativeFolder[MAX_PATH];
    wchar_t bestFolder[MAX_PATH] = L"";
    wchar_t bestExe[MAX_PATH] = L"";
    wchar_t bestVersion[16] = L"";

    int bestNumber = -1;

    const wchar_t* relative =
        folder + wcslen(g_versionFolder);

    wcscpy_s(relativeFolder, MAX_PATH, relative);

    swprintf_s(searchPath, MAX_PATH,
        L"%s\\Beyond The Game 2.*", g_gameRootPath);

    WIN32_FIND_DATAW data;
    HANDLE handle = FindFirstFileW(searchPath, &data);

    if (handle == INVALID_HANDLE_VALUE)
        return FALSE;

    do
    {
        if (!(data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY))
            continue;

        const wchar_t* prefix = L"Beyond The Game 2.";

        if (_wcsnicmp(data.cFileName, prefix,
                      wcslen(prefix)) != 0)
            continue;

        const wchar_t* versionText =
			data.cFileName + wcslen(prefix);

		if (!versionText[0])
			continue;

		BOOL validVersion = TRUE;

		for (const wchar_t* p = versionText; *p; ++p)
		{
			if (*p < L'0' || *p > L'9')
			{
				validVersion = FALSE;
				break;
			}
		}

		if (!validVersion)
			continue;

		int versionNumber = _wtoi(versionText);

		if (versionNumber < 0 ||
			versionNumber >= GAME_VERSION)
			continue;

		if (requestedVersion > 0)
		{
			// When a version is requested, accept only that exact version.
			if (versionNumber != requestedVersion)
				continue;
		}
		else
		{
			// Otherwise, keep searching for the highest older version.
			if (versionNumber <= bestNumber)
				continue;
		}

        wchar_t candidateFolder[MAX_PATH];
        wchar_t candidateExe[MAX_PATH];
        wchar_t candidatePath[MAX_PATH];
        //wchar_t compactVersion[16];

        swprintf_s(candidateFolder, MAX_PATH,
            L"%s%s", data.cFileName, relativeFolder);

		// Executable names are now independent of the game version.
		wcscpy_s(candidateExe, MAX_PATH, exe);

        swprintf_s(candidatePath, MAX_PATH,
            L"%s\\%s%s",
            g_gameRootPath,
            candidateFolder,
            candidateExe);

        DWORD attributes = GetFileAttributesW(candidatePath);

        if (attributes != INVALID_FILE_ATTRIBUTES &&
            !(attributes & FILE_ATTRIBUTE_DIRECTORY))
        {
            bestNumber = versionNumber;

            wcscpy_s(bestFolder, MAX_PATH, candidateFolder);
            wcscpy_s(bestExe, MAX_PATH, candidateExe);

            swprintf_s(bestVersion, 16, L"2.%02d",
                versionNumber);
        }

    } while (FindNextFileW(handle, &data));

    FindClose(handle);

    if (bestNumber < 0)
        return FALSE;

    wcscpy_s(olderFolder, MAX_PATH, bestFolder);
    wcscpy_s(olderExe, MAX_PATH, bestExe);
    wcscpy_s(olderVersion, 16, bestVersion);

    return TRUE;
}


// ============================================================
// BUILD A COMPLETE GAME EXECUTABLE PATH
// ============================================================

static BOOL BuildGamePath(
    const wchar_t* root,
    const wchar_t* folder,
    const wchar_t* exe,
    wchar_t* path,
    size_t pathSize)
{
    if (!root || !root[0] || !folder || !exe)
        return FALSE;

    if (wcscpy_s(path, pathSize, root) != 0)
        return FALSE;

    size_t length = wcslen(path);

    if (length > 0 && path[length - 1] != L'\\')
    {
        if (wcscat_s(path, pathSize, L"\\") != 0)
            return FALSE;
    }

    if (wcscat_s(path, pathSize, folder) != 0)
        return FALSE;

    if (wcscat_s(path, pathSize, exe) != 0)
        return FALSE;

    return TRUE;
}

// ============================================================
// C4F CUSTOM DARK/GOLD PROMPT
// ============================================================

struct C4FPromptData
{
    const wchar_t* title;
    const wchar_t* message;
    const wchar_t* leftButton;
    const wchar_t* rightButton;
    int result;
};

static const wchar_t C4F_PROMPT_CLASS[] = L"C4FCustomPromptWindow";

static LRESULT CALLBACK C4FPromptProc(
    HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    C4FPromptData* data = (C4FPromptData*)GetWindowLongPtrW(
        hwnd, GWLP_USERDATA);

    if (msg == WM_NCCREATE)
    {
        CREATESTRUCTW* createInfo = (CREATESTRUCTW*)lParam;
        SetWindowLongPtrW(
            hwnd, GWLP_USERDATA,
            (LONG_PTR)createInfo->lpCreateParams);
        return DefWindowProcW(hwnd, msg, wParam, lParam);
    }

    switch (msg)
    {
    case WM_ERASEBKGND:
        return 1;

    case WM_PAINT:
    {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(hwnd, &ps);

        RECT client;
        GetClientRect(hwnd, &client);

        HBRUSH background = CreateSolidBrush(BG_COLOR);
        HBRUSH buttonBrush = CreateSolidBrush(PANEL_COLOR);
        HBRUSH borderBrush = CreateSolidBrush(GOLD_COLOR);
        HPEN goldPen = CreatePen(PS_SOLID, 1, GOLD_COLOR);

        FillRect(dc, &client, background);
        FrameRect(dc, &client, borderBrush);

        SetBkMode(dc, TRANSPARENT);

        HFONT titleFont = CreateFontW(
            20, 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE,
            DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
            CLIP_DEFAULT_PRECIS, DEFAULT_QUALITY,
            DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");

        HFONT bodyFont = CreateFontW(
            16, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
            DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
            CLIP_DEFAULT_PRECIS, DEFAULT_QUALITY,
            DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");

        HFONT oldFont = (HFONT)SelectObject(dc, titleFont);

        SetTextColor(dc, GOLD_COLOR);

        RECT titleRect = { 25, 18, client.right - 25, 50 };
        DrawTextW(dc, data->title, -1, &titleRect,
                  DT_LEFT | DT_VCENTER | DT_SINGLELINE);

        SelectObject(dc, bodyFont);
        SetTextColor(dc, TEXT_COLOR);

        RECT messageRect = { 25, 65, client.right - 25, 145 };
        DrawTextW(dc, data->message, -1, &messageRect,
                  DT_LEFT | DT_TOP | DT_WORDBREAK);

        RECT leftButton = {
            25, client.bottom - 58,
            245, client.bottom - 18
        };

        RECT rightButton = {
            275, client.bottom - 58,
            495, client.bottom - 18
        };

		FillRect(dc, &leftButton, buttonBrush);
		FillRect(dc, &rightButton, buttonBrush);

		HGDIOBJ oldBrush = SelectObject(dc, GetStockObject(NULL_BRUSH));
		HGDIOBJ oldPen = SelectObject(dc, goldPen);

		Rectangle(dc, leftButton.left, leftButton.top,
				  leftButton.right, leftButton.bottom);

		Rectangle(dc, rightButton.left, rightButton.top,
				  rightButton.right, rightButton.bottom);

		SelectObject(dc, oldBrush);
		SelectObject(dc, oldPen);

        SetTextColor(dc, TEXT_COLOR);

        RECT leftText = leftButton;
        RECT rightText = rightButton;

        SelectObject(dc, bodyFont);

        DrawTextW(dc, data->leftButton, -1, &leftText,
                  DT_CENTER | DT_VCENTER | DT_SINGLELINE);

        DrawTextW(dc, data->rightButton, -1, &rightText,
                  DT_CENTER | DT_VCENTER | DT_SINGLELINE);

        SelectObject(dc, oldFont);

        DeleteObject(titleFont);
        DeleteObject(bodyFont);
        DeleteObject(background);
        DeleteObject(buttonBrush);
        DeleteObject(borderBrush);
        DeleteObject(goldPen);

        EndPaint(hwnd, &ps);
        return 0;
    }

    case WM_LBUTTONUP:
        if (data)
        {
            int x = LOWORD(lParam);
            int y = HIWORD(lParam);

            RECT client;
            GetClientRect(hwnd, &client);

            if (y >= client.bottom - 58 &&
                y <= client.bottom - 18)
            {
                if (x >= 25 && x <= 245)
                {
                    data->result = IDYES;
                    DestroyWindow(hwnd);
                    return 0;
                }

                if (x >= 275 && x <= 495)
                {
                    data->result = IDNO;
                    DestroyWindow(hwnd);
                    return 0;
                }
            }
        }
        return 0;

    case WM_KEYDOWN:
        if (data && wParam == VK_RETURN)
        {
            data->result = IDYES;
            DestroyWindow(hwnd);
            return 0;
        }
        if (data && wParam == VK_ESCAPE)
        {
            data->result = IDNO;
            DestroyWindow(hwnd);
            return 0;
        }
        break;

    case WM_CLOSE:
        if (data)
            data->result = IDNO;
        DestroyWindow(hwnd);
        return 0;
    }

    return DefWindowProcW(hwnd, msg, wParam, lParam);
}


static int C4FShowPrompt(
    HWND owner,
    const wchar_t* title,
    const wchar_t* message,
    const wchar_t* leftButton,
    const wchar_t* rightButton)
{
    HINSTANCE instance = GetModuleHandleW(NULL);

    WNDCLASSEXW wc;
    ZeroMemory(&wc, sizeof(wc));

    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = C4FPromptProc;
    wc.hInstance = instance;
    wc.hCursor = LoadCursor(NULL, IDC_ARROW);
    wc.lpszClassName = C4F_PROMPT_CLASS;

    if (!RegisterClassExW(&wc) &&
        GetLastError() != ERROR_CLASS_ALREADY_EXISTS)
        return IDNO;

    C4FPromptData data;
    data.title = title;
    data.message = message;
    data.leftButton = leftButton;
    data.rightButton = rightButton;
    data.result = IDNO;

    const int windowWidth = 520;
    const int windowHeight = 230;

    int x = (GetSystemMetrics(SM_CXSCREEN) - windowWidth) / 2;
    int y = (GetSystemMetrics(SM_CYSCREEN) - windowHeight) / 2;

    if (owner && IsWindow(owner))
    {
        RECT ownerRect;
        if (GetWindowRect(owner, &ownerRect))
        {
            x = ownerRect.left +
                ((ownerRect.right - ownerRect.left) - windowWidth) / 2;
            y = ownerRect.top +
                ((ownerRect.bottom - ownerRect.top) - windowHeight) / 2;
        }
    }

    HWND prompt = CreateWindowExW(
        WS_EX_DLGMODALFRAME,
        C4F_PROMPT_CLASS,
        L"Civ4Fans",
        WS_POPUP | WS_BORDER,
        x, y, windowWidth, windowHeight,
        owner, NULL, instance, &data);

    if (!prompt)
        return IDNO;

    if (owner && IsWindow(owner))
        EnableWindow(owner, FALSE);

    ShowWindow(prompt, SW_SHOW);
    UpdateWindow(prompt);
    SetForegroundWindow(prompt);

    MSG msg;
    while (IsWindow(prompt))
    {
        BOOL result = GetMessageW(&msg, NULL, 0, 0);

        if (result <= 0)
        {
            if (result == 0)
                PostQuitMessage((int)msg.wParam);
            break;
        }

        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    if (owner && IsWindow(owner))
    {
        EnableWindow(owner, TRUE);
        SetForegroundWindow(owner);
    }

    return data.result;
}

// ============================================================
// LAUNCH GAME
// ============================================================

void LaunchGame(HWND hwnd, const wchar_t* folder,
                const wchar_t* exe, const wchar_t* userDataFolder)
{
    wchar_t path[MAX_PATH];
	wchar_t selectedFolder[MAX_PATH];
	wchar_t selectedExe[MAX_PATH];

	// Start with the manually selected version, or the current launcher version.
	if (g_gameRootManuallySelected && g_gameInstallPath[0] != L'\0')
	{
		const wchar_t* versionFolder =
			wcsrchr(g_gameInstallPath, L'\\');

		if (!versionFolder)
			return;

		++versionFolder;

		// Keep the game-specific subfolder and executable unchanged.
		swprintf_s(selectedFolder, MAX_PATH, L"%s%s",
			versionFolder, folder + wcslen(g_versionFolder));
	}
	else
	{
		wcscpy_s(selectedFolder, MAX_PATH, folder);
	}

	wcscpy_s(selectedExe, MAX_PATH, exe);

	// If an older version was selected for the whole launcher,
	// resolve that exact version before considering the current one.
	if (g_preferredGameVersion > 0)
	{
		wchar_t preferredFolder[MAX_PATH] = L"";
		wchar_t preferredExe[MAX_PATH] = L"";
		wchar_t preferredVersion[16] = L"";

		if (!FindOlderGameVersion(
				folder, exe,
				g_preferredGameVersion,
				preferredFolder, preferredExe, preferredVersion))
		{
			int choice = C4FShowPrompt(
				hwnd,
				L"Preferred installation not found",
				L"The previously selected game version is not available "
				L"for this game.\n\n"
				L"Would you like to locate the current installation?",
				L"Locate installation",
				L"Cancel");

			if (choice != IDYES)
				return;

			wchar_t newRoot[MAX_PATH] = L"";
			wchar_t newPath[MAX_PATH] = L"";

			if (!BrowseForGameRoot(hwnd, newRoot, MAX_PATH))
				return;

			if (!BuildSelectedInstallGamePath(
				newRoot, folder, exe, newPath, MAX_PATH))
				return;

			DWORD newAttributes = GetFileAttributesW(newPath);

			if (newAttributes == INVALID_FILE_ATTRIBUTES ||
				(newAttributes & FILE_ATTRIBUTE_DIRECTORY))
			{
				C4FShowPrompt(
					hwnd,
					L"Installation not found",
					L"The selected root folder does not contain the "
					L"current game installation.",
					L"OK",
					L"Cancel");
				return;
			}

			// Save the new root and reset the launcher-wide preference.
			wcscpy_s(g_gameRootPath, MAX_PATH, newRoot);
			g_preferredGameVersion = 0;
			g_gameRootManuallySelected = true;

			// Use the version selected in the folder picker.
			const wchar_t* selectedVersionFolder =
				wcsrchr(g_gameInstallPath, L'\\');

			if (!selectedVersionFolder)
				return;

			++selectedVersionFolder;

			swprintf_s(selectedFolder, MAX_PATH, L"%s%s",
				selectedVersionFolder, folder + wcslen(g_versionFolder));

			SaveC4FSetting(L"GameRoot", g_gameRootPath);
		}
		else
		{
			wcscpy_s(selectedFolder, MAX_PATH, preferredFolder);
			wcscpy_s(selectedExe, MAX_PATH, preferredExe);
		}
	}

	// Build the executable path from the saved installation root.
	if (!BuildGamePath(g_gameRootPath, selectedFolder, selectedExe,
					   path, MAX_PATH))
		return;

	// Check whether the selected executable exists.
	DWORD exeAttributes = GetFileAttributesW(path);

	if (exeAttributes == INVALID_FILE_ATTRIBUTES ||
		(exeAttributes & FILE_ATTRIBUTE_DIRECTORY))
	{
		wchar_t olderFolder[MAX_PATH] = L"";
		wchar_t olderExe[MAX_PATH] = L"";
		wchar_t olderVersion[16] = L"";

		if (FindOlderGameVersion(
				folder, exe,
				g_preferredGameVersion,
				olderFolder, olderExe, olderVersion))
		{
			// Ask whether to locate the newer installation or use the older one.
			wchar_t promptMessage[256];

			swprintf_s(
				promptMessage,
				_countof(promptMessage),
				L"Your saved installation is for an older version, version %s.\n\n"
				L"Do you want to locate the newer version?",
				olderVersion);

			int choice = C4FShowPrompt(
				hwnd,
				L"Older game installation",
				promptMessage,
				L"Locate newer version",
				L"Use older version");

			if (choice == IDYES)
			{
				wchar_t newRoot[MAX_PATH] = L"";
				wchar_t newPath[MAX_PATH] = L"";

				if (!BrowseForGameRoot(hwnd, newRoot, MAX_PATH))
					return;

				if (!BuildSelectedInstallGamePath(
					newRoot, folder, exe, newPath, MAX_PATH))
					return;

				DWORD newAttributes = GetFileAttributesW(newPath);

				if (newAttributes == INVALID_FILE_ATTRIBUTES ||
					(newAttributes & FILE_ATTRIBUTE_DIRECTORY))
				{
					C4FShowPrompt(
						hwnd,
						L"Installation not found",
						L"The selected root folder does not contain the required "
						L"newer game installation.",
						L"OK",
						L"Cancel");
					return;
				}

				// Save the new root only after successful validation.
				wcscpy_s(g_gameRootPath, MAX_PATH, newRoot);

				// Return to the launcher's current version.
				g_preferredGameVersion = 0;
				g_gameRootManuallySelected = true;

				// Use the version selected in the folder picker.
				const wchar_t* selectedVersionFolder =
					wcsrchr(g_gameInstallPath, L'\\');

				if (!selectedVersionFolder)
					return;

				++selectedVersionFolder;

				swprintf_s(selectedFolder, MAX_PATH, L"%s%s",
					selectedVersionFolder, folder + wcslen(g_versionFolder));

				SaveC4FSetting(L"GameRoot", g_gameRootPath);

				// Use the current launcher version for this launch.
				wcscpy_s(selectedFolder, MAX_PATH, folder);
				wcscpy_s(selectedExe, MAX_PATH, exe);
			}
			else
			{
				// Keep the existing root and use this version for all four games.
				wcscpy_s(selectedFolder, MAX_PATH, olderFolder);
				wcscpy_s(selectedExe, MAX_PATH, olderExe);

				// olderVersion is formatted as "2.48", so extract 48.
				g_preferredGameVersion = _wtoi(olderVersion + 2);

				wchar_t versionSetting[16];

				swprintf_s(versionSetting, 16, L"%d",
					g_preferredGameVersion);

				SaveC4FSetting(
					L"OlderGameVersion",
					versionSetting);
			}
		}
		else
		{
			int choice = C4FShowPrompt(
				hwnd,
				L"Game installation not found",
				L"The required game installation could not be found.\n\n"
				L"Would you like to locate the ROOT folder containing "
				L"your four Civ4 installations?",
				L"Browse for folder",
				L"Cancel");

			if (choice != IDYES)
				return;

			wchar_t newRoot[MAX_PATH] = L"";
			wchar_t newPath[MAX_PATH] = L"";

			if (!BrowseForGameRoot(hwnd, newRoot, MAX_PATH))
				return;

			if (!BuildSelectedInstallGamePath(
				newRoot, folder, exe, newPath, MAX_PATH))
				return;

			DWORD newAttributes = GetFileAttributesW(newPath);

			if (newAttributes == INVALID_FILE_ATTRIBUTES ||
				(newAttributes & FILE_ATTRIBUTE_DIRECTORY))
			{
				C4FShowPrompt(
					hwnd,
					L"Installation not found",
					L"The selected root folder does not contain the required "
					L"game executable.",
					L"OK",
					L"Cancel");
				return;
			}

			wcscpy_s(g_gameRootPath, MAX_PATH, newRoot);

			// Return to the launcher's current version.
			g_preferredGameVersion = 0;
			g_gameRootManuallySelected = true;

			// Use the version selected in the folder picker.
			const wchar_t* selectedVersionFolder =
				wcsrchr(g_gameInstallPath, L'\\');

			if (!selectedVersionFolder)
				return;

			++selectedVersionFolder;

			swprintf_s(selectedFolder, MAX_PATH, L"%s%s",
				selectedVersionFolder, folder + wcslen(g_versionFolder));

			SaveC4FSetting(L"GameRoot", g_gameRootPath);
		}

		// Rebuild the path using the selected installation.
		if (!BuildGamePath(
				g_gameRootPath,
				selectedFolder,
				selectedExe,
				path,
				MAX_PATH))
			return;
	}

    if (g_cacheResetChecked)
    {
        if (!ClearSelectedGameCache(userDataFolder))
        {
            MessageBox(hwnd,
                L"Civ4Fans could not complete this cache reset.\n\n"
                L"The game has not been launched. Some cache files may "
                L"already have been removed; check that the folder is not "
                L"in use and try again.",
                L"Cache reset failed", MB_OK | MB_ICONWARNING);
            return;
        }
        // Keep the user's cache-reset preference enabled after launch.
        // It remains active until the user manually unchecks it.
    }

    HINSTANCE result = (HINSTANCE)ShellExecute(
        hwnd,
        L"open",
        path,
        NULL,
        NULL,
        SW_SHOWNORMAL
    );

    if ((INT_PTR)result <= 32)
    {
        MessageBox(
            hwnd,
            path,
            L"Unable to launch game. Check the executable path.",
            MB_OK | MB_ICONERROR
        );
    }
}

// ============================================================
// DRAW GAME PANEL
// ============================================================

void DrawPanel(HDC hdc, RECT rect, const wchar_t* title,
               const wchar_t* subtitle, const wchar_t* description,
               bool hovered)
{
    {
        Graphics panelGraphics(hdc);

        BYTE alpha = hovered ? 195 : 165;

        Color panelColor = hovered
            ? Color(alpha, 42, 42, 43)
            : Color(alpha, 23, 25, 29);

        SolidBrush brush(panelColor);

        panelGraphics.FillRectangle(
            &brush,
            rect.left,
            rect.top,
            rect.right - rect.left,
            rect.bottom - rect.top
        );
    }

    HPEN pen = CreatePen(
        PS_SOLID,
        1,
        hovered ? RGB(235, 197, 112)
                : RGB(166, 132, 70)
    );

    HPEN oldPen = (HPEN)SelectObject(hdc, pen);
    HBRUSH oldBrush = (HBRUSH)SelectObject(
        hdc, GetStockObject(NULL_BRUSH)
    );

    Rectangle(
        hdc,
        rect.left,
        rect.top,
        rect.right,
        rect.bottom
    );

    SelectObject(hdc, oldPen);
    SelectObject(hdc, oldBrush);
    DeleteObject(pen);

    SetBkMode(hdc, TRANSPARENT);

    HFONT titleFont = CreateFont(
        25, 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE,
        DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
        CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
        DEFAULT_PITCH, L"Arial"
    );

    HFONT oldFont = (HFONT)SelectObject(hdc, titleFont);
    SetTextColor(hdc, RGB(235, 207, 151));

    RECT titleRect = rect;
    titleRect.left += 24;
    titleRect.top += 7;
    titleRect.bottom = titleRect.top + 34;

    DrawText(
        hdc, title, -1, &titleRect,
        DT_LEFT | DT_VCENTER | DT_SINGLELINE
    );

    SelectObject(hdc, oldFont);
    DeleteObject(titleFont);

    HFONT descFont = CreateFont(
        16, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
        DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
        CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
        DEFAULT_PITCH, L"Arial"
    );

    oldFont = (HFONT)SelectObject(hdc, descFont);
    SetTextColor(hdc, RGB(220, 216, 205));

    RECT subtitleRect = rect;
    subtitleRect.left += 24;
    subtitleRect.top += 40;
    subtitleRect.right -= 18;
    subtitleRect.bottom = subtitleRect.top + 20;
    DrawText(hdc, subtitle, -1, &subtitleRect,
        DT_LEFT | DT_TOP | DT_SINGLELINE | DT_END_ELLIPSIS);

    RECT descRect = rect;
    descRect.left += 24;
    descRect.top += 62;
    descRect.right -= 18;
    descRect.bottom -= 4;

    DrawText(
        hdc, description, -1, &descRect,
        DT_LEFT | DT_TOP | DT_WORDBREAK
    );

    SelectObject(hdc, oldFont);
    DeleteObject(descFont);
}

// ============================================================
// DRAW CUSTOM LARGE ICONS CHECKBOX
// ============================================================

void DrawLargeIconCheckbox(HDC hdc, int clientWidth, int clientHeight)
{
    RECT rect = GetLargeIconCheckboxRect(clientWidth, clientHeight);

    // Translucent background matching the game panels
    {
        Graphics graphics(hdc);

        BYTE alpha = g_checkboxHovered ? 185 : 145;

        Color background = g_checkboxHovered
            ? Color(alpha, 42, 42, 43)
            : Color(alpha, 23, 25, 29);

        SolidBrush brush(background);

        graphics.FillRectangle(
            &brush,
            rect.left,
            rect.top,
            rect.right - rect.left,
            rect.bottom - rect.top
        );
    }

    // Gold outline
    HPEN borderPen = CreatePen(
        PS_SOLID,
        1,
        g_checkboxHovered
            ? RGB(235, 197, 112)
            : RGB(166, 132, 70)
    );

    HPEN oldPen = (HPEN)SelectObject(hdc, borderPen);
    HBRUSH oldBrush = (HBRUSH)SelectObject(
        hdc, GetStockObject(NULL_BRUSH)
    );

    Rectangle(
        hdc,
        rect.left,
        rect.top,
        rect.right,
        rect.bottom
    );

    SelectObject(hdc, oldPen);
    SelectObject(hdc, oldBrush);
    DeleteObject(borderPen);

    // Smaller checkbox square: 18 x 18 pixels
    RECT box = { rect.left + 14, rect.top + 17, rect.left + 32, rect.top + 35 };

    {
        Graphics graphics(hdc);

        Color boxBackground = g_largeIconChecked
            ? Color(210, 55, 43, 29)
            : Color(175, 12, 14, 17);

        SolidBrush boxBrush(boxBackground);

        graphics.FillRectangle(
            &boxBrush,
            box.left,
            box.top,
            box.right - box.left,
            box.bottom - box.top
        );
    }

    HPEN boxPen = CreatePen(
        PS_SOLID,
        1,
        g_largeIconChecked
            ? RGB(235, 197, 112)
            : RGB(166, 132, 70)
    );

    oldPen = (HPEN)SelectObject(hdc, boxPen);
    oldBrush = (HBRUSH)SelectObject(
        hdc, GetStockObject(NULL_BRUSH)
    );

    Rectangle(
        hdc,
        box.left,
        box.top,
        box.right,
        box.bottom
    );

    SelectObject(hdc, oldPen);
    SelectObject(hdc, oldBrush);
    DeleteObject(boxPen);

    // Smaller gold tick
    if (g_largeIconChecked)
    {
        HPEN tickPen = CreatePen(
            PS_SOLID, 2, RGB(235, 197, 112)
        );

        oldPen = (HPEN)SelectObject(hdc, tickPen);

        MoveToEx(hdc, rect.left + 17, rect.top + 25, NULL);
        LineTo(hdc, rect.left + 21, rect.top + 29);
        LineTo(hdc, rect.left + 29, rect.top + 20);

        SelectObject(hdc, oldPen);
        DeleteObject(tickPen);
    }

    // Smaller label
    SetBkMode(hdc, TRANSPARENT);

    HFONT font = CreateFont(
        15, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
        DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
        CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
        DEFAULT_PITCH, L"Arial"
    );

    HFONT oldFont = (HFONT)SelectObject(hdc, font);

    SetTextColor(
        hdc,
        g_checkboxHovered
            ? RGB(255, 235, 190)
            : RGB(240, 240, 235)
    );

    RECT textRect = { rect.left + 44, rect.top, rect.right - 14, rect.bottom };

    DrawText(
        hdc,
        L"Launch with Large Resource Icon Bubbles",
        -1,
        &textRect,
        DT_LEFT | DT_VCENTER | DT_SINGLELINE
    );

    SelectObject(hdc, oldFont);
    DeleteObject(font);
}

void DrawCacheResetCheckbox(HDC hdc, int clientWidth, int clientHeight)
{
    RECT rect = GetCacheResetCheckboxRect(clientWidth, clientHeight);
    BYTE alpha = g_cacheCheckboxHovered ? 185 : 145;
    Graphics graphics(hdc);
    Color background = g_cacheCheckboxHovered
        ? Color(alpha, 42, 42, 43) : Color(alpha, 23, 25, 29);
    SolidBrush brush(background);
    graphics.FillRectangle(&brush, rect.left, rect.top,
        rect.right - rect.left, rect.bottom - rect.top);

    HPEN borderPen = CreatePen(PS_SOLID, 1,
        g_cacheCheckboxHovered ? RGB(235, 197, 112) : RGB(166, 132, 70));
    HPEN oldPen = (HPEN)SelectObject(hdc, borderPen);
    HBRUSH oldBrush = (HBRUSH)SelectObject(hdc, GetStockObject(NULL_BRUSH));
    Rectangle(hdc, rect.left, rect.top, rect.right, rect.bottom);
    SelectObject(hdc, oldPen);
    SelectObject(hdc, oldBrush);
    DeleteObject(borderPen);

    RECT box = { rect.left + 14, rect.top + 17, rect.left + 32, rect.top + 35 };
    Graphics boxGraphics(hdc);
    Color boxBackground = g_cacheResetChecked
        ? Color(210, 55, 43, 29) : Color(175, 12, 14, 17);
    SolidBrush boxBrush(boxBackground);
    boxGraphics.FillRectangle(&boxBrush, box.left, box.top,
        box.right - box.left, box.bottom - box.top);

    HPEN boxPen = CreatePen(PS_SOLID, 1,
        g_cacheResetChecked ? RGB(235, 197, 112) : RGB(166, 132, 70));
    oldPen = (HPEN)SelectObject(hdc, boxPen);
    oldBrush = (HBRUSH)SelectObject(hdc, GetStockObject(NULL_BRUSH));
    Rectangle(hdc, box.left, box.top, box.right, box.bottom);
    SelectObject(hdc, oldPen);
    SelectObject(hdc, oldBrush);
    DeleteObject(boxPen);

    if (g_cacheResetChecked)
    {
        HPEN tickPen = CreatePen(PS_SOLID, 2, RGB(235, 197, 112));
        oldPen = (HPEN)SelectObject(hdc, tickPen);
        MoveToEx(hdc, rect.left + 17, rect.top + 25, NULL);
        LineTo(hdc, rect.left + 21, rect.top + 29);
        LineTo(hdc, rect.left + 29, rect.top + 20);
        SelectObject(hdc, oldPen);
        DeleteObject(tickPen);
    }

    SetBkMode(hdc, TRANSPARENT);
    HFONT font = CreateFont(15, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
        DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
        CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Arial");
    HFONT oldFont = (HFONT)SelectObject(hdc, font);
    SetTextColor(hdc, g_cacheCheckboxHovered
        ? RGB(255, 235, 190) : RGB(240, 240, 235));
    RECT textRect = { rect.left + 44, rect.top, rect.right - 14, rect.bottom };
    DrawText(hdc, L"Force cache reset on next launch", -1,
        &textRect, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
    SelectObject(hdc, oldFont);
    DeleteObject(font);
}

void DrawGameDirectoryRow(HDC hdc, int clientWidth, int clientHeight)
{
    RECT rect = GetGameDirectoryRect(clientWidth, clientHeight);

    BYTE alpha = g_gameDirectoryHovered ? 185 : 145;

    Graphics graphics(hdc);
    Color background = g_gameDirectoryHovered
        ? Color(alpha, 42, 42, 43)
        : Color(alpha, 23, 25, 29);

    SolidBrush brush(background);
    graphics.FillRectangle(&brush, rect.left, rect.top,
        rect.right - rect.left, rect.bottom - rect.top);

    HPEN borderPen = CreatePen(PS_SOLID, 1,
        g_gameDirectoryHovered
            ? RGB(235, 197, 112)
            : RGB(166, 132, 70));

    HPEN oldPen = (HPEN)SelectObject(hdc, borderPen);
    HBRUSH oldBrush =
        (HBRUSH)SelectObject(hdc, GetStockObject(NULL_BRUSH));

    Rectangle(hdc, rect.left, rect.top, rect.right, rect.bottom);

    SelectObject(hdc, oldPen);
    SelectObject(hdc, oldBrush);
    DeleteObject(borderPen);

    wchar_t directoryText[MAX_PATH + 128];

    if (g_gameRootManuallySelected)
    {
		swprintf_s(directoryText, _countof(directoryText),
			L"Game Directory selected: %s",
			g_gameInstallPath);
    }
    else
    {
        wcscpy_s(directoryText, _countof(directoryText),
            L"Game Directory: Auto-detected");
    }

    SetBkMode(hdc, TRANSPARENT);

    HFONT font = CreateFont(15, 0, 0, 0, FW_NORMAL,
        FALSE, FALSE, FALSE, DEFAULT_CHARSET,
        OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
        CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Arial");

    HFONT oldFont = (HFONT)SelectObject(hdc, font);

    SetTextColor(hdc, g_gameDirectoryHovered
        ? RGB(255, 235, 190)
        : RGB(240, 240, 235));

    RECT textRect = {
        rect.left + 14,
        rect.top,
        rect.right - 14,
        rect.bottom
    };

    DrawText(hdc, directoryText, -1, &textRect,
        DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);

    SelectObject(hdc, oldFont);
    DeleteObject(font);
}

// ============================================================
// LOBBY NICKNAME PROMPT
// Requested every time the user clicks Connect; never stored.
// ============================================================

static HWND g_nicknameEdit = NULL;
static wchar_t g_nicknameResult[64] = L"";
static bool g_nicknamePromptAccepted = false;
static HBRUSH g_nicknameEditBrush = NULL;
static HFONT g_nicknameEditFont = NULL;
static WNDPROC g_oldNicknameEditProc = NULL;

static bool IsValidNickname(const wchar_t* nick)
{
    size_t n = wcslen(nick);
    if (n < 1 || n > 24) return false;
    for (size_t i = 0; i < n; ++i)
    {
        wchar_t c = nick[i];
        if (!((c >= L'a' && c <= L'z') ||
              (c >= L'A' && c <= L'Z') ||
              (c >= L'0' && c <= L'9') || c == L'_' || c == L'-'))
            return false;
    }
    return true;
}

static void FinishNicknamePrompt(HWND hwnd, bool accept);

static void DrawNicknameButton(HDC hdc, const RECT& r, const wchar_t* label,
                               bool primary)
{
    HBRUSH bg = CreateSolidBrush(primary ? RGB(75, 54, 29) : RGB(35, 37, 40));
    FillRect(hdc, &r, bg);
    DeleteObject(bg);
    HPEN pen = CreatePen(PS_SOLID, 1,
        primary ? RGB(220, 177, 94) : RGB(130, 111, 78));
    HPEN oldPen = (HPEN)SelectObject(hdc, pen);
    HBRUSH nullBrush = (HBRUSH)GetStockObject(NULL_BRUSH);
    HBRUSH oldBrush = (HBRUSH)SelectObject(hdc, nullBrush);
    Rectangle(hdc, r.left, r.top, r.right, r.bottom);
    SelectObject(hdc, oldBrush);
    SelectObject(hdc, oldPen);
    DeleteObject(pen);

    HFONT font = CreateFont(15, 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE,
        DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
        CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Arial");
    HFONT oldFont = (HFONT)SelectObject(hdc, font);
    SetBkMode(hdc, TRANSPARENT);
    SetTextColor(hdc, RGB(245, 225, 185));
    RECT text = r;
    DrawText(hdc, label, -1, &text, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    SelectObject(hdc, oldFont);
    DeleteObject(font);
}

static LRESULT CALLBACK NicknameEditProc(HWND hwnd, UINT msg,
                                          WPARAM wParam, LPARAM lParam)
{
    if ((msg == WM_KEYDOWN && wParam == VK_RETURN) ||
        (msg == WM_CHAR && wParam == L'\r'))
    {
        FinishNicknamePrompt(GetParent(hwnd), true);
        return 0;
    }
    if ((msg == WM_KEYDOWN && wParam == VK_ESCAPE) ||
        (msg == WM_CHAR && wParam == 27))
    {
        FinishNicknamePrompt(GetParent(hwnd), false);
        return 0;
    }
    return CallWindowProc(g_oldNicknameEditProc, hwnd, msg, wParam, lParam);
}

static void FinishNicknamePrompt(HWND hwnd, bool accept)
{
    if (accept)
    {
        GetWindowText(g_nicknameEdit, g_nicknameResult, 64);
        if (!IsValidNickname(g_nicknameResult))
        {
            MessageBox(hwnd,
                L"Use 1-24 characters: letters, numbers, underscore or hyphen.",
                L"Invalid nickname", MB_OK | MB_ICONWARNING);
            SetFocus(g_nicknameEdit);
            return;
        }
        g_nicknamePromptAccepted = true;
        wcsncpy_s(g_lastEnteredNickname, 64, g_nicknameResult, _TRUNCATE);
        if (!SaveC4FSetting(L"LastNickname", g_lastEnteredNickname))
            OutputDebugStringW(L"Civ4Fans: nickname preference could not be saved.\n");
    }
    DestroyWindow(hwnd);
}

static LRESULT CALLBACK NicknamePromptProc(HWND hwnd, UINT msg,
                                            WPARAM wParam, LPARAM lParam)
{
    switch (msg)
    {
    case WM_CREATE:
        g_nicknameEdit = CreateWindowEx(WS_EX_CLIENTEDGE, L"EDIT", g_lastEnteredNickname,
            WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_AUTOHSCROLL,
            28, 91, 374, 30, hwnd, (HMENU)1001,
            GetModuleHandle(NULL), NULL);
        g_nicknameEditBrush = CreateSolidBrush(RGB(25, 27, 30));
        g_nicknameEditFont = CreateFont(16, 0, 0, 0, FW_NORMAL, FALSE, FALSE,
            FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
            CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Arial");
        SendMessage(g_nicknameEdit, WM_SETFONT, (WPARAM)g_nicknameEditFont, TRUE);
        g_oldNicknameEditProc = (WNDPROC)SetWindowLongPtr(g_nicknameEdit,
            GWLP_WNDPROC, (LONG_PTR)NicknameEditProc);
        return 0;

    case WM_CTLCOLOREDIT:
        SetTextColor((HDC)wParam, RGB(245, 235, 215));
        SetBkColor((HDC)wParam, RGB(25, 27, 30));
        return (LRESULT)g_nicknameEditBrush;

    case WM_PAINT:
    {
        PAINTSTRUCT ps;
        HDC hdc = BeginPaint(hwnd, &ps);
        RECT r;
        GetClientRect(hwnd, &r);
        HBRUSH bg = CreateSolidBrush(RGB(23, 25, 29));
        FillRect(hdc, &r, bg);
        DeleteObject(bg);
        HPEN border = CreatePen(PS_SOLID, 2, RGB(196, 158, 82));
        HPEN oldPen = (HPEN)SelectObject(hdc, border);
        HBRUSH nullBrush = (HBRUSH)GetStockObject(NULL_BRUSH);
        HBRUSH oldBrush = (HBRUSH)SelectObject(hdc, nullBrush);
        Rectangle(hdc, 1, 1, r.right - 1, r.bottom - 1);
        SelectObject(hdc, oldBrush);
        SelectObject(hdc, oldPen);
        DeleteObject(border);

        HFONT titleFont = CreateFont(23, 0, 0, 0, FW_BOLD, FALSE, FALSE,
            FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
            CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Georgia");
        HFONT oldFont = (HFONT)SelectObject(hdc, titleFont);
        SetBkMode(hdc, TRANSPARENT);
        SetTextColor(hdc, RGB(235, 207, 151));
        RECT title = { 28, 17, 402, 53 };
        DrawText(hdc, L"Connect to the Civ4 lobby", -1, &title,
            DT_LEFT | DT_VCENTER | DT_SINGLELINE);
        SelectObject(hdc, oldFont);
        DeleteObject(titleFont);

        HFONT bodyFont = CreateFont(14, 0, 0, 0, FW_NORMAL, FALSE, FALSE,
            FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
            CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Arial");
        oldFont = (HFONT)SelectObject(hdc, bodyFont);
        SetTextColor(hdc, RGB(220, 216, 205));
        RECT prompt = { 28, 56, 402, 82 };
        DrawText(hdc, L"Choose the nickname other players will see.", -1,
            &prompt, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
        RECT suffix = { 28, 126, 402, 149 };
        SetTextColor(hdc, RGB(178, 164, 137));
        DrawText(hdc, L"Your IRC nickname will have _C4F appended.", -1,
            &suffix, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
        SelectObject(hdc, oldFont);
        DeleteObject(bodyFont);

        RECT cancelRect = { 238, 164, 320, 198 };
        RECT connectRect = { 328, 164, 402, 198 };
        DrawNicknameButton(hdc, cancelRect, L"Cancel", false);
        DrawNicknameButton(hdc, connectRect, L"Connect", true);
        EndPaint(hwnd, &ps);
        return 0;
    }

    case WM_LBUTTONUP:
    {
        int x = LOWORD(lParam), y = HIWORD(lParam);
        if (y >= 164 && y <= 198)
        {
            if (x >= 328 && x <= 402) FinishNicknamePrompt(hwnd, true);
            else if (x >= 238 && x <= 320) FinishNicknamePrompt(hwnd, false);
        }
        return 0;
    }

    case WM_KEYDOWN:
        if (wParam == VK_RETURN) { FinishNicknamePrompt(hwnd, true); return 0; }
        if (wParam == VK_ESCAPE) { FinishNicknamePrompt(hwnd, false); return 0; }
        break;

    case WM_CLOSE:
        FinishNicknamePrompt(hwnd, false);
        return 0;

    case WM_DESTROY:
        if (g_nicknameEditFont)
        {
            DeleteObject(g_nicknameEditFont);
            g_nicknameEditFont = NULL;
        }
        if (g_nicknameEditBrush)
        {
            DeleteObject(g_nicknameEditBrush);
            g_nicknameEditBrush = NULL;
        }
        g_nicknameEdit = NULL;
        return 0;
    }
    return DefWindowProc(hwnd, msg, wParam, lParam);
}

static bool PromptForNickname(HWND owner, wchar_t* result, size_t resultCount)
{
    const wchar_t CLASS_NAME[] = L"C4FNicknamePromptWindow";
    static bool registered = false;
    if (!registered)
    {
        WNDCLASS wc = {};
        wc.lpfnWndProc = NicknamePromptProc;
        wc.hInstance = GetModuleHandle(NULL);
        wc.lpszClassName = CLASS_NAME;
        wc.hCursor = LoadCursor(NULL, IDC_ARROW);
        if (!RegisterClass(&wc) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS)
            return false;
        registered = true;
    }

    g_nicknamePromptAccepted = false;
    g_nicknameResult[0] = L'\0';
    HWND dlg = CreateWindowEx(WS_EX_TOOLWINDOW | WS_EX_TOPMOST,
        CLASS_NAME, L"", WS_POPUP,
        CW_USEDEFAULT, CW_USEDEFAULT, 430, 218,
        owner, NULL, GetModuleHandle(NULL), NULL);
    if (!dlg) return false;

    RECT ownerRect;
    if (owner) GetWindowRect(owner, &ownerRect);
    else SetRect(&ownerRect, 0, 0, GetSystemMetrics(SM_CXSCREEN), GetSystemMetrics(SM_CYSCREEN));
    int x = ownerRect.left + ((ownerRect.right - ownerRect.left) - 430) / 2;
    int y = ownerRect.top + ((ownerRect.bottom - ownerRect.top) - 218) / 2;
    SetWindowPos(dlg, HWND_TOPMOST, x, y, 430, 218, SWP_SHOWWINDOW);

    if (owner) EnableWindow(owner, FALSE);
    SetFocus(g_nicknameEdit);
    MSG msg;
    while (IsWindow(dlg) && GetMessage(&msg, NULL, 0, 0) > 0)
    {
        // This is a custom popup, not a dialog resource. Let the edit control
        // receive keyboard messages directly so its subclass can handle Enter.
        TranslateMessage(&msg);
        DispatchMessage(&msg);
    }
    if (owner)
    {
        EnableWindow(owner, TRUE);
        SetForegroundWindow(owner);
    }
    if (!g_nicknamePromptAccepted) return false;
    wcsncpy_s(result, resultCount, g_nicknameResult, _TRUNCATE);
    return true;
}

static void StartLobbyConnection(HWND hwnd)
{
    wchar_t nickname[64];
    if (!PromptForNickname(hwnd, nickname, 64))
    {
        ChatPanel_SetStatus(L"Connection cancelled.");
        InvalidateRect(hwnd, NULL, FALSE);
        return;
    }

    IRC_Disconnect();
    g_currentIrcNickname[0] = L'\0';
    ChatPanel_SetConnectionState(false, NULL);
    ChatPanel_SetStatus(L"Starting connection...");
    InvalidateRect(hwnd, NULL, FALSE);
    if (!IRC_Connect(nickname))
        ChatPanel_SetStatus(L"Could not start the IRC connection.");
    else
    {
        wcsncpy_s(g_currentIrcNickname, 64, nickname, _TRUNCATE);
        wcscat_s(g_currentIrcNickname, 64, IRC_NICK_SUFFIX);
    }
    InvalidateRect(hwnd, NULL, FALSE);
}

// ============================================================
// WINDOW PROCEDURE
// ============================================================

LRESULT CALLBACK WindowProc(
    HWND hwnd, UINT uMsg, WPARAM wParam, LPARAM lParam)
{
    static int hoveredPanel = 0;

    switch (uMsg)
    {
    case WM_CTLCOLOREDIT:
        if ((HWND)lParam != NULL &&
            (GetDlgCtrlID((HWND)lParam) == C4F_CHAT_INPUT_ID ||
             GetDlgCtrlID((HWND)lParam) == C4F_PRIVATE_INPUT_ID))
        {
            SetTextColor((HDC)wParam, RGB(245, 235, 215));
            SetBkColor((HDC)wParam, RGB(25, 27, 30));
            return (LRESULT)ChatPanel_GetEditBrush();
        }
        break;

    case WM_DRAWITEM:
        if (wParam == C4F_CHAT_SEND_ID || wParam == C4F_PRIVATE_SEND_ID)
        {
            DRAWITEMSTRUCT* dis = (DRAWITEMSTRUCT*)lParam;
            if (dis != NULL)
            {
                HBRUSH bg = CreateSolidBrush(RGB(68, 49, 25));
                FillRect(dis->hDC, &dis->rcItem, bg);
                DeleteObject(bg);
                HPEN pen = CreatePen(PS_SOLID, 1, RGB(220, 177, 94));
                HPEN oldPen = (HPEN)SelectObject(dis->hDC, pen);
                HBRUSH oldBrush = (HBRUSH)SelectObject(dis->hDC,
                    GetStockObject(NULL_BRUSH));
                Rectangle(dis->hDC, dis->rcItem.left, dis->rcItem.top,
                    dis->rcItem.right, dis->rcItem.bottom);
                SelectObject(dis->hDC, oldBrush);
                SelectObject(dis->hDC, oldPen);
                DeleteObject(pen);
                HFONT font = CreateFont(15, 0, 0, 0, FW_BOLD, FALSE, FALSE,
                    FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                    CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Arial");
                HFONT oldFont = (HFONT)SelectObject(dis->hDC, font);
                SetBkMode(dis->hDC, TRANSPARENT);
                SetTextColor(dis->hDC, RGB(245, 220, 175));
                DrawText(dis->hDC, L"Send", -1, &dis->rcItem,
                    DT_CENTER | DT_VCENTER | DT_SINGLELINE);
                SelectObject(dis->hDC, oldFont);
                DeleteObject(font);
            }
            return TRUE;
        }
        break;

    case WM_COMMAND:
        if (LOWORD(wParam) == C4F_PRIVATE_SEND_ID && HIWORD(wParam) == BN_CLICKED)
        {
            wchar_t message[512];
            ChatPanel_GetPrivateInputText(message, 512);
            const wchar_t* recipient = ChatPanel_GetActiveRecipient();
            if (message[0] != L'\0' && recipient && recipient[0])
            {
                BOOL sent = IRC_SendPrivateMessage(recipient, message);
                if (sent)
                {
                    ChatPanel_AddPrivateEcho(recipient, message);
                    ChatPanel_ClearPrivateInput();
                }
                else ChatPanel_SetStatus(L"Not connected, or private message could not be sent.");
                InvalidateRect(hwnd, NULL, FALSE);
            }
            return 0;
        }
        if (LOWORD(wParam) == C4F_CHAT_SEND_ID && HIWORD(wParam) == BN_CLICKED)
        {
            wchar_t message[512];
            ChatPanel_GetInputText(message, 512);
            if (message[0] != L'\0')
            {
                BOOL sent = IRC_SendMessage(message);
                if (sent)
                {
                    ChatPanel_AddMessage(L"You", message);
                    ChatPanel_ClearInput();
                }
                else ChatPanel_SetStatus(L"Not connected, or message could not be sent.");
                InvalidateRect(hwnd, NULL, FALSE);
            }
            return 0;
        }
        break;

    case WM_TIMER:
        if (wParam == 0xC4F2)
        {
            ChatPanel_TimerTick(hwnd);
            return 0;
        }
        break;

    case WM_IRC_STATUS:
    case WM_IRC_MESSAGE:
    {
        IRC_EVENT* eventData = (IRC_EVENT*)lParam;
        if (eventData != NULL)
        {
            if (uMsg == WM_IRC_STATUS)
            {
                if (wcsstr(eventData->text, L"Joined") != NULL)
                {
                    ChatPanel_SetConnectionState(true, g_currentIrcNickname);
                    ChatPanel_SetStatus(L"");
                }
                else
                {
                    if (wcsstr(eventData->text, L"closed") != NULL ||
                        wcsstr(eventData->text, L"Could not") != NULL ||
                        wcsstr(eventData->text, L"resolve") != NULL)
                    {
                        ChatPanel_SetConnectionState(false, NULL);
                        g_currentIrcNickname[0] = L'\0';
                    }
                    ChatPanel_SetStatus(eventData->text);
                }
            }
            else if (_wcsicmp(eventData->sender, g_currentIrcNickname) != 0)
            {
                if (eventData->isPrivate)
                    ChatPanel_AddPrivateMessage(eventData->sender, eventData->sender, eventData->text);
                else
                    ChatPanel_AddMessage(eventData->sender, eventData->text);
            }
            IRC_FreeEvent(eventData);
            InvalidateRect(hwnd, NULL, FALSE);
        }
        return 0;
    }

    case WM_IRC_MEMBERS:
    {
        IRC_MEMBER_LIST* memberList = (IRC_MEMBER_LIST*)lParam;
        if (memberList != NULL)
        {
            ChatPanel_SetMembers(memberList->members, memberList->count);
            IRC_FreeMemberList(memberList);
            InvalidateRect(hwnd, NULL, FALSE);
        }
        return 0;
    }

    // WM_PAINT draws the complete background itself. Prevent Windows from
    // erasing it first, which would otherwise cause visible flicker.
    case WM_ERASEBKGND:
        return 1;

    case WM_RBUTTONUP:
    {
        int x = LOWORD(lParam), y = HIWORD(lParam);
        RECT client; GetClientRect(hwnd, &client);
        if (ChatPanel_IsEnabled() && ChatPanel_HandleMemberClick(x, y, client.right, client.bottom))
        {
            InvalidateRect(hwnd, NULL, FALSE);
            return 0;
        }
        break;
    }

    case WM_MOUSEMOVE:
    {
        POINT pt;
        pt.x = LOWORD(lParam);
        pt.y = HIWORD(lParam);

        if (g_useCustomTitleBar)
        {
            RECT titleClient; GetClientRect(hwnd, &titleClient);
            int hoverButton = 0;
            if (pt.y >= 0 && pt.y < CUSTOM_TITLEBAR_HEIGHT)
            {
                if (pt.x >= titleClient.right - 44) hoverButton = 2;
                else if (pt.x >= titleClient.right - 88) hoverButton = 1;
            }
            if (hoverButton != g_titleBarHoverButton)
            {
                g_titleBarHoverButton = hoverButton;
                RECT titleRect = { titleClient.right - 88, 0, titleClient.right, CUSTOM_TITLEBAR_HEIGHT };
                InvalidateRect(hwnd, &titleRect, FALSE);
            }
        }

        RECT client;
        GetClientRect(hwnd, &client);
        RECT checkboxRect = GetLargeIconCheckboxRect(client.right, client.bottom);
        RECT cacheCheckboxRect = GetCacheResetCheckboxRect(client.right, client.bottom);

		RECT gameDirectoryRect = GetGameDirectoryRect(client.right, client.bottom);

		bool newGameDirectoryHovered =
			pt.x >= gameDirectoryRect.left && pt.x < gameDirectoryRect.right &&
			pt.y >= gameDirectoryRect.top && pt.y < gameDirectoryRect.bottom;

		if (newGameDirectoryHovered != g_gameDirectoryHovered)
		{
			g_gameDirectoryHovered = newGameDirectoryHovered;
			InvalidateRect(hwnd, &gameDirectoryRect, FALSE);
		}

        bool newCacheCheckboxHovered =
            pt.x >= cacheCheckboxRect.left && pt.x < cacheCheckboxRect.right &&
            pt.y >= cacheCheckboxRect.top && pt.y < cacheCheckboxRect.bottom;
        if (newCacheCheckboxHovered != g_cacheCheckboxHovered)
        {
            g_cacheCheckboxHovered = newCacheCheckboxHovered;
            InvalidateRect(hwnd, &cacheCheckboxRect, FALSE);
        }

        bool newChatToggleHovered =
            ChatPanel_ToggleHoverHitTest(pt.x, pt.y, client.bottom);
        static bool chatToggleWasHovered = false;
        if (newChatToggleHovered != chatToggleWasHovered)
        {
            chatToggleWasHovered = newChatToggleHovered;
            ChatPanel_SetToggleHovered(newChatToggleHovered);
            InvalidateRect(hwnd, NULL, FALSE);
        }

        bool newCheckboxHovered =
            pt.x >= checkboxRect.left &&
            pt.x < checkboxRect.right &&
            pt.y >= checkboxRect.top &&
            pt.y < checkboxRect.bottom;

        if (newCheckboxHovered != g_checkboxHovered)
        {
            g_checkboxHovered = newCheckboxHovered;
            InvalidateRect(hwnd, &checkboxRect, FALSE);
        }

        int newHovered = 0;
        int panelRight = GetGamePanelRight(client.right);

        if (pt.x >= PANEL_LEFT && pt.x <= panelRight)
        {
            if (pt.y >= 105 && pt.y <= 209) newHovered = 1;
            else if (pt.y >= 219 && pt.y <= 323) newHovered = 2;
            else if (pt.y >= 333 && pt.y <= 437) newHovered = 3;
            else if (pt.y >= 447 && pt.y <= 551) newHovered = 4;
        }

        if (newHovered != hoveredPanel)
        {
            hoveredPanel = newHovered;
            InvalidateRect(hwnd, NULL, FALSE);
        }

        TRACKMOUSEEVENT tme;
        tme.cbSize = sizeof(tme);
        tme.dwFlags = TME_LEAVE;
        tme.hwndTrack = hwnd;
        tme.dwHoverTime = 0;
        TrackMouseEvent(&tme);
        return 0;
    }

    case WM_MOUSELEAVE:
    {
        if (hoveredPanel != 0)
        {
            hoveredPanel = 0;
            InvalidateRect(hwnd, NULL, FALSE);
        }

        if (g_checkboxHovered)
        {
            g_checkboxHovered = false;
            RECT client;
            GetClientRect(hwnd, &client);
            RECT checkboxRect = GetLargeIconCheckboxRect(client.right, client.bottom);
            InvalidateRect(hwnd, &checkboxRect, FALSE);
        }
        if (g_cacheCheckboxHovered)
        {
            g_cacheCheckboxHovered = false;
            RECT client;
            GetClientRect(hwnd, &client);
            RECT cacheRect = GetCacheResetCheckboxRect(client.right, client.bottom);
            InvalidateRect(hwnd, &cacheRect, FALSE);
        }

		if (g_gameDirectoryHovered)
		{
			g_gameDirectoryHovered = false;
			RECT client;
			GetClientRect(hwnd, &client);
			RECT directoryRect = GetGameDirectoryRect(client.right, client.bottom);
			InvalidateRect(hwnd, &directoryRect, FALSE);
		}

        ChatPanel_SetToggleHovered(false);
        if (g_titleBarHoverButton != 0)
        {
            g_titleBarHoverButton = 0;
            InvalidateRect(hwnd, NULL, FALSE);
        }
        InvalidateRect(hwnd, NULL, FALSE);
        return 0;
    }

    case WM_LBUTTONDOWN:
    {
        if (g_useCustomTitleBar)
        {
            int x = LOWORD(lParam);
            int y = HIWORD(lParam);
            RECT client; GetClientRect(hwnd, &client);
            if (y >= 0 && y < CUSTOM_TITLEBAR_HEIGHT)
            {
                if (x >= client.right - 44)
                    g_titleBarPressedButton = 2;
                else if (x >= client.right - 88)
                    g_titleBarPressedButton = 1;
                else
                {
                    ReleaseCapture();
                    SendMessage(hwnd, WM_NCLBUTTONDOWN, HTCAPTION, 0);
                    return 0;
                }
                SetCapture(hwnd);
                InvalidateRect(hwnd, NULL, FALSE);
                return 0;
            }
        }
        break;
    }

    case WM_LBUTTONUP:
    {
        int x = LOWORD(lParam);
        int y = HIWORD(lParam);

        RECT client;
        GetClientRect(hwnd, &client);

        if (g_useCustomTitleBar && g_titleBarPressedButton != 0)
        {
            int releasedButton = 0;
            if (y >= 0 && y < CUSTOM_TITLEBAR_HEIGHT)
            {
                if (x >= client.right - 44) releasedButton = 2;
                else if (x >= client.right - 88) releasedButton = 1;
            }
            int pressedButton = g_titleBarPressedButton;
            g_titleBarPressedButton = 0;
            ReleaseCapture();
            if (releasedButton == pressedButton)
            {
                if (pressedButton == 2) PostMessage(hwnd, WM_CLOSE, 0, 0);
                else SendMessage(hwnd, WM_SYSCOMMAND, SC_MINIMIZE, 0);
            }
            InvalidateRect(hwnd, NULL, FALSE);
            return 0;
        }

        // Clicking either mouse button on a lobby member opens their private conversation.
        if (ChatPanel_IsEnabled() && ChatPanel_HandleMemberClick(x, y, client.right, client.bottom))
        {
            InvalidateRect(hwnd, NULL, FALSE);
            return 0;
        }

        // Toggle lobby chat preview
        if (ChatPanel_ToggleHitTest(x, y, client.bottom))
        {
            ChatPanel_Toggle();
            SaveC4FBooleanSetting(L"EnableLobbyChat", ChatPanel_IsEnabled());
            ChatPanel_SetInputVisible(ChatPanel_IsEnabled());
            InvalidateRect(hwnd, NULL, FALSE);
            return 0;
        }

        // Connect button inside the Lobby Chat panel
        if (ChatPanel_ConnectHitTest(x, y, client.right, client.bottom))
        {
            if (IRC_IsConnected())
            {
                IRC_Disconnect();
                g_currentIrcNickname[0] = L'\0';
                ChatPanel_SetConnectionState(false, NULL);
                ChatPanel_SetStatus(L"Disconnected.");
                InvalidateRect(hwnd, NULL, FALSE);
            }
            else
            {
                StartLobbyConnection(hwnd);
            }
            return 0;
        }

		// Select game directory
		RECT gameDirectoryRect = GetGameDirectoryRect(client.right, client.bottom);

		if (x >= gameDirectoryRect.left && x < gameDirectoryRect.right &&
			y >= gameDirectoryRect.top && y < gameDirectoryRect.bottom)
		{
			wchar_t selectedRoot[MAX_PATH] = L"";

			if (BrowseForGameRoot(hwnd, selectedRoot, MAX_PATH))
			{
				wcscpy_s(g_gameRootPath, MAX_PATH, selectedRoot);

				g_gameRootManuallySelected = true;
				g_preferredGameVersion = 0;

				SaveC4FSetting(L"", L"");

				InvalidateRect(hwnd, &gameDirectoryRect, FALSE);
			}

			return 0;
		}

        // Toggle one-shot cache reset checkbox
        RECT cacheCheckboxRect = GetCacheResetCheckboxRect(client.right, client.bottom);
        if (x >= cacheCheckboxRect.left && x < cacheCheckboxRect.right &&
            y >= cacheCheckboxRect.top && y < cacheCheckboxRect.bottom)
        {
            g_cacheResetChecked = !g_cacheResetChecked;
            SaveC4FBooleanSetting(L"ForceCacheReset", g_cacheResetChecked);
            InvalidateRect(hwnd, &cacheCheckboxRect, FALSE);
            return 0;
        }

        // Toggle large resource icon checkbox
        RECT checkboxRect = GetLargeIconCheckboxRect(client.right, client.bottom);
        if (x >= checkboxRect.left &&
            x < checkboxRect.right &&
            y >= checkboxRect.top &&
            y < checkboxRect.bottom)
        {
            g_largeIconChecked = !g_largeIconChecked;
            SaveC4FBooleanSetting(L"LargeResourceIcon", g_largeIconChecked);

            InvalidateRect(hwnd, &checkboxRect, FALSE);

            return 0;
        }

        int panelRight = GetGamePanelRight(client.right);

        if (x >= PANEL_LEFT && x <= panelRight)
        {
            wchar_t folder[MAX_PATH];
            wchar_t exe[MAX_PATH];

            const wchar_t* suffix =
                g_largeIconChecked ? L"_LargeIcon" : L"";

            // Beyond The Game
            if (y >= 105 && y <= 209)
            {
                swprintf_s(
                    folder, MAX_PATH,
                    L"%s\\Beyond The Game\\",
                    g_versionFolder
                );

				swprintf_s(exe, MAX_PATH,
					L"BeyondTheGame%s.exe", suffix);

                LaunchGame(hwnd, folder, exe, L"Beyond The Game");
            }

            // Beyond The Perun
            else if (y >= 219 && y <= 323)
            {
                swprintf_s(
                    folder, MAX_PATH,
                    L"%s\\Beyond The Perun\\",
                    g_versionFolder
                );

				swprintf_s(exe, MAX_PATH,
					L"BeyondThePerun%s.exe", suffix);

                LaunchGame(hwnd, folder, exe, L"Beyond The Perun");
            }

            // Beyond The QoL
            else if (y >= 333 && y <= 437)
            {
                swprintf_s(
                    folder, MAX_PATH,
                    L"%s\\Beyond The QoL\\",
                    g_versionFolder
                );

				swprintf_s(exe, MAX_PATH,
					L"BeyondTheQoL%s.exe", suffix);

                LaunchGame(hwnd, folder, exe, L"Beyond The QoL");
            }

            // Beyond The Sword
            else if (y >= 447 && y <= 551)
            {
                swprintf_s(
                    folder, MAX_PATH,
                    L"%s\\Beyond The Sword\\",
                    g_versionFolder
                );

                swprintf_s(
                    exe, MAX_PATH,
                    L"Civ4BeyondSword%s.exe",
                    suffix
                );

                LaunchGame(hwnd, folder, exe, L"Beyond The Sword");
            }
        }

        return 0;
    }

    case WM_PAINT:
    {
        PAINTSTRUCT ps;
        HDC hdc = BeginPaint(hwnd, &ps);

        RECT client;
        GetClientRect(hwnd, &client);
        int clientWidth = client.right - client.left;
        int clientHeight = client.bottom - client.top;

        // Draw the entire frame into an off-screen bitmap first. The user only
        // sees the completed frame when BitBlt copies it to the window.
        HDC bufferDC = CreateCompatibleDC(hdc);
        HBITMAP bufferBitmap = NULL;
        HBITMAP oldBitmap = NULL;
        HDC drawDC = hdc;

        if (bufferDC != NULL && clientWidth > 0 && clientHeight > 0)
        {
            bufferBitmap = CreateCompatibleBitmap(hdc, clientWidth, clientHeight);
            if (bufferBitmap != NULL)
            {
                oldBitmap = (HBITMAP)SelectObject(bufferDC, bufferBitmap);
                drawDC = bufferDC;
            }
            else
            {
                DeleteDC(bufferDC);
                bufferDC = NULL;
            }
        }
        else if (bufferDC != NULL)
        {
            DeleteDC(bufferDC);
            bufferDC = NULL;
        }

        {
            Graphics graphics(drawDC);
            graphics.SetInterpolationMode(InterpolationModeHighQualityBicubic);

            // Background image
            if (g_background != NULL && g_background->GetLastStatus() == Ok)
            {
                graphics.DrawImage(g_background, 0, 0, clientWidth, clientHeight);
            }
            else
            {
                HBRUSH bgBrush = CreateSolidBrush(RGB(25, 29, 36));
                FillRect(drawDC, &client, bgBrush);
                DeleteObject(bgBrush);
            }

            SetBkMode(drawDC, TRANSPARENT);

            // Main heading: elegant serif lettering, warm gold fill, dark gold outline.
            graphics.SetSmoothingMode(SmoothingModeAntiAlias);
            graphics.SetTextRenderingHint(TextRenderingHintAntiAliasGridFit);

            FontFamily headingFamily(L"Georgia");
            GraphicsPath headingPath;
            StringFormat headingFormat;
            headingFormat.SetAlignment(StringAlignmentCenter);
            headingFormat.SetLineAlignment(StringAlignmentCenter);

            RectF headingRect(0.0f, g_useCustomTitleBar ? 34.0f : 22.0f, (REAL)clientWidth, 58.0f);
            headingPath.AddString(
                g_heading, -1,
                &headingFamily,
                FontStyleBold,
                32.0f,
                headingRect,
                &headingFormat
            );

            Pen headingOutline(Color(245, 92, 55, 20), 3.0f);
            headingOutline.SetLineJoin(LineJoinRound);
            graphics.DrawPath(&headingOutline, &headingPath);

            LinearGradientBrush headingGold(
                PointF(0.0f, g_useCustomTitleBar ? 34.0f : 22.0f), PointF(0.0f, g_useCustomTitleBar ? 84.0f : 72.0f),
                Color(255, 255, 225, 145),
                Color(255, 190, 125, 35)
            );
            graphics.FillPath(&headingGold, &headingPath);

            int panelRight = GetGamePanelRight(client.right);

            RECT panel1 = { PANEL_LEFT, 105, panelRight, 209 };
            DrawPanel(drawDC, panel1,
                L"Beyond the Game",
                g_panel1Subtitle,
                L"Balances for all starting eras, and all game formats",
                hoveredPanel == 1);

            RECT panel2 = { PANEL_LEFT, 219, panelRight, 323 };
            DrawPanel(drawDC, panel2,
                L"Ancient Wars",
                g_panel2Subtitle,
                L"Original ideas from Perun and Civ4Players community",
                hoveredPanel == 2);

            RECT panel3 = { PANEL_LEFT, 333, panelRight, 437 };
            DrawPanel(drawDC, panel3,
                L"Beyond The Quality of Life",
                g_panel3Subtitle,
                L"BTS enhanced with quality of life features, changeable timer, monitor, reporting, spectator, etc.",
                hoveredPanel == 3);

            RECT panel4 = { PANEL_LEFT, 447, panelRight, 551 };
            DrawPanel(drawDC, panel4,
                L"Beyond the Sword",
                g_panel4Subtitle,
                L"Firaxis Final Release unchanged",
                hoveredPanel == 4);

            if (ChatPanel_IsEnabled())
                ChatPanel_Draw(drawDC, clientWidth, clientHeight);

			DrawCacheResetCheckbox(drawDC, clientWidth, clientHeight);
			DrawGameDirectoryRow(drawDC, clientWidth, clientHeight);
			DrawLargeIconCheckbox(drawDC, clientWidth, clientHeight);
            ChatPanel_DrawToggle(drawDC, clientWidth, clientHeight);
            DrawWindows10CustomTitleBar(drawDC, clientWidth);
        }

        // Present the completed frame in one operation, then release the buffer.
        if (drawDC == bufferDC && bufferDC != NULL && bufferBitmap != NULL)
        {
            BitBlt(hdc, 0, 0, clientWidth, clientHeight,
                bufferDC, 0, 0, SRCCOPY);
            SelectObject(bufferDC, oldBitmap);
            DeleteObject(bufferBitmap);
            DeleteDC(bufferDC);
        }

        EndPaint(hwnd, &ps);
        return 0;
    }

    case WM_VSCROLL:
        ChatPanel_HandleScroll(wParam, lParam);
        return 0;

    case WM_MOUSEWHEEL:
        ChatPanel_HandleMouseWheel(wParam);
        return 0;

    case WM_DESTROY:
        SaveAllC4FSettings();
        IRC_Disconnect();
        PostQuitMessage(0);
        return 0;
    }

    return DefWindowProc(hwnd, uMsg, wParam, lParam);
}

// ============================================================
// MAIN
// ============================================================

int WINAPI wWinMain(
    HINSTANCE hInstance,
    HINSTANCE hPrevInstance,
    LPWSTR lpCmdLine,
    int nCmdShow)
{
    // Load persistent Civ4Fans preferences before creating the UI.
    if (InitialiseC4FSettingsPath())
        LoadC4FSettings();
    else
        OutputDebugStringW(L"Civ4Fans: could not initialise preferences path; preferences will not persist.\n");

    // Generate version-dependent text and paths
    swprintf_s(
        g_versionDisplay, 16,
        L"2.%02d", GAME_VERSION
    );

    swprintf_s(
        g_versionCompact, 16,
        L"%d", 200 + GAME_VERSION
    );

    swprintf_s(
        g_versionFolder, 64,
        L"Beyond The Game %s",
        g_versionDisplay
    );

	// Default installation root: the directory containing the version folder.
	if (g_gameRootPath[0] == L'\0')
	{
		wchar_t launcherDirectory[MAX_PATH] = L"";

		DWORD length = GetModuleFileNameW(
			NULL, launcherDirectory, MAX_PATH);

		if (length > 0 && length < MAX_PATH)
		{
			wchar_t* lastSlash = wcsrchr(launcherDirectory, L'\\');

			if (lastSlash)
			{
				*lastSlash = L'\0';

				wcscpy_s(g_gameRootPath, MAX_PATH, launcherDirectory);
				g_gameRootManuallySelected = false;
				SaveC4FSetting(L"GameRoot", g_gameRootPath);
			}
		}
	}

	// For automatic detection, use the version matching this launcher.
	if (!g_gameRootManuallySelected && g_gameRootPath[0] != L'\0')
	{
		swprintf_s(g_gameInstallPath, MAX_PATH,
			L"%s\\%s", g_gameRootPath, g_versionFolder);

		SaveC4FSetting(L"GameInstallPath", g_gameInstallPath);
	}

    swprintf_s(
		g_windowTitle, 64,
		L"%s",
		APP_NAME
    );

    swprintf_s(
        g_heading, 64,
        L"BEYOND THE GAME %s",
        g_versionDisplay
    );

    // Panel subtitles: keep the game-version number tied to GAME_VERSION.
    wcscpy_s(g_panel1Subtitle, L"Full Expansion");
    wcscpy_s(g_panel2Subtitle, L"Expansion and Balance for Ancient and TBG Teamers");
    swprintf_s(g_panel3Subtitle, 64, L"Beyond the Sword %s", g_versionDisplay);
    wcscpy_s(g_panel4Subtitle, L"Beyond the Sword 3.19");

    // Start GDI+
    GdiplusStartupInput gdiplusStartupInput;

    if (GdiplusStartup(
        &g_gdiplusToken,
        &gdiplusStartupInput,
        NULL) != Ok)
    {
        return 0;
    }

    // Load the background embedded in the executable's resources.
    g_background = LoadBackgroundFromResource(hInstance);

    // Register window class
    const wchar_t CLASS_NAME[] = L"BTGLauncherWindow";

    WNDCLASS wc = {};

    wc.lpfnWndProc = WindowProc;
    wc.hInstance = hInstance;
    wc.lpszClassName = CLASS_NAME;
    wc.hCursor = LoadCursor(NULL, IDC_ARROW);
    wc.hIcon = LoadIcon(
        hInstance,
        MAKEINTRESOURCE(IDI_APP_ICON)
    );

    if (!RegisterClass(&wc))
    {
        DestroyBackground();

        GdiplusShutdown(g_gdiplusToken);
        return 0;
    }

    // Select one of two fixed client layouts using the usable desktop area.
    // SPI_GETWORKAREA excludes the Windows taskbar.
    RECT workArea;
    SystemParametersInfo(SPI_GETWORKAREA, 0, &workArea, 0);
    int availableWidth = workArea.right - workArea.left;
    int availableHeight = workArea.bottom - workArea.top;

    if (availableWidth >= LARGE_WINDOW_WIDTH &&
        availableHeight >= LARGE_WINDOW_HEIGHT)
    {
        g_windowClientWidth = LARGE_WINDOW_WIDTH;
        g_windowClientHeight = LARGE_WINDOW_HEIGHT;
    }
    else
    {
        g_windowClientWidth = COMPACT_WINDOW_WIDTH;
        g_windowClientHeight = COMPACT_WINDOW_HEIGHT;
    }

    g_useCustomTitleBar = IsWindows10OrEarlier();
    DWORD actualWindowStyle = WINDOW_STYLE;
    if (g_useCustomTitleBar)
    {
        // Use a true popup-style top-level window on Windows 10. Merely
        // removing WS_CAPTION can leave native non-client frame elements
        // visible on some Windows configurations, creating a second bar.
        // The app draws its own title bar and handles minimise/close itself.
        actualWindowStyle = WS_POPUP | WS_CLIPCHILDREN | WS_SYSMENU | WS_MINIMIZEBOX;
    }

    RECT windowRect = {
        0, 0, g_windowClientWidth, g_windowClientHeight
    };

    AdjustWindowRect(
        &windowRect,
        actualWindowStyle,
        FALSE
    );

    // Create launcher window
    HWND hwnd = CreateWindowEx(
        0,
        CLASS_NAME,
        g_windowTitle,
        actualWindowStyle,
        workArea.left + ((availableWidth - (windowRect.right - windowRect.left)) / 2),
        workArea.top + ((availableHeight - (windowRect.bottom - windowRect.top)) / 2),
        windowRect.right - windowRect.left,
        windowRect.bottom - windowRect.top,
        NULL,
        NULL,
        hInstance,
        NULL
    );

    if (hwnd == NULL)
    {
        DestroyBackground();

        GdiplusShutdown(g_gdiplusToken);
        return 0;
    }

    if (!g_useCustomTitleBar)
        ApplyCiv4FansTitleBar(hwnd);

    ChatPanel_CreateInputControls(hwnd);
    ChatPanel_LayoutInputControls(hwnd);
    ChatPanel_SetInputVisible(ChatPanel_IsEnabled());

    if (IRC_Initialize(hwnd))
        ChatPanel_SetStatus(L"Not connected. Click Connect when you are ready.");
    else
        ChatPanel_SetStatus(L"Winsock initialisation failed.");

    ShowWindow(hwnd, nCmdShow);
    UpdateWindow(hwnd);

    // Message loop
    MSG msg = {};

    while (GetMessage(&msg, NULL, 0, 0) > 0)
    {
        TranslateMessage(&msg);
        DispatchMessage(&msg);
    }

    // Cleanup
    DestroyBackground();

    GdiplusShutdown(g_gdiplusToken);

    return (int)msg.wParam;
}
