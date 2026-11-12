#ifndef C4F_CHAT_PANEL_H
#define C4F_CHAT_PANEL_H

#define C4F_CHAT_INPUT_ID  2001
#define C4F_CHAT_SEND_ID   2002
#define C4F_PRIVATE_INPUT_ID 2004
#define C4F_PRIVATE_SEND_ID  2005

#include <windows.h>

bool ChatPanel_IsEnabled();
void ChatPanel_SetEnabled(bool enabled);
void ChatPanel_Toggle();
bool ChatPanel_ToggleHitTest(int x, int y, int clientHeight);
bool ChatPanel_ToggleHoverHitTest(int x, int y, int clientHeight);
bool ChatPanel_ConnectHitTest(int x, int y, int clientWidth, int clientHeight);
void ChatPanel_SetToggleHovered(bool hovered);
void ChatPanel_SetStatus(const wchar_t* status);
void ChatPanel_SetConnectionState(bool connected, const wchar_t* nickname);
void ChatPanel_CreateInputControls(HWND parent);
void ChatPanel_LayoutInputControls(HWND parent);
void ChatPanel_HandleScroll(WPARAM wParam, LPARAM lParam);
void ChatPanel_HandleMouseWheel(WPARAM wParam);
void ChatPanel_SetInputVisible(bool visible);
void ChatPanel_GetInputText(wchar_t* buffer, int bufferCount);
void ChatPanel_ClearInput();
void ChatPanel_GetPrivateInputText(wchar_t* buffer, int bufferCount);
void ChatPanel_ClearPrivateInput();
HBRUSH ChatPanel_GetEditBrush();
void ChatPanel_AddMessage(const wchar_t* sender, const wchar_t* message);
void ChatPanel_AddPrivateMessage(const wchar_t* peer, const wchar_t* sender, const wchar_t* message);
void ChatPanel_AddPrivateEcho(const wchar_t* peer, const wchar_t* message);
bool ChatPanel_HandleMemberClick(int x, int y, int clientWidth, int clientHeight);
bool ChatPanel_IsPrivateConversationActive();
const wchar_t* ChatPanel_GetActiveRecipient();
void ChatPanel_SetMembers(const wchar_t members[][64], int count);
void ChatPanel_TimerTick(HWND parent);
void ChatPanel_Draw(HDC hdc, int clientWidth, int clientHeight);
void ChatPanel_DrawToggle(HDC hdc, int clientWidth, int clientHeight);

#endif
