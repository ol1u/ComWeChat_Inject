#pragma once

#include <Windows.h>

void InitializeWeChatAutoLoginProcess();
void MaybeStartWeChatAutoLogin(DWORD pid, int port);
int RunWeChatAutoLoginDetectMode(
    const wchar_t* command_line,
    DWORD pid,
    int port);
