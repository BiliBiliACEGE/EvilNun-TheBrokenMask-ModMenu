#include "common.h"
#include "build_version.h"

namespace ModMenu {

const char* GetBuildStamp() { return MODMENU_BUILD_STAMP; }

bool g_MenuVisible = true;   // 注入后默认显示, 让用户立刻确认注入成功
bool g_Initialized = false;
bool g_BlockPhilippaTick = false;

unsigned long long g_CatchBlockedTick = 0; // dllmain 写 (层5拦截处决链), 守卫让路用

bool g_CatchNunResetPending = false; // dllmain 立旗, menu.cpp 渲染线程消费

static FILE* g_LogFile = nullptr;
static std::mutex g_LogMutex;

static void OpenLogFile()
{
    if (!g_LogFile)
    {
        // 日志写到游戏 exe 同目录 (工作目录可能不是游戏目录)
        wchar_t exePath[MAX_PATH]{};
        GetModuleFileNameW(nullptr, exePath, MAX_PATH);
        wchar_t* slash = wcsrchr(exePath, L'\\');
        if (slash) *(slash + 1) = 0;
        wcscat_s(exePath, L"EvilNunModMenu.log");
        g_LogFile = _wfopen(exePath, L"w");
    }
}

void Log(const char* fmt, ...)
{
    std::lock_guard<std::mutex> lk(g_LogMutex);
    OpenLogFile();
    if (!g_LogFile) return;
    va_list args;
    va_start(args, fmt);
    vfprintf(g_LogFile, fmt, args);
    va_end(args);
    fprintf(g_LogFile, "\n");
    fflush(g_LogFile);
}

void LogW(const wchar_t* fmt, ...)
{
    std::lock_guard<std::mutex> lk(g_LogMutex);
    OpenLogFile();
    if (!g_LogFile) return;
    va_list args;
    va_start(args, fmt);
    vfwprintf(g_LogFile, fmt, args);
    va_end(args);
    fwprintf(g_LogFile, L"\n");
    fflush(g_LogFile);
}

} // namespace ModMenu
