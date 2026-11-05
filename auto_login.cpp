#ifndef NOMINMAX
#define NOMINMAX
#endif

#include "auto_login.h"

#include <winhttp.h>

#include <algorithm>
#include <cwctype>
#include <iomanip>
#include <mutex>
#include <set>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#pragma comment(lib, "winhttp.lib")

namespace {

const wchar_t* kStateFileName = L"auto-login-state.json";
const wchar_t* kLogFileName = L"WeChatHook-auto-login.log";

struct WindowCandidate
{
    HWND hwnd;
    std::wstring title;
    std::wstring class_name;
    RECT rect;
};

struct GreenSignature
{
    int count = 0;
    double x = 0.0;
    double y = 0.0;
    int left = 0;
    int top = 0;
    int right = 0;
    int bottom = 0;
};

struct AutoLoginState
{
    std::wstring event_id;
    std::wstring state;
    DWORD pid = 0;
    int width = 0;
    int height = 0;
    int green_pixels = 0;
    int click_x = 0;
    int click_y = 0;
    std::wstring detail;
};

struct EnumContext
{
    DWORD pid;
    std::vector<WindowCandidate>* candidates;
};

std::mutex g_started_lock;
std::set<DWORD> g_started_pids;

std::string WideToUtf8(const std::wstring& value)
{
    if (value.empty())
    {
        return "";
    }
    const int size = WideCharToMultiByte(
        CP_UTF8, WC_ERR_INVALID_CHARS, value.c_str(), -1, nullptr, 0, nullptr, nullptr);
    if (size <= 0)
    {
        return "";
    }
    std::vector<char> utf8(static_cast<size_t>(size));
    WideCharToMultiByte(
        CP_UTF8, 0, value.c_str(), -1, utf8.data(), size, nullptr, nullptr);
    return std::string(utf8.data(), static_cast<size_t>(size - 1));
}

std::wstring GetEnvironment(const wchar_t* name)
{
    const DWORD size = GetEnvironmentVariableW(name, nullptr, 0);
    if (size == 0)
    {
        return L"";
    }
    std::vector<wchar_t> buffer(size);
    const DWORD written = GetEnvironmentVariableW(name, buffer.data(), size);
    if (written == 0)
    {
        return L"";
    }
    return std::wstring(buffer.data(), written);
}

std::wstring Lowercase(std::wstring value)
{
    std::transform(
        value.begin(), value.end(), value.begin(),
        [](wchar_t character) { return static_cast<wchar_t>(towlower(character)); });
    return value;
}

std::wstring Trim(const std::wstring& value)
{
    const size_t start = value.find_first_not_of(L" \t\r\n");
    if (start == std::wstring::npos)
    {
        return L"";
    }
    const size_t end = value.find_last_not_of(L" \t\r\n");
    return value.substr(start, end - start + 1);
}

bool IsAutoLoginDisabled()
{
    const std::wstring value = Lowercase(Trim(GetEnvironment(L"WECHAT_AUTO_LOGIN")));
    return value == L"0" || value == L"false" || value == L"no" || value == L"off";
}

bool IsDetectOnly()
{
    const std::wstring value = Lowercase(Trim(GetEnvironment(L"WECHAT_AUTO_LOGIN")));
    return value == L"detect" || value == L"dry-run" || value == L"dry_run";
}

bool IsTruthyEnvironment(const wchar_t* name)
{
    const std::wstring value = Lowercase(Trim(GetEnvironment(name)));
    return value == L"1" || value == L"true" || value == L"yes" || value == L"on";
}

std::wstring ExecutableDirectory()
{
    std::vector<wchar_t> path(MAX_PATH);
    while (true)
    {
        const DWORD copied =
            GetModuleFileNameW(nullptr, path.data(), static_cast<DWORD>(path.size()));
        if (copied == 0)
        {
            return L".";
        }
        if (copied < path.size() - 1)
        {
            const std::wstring full_path(path.data(), copied);
            const size_t slash = full_path.find_last_of(L"\\/");
            return slash == std::wstring::npos ? L"." : full_path.substr(0, slash);
        }
        path.resize(path.size() * 2);
    }
}

std::wstring JoinPath(const std::wstring& directory, const wchar_t* name)
{
    if (directory.empty() || directory.back() == L'\\' || directory.back() == L'/')
    {
        return directory + name;
    }
    return directory + L"\\" + name;
}

std::wstring StateFilePath()
{
    const std::wstring configured =
        Trim(GetEnvironment(L"WECHAT_AUTO_LOGIN_STATE_FILE"));
    return configured.empty() ? JoinPath(ExecutableDirectory(), kStateFileName) : configured;
}

std::wstring LogFilePath()
{
    const std::wstring configured =
        Trim(GetEnvironment(L"WECHAT_AUTO_LOGIN_LOG_FILE"));
    return configured.empty() ? JoinPath(ExecutableDirectory(), kLogFileName) : configured;
}

void AppendLog(const std::wstring& message)
{
    SYSTEMTIME local_time{};
    GetLocalTime(&local_time);
    std::wostringstream line;
    line << L"[" << std::setfill(L'0')
         << std::setw(4) << local_time.wYear << L"-"
         << std::setw(2) << local_time.wMonth << L"-"
         << std::setw(2) << local_time.wDay << L" "
         << std::setw(2) << local_time.wHour << L":"
         << std::setw(2) << local_time.wMinute << L":"
         << std::setw(2) << local_time.wSecond << L"."
         << std::setw(3) << local_time.wMilliseconds << L"] "
         << message << L"\r\n";
    const std::string utf8 = WideToUtf8(line.str());

    HANDLE file = CreateFileW(
        LogFilePath().c_str(),
        FILE_APPEND_DATA,
        FILE_SHARE_READ | FILE_SHARE_WRITE,
        nullptr,
        OPEN_ALWAYS,
        FILE_ATTRIBUTE_NORMAL,
        nullptr);
    if (file == INVALID_HANDLE_VALUE)
    {
        return;
    }
    DWORD written = 0;
    WriteFile(file, utf8.data(), static_cast<DWORD>(utf8.size()), &written, nullptr);
    CloseHandle(file);
}

std::string JsonEscape(const std::wstring& value)
{
    const std::string utf8 = WideToUtf8(value);
    std::ostringstream escaped;
    for (const unsigned char character : utf8)
    {
        switch (character)
        {
        case '"':
            escaped << "\\\"";
            break;
        case '\\':
            escaped << "\\\\";
            break;
        case '\b':
            escaped << "\\b";
            break;
        case '\f':
            escaped << "\\f";
            break;
        case '\n':
            escaped << "\\n";
            break;
        case '\r':
            escaped << "\\r";
            break;
        case '\t':
            escaped << "\\t";
            break;
        default:
            if (character < 0x20)
            {
                escaped << "\\u" << std::hex << std::setw(4) << std::setfill('0')
                        << static_cast<int>(character) << std::dec;
            }
            else
            {
                escaped << static_cast<char>(character);
            }
            break;
        }
    }
    return escaped.str();
}

std::wstring IsoTimestampUtc()
{
    SYSTEMTIME time{};
    GetSystemTime(&time);
    std::wostringstream value;
    value << std::setfill(L'0')
          << std::setw(4) << time.wYear << L"-"
          << std::setw(2) << time.wMonth << L"-"
          << std::setw(2) << time.wDay << L"T"
          << std::setw(2) << time.wHour << L":"
          << std::setw(2) << time.wMinute << L":"
          << std::setw(2) << time.wSecond << L"."
          << std::setw(3) << time.wMilliseconds << L"Z";
    return value.str();
}

std::wstring CreateEventId(DWORD pid)
{
    FILETIME file_time{};
    GetSystemTimeAsFileTime(&file_time);
    ULARGE_INTEGER ticks{};
    ticks.LowPart = file_time.dwLowDateTime;
    ticks.HighPart = file_time.dwHighDateTime;
    std::wostringstream value;
    value << ticks.QuadPart << L"-" << pid;
    return value.str();
}

bool WriteStateFile(const AutoLoginState& state)
{
    std::ostringstream json;
    json << "{"
         << "\"schemaVersion\":1,"
         << "\"eventId\":\"" << JsonEscape(state.event_id) << "\","
         << "\"state\":\"" << JsonEscape(state.state) << "\","
         << "\"pid\":" << state.pid << ","
         << "\"createdAt\":\"" << JsonEscape(IsoTimestampUtc()) << "\","
         << "\"window\":{\"width\":" << state.width << ",\"height\":" << state.height << "},"
         << "\"detection\":{\"greenPixels\":" << state.green_pixels
         << ",\"clickX\":" << state.click_x << ",\"clickY\":" << state.click_y << "},"
         << "\"detail\":\"" << JsonEscape(state.detail) << "\""
         << "}";

    const std::wstring target = StateFilePath();
    const std::wstring temporary =
        target + L".tmp." + std::to_wstring(GetCurrentProcessId());
    const std::string utf8 = json.str();
    HANDLE file = CreateFileW(
        temporary.c_str(),
        GENERIC_WRITE,
        0,
        nullptr,
        CREATE_ALWAYS,
        FILE_ATTRIBUTE_NORMAL,
        nullptr);
    if (file == INVALID_HANDLE_VALUE)
    {
        AppendLog(L"failed to create auto-login state file: " + target);
        return false;
    }
    DWORD written = 0;
    const BOOL wrote =
        WriteFile(file, utf8.data(), static_cast<DWORD>(utf8.size()), &written, nullptr);
    FlushFileBuffers(file);
    CloseHandle(file);
    if (!wrote || written != utf8.size())
    {
        DeleteFileW(temporary.c_str());
        AppendLog(L"failed to write auto-login state file: " + target);
        return false;
    }
    if (!MoveFileExW(
            temporary.c_str(),
            target.c_str(),
            MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
    {
        DeleteFileW(temporary.c_str());
        AppendLog(L"failed to replace auto-login state file: " + target);
        return false;
    }
    return true;
}

bool IsProcessRunning(DWORD pid)
{
    HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (process == nullptr)
    {
        return false;
    }
    DWORD exit_code = 0;
    const bool running = GetExitCodeProcess(process, &exit_code) && exit_code == STILL_ACTIVE;
    CloseHandle(process);
    return running;
}

bool ContainsAny(const std::wstring& value, const std::vector<std::wstring>& needles)
{
    for (const std::wstring& needle : needles)
    {
        if (value.find(needle) != std::wstring::npos)
        {
            return true;
        }
    }
    return false;
}

BOOL CALLBACK EnumWindowsForProcess(HWND hwnd, LPARAM parameter)
{
    auto* context = reinterpret_cast<EnumContext*>(parameter);

    DWORD pid = 0;
    GetWindowThreadProcessId(hwnd, &pid);
    if (pid != context->pid || !IsWindowVisible(hwnd))
    {
        return TRUE;
    }

    RECT rect{};
    if (!GetWindowRect(hwnd, &rect))
    {
        return TRUE;
    }
    const int width = rect.right - rect.left;
    const int height = rect.bottom - rect.top;
    if (width < 200 || height < 260 || width > 800 || height > 1100)
    {
        return TRUE;
    }
    const double ratio = static_cast<double>(width) / static_cast<double>(height);
    if (ratio < 0.48 || ratio > 1.05)
    {
        return TRUE;
    }

    wchar_t title[512] = {0};
    wchar_t class_name[512] = {0};
    GetWindowTextW(hwnd, title, 512);
    GetClassNameW(hwnd, class_name, 512);

    const std::wstring title_value(title);
    const std::wstring class_value(class_name);
    if (!ContainsAny(Lowercase(title_value), { L"\u5FAE\u4FE1", L"wechat" }) &&
        !ContainsAny(Lowercase(class_value), { L"wechat", L"wx" }))
    {
        return TRUE;
    }

    context->candidates->push_back({ hwnd, title_value, class_value, rect });
    return TRUE;
}

std::vector<WindowCandidate> FindLoginWindowCandidates(DWORD pid)
{
    std::vector<WindowCandidate> candidates;
    EnumContext context{ pid, &candidates };
    EnumWindows(EnumWindowsForProcess, reinterpret_cast<LPARAM>(&context));

    std::sort(
        candidates.begin(), candidates.end(),
        [](const WindowCandidate& left, const WindowCandidate& right) {
            const long left_area =
                (left.rect.right - left.rect.left) * (left.rect.bottom - left.rect.top);
            const long right_area =
                (right.rect.right - right.rect.left) * (right.rect.bottom - right.rect.top);
            return left_area < right_area;
        });
    return candidates;
}

bool QueryLoginState(int port, bool& is_login)
{
    is_login = false;
    HINTERNET session = WinHttpOpen(
        L"WeChatHook auto-login/1.0",
        WINHTTP_ACCESS_TYPE_NO_PROXY,
        WINHTTP_NO_PROXY_NAME,
        WINHTTP_NO_PROXY_BYPASS,
        0);
    if (session == nullptr)
    {
        return false;
    }
    WinHttpSetTimeouts(session, 1000, 1000, 1000, 2500);
    HINTERNET connection =
        WinHttpConnect(session, L"127.0.0.1", static_cast<INTERNET_PORT>(port), 0);
    if (connection == nullptr)
    {
        WinHttpCloseHandle(session);
        return false;
    }
    HINTERNET request = WinHttpOpenRequest(
        connection,
        L"POST",
        L"/api/?type=0",
        nullptr,
        WINHTTP_NO_REFERER,
        WINHTTP_DEFAULT_ACCEPT_TYPES,
        0);
    if (request == nullptr)
    {
        WinHttpCloseHandle(connection);
        WinHttpCloseHandle(session);
        return false;
    }

    static const wchar_t headers[] = L"Content-Type: application/json\r\n";
    static const char body[] = "{}";
    bool ok = WinHttpSendRequest(
                  request,
                  headers,
                  static_cast<DWORD>(-1L),
                  const_cast<char*>(body),
                  static_cast<DWORD>(sizeof(body) - 1),
                  static_cast<DWORD>(sizeof(body) - 1),
                  0) &&
              WinHttpReceiveResponse(request, nullptr);

    std::string response;
    if (ok)
    {
        while (true)
        {
            DWORD available = 0;
            if (!WinHttpQueryDataAvailable(request, &available) || available == 0)
            {
                break;
            }
            std::vector<char> buffer(available);
            DWORD read = 0;
            if (!WinHttpReadData(request, buffer.data(), available, &read))
            {
                ok = false;
                break;
            }
            response.append(buffer.data(), read);
            if (response.size() > 64 * 1024)
            {
                break;
            }
        }
    }

    WinHttpCloseHandle(request);
    WinHttpCloseHandle(connection);
    WinHttpCloseHandle(session);
    if (!ok)
    {
        return false;
    }

    const std::string key = "\"is_login\"";
    const size_t position = response.find(key);
    if (position == std::string::npos)
    {
        return false;
    }
    size_t cursor = position + key.size();
    while (cursor < response.size() &&
           (response[cursor] == ' ' || response[cursor] == '\t' ||
            response[cursor] == '\r' || response[cursor] == '\n'))
    {
        ++cursor;
    }
    if (cursor >= response.size() || response[cursor] != ':')
    {
        return false;
    }
    ++cursor;
    while (cursor < response.size() &&
           (response[cursor] == ' ' || response[cursor] == '\t' ||
            response[cursor] == '\r' || response[cursor] == '\n'))
    {
        ++cursor;
    }
    if (cursor >= response.size())
    {
        return false;
    }
    is_login = response[cursor] == '1' || response[cursor] == 't' ||
               response[cursor] == 'T';
    return true;
}

bool ContainsVisiblePixels(const std::vector<unsigned char>& pixels)
{
    size_t non_black = 0;
    for (size_t index = 0; index + 3 < pixels.size(); index += 4)
    {
        if (pixels[index] != 0 || pixels[index + 1] != 0 || pixels[index + 2] != 0)
        {
            ++non_black;
        }
    }
    return non_black >= pixels.size() / 4 / 100;
}

bool ReadBitmapPixels(
    HDC memory_dc,
    HBITMAP bitmap,
    int width,
    int height,
    std::vector<unsigned char>& pixels)
{
    BITMAPINFO info{};
    info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    info.bmiHeader.biWidth = width;
    info.bmiHeader.biHeight = -height;
    info.bmiHeader.biPlanes = 1;
    info.bmiHeader.biBitCount = 32;
    info.bmiHeader.biCompression = BI_RGB;

    pixels.resize(static_cast<size_t>(width) * static_cast<size_t>(height) * 4);
    const int scanlines = GetDIBits(
        memory_dc,
        bitmap,
        0,
        static_cast<UINT>(height),
        pixels.data(),
        &info,
        DIB_RGB_COLORS);
    return scanlines == height;
}

bool CaptureWindowPixels(
    HWND hwnd,
    const RECT& rect,
    std::vector<unsigned char>& pixels,
    int& width,
    int& height)
{
    width = rect.right - rect.left;
    height = rect.bottom - rect.top;
    if (width <= 0 || height <= 0)
    {
        return false;
    }

    HDC screen_dc = GetDC(nullptr);
    HDC memory_dc = CreateCompatibleDC(screen_dc);
    HBITMAP bitmap = CreateCompatibleBitmap(screen_dc, width, height);
    HGDIOBJ previous = SelectObject(memory_dc, bitmap);

    bool captured =
        PrintWindow(hwnd, memory_dc, 2 /* PW_RENDERFULLCONTENT */) != FALSE &&
        ReadBitmapPixels(memory_dc, bitmap, width, height, pixels);
    if (!captured || !ContainsVisiblePixels(pixels))
    {
        captured =
            BitBlt(
                memory_dc,
                0,
                0,
                width,
                height,
                screen_dc,
                rect.left,
                rect.top,
                SRCCOPY | CAPTUREBLT) != FALSE &&
            ReadBitmapPixels(memory_dc, bitmap, width, height, pixels);
    }

    SelectObject(memory_dc, previous);
    DeleteObject(bitmap);
    DeleteDC(memory_dc);
    ReleaseDC(nullptr, screen_dc);
    return captured;
}

bool SaveCaptureBitmap(
    const std::vector<unsigned char>& pixels,
    int width,
    int height,
    const std::wstring& path)
{
    const DWORD image_size =
        static_cast<DWORD>(width) * static_cast<DWORD>(height) * 4;
    BITMAPFILEHEADER file_header{};
    BITMAPINFOHEADER info{};
    info.biSize = sizeof(BITMAPINFOHEADER);
    info.biWidth = width;
    info.biHeight = -height;
    info.biPlanes = 1;
    info.biBitCount = 32;
    info.biCompression = BI_RGB;
    info.biSizeImage = image_size;
    file_header.bfType = 0x4D42;
    file_header.bfOffBits = sizeof(BITMAPFILEHEADER) + sizeof(BITMAPINFOHEADER);
    file_header.bfSize = file_header.bfOffBits + image_size;

    HANDLE file = CreateFileW(
        path.c_str(),
        GENERIC_WRITE,
        0,
        nullptr,
        CREATE_ALWAYS,
        FILE_ATTRIBUTE_NORMAL,
        nullptr);
    if (file == INVALID_HANDLE_VALUE)
    {
        return false;
    }
    DWORD written = 0;
    const bool ok =
        WriteFile(file, &file_header, sizeof(file_header), &written, nullptr) &&
        WriteFile(file, &info, sizeof(info), &written, nullptr) &&
        WriteFile(file, pixels.data(), image_size, &written, nullptr);
    CloseHandle(file);
    return ok;
}

GreenSignature FindGreenButton(
    const std::vector<unsigned char>& pixels,
    int width,
    int height)
{
    GreenSignature signature;
    if (width <= 0 || height <= 0)
    {
        return signature;
    }

    const int start_x = width * 8 / 100;
    const int end_x = width * 94 / 100;
    const int start_y = height * 55 / 100;
    const int end_y = height * 95 / 100;
    long long x_sum = 0;
    long long y_sum = 0;

    constexpr int kSampleStep = 2;
    const int sample_width = (end_x - start_x + kSampleStep - 1) / kSampleStep;
    const int sample_height = (end_y - start_y + kSampleStep - 1) / kSampleStep;
    if (sample_width <= 0 || sample_height <= 0)
    {
        return signature;
    }

    std::vector<unsigned char> green_mask(
        static_cast<size_t>(sample_width) * sample_height, 0);
    for (int y = start_y; y < end_y; y += kSampleStep)
    {
        for (int x = start_x; x < end_x; x += kSampleStep)
        {
            const size_t offset = (static_cast<size_t>(y) * width + x) * 4;
            const int blue = pixels[offset];
            const int green = pixels[offset + 1];
            const int red = pixels[offset + 2];
            if (green >= 105 && green > red * 1.28 && green > blue * 1.12)
            {
                const int sample_x = (x - start_x) / kSampleStep;
                const int sample_y = (y - start_y) / kSampleStep;
                green_mask[
                    static_cast<size_t>(sample_y) * sample_width + sample_x] = 1;
            }
        }
    }

    std::vector<unsigned char> visited(green_mask.size(), 0);
    std::vector<int> stack;
    std::vector<int> component;
    for (int sample_y = 0; sample_y < sample_height; ++sample_y)
    {
        for (int sample_x = 0; sample_x < sample_width; ++sample_x)
        {
            const int seed = sample_y * sample_width + sample_x;
            if (!green_mask[seed] || visited[seed])
            {
                continue;
            }

            component.clear();
            stack.clear();
            stack.push_back(seed);
            visited[seed] = 1;
            while (!stack.empty())
            {
                const int current = stack.back();
                stack.pop_back();
                component.push_back(current);
                const int current_x = current % sample_width;
                const int current_y = current / sample_width;
                for (int delta_y = -1; delta_y <= 1; ++delta_y)
                {
                    for (int delta_x = -1; delta_x <= 1; ++delta_x)
                    {
                        const int next_x = current_x + delta_x;
                        const int next_y = current_y + delta_y;
                        if (next_x < 0 || next_x >= sample_width ||
                            next_y < 0 || next_y >= sample_height)
                        {
                            continue;
                        }
                        const int next = next_y * sample_width + next_x;
                        if (green_mask[next] && !visited[next])
                        {
                            visited[next] = 1;
                            stack.push_back(next);
                        }
                    }
                }
            }

            if (component.size() <= static_cast<size_t>(signature.count))
            {
                continue;
            }

            signature.count = static_cast<int>(component.size());
            x_sum = 0;
            y_sum = 0;
            int min_x = sample_width;
            int min_y = sample_height;
            int max_x = 0;
            int max_y = 0;
            for (const int index : component)
            {
                const int component_x = index % sample_width;
                const int component_y = index / sample_width;
                min_x = std::min(min_x, component_x);
                min_y = std::min(min_y, component_y);
                max_x = std::max(max_x, component_x);
                max_y = std::max(max_y, component_y);
                x_sum += component_x;
                y_sum += component_y;
            }
            signature.x =
                static_cast<double>(x_sum) / signature.count * kSampleStep + start_x;
            signature.y =
                static_cast<double>(y_sum) / signature.count * kSampleStep + start_y;
            signature.left = start_x + min_x * kSampleStep;
            signature.top = start_y + min_y * kSampleStep;
            signature.right = start_x + max_x * kSampleStep;
            signature.bottom = start_y + max_y * kSampleStep;
        }
    }
    return signature;
}

bool IsLikelyGreenButton(
    const GreenSignature& signature,
    int width,
    int height)
{
    if (signature.count < 300 || width <= 0 || height <= 0)
    {
        return false;
    }

    const int component_width = signature.right - signature.left + 1;
    const int component_height = signature.bottom - signature.top + 1;
    if (component_width < width * 35 / 100 ||
        component_width > width * 90 / 100 ||
        component_height < std::max(8, height * 3 / 100) ||
        component_height > height * 20 / 100)
    {
        return false;
    }

    const double aspect =
        static_cast<double>(component_width) / component_height;
    if (aspect < 2.0 || aspect > 12.0)
    {
        return false;
    }

    const double center_x = signature.x / width;
    const double center_y = signature.y / height;
    return center_x >= 0.40 && center_x <= 0.60 &&
           center_y >= 0.65 && center_y <= 0.90;
}

bool ClickCandidate(HWND hwnd, const RECT& rect, int x, int y, bool& used_send_input)
{
    used_send_input = false;
    if (hwnd == nullptr)
    {
        return false;
    }

    const int screen_x = rect.left + x;
    const int screen_y = rect.top + y;
    ShowWindow(hwnd, SW_RESTORE);
    SetForegroundWindow(hwnd);
    SetWindowPos(hwnd, HWND_TOP, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_SHOWWINDOW);
    Sleep(150);

    if (GetForegroundWindow() == hwnd)
    {
        POINT previous_cursor{};
        const bool had_cursor = GetCursorPos(&previous_cursor) != FALSE;
        SetCursorPos(screen_x, screen_y);
        INPUT inputs[2]{};
        inputs[0].type = INPUT_MOUSE;
        inputs[0].mi.dwFlags = MOUSEEVENTF_LEFTDOWN;
        inputs[1].type = INPUT_MOUSE;
        inputs[1].mi.dwFlags = MOUSEEVENTF_LEFTUP;
        const UINT sent = SendInput(2, inputs, sizeof(INPUT));
        Sleep(80);
        if (had_cursor)
        {
            SetCursorPos(previous_cursor.x, previous_cursor.y);
        }
        used_send_input = sent == 2;
        if (used_send_input)
        {
            return true;
        }
    }

    POINT client_point{ screen_x, screen_y };
    ScreenToClient(hwnd, &client_point);
    const LPARAM position = MAKELPARAM(client_point.x, client_point.y);
    PostMessageW(hwnd, WM_MOUSEMOVE, 0, position);
    PostMessageW(hwnd, WM_LBUTTONDOWN, MK_LBUTTON, position);
    PostMessageW(hwnd, WM_LBUTTONUP, 0, position);
    return true;
}

void AutoLoginWorker(DWORD pid, int port, std::wstring event_id)
{
    AppendLog(
        L"auto-login worker started: pid=" + std::to_wstring(pid) +
        L" port=" + std::to_wstring(port));

    const bool detect_only = IsDetectOnly();
    const ULONGLONG deadline = GetTickCount64() + 15'000;
    int observed_without_green = 0;

    while (GetTickCount64() < deadline && IsProcessRunning(pid))
    {
        bool is_login = false;
        if (QueryLoginState(port, is_login) && is_login)
        {
            AppendLog(L"auto-login skipped: WeChat is already logged in");
            return;
        }

        const std::vector<WindowCandidate> candidates = FindLoginWindowCandidates(pid);
        if (candidates.empty())
        {
            Sleep(700);
            continue;
        }

        const WindowCandidate window = candidates.front();
        const int width = window.rect.right - window.rect.left;
        const int height = window.rect.bottom - window.rect.top;
        std::vector<unsigned char> pixels;
        int captured_width = 0;
        int captured_height = 0;
        if (!CaptureWindowPixels(
                window.hwnd, window.rect, pixels, captured_width, captured_height))
        {
            Sleep(700);
            continue;
        }

        if (IsTruthyEnvironment(L"WECHAT_AUTO_LOGIN_DEBUG_SCREENSHOT"))
        {
            const std::wstring capture_path =
                JoinPath(ExecutableDirectory(), L"auto-login-capture.bmp");
            SaveCaptureBitmap(pixels, captured_width, captured_height, capture_path);
        }

        const GreenSignature green =
            FindGreenButton(pixels, captured_width, captured_height);
        std::wostringstream observation;
        observation << L"login window observation: title=" << window.title
                    << L" class=" << window.class_name
                    << L" size=" << width << L"x" << height
                    << L" green_pixels=" << green.count
                    << L" bbox=" << green.left << L"," << green.top << L"-"
                    << green.right << L"," << green.bottom
                    << L" centroid=" << static_cast<int>(green.x) << L","
                    << static_cast<int>(green.y);
        AppendLog(observation.str());

        if (IsLikelyGreenButton(green, captured_width, captured_height))
        {
            AutoLoginState state;
            state.event_id = event_id;
            state.pid = pid;
            state.width = width;
            state.height = height;
            state.green_pixels = green.count;
            state.click_x = static_cast<int>(green.x);
            state.click_y = static_cast<int>(green.y);
            state.state = L"direct_login";
            state.detail = L"green login button detected";
            WriteStateFile(state);

            if (detect_only)
            {
                AppendLog(L"detect-only: direct login button detected; click suppressed");
                return;
            }

            for (int attempt = 1; attempt <= 2; ++attempt)
            {
                bool used_send_input = false;
                if (!ClickCandidate(
                        window.hwnd,
                        window.rect,
                        state.click_x,
                        state.click_y,
                        used_send_input))
                {
                    break;
                }
                AppendLog(
                    L"direct login click attempt " + std::to_wstring(attempt) +
                    L" sent via " + (used_send_input ? L"SendInput" : L"PostMessage"));
                Sleep(1800);
                if (QueryLoginState(port, is_login) && is_login)
                {
                    AutoLoginState success = state;
                    success.state = L"login_success";
                    success.detail = L"login state verified after direct login click";
                    WriteStateFile(success);
                    AppendLog(L"auto-login succeeded");
                    return;
                }
            }

            AutoLoginState failed = state;
            failed.state = L"direct_login_failed";
            failed.detail = L"login state was not verified after bounded click attempts";
            WriteStateFile(failed);
            AppendLog(L"auto-login click did not reach logged-in state");
            return;
        }

        ++observed_without_green;
        if (observed_without_green >= 2)
        {
            // Leave QR/manual-login states untouched; Telegram provides the fallback.
            AutoLoginState state;
            state.event_id = event_id;
            state.state = L"no_direct_login_button";
            state.pid = pid;
            state.width = width;
            state.height = height;
            state.green_pixels = green.count;
            state.detail = L"login window left unchanged because no direct-login button was visible";
            WriteStateFile(state);
            AppendLog(L"no direct login button observed; login window left unchanged");
            return;
        }
        Sleep(600);
    }

    AutoLoginState state;
    state.event_id = event_id;
    state.state = L"unknown";
    state.pid = pid;
    state.detail = L"login window was not classified before timeout";
    WriteStateFile(state);
    AppendLog(L"auto-login worker finished without a classified login window");
}

bool IsDetectModeRequested(const wchar_t* command_line)
{
    if (command_line == nullptr)
    {
        return false;
    }
    const std::wstring command = Lowercase(command_line);
    return command.find(L"--auto-login-detect") != std::wstring::npos ||
           command.find(L"/auto-login-detect") != std::wstring::npos;
}

} // namespace

void InitializeWeChatAutoLoginProcess()
{
    using SetProcessDpiAwarenessContextFn = BOOL(WINAPI*)(DPI_AWARENESS_CONTEXT);
    HMODULE user32 = GetModuleHandleW(L"user32.dll");
    if (user32 != nullptr)
    {
        auto set_context = reinterpret_cast<SetProcessDpiAwarenessContextFn>(
            GetProcAddress(user32, "SetProcessDpiAwarenessContext"));
        if (set_context != nullptr)
        {
            set_context(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
        }
    }
}

void MaybeStartWeChatAutoLogin(DWORD pid, int port)
{
    if (pid == 0 || IsAutoLoginDisabled())
    {
        if (pid != 0)
        {
            AppendLog(L"auto-login disabled by WECHAT_AUTO_LOGIN");
        }
        return;
    }

    {
        std::lock_guard<std::mutex> lock(g_started_lock);
        if (!g_started_pids.insert(pid).second)
        {
            AppendLog(
                L"auto-login worker already started for pid=" +
                std::to_wstring(pid));
            return;
        }
    }

    const std::wstring event_id = CreateEventId(pid);
    std::thread(AutoLoginWorker, pid, port, event_id).detach();
}

int RunWeChatAutoLoginDetectMode(
    const wchar_t* command_line,
    DWORD pid,
    int port)
{
    if (!IsDetectModeRequested(command_line))
    {
        return -1;
    }

    InitializeWeChatAutoLoginProcess();
    AppendLog(
        L"diagnostic auto-login started: pid=" + std::to_wstring(pid) +
        L" port=" + std::to_wstring(port));
    if (pid == 0)
    {
        AutoLoginState state;
        state.state = L"unknown";
        state.detail = L"WeChat process was not found";
        WriteStateFile(state);
        AppendLog(L"diagnostic auto-login failed: WeChat process not found");
        return 2;
    }

    bool is_login = false;
    if (QueryLoginState(port, is_login) && is_login)
    {
        AutoLoginState state;
        state.event_id = CreateEventId(pid);
        state.state = L"already_logged_in";
        state.pid = pid;
        state.detail = L"type=0 reported an active login before window detection";
        WriteStateFile(state);
        AppendLog(L"diagnostic auto-login: already logged in");
        return 0;
    }

    const ULONGLONG deadline = GetTickCount64() + 15'000;
    int observed_without_green = 0;
    while (GetTickCount64() < deadline && IsProcessRunning(pid))
    {
        const std::vector<WindowCandidate> candidates = FindLoginWindowCandidates(pid);
        if (candidates.empty())
        {
            Sleep(700);
            continue;
        }

        const WindowCandidate window = candidates.front();
        std::vector<unsigned char> pixels;
        int captured_width = 0;
        int captured_height = 0;
        if (!CaptureWindowPixels(
                window.hwnd, window.rect, pixels, captured_width, captured_height))
        {
            Sleep(700);
            continue;
        }

        const std::wstring capture_path =
            JoinPath(ExecutableDirectory(), L"auto-login-detect-capture.bmp");
        SaveCaptureBitmap(
            pixels, captured_width, captured_height, capture_path);

        const GreenSignature green =
            FindGreenButton(pixels, captured_width, captured_height);
        std::wostringstream observation;
        observation << L"diagnostic observation: title=" << window.title
                    << L" class=" << window.class_name
                    << L" size=" << captured_width << L"x" << captured_height
                    << L" green_pixels=" << green.count
                    << L" bbox=" << green.left << L"," << green.top << L"-"
                    << green.right << L"," << green.bottom
                    << L" centroid=" << static_cast<int>(green.x) << L","
                    << static_cast<int>(green.y);
        AppendLog(observation.str());

        AutoLoginState state;
        state.event_id = CreateEventId(pid);
        state.pid = pid;
        state.width = captured_width;
        state.height = captured_height;
        state.green_pixels = green.count;
        state.click_x = static_cast<int>(green.x);
        state.click_y = static_cast<int>(green.y);

        if (IsLikelyGreenButton(green, captured_width, captured_height))
        {
            state.state = L"direct_login";
            state.detail = L"diagnostic only: green login button detected";
            WriteStateFile(state);
            AppendLog(L"diagnostic auto-login: direct login button detected; no click performed");
            return 0;
        }

        ++observed_without_green;
        if (observed_without_green >= 2)
        {
            state.state = L"no_direct_login_button";
            state.detail = L"diagnostic only: login window has no direct-login button";
            WriteStateFile(state);
            AppendLog(L"diagnostic auto-login: no direct login button; no click performed");
            return 0;
        }
        Sleep(600);
    }

    AutoLoginState state;
    state.event_id = CreateEventId(pid);
    state.state = L"unknown";
    state.pid = pid;
    state.detail = L"diagnostic mode did not classify a login window before timeout";
    WriteStateFile(state);
    AppendLog(L"diagnostic auto-login finished without a classified login window");
    return 3;
}
