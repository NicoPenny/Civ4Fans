#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <process.h>
#include <stdio.h>
#include <wchar.h>
#include <string.h>
#include "C4F_IRC.h"

#pragma comment(lib, "Ws2_32.lib")

static const char* IRC_SERVER = "kvali.pro";
static const char* IRC_PORT = "6667";
static const char* IRC_CHANNEL = "#GSP!civ4bts";

// ============================================================
// CIV4FANS WELCOME MESSAGE
// Edit this text and rebuild Civ4Fans to change the wording.
// %s will be replaced by the joining player's nickname.
// ============================================================

static const wchar_t* C4F_WELCOME_MESSAGE =
    L"Welcome %s to the Civ4Fans lobby! Have a good game.";

static HWND g_window = NULL;
static wchar_t g_localNickname[64] = L"";
static SOCKET g_socket = INVALID_SOCKET;
static HANDLE g_thread = NULL;
static CRITICAL_SECTION g_sendLock;
static BOOL g_lockReady = FALSE;
static BOOL g_wsaReady = FALSE;
static volatile LONG g_stop = 0;
static volatile LONG g_connected = 0;
static volatile LONG g_connecting = 0;
static wchar_t g_memberNames[128][64] = {};
static int g_memberCount = 0;
static BOOL g_batchMemberUpdate = FALSE;

static void DecodeIRCText(const char* source, wchar_t* destination, int capacity);

static void PostMemberList(void)
{
    IRC_MEMBER_LIST* list;
    if (!g_window) return;
    list = (IRC_MEMBER_LIST*)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY,
        sizeof(IRC_MEMBER_LIST));
    if (!list) return;
    list->count = g_memberCount;
    for (int i = 0; i < g_memberCount; ++i)
        wcsncpy_s(list->members[i], 64, g_memberNames[i], _TRUNCATE);
    if (!PostMessage(g_window, WM_IRC_MEMBERS, 0, (LPARAM)list))
        HeapFree(GetProcessHeap(), 0, list);
}

void IRC_FreeMemberList(IRC_MEMBER_LIST* memberList)
{
    if (memberList) HeapFree(GetProcessHeap(), 0, memberList);
}

static void ClearMembers(void)
{
    g_memberCount = 0;
    ZeroMemory(g_memberNames, sizeof(g_memberNames));
    PostMemberList();
}

static BOOL FindMember(const wchar_t* nickname)
{
    for (int i = 0; i < g_memberCount; ++i)
        if (_wcsicmp(g_memberNames[i], nickname) == 0) return TRUE;
    return FALSE;
}

static void AddMember(const char* nickname)
{
    wchar_t decoded[64];
    const char* clean = nickname;
    if (!clean) return;
    while (*clean == '~' || *clean == '&' || *clean == '@' ||
           *clean == '%' || *clean == '+') ++clean;
    if (!*clean) return;
    DecodeIRCText(clean, decoded, 64);
    if (!decoded[0] || FindMember(decoded) || g_memberCount >= 128) return;
    wcsncpy_s(g_memberNames[g_memberCount], 64, decoded, _TRUNCATE);
    ++g_memberCount;
    if (!g_batchMemberUpdate) PostMemberList();
}

static void RemoveMember(const char* nickname)
{
    wchar_t decoded[64];
    if (!nickname || !nickname[0]) return;
    DecodeIRCText(nickname, decoded, 64);
    for (int i = 0; i < g_memberCount; ++i)
    {
        if (_wcsicmp(g_memberNames[i], decoded) == 0)
        {
            for (int j = i; j < g_memberCount - 1; ++j)
                wcscpy_s(g_memberNames[j], 64, g_memberNames[j + 1]);
            --g_memberCount;
            g_memberNames[g_memberCount][0] = L'\0';
            PostMemberList();
            return;
        }
    }
}

static void RenameMember(const char* oldNickname, const char* newNickname)
{
    wchar_t oldDecoded[64], newDecoded[64];
    if (!oldNickname || !newNickname) return;
    DecodeIRCText(oldNickname, oldDecoded, 64);
    DecodeIRCText(newNickname, newDecoded, 64);
    for (int i = 0; i < g_memberCount; ++i)
    {
        if (_wcsicmp(g_memberNames[i], oldDecoded) == 0)
        {
            wcsncpy_s(g_memberNames[i], 64, newDecoded, _TRUNCATE);
            PostMemberList();
            return;
        }
    }
}

static void NickFromPrefix(const char* prefix, char* nickname, size_t capacity)
{
    const char* bang;
    size_t length;
    if (!nickname || capacity == 0) return;
    nickname[0] = '\0';
    if (!prefix) return;
    bang = strchr(prefix, '!');
    length = bang ? (size_t)(bang - prefix) : strlen(prefix);
    if (length >= capacity) length = capacity - 1;
    memcpy(nickname, prefix, length);
    nickname[length] = '\0';
}

static BOOL FirstParameterIsChannel(const char* parameters)
{
    char channel[128];
    const char* p = parameters;
    size_t length = 0;
    if (!p) return FALSE;
    while (*p == ' ') ++p;
    if (*p == ':') ++p;
    while (p[length] && p[length] != ' ' && length < sizeof(channel) - 1)
        ++length;
    memcpy(channel, p, length);
    channel[length] = '\0';
    return _stricmp(channel, IRC_CHANNEL) == 0;
}

static void PostStatus(const wchar_t* text)
{
    IRC_EVENT* e;
    if (!g_window) return;
    e = (IRC_EVENT*)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(IRC_EVENT));
    if (!e) return;
    wcsncpy_s(e->text, 512, text, _TRUNCATE);
    if (!PostMessage(g_window, WM_IRC_STATUS, 0, (LPARAM)e))
        HeapFree(GetProcessHeap(), 0, e);
}

// Decode IRC text as UTF-8. If a legacy client sends invalid UTF-8,
// fall back to Windows-1252 so existing Western-European messages remain readable.
static void DecodeIRCText(const char* source, wchar_t* destination, int capacity)
{
    if (!source || !destination || capacity <= 0) return;

    destination[0] = L'\0';
    if (MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS,
        source, -1, destination, capacity) > 0)
        return;

    MultiByteToWideChar(1252, 0, source, -1, destination, capacity);
}

static void PostChat(const char* nick, const char* target, const char* text)
{
    IRC_EVENT* e;
    if (!g_window) return;
    e = (IRC_EVENT*)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(IRC_EVENT));
    if (!e) return;
    DecodeIRCText(nick, e->sender, 64);
    DecodeIRCText(target, e->target, 64);
    e->isPrivate = (_stricmp(target, IRC_CHANNEL) != 0);
    DecodeIRCText(text, e->text, 512);
    if (!PostMessage(g_window, WM_IRC_MESSAGE, 0, (LPARAM)e))
        HeapFree(GetProcessHeap(), 0, e);
}

void IRC_FreeEvent(IRC_EVENT* e)
{
    if (e) HeapFree(GetProcessHeap(), 0, e);
}

static BOOL SendRaw(const char* data)
{
    int len = (int)strlen(data), done = 0, n;
    if (g_socket == INVALID_SOCKET) return FALSE;
    EnterCriticalSection(&g_sendLock);
    while (done < len)
    {
        n = send(g_socket, data + done, len - done, 0);
        if (n <= 0)
        {
            LeaveCriticalSection(&g_sendLock);
            return FALSE;
        }
        done += n;
    }
    LeaveCriticalSection(&g_sendLock);
    return TRUE;
}

static BOOL SendLine(const char* line)
{
    char b[1200];
    if (strlen(line) > sizeof(b) - 3) return FALSE;
    strcpy_s(b, sizeof(b), line);
    strcat_s(b, sizeof(b), "\r\n");
    return SendRaw(b);
}

// ============================================================
// WELCOME MESSAGE SENDER ELECTION
// Penny_C4F has priority. Otherwise, the alphabetically
// first connected _C4F member sends the welcome.
// ============================================================

static BOOL IsLocalWelcomeSender(void)
{
    const wchar_t* winner = NULL;
    int i;

    // Penny always has priority.
    for (i = 0; i < g_memberCount; ++i)
    {
        if (_wcsicmp(g_memberNames[i], L"Penny_C4F") == 0)
        {
            winner = g_memberNames[i];
            break;
        }
    }

    // Otherwise, elect the alphabetically first C4F member.
    if (!winner)
    {
        for (i = 0; i < g_memberCount; ++i)
        {
            size_t length = wcslen(g_memberNames[i]);
            size_t suffixLength = wcslen(IRC_NICK_SUFFIX);

            if (length < suffixLength ||
                _wcsicmp(g_memberNames[i] + length - suffixLength,
                         IRC_NICK_SUFFIX) != 0)
                continue;

            if (!winner ||
                _wcsicmp(g_memberNames[i], winner) < 0)
            {
                winner = g_memberNames[i];
            }
        }
    }

    return winner != NULL &&
           g_localNickname[0] != L'\0' &&
           _wcsicmp(winner, g_localNickname) == 0;
}

// ============================================================
// SEND WELCOME MESSAGE
// ============================================================

static void SendWelcomeMessage(const wchar_t* joiningNickname)
{
    wchar_t welcomeText[512];
    char message[512];
    char ircLine[768];
    int i;

    if (!joiningNickname || !joiningNickname[0])
        return;

    // Format the welcome text, inserting the joining player's nickname.
    if (swprintf_s(welcomeText, 512, C4F_WELCOME_MESSAGE,
                   joiningNickname) < 0)
        return;

    // Prevent accidental IRC command injection.
    for (i = 0; welcomeText[i]; ++i)
    {
        if (welcomeText[i] == L'\r' || welcomeText[i] == L'\n')
            return;
    }

    // Convert to the same Windows-1252 encoding used by outgoing chat.
    if (!WideCharToMultiByte(CP_ACP, 0, welcomeText, -1,
                             message, sizeof(message),
                             NULL, NULL))
        return;

    // Send through the elected client's own IRC nickname.
    sprintf_s(ircLine, sizeof(ircLine),
              "PRIVMSG %s :%s", IRC_CHANNEL, message);

    SendLine(ircLine);
}

static void MakeNick(const wchar_t* base, char* out, size_t outSize)
{
    wchar_t nick[128];
    char utf8[256];
    size_t n;
    if (!base || !base[0]) base = L"Civ4Fan";
    wcsncpy_s(nick, 128, base, 24);
    nick[24] = L'\0';
    n = wcslen(nick);
    if (n < 4 || _wcsicmp(nick + n - 4, L"_C4F") != 0)
        wcscat_s(nick, 128, L"_C4F");
    WideCharToMultiByte(CP_UTF8, 0, nick, -1, utf8, sizeof(utf8), NULL, NULL);
    strncpy_s(out, outSize, utf8, _TRUNCATE);
}

static unsigned __stdcall IrcWorker(void* arg)
{
	// Pending welcome messages, delayed to allow member lists to update.
	wchar_t pendingWelcomeNick[128][64] = { 0 };
	DWORD pendingWelcomeAt[128] = { 0 };
	int pendingWelcomeCount = 0;

	// Short receive timeout so pending welcomes can be processed
	// even when the lobby is otherwise quiet.
	DWORD receiveTimeout = 250;


    const wchar_t* requestedNick = (const wchar_t*)arg;
    wchar_t nickCopy[128];
    char nick[128], userLine[256], rx[4096], line[4096];
    struct addrinfo hints, *list = NULL, *p;
    int rc, got, used = 0, i;
    BOOL joined = FALSE;

    wcsncpy_s(nickCopy, 128, requestedNick ? requestedNick : L"Civ4Fan", _TRUNCATE);
    HeapFree(GetProcessHeap(), 0, (void*)arg);
    MakeNick(nickCopy, nick, sizeof(nick));

	// Store our actual IRC nickname for sender election.
	g_localNickname[0] = L'\0';
	MultiByteToWideChar(CP_UTF8, 0, nick, -1,
						g_localNickname, 64);

    ClearMembers();
    PostStatus(L"Connecting to kvali.pro...");
    ZeroMemory(&hints, sizeof(hints));
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_protocol = IPPROTO_TCP;

    rc = getaddrinfo(IRC_SERVER, IRC_PORT, &hints, &list);
    if (rc != 0)
    {
        PostStatus(L"Could not resolve the IRC server.");
        goto finished;
    }

    for (p = list; p; p = p->ai_next)
    {
        g_socket = socket(p->ai_family, p->ai_socktype, p->ai_protocol);
        if (g_socket == INVALID_SOCKET) continue;
        if (connect(g_socket, p->ai_addr, (int)p->ai_addrlen) == 0) break;
        closesocket(g_socket);
        g_socket = INVALID_SOCKET;
    }

	// Set a short receive timeout for pending welcome messages.
	if (g_socket != INVALID_SOCKET)
	{
		setsockopt(g_socket, SOL_SOCKET, SO_RCVTIMEO,
			(const char*)&receiveTimeout, sizeof(receiveTimeout));
	}

    freeaddrinfo(list);
    list = NULL;

    if (g_socket == INVALID_SOCKET)
    {
        PostStatus(L"Could not connect to the IRC server.");
        goto finished;
    }

    {
        char cmd[256];
        sprintf_s(cmd, sizeof(cmd), "NICK %s", nick);
        SendLine(cmd);
    }
    sprintf_s(userLine, sizeof(userLine), "USER %s 0 * :Civ4Fans client", nick);
    SendLine(userLine);
    PostStatus(L"Connected; waiting to join the channel...");

    while (InterlockedCompareExchange(&g_stop, 0, 0) == 0)
    {
        got = recv(g_socket, rx, sizeof(rx), 0);
		if (got == 0) break;

		if (got == SOCKET_ERROR)
		{
			int error = WSAGetLastError();

			// A receive timeout is normal; continue so pending
			// welcome messages can be processed.
			if (error == WSAETIMEDOUT || error == WSAEWOULDBLOCK)
			{
				got = 0;
			}
			else
			{
				break;
			}
		}

        for (i = 0; i < got; ++i)
        {
            char* space;
            char* msg;
            char* bang;
            char* nickStart;
            char* cmdline;
            char pong[512];

            if (rx[i] == '\r') continue;
            if (rx[i] != '\n')
            {
                if (used < (int)sizeof(line) - 1) line[used++] = rx[i];
                continue;
            }

            line[used] = '\0';
            used = 0;

            if (_strnicmp(line, "PING ", 5) == 0)
            {
                sprintf_s(pong, sizeof(pong), "PONG %s", line + 5);
                SendLine(pong);
                continue;
            }

            if (strstr(line, " 001 ") != NULL && !joined)
            {
                {
                    char joinLine[256];
                    sprintf_s(joinLine, sizeof(joinLine), "JOIN %s", IRC_CHANNEL);
                    SendLine(joinLine);
                }
                joined = TRUE;
                InterlockedExchange(&g_connected, 1);
                PostStatus(L"Joined #GSP!civ4bts.");
                continue;
            }

            if (line[0] != ':') continue;
            space = strchr(line, ' ');
            if (!space) continue;
            *space = '\0';
            cmdline = space + 1;
            nickStart = line + 1;
            {
                char sourceNick[128];
                NickFromPrefix(nickStart, sourceNick, sizeof(sourceNick));

                // Initial channel member list (RPL_NAMREPLY, numeric 353).
                if (strncmp(cmdline, "353 ", 4) == 0)
                {
                    char* names = strstr(cmdline, " :");
                    if (names)
                    {
                        char* token;
                        char* context = NULL;
                        names += 2;
                        g_batchMemberUpdate = TRUE;
                        token = strtok_s(names, " ", &context);
                        while (token)
                        {
                            AddMember(token);
                            token = strtok_s(NULL, " ", &context);
                        }
                        g_batchMemberUpdate = FALSE;
                        PostMemberList();
                    }
                    continue;
                }

				// A JOIN event adds the joining nickname to the live list
				// and schedules a welcome message after a short delay.
				if (strncmp(cmdline, "JOIN ", 5) == 0)
				{
					if (FirstParameterIsChannel(cmdline + 5))
					{
						wchar_t joiningNick[64] = L"";

						AddMember(sourceNick);

						MultiByteToWideChar(CP_UTF8, 0, sourceNick, -1,
											joiningNick, 64);

						// Never welcome ourselves.
						if (joiningNick[0] &&
							_wcsicmp(joiningNick, g_localNickname) != 0 &&
							pendingWelcomeCount < 128)
						{
							int pendingIndex = pendingWelcomeCount++;

							wcscpy_s(pendingWelcomeNick[pendingIndex], 64,
									 joiningNick);

							pendingWelcomeAt[pendingIndex] = GetTickCount() + 1500;
						}
					}
					continue;
				}

                // PART and KICK only affect this channel; QUIT affects all channels.
                if (strncmp(cmdline, "PART ", 5) == 0)
                {
                    if (FirstParameterIsChannel(cmdline + 5))
                        RemoveMember(sourceNick);
                    continue;
                }
                if (strncmp(cmdline, "QUIT", 4) == 0)
                {
                    RemoveMember(sourceNick);
                    continue;
                }
                if (strncmp(cmdline, "KICK ", 5) == 0)
                {
                    char* target = cmdline + 5;
                    char* separator = strchr(target, ' ');
                    if (separator)
                    {
                        *separator = '\0';
                        if (_stricmp(target, IRC_CHANNEL) == 0)
                        {
                            char* kicked = separator + 1;
                            while (*kicked == ' ') ++kicked;
                            separator = strchr(kicked, ' ');
                            if (separator) *separator = '\0';
                            RemoveMember(kicked);
                        }
                    }
                    continue;
                }
                if (strncmp(cmdline, "NICK ", 5) == 0)
                {
                    char* newNick = cmdline + 5;
                    if (*newNick == ':') ++newNick;
                    RenameMember(sourceNick, newNick);
                    continue;
                }
            }

            if (strncmp(cmdline, "PRIVMSG ", 8) == 0)
            {
                char target[128];
                char* targetEnd = strchr(cmdline + 8, ' ');
                if (!targetEnd) continue;
                *targetEnd = '\0';
                strncpy_s(target, sizeof(target), cmdline + 8, _TRUNCATE);
                msg = strstr(targetEnd + 1, ":");
                if (!msg) continue;
                ++msg;
                while (*msg == ' ') ++msg;
                bang = strchr(nickStart, '!');
                if (bang) *bang = '\0';
                PostChat(nickStart, target, msg);
            }
        }

        // Process welcome messages whose delay has expired.
        {
            DWORD now = GetTickCount();
            int pendingIndex = 0;

            while (pendingIndex < pendingWelcomeCount)
            {
                if ((LONG)(now - pendingWelcomeAt[pendingIndex]) >= 0)
                {
                    // Only the elected C4F client sends the welcome.
                    if (IsLocalWelcomeSender())
                    {
                        SendWelcomeMessage(
                            pendingWelcomeNick[pendingIndex]);
                    }

                    // Remove this entry from the pending queue.
                    for (int j = pendingIndex;
                         j < pendingWelcomeCount - 1; ++j)
                    {
                        wcscpy_s(pendingWelcomeNick[j], 64,
                                 pendingWelcomeNick[j + 1]);

                        pendingWelcomeAt[j] = pendingWelcomeAt[j + 1];
                    }

                    --pendingWelcomeCount;
                }
                else
                {
                    ++pendingIndex;
                }
            }
        }
    }

finished:
    ClearMembers();
    if (list) freeaddrinfo(list);
    InterlockedExchange(&g_connected, 0);
    if (g_socket != INVALID_SOCKET)
    {
        shutdown(g_socket, SD_BOTH);
        closesocket(g_socket);
        g_socket = INVALID_SOCKET;
    }
    if (InterlockedCompareExchange(&g_stop, 0, 0) == 0)
        PostStatus(L"IRC connection closed.");
    InterlockedExchange(&g_connecting, 0);
    return 0;
}

BOOL IRC_Initialize(HWND targetWindow)
{
    WSADATA data;
    g_window = targetWindow;
    if (!g_lockReady)
    {
        InitializeCriticalSection(&g_sendLock);
        g_lockReady = TRUE;
    }
    if (!g_wsaReady)
    {
        if (WSAStartup(MAKEWORD(2, 2), &data) != 0) return FALSE;
        g_wsaReady = TRUE;
    }
    return TRUE;
}

BOOL IRC_Connect(const wchar_t* baseNickname)
{
    wchar_t* copy;
    size_t count;
    uintptr_t handle;

    if (!g_window || !g_wsaReady) return FALSE;
    if (InterlockedCompareExchange(&g_connecting, 1, 0) != 0) return FALSE;

    if (g_thread)
    {
        WaitForSingleObject(g_thread, INFINITE);
        CloseHandle(g_thread);
        g_thread = NULL;
    }

    if (!baseNickname) baseNickname = L"Civ4Fan";
    count = wcslen(baseNickname) + 1;
    copy = (wchar_t*)HeapAlloc(GetProcessHeap(), 0, count * sizeof(wchar_t));
    if (!copy)
    {
        InterlockedExchange(&g_connecting, 0);
        return FALSE;
    }
    wcscpy_s(copy, count, baseNickname);
    InterlockedExchange(&g_stop, 0);

    handle = _beginthreadex(NULL, 0, IrcWorker, copy, 0, NULL);
    if (!handle)
    {
        HeapFree(GetProcessHeap(), 0, copy);
        InterlockedExchange(&g_connecting, 0);
        return FALSE;
    }
    g_thread = (HANDLE)handle;
    return TRUE;
}

BOOL IRC_SendMessage(const wchar_t* message)
{
    char encoded[1000], line[1200];
    if (!message || !IRC_IsConnected()) return FALSE;
    // The current Civ4 lobby clients interpret outgoing chat as a
    // Western European single-byte encoding (CP1252), not UTF-8.
    // This restores French accents; Russian requires UTF-8 support
    // across the other lobby clients as well.
    if (!WideCharToMultiByte(1252, 0, message, -1, encoded,
        sizeof(encoded), NULL, NULL))
        return FALSE;
    if (strchr(encoded, '\r') || strchr(encoded, '\n')) return FALSE;
    sprintf_s(line, sizeof(line), "PRIVMSG %s :%s", IRC_CHANNEL, encoded);
    return SendLine(line);
}

BOOL IRC_SendPrivateMessage(const wchar_t* nickname, const wchar_t* message)
{
    char encodedNick[256], encodedMessage[1000], line[1200];
    if (!nickname || !nickname[0] || !message || !IRC_IsConnected()) return FALSE;
    if (!WideCharToMultiByte(CP_UTF8, 0, nickname, -1, encodedNick,
        sizeof(encodedNick), NULL, NULL)) return FALSE;
    if (!WideCharToMultiByte(1252, 0, message, -1, encodedMessage,
        sizeof(encodedMessage), NULL, NULL)) return FALSE;
    if (strchr(encodedMessage, '\r') || strchr(encodedMessage, '\n')) return FALSE;
    sprintf_s(line, sizeof(line), "PRIVMSG %s :%s", encodedNick, encodedMessage);
    return SendLine(line);
}

void IRC_Disconnect(void)
{
    InterlockedExchange(&g_stop, 1);
    if (g_socket != INVALID_SOCKET) shutdown(g_socket, SD_BOTH);
    if (g_thread)
    {
        WaitForSingleObject(g_thread, INFINITE);
        CloseHandle(g_thread);
        g_thread = NULL;
    }
    InterlockedExchange(&g_connected, 0);
    InterlockedExchange(&g_connecting, 0);
}

BOOL IRC_IsConnected(void)
{
    return InterlockedCompareExchange(&g_connected, 0, 0) != 0;
}
