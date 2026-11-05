// WeChatHook.cpp : 定义应用程序的入口点。
//

#include "framework.h"
#include "WeChatHook.h"
#include <Windows.h>
#include "auto_login.h"
#include "utils.h"

int GetPortFromEnv() {  
    char* envPort = nullptr;  
    size_t len = 0;  
    if (_dupenv_s(&envPort, &len, "WECHAT_HOOK_PORT") == 0 && envPort != nullptr) {  
        int port = atoi(envPort);  
        free(envPort);  
        if (port > 0 && port <= 65535) {  
            return port;  
        }  
    }  
    return 18888;  
}

// 此代码模块中包含的函数的前向声明:
INT_PTR CALLBACK    About(HWND, UINT, WPARAM, LPARAM);
INT_PTR CALLBACK DialogProc(_In_ HWND hDlg, _In_ UINT message, _In_ WPARAM wParam, _In_ LPARAM lParam);

int APIENTRY wWinMain(_In_ HINSTANCE hInstance,
                     _In_opt_ HINSTANCE hPrevInstance,
                     _In_ LPWSTR    lpCmdLine,
                     _In_ int       nCmdShow)
{
	BOOL result;
	DWORD PID = ProcessNameFindPID(WeChatProcessName);
	int autoLoginDetect = RunWeChatAutoLoginDetectMode(lpCmdLine, PID, GetPortFromEnv());
	if (autoLoginDetect >= 0)
	{
		return autoLoginDetect;
	}
	InitializeWeChatAutoLoginProcess();
	if (PID == 0)
	{
		return 0;
	}
	else
	{
		result = StartListen(PID, GetPortFromEnv());
		if(result == 0)
		{
			return 0;
		}
		MaybeStartWeChatAutoLogin(PID, GetPortFromEnv());
	}
	DialogBox(hInstance, MAKEINTRESOURCE(ID_MAIN), NULL, DialogProc);
    return 0;
}

INT_PTR CALLBACK DialogProc(_In_ HWND hDlg, _In_ UINT message, _In_ WPARAM wParam, _In_ LPARAM lParam)
{
	BOOL result;
	UNREFERENCED_PARAMETER(lParam);
	switch (message)
	{
	case WM_INITDIALOG:
		return (INT_PTR)TRUE;

	case WM_COMMAND:
		if (LOWORD(wParam) == INJECT)
		{
			DWORD PID = ProcessNameFindPID(WeChatProcessName);
			if (PID == 0)
			{
				return 0;
			}
			if (StartListen(PID, GetPortFromEnv()))
			{
				MaybeStartWeChatAutoLogin(PID, GetPortFromEnv());
			}
		}
		if (LOWORD(wParam) == UNJECT)
		{
			DWORD PID = ProcessNameFindPID(WeChatProcessName);
			if (PID == 0)
			{
				MessageBox(NULL, "未找到进程", "错误", 0);
				return 0;
			}
			result = StopListen(PID);
			if (result == 0)
			{
				MessageBox(NULL, "未找到DLL", "错误", 0);
				return 0;
			}
		}
		break;
	case WM_CLOSE:
		EndDialog(hDlg, 0);
		break;
	}
	return (INT_PTR)FALSE;
}
