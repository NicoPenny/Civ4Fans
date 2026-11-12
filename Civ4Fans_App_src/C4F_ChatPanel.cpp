// ============================================================
// IRC CHAT FEATURE SWITCH
// false = hide all IRC UI (community release)
// true  = show IRC UI (development)
// ============================================================

const bool ENABLE_IRC_CHAT_UI = true;

#include "C4F_ChatPanel.h"
#include <gdiplus.h>
#include <wchar.h>
#include <vector>


using namespace Gdiplus;

namespace
{
    bool g_chatEnabled = false;
    bool g_toggleHovered = false;

    const int CHAT_TOGGLE_LEFT = 40;
    const int CHAT_TOGGLE_RIGHT = 650;
    const int CHAT_TOGGLE_HEIGHT = 48;
    const int CHAT_TOGGLE_BOTTOM_MARGIN = 10;
    const int CHAT_MEMBER_COLUMN_WIDTH = 190;
    const UINT CHAT_FLASH_TIMER_ID = 0xC4F2;

    int GetChatPanelWidth(int clientWidth)
    {
        // The larger launcher layout gives the transcript substantially more room.
        return clientWidth >= 1500 ? 850 : 650;
    }
    const int CHAT_COLUMN_GAP = 12;
    const int CHAT_SCROLLBAR_WIDTH = 16;
    const int CHAT_SCROLLBAR_ID = 2003;
    const wchar_t CHAT_SCROLLBAR_CLASS[] = L"C4F_CustomChatScrollBar";
    const COLORREF CHAT_GOLD = RGB(196, 158, 82);
    wchar_t g_chatStatus[256] = L"Not connected";
    wchar_t g_chatNickname[64] = L"";
    bool g_chatConnected = false;
    struct ScrollState { int max; int page; int position; bool dragging; int dragStartY; int dragStartPos; };
    ScrollState g_publicScroll = { 0, 1, 0, false, 0, 0 };
    ScrollState g_privateScroll = { 0, 1, 0, false, 0, 0 };
    struct ChatLine { wchar_t sender[64]; wchar_t text[512]; wchar_t time[8]; };
    // Keep the complete chat transcript for the lifetime of the application.
    // A fixed 30-line buffer silently discarded older lobby discussions.
    std::vector<ChatLine> g_messages;
    struct PrivateConversation
    {
        wchar_t peer[64];
        std::vector<ChatLine> messages;
        bool unread;
    };
    bool g_flashPhase = false;
    std::vector<PrivateConversation> g_privateConversations;
    int g_activePrivateConversation = -1;
    wchar_t g_lobbyMembers[128][64] = {};
    int g_lobbyMemberCount = 0;
    HWND g_inputEdit = NULL;
    HWND g_sendButton = NULL;
    HWND g_privateInputEdit = NULL;
    HWND g_privateSendButton = NULL;
    HBRUSH g_inputBrush = NULL;
    HFONT g_inputFont = NULL;
    WNDPROC g_oldInputProc = NULL;
    WNDPROC g_oldPrivateInputProc = NULL;
    HWND g_chatScrollbar = NULL;
    HWND g_privateScrollbar = NULL;
    bool g_scrollToBottom = true;
    const int CHAT_MESSAGE_GAP = 7;

    RECT GetChatPanelRect(int clientWidth, int clientHeight)
    {
        RECT r;
        r.left = clientWidth - 40 - GetChatPanelWidth(clientWidth);
        r.top = 105;
        r.right = clientWidth - 40;
        r.bottom = clientHeight - 10;
        return r;
    }

    int GetChatColumnLeft(const RECT& panel)
    {
        return panel.left + 18;
    }

    int GetChatColumnRight(const RECT& panel)
    {
        int memberLeft = panel.right - 18 - CHAT_MEMBER_COLUMN_WIDTH;
        return memberLeft - CHAT_COLUMN_GAP;
    }

    RECT GetChatMessageArea(const RECT& panel)
    {
        RECT area;
        area.left = GetChatColumnLeft(panel);
        area.top = panel.top + 140;
        area.right = GetChatColumnRight(panel) - CHAT_SCROLLBAR_WIDTH - 4;
        area.bottom = panel.bottom - 92;
        return area;
    }

    RECT GetMemberArea(const RECT& panel)
    {
        RECT area = { GetChatColumnRight(panel) + CHAT_COLUMN_GAP,
            panel.top + 140, panel.right - 18, panel.bottom - 92 };
        return area;
    }

    int GetPrivateBoxTop(const RECT& memberArea)
    {
        // Move the private-chat dock upward to give the conversation more room.
        return memberArea.bottom - 220;
    }

    bool IsC4FMember(const wchar_t* nickname)
    {
        if (!nickname) return false;
        size_t length = wcslen(nickname);
        return length >= 4 && _wcsicmp(nickname + length - 4, L"_C4F") == 0;
    }

    int FindPrivateConversation(const wchar_t* peer)
    {
        for (int i = 0; i < (int)g_privateConversations.size(); ++i)
            if (_wcsicmp(g_privateConversations[i].peer, peer) == 0) return i;
        return -1;
    }

    int EnsurePrivateConversation(const wchar_t* peer)
    {
        int index = FindPrivateConversation(peer);
        if (index >= 0) return index;
        PrivateConversation conversation;
        conversation.peer[0] = L'\0';
        conversation.unread = false;
        wcsncpy_s(conversation.peer, 64, peer, _TRUNCATE);
        g_privateConversations.push_back(conversation);
        return (int)g_privateConversations.size() - 1;
    }


    void AddLine(std::vector<ChatLine>& messages, const wchar_t* sender, const wchar_t* message)
    {
        messages.push_back(ChatLine());
        ChatLine& line = messages.back();
        wcsncpy_s(line.sender, 64, sender ? sender : L"?", _TRUNCATE);
        wcsncpy_s(line.text, 512, message ? message : L"", _TRUNCATE);
        SYSTEMTIME now;
        GetLocalTime(&now);
        swprintf_s(line.time, 8, L"%02u:%02u", now.wHour, now.wMinute);
    }

    void DrawMemberSection(HDC hdc, const RECT& area, const wchar_t* heading,
                           const wchar_t* const* names, int count, bool c4fSection)
    {
        RECT headingRect = area;
        headingRect.bottom = headingRect.top + 22;
        SetTextColor(hdc, RGB(224, 190, 128));
        DrawText(hdc, heading, -1, &headingRect,
            DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);

        HFONT memberFont = CreateFont(14, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
            DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
            CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Arial");
        HFONT oldMemberFont = (HFONT)SelectObject(hdc, memberFont);
        int y = area.top + 24;
        const int rowHeight = 18;
        int availableRows = (area.bottom - y) / rowHeight;
        if (availableRows < 0) availableRows = 0;
        int shown = count < availableRows ? count : availableRows;
        for (int i = 0; i < shown; ++i)
        {
            RECT row = { area.left, y, area.right, y + rowHeight };
            bool unread = false;
            if (c4fSection)
            {
                int ci = FindPrivateConversation(names[i]);
                unread = ci >= 0 && g_privateConversations[ci].unread;
            }
            if (unread)
            {
                // Highlight only the nickname, not the full member-list row.
                SIZE nameSize = { 0, 0 };
                GetTextExtentPoint32(hdc, names[i], (int)wcslen(names[i]), &nameSize);
                RECT nameRect = row;
                nameRect.right = row.left + nameSize.cx + 8;
                if (nameRect.right > row.right) nameRect.right = row.right;
                nameRect.top += 1;
                nameRect.bottom -= 1;

                if (g_flashPhase)
                {
                    HBRUSH flashBg = CreateSolidBrush(RGB(126, 91, 43));
                    FillRect(hdc, &nameRect, flashBg);
                    DeleteObject(flashBg);
                    SetTextColor(hdc, RGB(255, 239, 207));
                }
                else
                {
                    SetTextColor(hdc, RGB(238, 201, 139));
                }
                RECT nameText = row;
                nameText.right = nameRect.right;
                DrawText(hdc, names[i], -1, &nameText,
                    DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
            }
            else
            {
                SetTextColor(hdc, RGB(220, 216, 205));
                DrawText(hdc, names[i], -1, &row,
                    DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
            }
            y += rowHeight;
        }

        if (count == 0)
        {
            RECT emptyRow = { area.left, y, area.right, y + rowHeight };
            SetTextColor(hdc, RGB(135, 132, 124));
            DrawText(hdc, L"(none)", -1, &emptyRow,
                DT_LEFT | DT_VCENTER | DT_SINGLELINE);
        }
        else if (count > shown)
        {
            wchar_t more[32];
            swprintf_s(more, 32, L"+ %d more", count - shown);
            RECT moreRow = { area.left, y, area.right, y + rowHeight };
            SetTextColor(hdc, RGB(160, 151, 132));
            DrawText(hdc, more, -1, &moreRow,
                DT_LEFT | DT_VCENTER | DT_SINGLELINE);
        }
        SelectObject(hdc, oldMemberFont);
        DeleteObject(memberFont);
    }

    LRESULT CALLBACK ChatInputProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
    {
        if (msg == WM_KEYDOWN && wParam == VK_RETURN)
        {
            HWND parent = GetParent(hwnd);
            bool privateInput = (hwnd == g_privateInputEdit);
            SendMessage(parent, WM_COMMAND,
                MAKEWPARAM(privateInput ? C4F_PRIVATE_SEND_ID : C4F_CHAT_SEND_ID, BN_CLICKED),
                (LPARAM)(privateInput ? g_privateSendButton : g_sendButton));
            return 0;
        }
        WNDPROC oldProc = (hwnd == g_privateInputEdit) ? g_oldPrivateInputProc : g_oldInputProc;
        return CallWindowProc(oldProc, hwnd, msg, wParam, lParam);
    }

    ScrollState* GetScrollState(HWND hwnd)
    {
        return hwnd == g_privateScrollbar ? &g_privateScroll : &g_publicScroll;
    }

    void SetCustomScrollPosition(HWND hwnd, int position)
    {
        ScrollState* state = GetScrollState(hwnd);
        if (position < 0) position = 0;
        if (position > state->max) position = state->max;
        state->position = position;
        if (hwnd == g_chatScrollbar)
            g_scrollToBottom = (state->position >= state->max);
        InvalidateRect(hwnd, NULL, FALSE);
        HWND parent = GetParent(hwnd);
        if (parent)
        {
            RECT client; GetClientRect(parent, &client);
            RECT panel = GetChatPanelRect(client.right, client.bottom);
            RECT area = (hwnd == g_privateScrollbar) ? GetMemberArea(panel) : GetChatMessageArea(panel);
            InvalidateRect(parent, &area, FALSE);
            UpdateWindow(parent);
        }
        UpdateWindow(hwnd);
    }

    LRESULT CALLBACK ChatScrollBarProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
    {
        ScrollState* state = GetScrollState(hwnd);
        RECT r; GetClientRect(hwnd, &r);
        const int h = r.bottom - r.top;
        const int arrow = 16;
        const int trackTop = arrow;
        const int trackBottom = h - arrow;
        const int trackHeight = trackBottom - trackTop;
        int thumbHeight = trackHeight;
        if (state->max > 0 && state->page > 0)
        {
            thumbHeight = (int)((double)trackHeight * state->page / (state->page + state->max));
            if (thumbHeight < 24) thumbHeight = 24;
            if (thumbHeight > trackHeight) thumbHeight = trackHeight;
        }
        int travel = trackHeight - thumbHeight;
        int thumbTop = trackTop;
        if (state->max > 0 && travel > 0)
            thumbTop += (int)((double)travel * state->position / state->max);
        switch (msg)
        {
        case WM_ERASEBKGND: return 1;
        case WM_PAINT:
        {
            PAINTSTRUCT ps; HDC hdc = BeginPaint(hwnd, &ps);
            HDC mem = CreateCompatibleDC(hdc);
            HBITMAP bmp = CreateCompatibleBitmap(hdc, r.right, r.bottom);
            HBITMAP oldBmp = (HBITMAP)SelectObject(mem, bmp);
            RECT all = { 0, 0, r.right, r.bottom };
            HBRUSH bg = CreateSolidBrush(RGB(38,39,40)); FillRect(mem, &all, bg); DeleteObject(bg);
            HBRUSH track = CreateSolidBrush(RGB(58,53,45));
            RECT tr = { 2, trackTop, r.right-2, trackBottom }; FillRect(mem, &tr, track); DeleteObject(track);
            HBRUSH thumb = CreateSolidBrush(state->dragging ? RGB(190,157,105) : RGB(145,117,76));
            RECT th = { 2, thumbTop, r.right-2, thumbTop + thumbHeight }; FillRect(mem, &th, thumb); DeleteObject(thumb);
            HPEN gold = CreatePen(PS_SOLID, 1, RGB(220,195,150)); HPEN oldPen = (HPEN)SelectObject(mem, gold);
            HBRUSH noBrush = (HBRUSH)SelectObject(mem, GetStockObject(NULL_BRUSH));
            Rectangle(mem, th.left, th.top, th.right, th.bottom);
            SelectObject(mem, noBrush); SelectObject(mem, oldPen); DeleteObject(gold);
            POINT up[3] = { {r.right/2,4},{5,12},{r.right-5,12} };
            POINT down[3] = { {5,h-12},{r.right-5,h-12},{r.right/2,h-4} };
            HBRUSH ab = CreateSolidBrush(RGB(220,184,119)); SelectObject(mem, ab);
            Polygon(mem, up, 3); Polygon(mem, down, 3); DeleteObject(ab);
            BitBlt(hdc,0,0,r.right,r.bottom,mem,0,0,SRCCOPY);
            SelectObject(mem,oldBmp); DeleteObject(bmp); DeleteDC(mem); EndPaint(hwnd,&ps); return 0;
        }
        case WM_LBUTTONDOWN:
        {
            int y=(short)HIWORD(lParam);
            if(y<arrow) SetCustomScrollPosition(hwnd,state->position-24);
            else if(y>=h-arrow) SetCustomScrollPosition(hwnd,state->position+24);
            else if(y>=thumbTop && y<thumbTop+thumbHeight)
            { state->dragging=true; state->dragStartY=y; state->dragStartPos=state->position; SetCapture(hwnd); }
            else if(y<thumbTop) SetCustomScrollPosition(hwnd,state->position-state->page);
            else SetCustomScrollPosition(hwnd,state->position+state->page);
            return 0;
        }
        case WM_MOUSEMOVE:
            if(state->dragging && travel>0)
            { int y=(short)HIWORD(lParam); int d=y-state->dragStartY;
              SetCustomScrollPosition(hwnd,state->dragStartPos+(int)((double)d*state->max/travel)); }
            return 0;
        case WM_LBUTTONUP:
            if(state->dragging){state->dragging=false;ReleaseCapture();} return 0;
        case WM_CAPTURECHANGED: state->dragging=false; return 0;
        }
        return DefWindowProc(hwnd,msg,wParam,lParam);
    }

}

bool ChatPanel_IsEnabled()
{
    return ENABLE_IRC_CHAT_UI && g_chatEnabled;
}

void ChatPanel_SetEnabled(bool enabled)
{
    if (ENABLE_IRC_CHAT_UI)
        g_chatEnabled = enabled;
}

void ChatPanel_Toggle()
{
    if (!ENABLE_IRC_CHAT_UI)
        return;

    g_chatEnabled = !g_chatEnabled;
}

bool ChatPanel_ToggleHitTest(int x, int y, int clientHeight)
{
    if (!ENABLE_IRC_CHAT_UI)
        return false;

    int top = clientHeight - CHAT_TOGGLE_BOTTOM_MARGIN - CHAT_TOGGLE_HEIGHT;
    return x >= CHAT_TOGGLE_LEFT && x < CHAT_TOGGLE_RIGHT &&
           y >= top && y < top + CHAT_TOGGLE_HEIGHT;
}

bool ChatPanel_ToggleHoverHitTest(int x, int y, int clientHeight)
{
    return ChatPanel_ToggleHitTest(x, y, clientHeight);
}

bool ChatPanel_ConnectHitTest(int x, int y, int clientWidth, int clientHeight)
{
    if (!ENABLE_IRC_CHAT_UI || !g_chatEnabled) return false;
    const int left = clientWidth - 40 - GetChatPanelWidth(clientWidth) + 18;
    const int top = 105 + 84;
    const int right = clientWidth - 40 - 18;
    return x >= left && x < right && y >= top && y < top + 30;
}

void ChatPanel_SetToggleHovered(bool hovered)
{
    g_toggleHovered = hovered;
}



void ChatPanel_SetStatus(const wchar_t* status)
{
    if (!status) status = L"";
    wcsncpy_s(g_chatStatus, 256, status, _TRUNCATE);
}

void ChatPanel_SetConnectionState(bool connected, const wchar_t* nickname)
{
    g_chatConnected = connected;
    if (connected && nickname)
        wcsncpy_s(g_chatNickname, 64, nickname, _TRUNCATE);
    else
        g_chatNickname[0] = L'\0';
    if (!connected && (!g_chatStatus[0] || wcscmp(g_chatStatus, L"Joined #GSP!civ4bts.") == 0))
        wcscpy_s(g_chatStatus, 256, L"Not connected");
}

void ChatPanel_AddMessage(const wchar_t* sender, const wchar_t* message)
{
    // Preserve all messages. If the user is reading older history, do not
    // force the view back to the bottom when a new message arrives.
    AddLine(g_messages, sender, message);
}

void ChatPanel_AddPrivateMessage(const wchar_t* peer, const wchar_t* sender, const wchar_t* message)
{
    if (!peer || !peer[0] || !IsC4FMember(peer)) return;
    int index = EnsurePrivateConversation(peer);
    AddLine(g_privateConversations[index].messages, sender, message);
    if (g_activePrivateConversation == index)
    {
        g_privateConversations[index].unread = false;
        g_privateScroll.position = 0x3fffffff; // Clamp to the new bottom during the next paint.
    }
    else if (sender && _wcsicmp(sender, L"You") != 0)
        g_privateConversations[index].unread = true;
}

void ChatPanel_AddPrivateEcho(const wchar_t* peer, const wchar_t* message)
{
    ChatPanel_AddPrivateMessage(peer, L"You", message);
}

bool ChatPanel_IsPrivateConversationActive()
{
    return g_activePrivateConversation >= 0 &&
        g_activePrivateConversation < (int)g_privateConversations.size();
}

const wchar_t* ChatPanel_GetActiveRecipient()
{
    return ChatPanel_IsPrivateConversationActive()
        ? g_privateConversations[g_activePrivateConversation].peer : L"";
}

bool ChatPanel_HandleMemberClick(int x, int y, int clientWidth, int clientHeight)
{
    RECT panel = GetChatPanelRect(clientWidth, clientHeight);
    RECT area = GetMemberArea(panel);
    int privateTop = GetPrivateBoxTop(area);
    if (x < area.left || x >= area.right || y < area.top || y >= area.bottom)
        return false;

    int c4fCount = 0;
    const wchar_t* c4fNames[128];
    for (int i = 0; i < g_lobbyMemberCount; ++i)
        if (IsC4FMember(g_lobbyMembers[i])) c4fNames[c4fCount++] = g_lobbyMembers[i];

    // Clicking the private-chat heading returns the input to the public lobby.
    if (y >= privateTop && y < privateTop + 24)
    {
        g_activePrivateConversation = -1;
        ChatPanel_LayoutInputControls(GetParent(g_inputEdit));
        if (g_inputEdit) SetFocus(g_inputEdit);
        InvalidateRect(GetParent(g_inputEdit), NULL, FALSE);
        return true;
    }

    // The public lobby roster is informational only. Only C4F members can be messaged.
    int listBottom = privateTop - 8;
    int split = area.top + ((listBottom - area.top) * 68) / 100;
    const int rowHeight = 18;
    if (y >= area.top + 24 && y < split - 5)
        return true; // Ordinary lobby users are deliberately not clickable.

    if (y >= split + 29 && y < listBottom)
    {
        int row = (y - (split + 29)) / rowHeight;
        int available = (listBottom - (split + 29)) / rowHeight;
        int shown = c4fCount < available ? c4fCount : available;
        if (row >= 0 && row < shown)
        {
            g_activePrivateConversation = EnsurePrivateConversation(c4fNames[row]);
            g_privateConversations[g_activePrivateConversation].unread = false;
            g_privateScroll.position = 0x3fffffff;
            ChatPanel_LayoutInputControls(GetParent(g_inputEdit));
            if (g_privateInputEdit) SetFocus(g_privateInputEdit);
            InvalidateRect(GetParent(g_inputEdit), NULL, FALSE);
            return true;
        }
    }
    return false;
}

void ChatPanel_SetMembers(const wchar_t members[][64], int count)
{
    if (count < 0) count = 0;
    if (count > 128) count = 128;
    g_lobbyMemberCount = count;
    for (int i = 0; i < count; ++i)
        wcsncpy_s(g_lobbyMembers[i], 64, members[i], _TRUNCATE);
    for (int i = count; i < 128; ++i)
        g_lobbyMembers[i][0] = L'\0';
}

void ChatPanel_CreateInputControls(HWND parent)
{
    if (g_inputEdit) return;
    g_inputBrush = CreateSolidBrush(RGB(25, 27, 30));
    g_inputFont = CreateFont(16, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
        DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
        CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Arial");

    g_inputEdit = CreateWindowEx(WS_EX_CLIENTEDGE, L"EDIT", L"",
        WS_CHILD | WS_TABSTOP | ES_AUTOHSCROLL,
        0, 0, 238, 30, parent, (HMENU)C4F_CHAT_INPUT_ID,
        GetModuleHandle(NULL), NULL);
    g_sendButton = CreateWindowEx(0, L"BUTTON", L"Send",
        WS_CHILD | WS_TABSTOP | BS_OWNERDRAW,
        0, 0, 58, 30, parent, (HMENU)C4F_CHAT_SEND_ID,
        GetModuleHandle(NULL), NULL);
    g_privateInputEdit = CreateWindowEx(WS_EX_CLIENTEDGE, L"EDIT", L"",
        WS_CHILD | WS_TABSTOP | ES_AUTOHSCROLL,
        0, 0, 130, 28, parent, (HMENU)C4F_PRIVATE_INPUT_ID,
        GetModuleHandle(NULL), NULL);
    g_privateSendButton = CreateWindowEx(0, L"BUTTON", L"Send",
        WS_CHILD | WS_TABSTOP | BS_OWNERDRAW,
        0, 0, 44, 28, parent, (HMENU)C4F_PRIVATE_SEND_ID,
        GetModuleHandle(NULL), NULL);

    if (g_inputEdit)
    {
        SendMessage(g_inputEdit, WM_SETFONT, (WPARAM)g_inputFont, TRUE);
        g_oldInputProc = (WNDPROC)SetWindowLongPtr(g_inputEdit, GWLP_WNDPROC,
            (LONG_PTR)ChatInputProc);
    }
    if (g_privateInputEdit)
    {
        SendMessage(g_privateInputEdit, WM_SETFONT, (WPARAM)g_inputFont, TRUE);
        g_oldPrivateInputProc = (WNDPROC)SetWindowLongPtr(g_privateInputEdit, GWLP_WNDPROC,
            (LONG_PTR)ChatInputProc);
    }
    if (g_sendButton)
        SendMessage(g_sendButton, WM_SETFONT, (WPARAM)g_inputFont, TRUE);
    if (g_privateSendButton)
        SendMessage(g_privateSendButton, WM_SETFONT, (WPARAM)g_inputFont, TRUE);

    WNDCLASS wc = {};
    wc.lpfnWndProc = ChatScrollBarProc;
    wc.hInstance = GetModuleHandle(NULL);
    wc.lpszClassName = CHAT_SCROLLBAR_CLASS;
    wc.hCursor = LoadCursor(NULL, IDC_ARROW);
    RegisterClass(&wc);

    g_chatScrollbar = CreateWindowEx(0, CHAT_SCROLLBAR_CLASS, NULL,
        WS_CHILD | WS_CLIPSIBLINGS,
        0, 0, 16, 100, parent, (HMENU)CHAT_SCROLLBAR_ID,
        GetModuleHandle(NULL), NULL);
    g_privateScrollbar = CreateWindowEx(0, CHAT_SCROLLBAR_CLASS, NULL,
        WS_CHILD | WS_CLIPSIBLINGS,
        0, 0, 12, 100, parent, (HMENU)2006,
        GetModuleHandle(NULL), NULL);

    ChatPanel_LayoutInputControls(parent);
    SetTimer(parent, CHAT_FLASH_TIMER_ID, 500, NULL);
}

void ChatPanel_LayoutInputControls(HWND parent)
{
    if (!g_inputEdit || !g_sendButton) return;
    RECT client;
    GetClientRect(parent, &client);
    RECT panel = GetChatPanelRect(client.right, client.bottom);
    const int chatLeft = GetChatColumnLeft(panel);
    const int chatRight = GetChatColumnRight(panel);
    const int inputY = panel.bottom - 56;
    const int sendWidth = 58;
    const int inputGap = 8;
    int editWidth = chatRight - chatLeft - sendWidth - inputGap;
    if (editWidth < 100) editWidth = 100;
    SetWindowPos(g_inputEdit, NULL, chatLeft, inputY,
        editWidth, 30, SWP_NOZORDER | SWP_NOACTIVATE);
    SetWindowPos(g_sendButton, NULL, chatRight - sendWidth, inputY,
        sendWidth, 30, SWP_NOZORDER | SWP_NOACTIVATE);

    RECT memberArea = GetMemberArea(panel);
    int privateInputY = panel.bottom - 56;
    int privateLeft = memberArea.left + 2;
    int privateRight = memberArea.right - 2;
    int privateSendWidth = 44;
    int privateEditWidth = privateRight - privateLeft - privateSendWidth - 5;
    if (privateEditWidth < 65) privateEditWidth = 65;
    if (g_privateInputEdit)
        SetWindowPos(g_privateInputEdit, NULL, privateLeft, privateInputY,
            privateEditWidth, 30, SWP_NOZORDER | SWP_NOACTIVATE);
    if (g_privateSendButton)
        SetWindowPos(g_privateSendButton, NULL, privateRight - privateSendWidth, privateInputY,
            privateSendWidth, 30, SWP_NOZORDER | SWP_NOACTIVATE);

    if (g_chatScrollbar)
        SetWindowPos(g_chatScrollbar, NULL,
            chatRight - CHAT_SCROLLBAR_WIDTH, panel.top + 140,
            CHAT_SCROLLBAR_WIDTH, (panel.bottom - 92) - (panel.top + 140),
            SWP_NOZORDER | SWP_NOACTIVATE);
    if (g_privateScrollbar)
    {
        int privateMessagesTop = GetPrivateBoxTop(memberArea) + 25;
        SetWindowPos(g_privateScrollbar, NULL,
            memberArea.right - 13, privateMessagesTop,
            12, (panel.bottom - 64) - privateMessagesTop,
            SWP_NOZORDER | SWP_NOACTIVATE);
    }
    if (g_privateInputEdit) ShowWindow(g_privateInputEdit,
        ChatPanel_IsPrivateConversationActive() ? SW_SHOW : SW_HIDE);
    if (g_privateSendButton) ShowWindow(g_privateSendButton,
        ChatPanel_IsPrivateConversationActive() ? SW_SHOW : SW_HIDE);
    if (g_privateScrollbar) ShowWindow(g_privateScrollbar,
        ChatPanel_IsPrivateConversationActive() && g_privateScroll.max > 0 ? SW_SHOW : SW_HIDE);
}

void ChatPanel_SetInputVisible(bool visible)
{
    if (g_inputEdit) ShowWindow(g_inputEdit, visible ? SW_SHOW : SW_HIDE);
    if (g_sendButton) ShowWindow(g_sendButton, visible ? SW_SHOW : SW_HIDE);
    if (g_privateInputEdit) ShowWindow(g_privateInputEdit,
        visible && ChatPanel_IsPrivateConversationActive() ? SW_SHOW : SW_HIDE);
    if (g_privateSendButton) ShowWindow(g_privateSendButton,
        visible && ChatPanel_IsPrivateConversationActive() ? SW_SHOW : SW_HIDE);
    if (g_chatScrollbar) ShowWindow(g_chatScrollbar, visible && g_publicScroll.max > 0 ? SW_SHOW : SW_HIDE);
    if (g_privateScrollbar) ShowWindow(g_privateScrollbar,
        visible && ChatPanel_IsPrivateConversationActive() && g_privateScroll.max > 0 ? SW_SHOW : SW_HIDE);
}

void ChatPanel_GetInputText(wchar_t* buffer, int bufferCount)
{
    if (!buffer || bufferCount <= 0) return;
    buffer[0] = L'\0';
    if (g_inputEdit) GetWindowText(g_inputEdit, buffer, bufferCount);
}

void ChatPanel_ClearInput()
{
    if (g_inputEdit) SetWindowText(g_inputEdit, L"");
}

void ChatPanel_GetPrivateInputText(wchar_t* buffer, int bufferCount)
{
    if (!buffer || bufferCount <= 0) return;
    buffer[0] = L'\0';
    if (g_privateInputEdit) GetWindowText(g_privateInputEdit, buffer, bufferCount);
}

void ChatPanel_ClearPrivateInput()
{
    if (g_privateInputEdit) SetWindowText(g_privateInputEdit, L"");
}

HBRUSH ChatPanel_GetEditBrush()
{
    return g_inputBrush;
}

void ChatPanel_HandleScroll(WPARAM wParam, LPARAM lParam)
{
    // Scrolling is handled by the custom-drawn scrollbar control.
    (void)wParam; (void)lParam;
}

void ChatPanel_HandleMouseWheel(WPARAM wParam)
{
    if (!g_chatScrollbar || !IsWindowVisible(g_chatScrollbar)) return;
    int delta = GET_WHEEL_DELTA_WPARAM(wParam);
    int notches = delta / WHEEL_DELTA;
    if (notches == 0) return;
    SetCustomScrollPosition(g_chatScrollbar, g_publicScroll.position - notches * 60);
}


void ChatPanel_TimerTick(HWND parent)
{
    g_flashPhase = !g_flashPhase;
    bool hasUnread = false;
    for (size_t i = 0; i < g_privateConversations.size(); ++i)
        if (g_privateConversations[i].unread) { hasUnread = true; break; }
    if (hasUnread) InvalidateRect(parent, NULL, FALSE);
}

void ChatPanel_DrawToggle(HDC hdc, int clientWidth, int clientHeight)
{
    if (!ENABLE_IRC_CHAT_UI)
        return;

    int top = clientHeight - CHAT_TOGGLE_BOTTOM_MARGIN - CHAT_TOGGLE_HEIGHT;
    RECT r = { CHAT_TOGGLE_LEFT, top, CHAT_TOGGLE_RIGHT, top + CHAT_TOGGLE_HEIGHT };
    Graphics graphics(hdc);
    graphics.SetSmoothingMode(SmoothingModeAntiAlias);

    SolidBrush bg(g_toggleHovered ? Color(190, 42, 42, 43)
                                  : Color(145, 23, 25, 29));
    graphics.FillRectangle(&bg, r.left, r.top, r.right-r.left, r.bottom-r.top);

    Pen border(g_toggleHovered ? Color(255, 235, 197, 112)
                               : Color(255, 166, 132, 70), 1.0f);
    graphics.DrawRectangle(&border, r.left, r.top,
                           r.right-r.left, r.bottom-r.top);

    RECT box = { r.left + 14, r.top + 16, r.left + 32, r.top + 34 };
    SolidBrush boxBg(g_chatEnabled ? Color(220, 55, 43, 29)
                                   : Color(175, 12, 14, 17));
    graphics.FillRectangle(&boxBg, box.left, box.top,
                           box.right-box.left, box.bottom-box.top);
    Pen boxBorder(g_chatEnabled ? Color(255, 235, 197, 112)
                                : Color(255, 166, 132, 70), 1.0f);
    graphics.DrawRectangle(&boxBorder, box.left, box.top,
                           box.right-box.left, box.bottom-box.top);

    if (g_chatEnabled)
    {
        Pen tick(Color(255, 235, 197, 112), 2.0f);
        graphics.DrawLine(&tick, r.left + 17, r.top + 24, r.left + 21, r.top + 28);
        graphics.DrawLine(&tick, r.left + 21, r.top + 28, r.left + 29, r.top + 20);
    }

    HFONT font = CreateFont(16, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
        DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
        CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Arial");
    HFONT oldFont = (HFONT)SelectObject(hdc, font);
    SetBkMode(hdc, TRANSPARENT);
    SetTextColor(hdc, g_toggleHovered ? RGB(255,235,190) : RGB(240,240,235));

    RECT text = { r.left + 42, r.top, r.right - 14, r.bottom };
    DrawText(hdc, L"Enable Lobby Chat", -1, &text,
             DT_LEFT | DT_VCENTER | DT_SINGLELINE);

    SelectObject(hdc, oldFont);
    DeleteObject(font);
}

void ChatPanel_Draw(HDC hdc, int clientWidth, int clientHeight)
{
    if (!ENABLE_IRC_CHAT_UI || !g_chatEnabled)
        return;

    const int padding = 18;
    RECT r = GetChatPanelRect(clientWidth, clientHeight);
    if (r.right <= r.left || r.bottom <= r.top)
        return;

    Graphics graphics(hdc);
    graphics.SetSmoothingMode(SmoothingModeAntiAlias);
    SolidBrush bg(Color(205, 18, 20, 24));
    graphics.FillRectangle(&bg, r.left, r.top,
                           r.right-r.left, r.bottom-r.top);
    Pen border(Color(255, 196, 158, 82), 1.0f);
    graphics.DrawRectangle(&border, r.left, r.top,
                           r.right-r.left, r.bottom-r.top);

    SetBkMode(hdc, TRANSPARENT);
    HFONT titleFont = CreateFont(22, 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE,
        DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
        CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Georgia");
    HFONT oldFont = (HFONT)SelectObject(hdc, titleFont);
    SetTextColor(hdc, RGB(235, 207, 151));

    RECT title = { r.left + padding, r.top + 12,
                   r.right - padding, r.top + 45 };
    wchar_t titleText[128];
    if (g_chatConnected && g_chatNickname[0])
        swprintf_s(titleText, 128, L"Lobby Chat - Logged as %s", g_chatNickname);
    else
        wcscpy_s(titleText, 128, L"Lobby Chat");
    DrawText(hdc, titleText, -1, &title,
             DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
    SelectObject(hdc, oldFont);
    DeleteObject(titleFont);

    HFONT bodyFont = CreateFont(15, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
        DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
        CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Arial");
    oldFont = (HFONT)SelectObject(hdc, bodyFont);
    SetTextColor(hdc, RGB(220, 216, 205));

    RECT status = { r.left + padding, r.top + 50,
                    r.right - padding, r.top + 78 };
    const wchar_t* visibleStatus = g_chatConnected ? L"Connected to the Civ4 lobby" : g_chatStatus;
    DrawText(hdc, visibleStatus, -1, &status,
             DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);

    RECT connectButton = { r.left + padding, r.top + 84,
                           r.right - padding, r.top + 114 };
    SolidBrush connectBg(Color(220, 68, 49, 25));
    graphics.FillRectangle(&connectBg, connectButton.left, connectButton.top,
        connectButton.right - connectButton.left,
        connectButton.bottom - connectButton.top);
    Pen connectBorder(Color(255, 220, 177, 94), 1.2f);
    graphics.DrawRectangle(&connectBorder, connectButton.left, connectButton.top,
        connectButton.right - connectButton.left,
        connectButton.bottom - connectButton.top);
    RECT connectText = connectButton;
    SetTextColor(hdc, RGB(245, 220, 175));
    DrawText(hdc, g_chatConnected ? L"Disconnect" : L"Connect to lobby", -1, &connectText,
             DT_CENTER | DT_VCENTER | DT_SINGLELINE);

    HPEN divider = CreatePen(PS_SOLID, 1, RGB(105, 87, 54));
    HPEN oldPen = (HPEN)SelectObject(hdc, divider);
    MoveToEx(hdc, r.left + padding, r.top + 126, NULL);
    LineTo(hdc, r.right - padding, r.top + 126);
    SelectObject(hdc, oldPen);
    DeleteObject(divider);

    // The chat transcript and input occupy the left side; live lobby members
    // are shown in a dedicated right-hand column.
    RECT messageArea = GetChatMessageArea(r);
    RECT memberArea = GetMemberArea(r);
    int privateTop = GetPrivateBoxTop(memberArea);

    HPEN memberDivider = CreatePen(PS_SOLID, 1, RGB(82, 72, 55));
    oldPen = (HPEN)SelectObject(hdc, memberDivider);
    MoveToEx(hdc, memberArea.left - 7, memberArea.top, NULL);
    LineTo(hdc, memberArea.left - 7, memberArea.bottom);
    SelectObject(hdc, oldPen);
    DeleteObject(memberDivider);

    // Split the member column into the general BtS lobby and Civ4Fans users.
    int regularCount = 0;
    int c4fCount = 0;
    const wchar_t* regularNames[128];
    const wchar_t* c4fNames[128];
    for (int i = 0; i < g_lobbyMemberCount; ++i)
    {
        if (IsC4FMember(g_lobbyMembers[i]))
            c4fNames[c4fCount++] = g_lobbyMembers[i];
        else
            regularNames[regularCount++] = g_lobbyMembers[i];
    }

    int listBottom = privateTop - 8;
    int memberSplit = memberArea.top + ((listBottom - memberArea.top) * 68) / 100;
    RECT regularArea = { memberArea.left, memberArea.top,
                         memberArea.right, memberSplit - 5 };
    RECT c4fArea = { memberArea.left, memberSplit + 5,
                     memberArea.right, listBottom };
    const wchar_t* regularHeading = L"Beyond the Sword Lobby";
    const wchar_t* c4fHeading = L"C4F Members (click to chat)";

    HFONT memberHeadingFont = CreateFont(12, 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE,
        DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
        CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Arial");
    HFONT oldMemberFont = (HFONT)SelectObject(hdc, memberHeadingFont);
    DrawMemberSection(hdc, regularArea, regularHeading, regularNames, regularCount, false);
    DrawMemberSection(hdc, c4fArea, c4fHeading, c4fNames, c4fCount, true);

    // Private conversation docked beneath the roster; the public transcript remains visible.
    RECT privateFrame = { memberArea.left, privateTop, memberArea.right, memberArea.bottom + 36 };
    HPEN privatePen = CreatePen(PS_SOLID, 1, RGB(105, 87, 54));
    HPEN oldPrivatePen = (HPEN)SelectObject(hdc, privatePen);
    MoveToEx(hdc, privateFrame.left, privateFrame.top, NULL);
    LineTo(hdc, privateFrame.right, privateFrame.top);
    SelectObject(hdc, oldPrivatePen); DeleteObject(privatePen);
    RECT privateHeading = { memberArea.left + 2, privateTop + 3, memberArea.right - 2, privateTop + 22 };
    SetTextColor(hdc, RGB(224, 190, 128));
    wchar_t privateTitle[96];
    if (ChatPanel_IsPrivateConversationActive())
        swprintf_s(privateTitle, 96, L"Private: %s", ChatPanel_GetActiveRecipient());
    else
        wcscpy_s(privateTitle, L"Private chat - select a C4F member");
    DrawText(hdc, privateTitle, -1, &privateHeading, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
    if (ChatPanel_IsPrivateConversationActive())
    {
        const std::vector<ChatLine>& pm = g_privateConversations[g_activePrivateConversation].messages;
        RECT privateMessages = { memberArea.left + 2, privateTop + 25,
            memberArea.right - 17, memberArea.bottom + 28 };
        int privateViewport = privateMessages.bottom - privateMessages.top;
        int privateWidth = privateMessages.right - privateMessages.left;
        HFONT miniFont = CreateFont(14, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
            DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
            CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Arial");
        HFONT oldMini = (HFONT)SelectObject(hdc, miniFont);
        std::vector<int> privateHeights(pm.size(), 0);
        int privateContentHeight = 0;
        for (int m = 0; m < (int)pm.size(); ++m)
        {
            wchar_t mini[640];
            swprintf_s(mini, 640, L"%s: %s", pm[m].sender, pm[m].text);
            RECT mr = { 0, 0, privateWidth, 0 };
            int mh = DrawText(hdc, mini, -1, &mr, DT_LEFT | DT_TOP | DT_WORDBREAK | DT_CALCRECT);
            if (mh < 15) mh = 15;
            privateHeights[m] = mh;
            privateContentHeight += mh + 3;
        }
        g_privateScroll.max = privateContentHeight > privateViewport ? privateContentHeight - privateViewport : 0;
        g_privateScroll.page = privateViewport > 0 ? privateViewport : 1;
        if (g_privateScroll.position > g_privateScroll.max) g_privateScroll.position = g_privateScroll.max;
        if (g_privateScrollbar)
        {
            ShowWindow(g_privateScrollbar, g_privateScroll.max > 0 ? SW_SHOW : SW_HIDE);
            InvalidateRect(g_privateScrollbar, NULL, FALSE);
        }
        int savedPrivate = SaveDC(hdc);
        IntersectClipRect(hdc, privateMessages.left, privateMessages.top,
            privateMessages.right, privateMessages.bottom);
        int py = privateMessages.top - g_privateScroll.position;
        for (int m = 0; m < (int)pm.size(); ++m)
        {
            wchar_t mini[640];
            swprintf_s(mini, 640, L"%s: %s", pm[m].sender, pm[m].text);
            RECT mr = { privateMessages.left, py, privateMessages.right, py + privateHeights[m] };
            SetTextColor(hdc, RGB(220,216,205));
            DrawText(hdc, mini, -1, &mr, DT_LEFT | DT_TOP | DT_WORDBREAK);
            py += privateHeights[m] + 3;
        }
        RestoreDC(hdc, savedPrivate);
        SelectObject(hdc, oldMini); DeleteObject(miniFont);
    }
    else
    {
        RECT hint = { memberArea.left + 2, privateTop + 28, memberArea.right - 2, memberArea.bottom - 45 };
        SetTextColor(hdc, RGB(145, 139, 127));
        DrawText(hdc, L"Select a name ending in _C4F to start a private conversation.", -1,
            &hint, DT_LEFT | DT_TOP | DT_WORDBREAK);
    }
    SelectObject(hdc, oldMemberFont);
    DeleteObject(memberHeadingFont);

    std::vector<ChatLine>& visibleMessages = g_messages;
    int viewportHeight = messageArea.bottom - messageArea.top;
    int textWidth = messageArea.right - messageArea.left;
    std::vector<int> heights(visibleMessages.size(), 0);
    int contentHeight = 0;
    for (int i = 0; i < (int)visibleMessages.size(); ++i)
    {
        wchar_t line[640];
        swprintf_s(line, 640, L"%s  %s: %s", visibleMessages[i].time,
                   visibleMessages[i].sender, visibleMessages[i].text);
        RECT measure = { 0, 0, textWidth, 0 };
        int h = DrawText(hdc, line, -1, &measure,
            DT_LEFT | DT_TOP | DT_WORDBREAK | DT_CALCRECT);
        if (h < 20) h = 20;
        heights[i] = h;
        contentHeight += h + CHAT_MESSAGE_GAP;
    }

    int maxScroll = contentHeight > viewportHeight ? contentHeight - viewportHeight : 0;
    if (g_scrollToBottom) g_publicScroll.position = maxScroll;
    if (g_publicScroll.position > maxScroll) g_publicScroll.position = maxScroll;
    if (g_publicScroll.position < 0) g_publicScroll.position = 0;
    g_publicScroll.max = maxScroll;
    g_publicScroll.page = viewportHeight > 0 ? viewportHeight : 1;
    if (g_chatScrollbar)
    {
        ShowWindow(g_chatScrollbar, maxScroll > 0 ? SW_SHOW : SW_HIDE);
        InvalidateRect(g_chatScrollbar, NULL, FALSE);
    }

    int savedDC = SaveDC(hdc);
    IntersectClipRect(hdc, messageArea.left, messageArea.top,
                      messageArea.right, messageArea.bottom);
    int y = messageArea.top - g_publicScroll.position;
    for (int i = 0; i < (int)visibleMessages.size(); ++i)
    {
        wchar_t line[640];
        swprintf_s(line, 640, L"%s  %s: %s", visibleMessages[i].time,
                   visibleMessages[i].sender, visibleMessages[i].text);
        RECT messageRect = { messageArea.left, y,
                             messageArea.right, y + heights[i] };
        DrawText(hdc, line, -1, &messageRect,
                 DT_LEFT | DT_TOP | DT_WORDBREAK);
        y += heights[i] + CHAT_MESSAGE_GAP;
    }
    RestoreDC(hdc, savedDC);

    if (visibleMessages.empty())
    {
        RECT note = { messageArea.left, messageArea.top,
                      messageArea.right, messageArea.bottom };
        DrawText(hdc, L"Messages from the Civ4 lobby will appear here.",
            -1, &note, DT_LEFT | DT_TOP | DT_WORDBREAK);
    }

    RECT inputHint = { messageArea.left, r.bottom - 82,
                       GetChatColumnRight(r), r.bottom - 62 };
    SetTextColor(hdc, RGB(178, 164, 137));
    DrawText(hdc, L"Type a message and press Enter", -1, &inputHint,
             DT_LEFT | DT_VCENTER | DT_SINGLELINE);

    SelectObject(hdc, oldFont);
    DeleteObject(bodyFont);
}
