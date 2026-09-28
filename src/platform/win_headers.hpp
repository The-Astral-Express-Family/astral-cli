#pragma once

// Win32 公共头引入样板（browser.cpp / tty.cpp 共用，勿再各自抄宏守卫）：
//   - WIN32_LEAN_AND_MEAN / NOMINMAX 可能已被传递包含定义过，无守卫 #define
//     在 -Werror 下会触发宏重定义告警；
//   - 次级头（shellapi.h 等）依赖 windows.h 的类型声明，必须排在本头之后。
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
