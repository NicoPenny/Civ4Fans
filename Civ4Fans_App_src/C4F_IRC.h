#ifndef C4F_IRC_H
#define C4F_IRC_H

#include <windows.h>

#define WM_IRC_STATUS  (WM_APP + 101)
#define WM_IRC_MESSAGE (WM_APP + 102)
#define WM_IRC_MEMBERS (WM_APP + 103)
#define IRC_NICK_SUFFIX L"_C4F"

typedef struct IRC_MEMBER_LIST
{
    int count;
    wchar_t members[128][64];
} IRC_MEMBER_LIST;

typedef struct IRC_EVENT
{
    wchar_t sender[64];
    wchar_t text[512];
    wchar_t target[64];
    BOOL isPrivate;
} IRC_EVENT;

/* Call IRC_FreeEvent() on every event pointer received in lParam. */
BOOL IRC_Initialize(HWND targetWindow);
BOOL IRC_Connect(const wchar_t* baseNickname);
BOOL IRC_SendMessage(const wchar_t* message);
BOOL IRC_SendPrivateMessage(const wchar_t* nickname, const wchar_t* message);
void IRC_Disconnect(void);
BOOL IRC_IsConnected(void);
void IRC_FreeEvent(IRC_EVENT* eventData);
void IRC_FreeMemberList(IRC_MEMBER_LIST* memberList);

#endif
