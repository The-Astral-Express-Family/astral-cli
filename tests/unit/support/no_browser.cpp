#include <catch2/interfaces/catch_interfaces_reporter.hpp>
#include <catch2/reporters/catch_reporter_event_listener.hpp>
#include <catch2/reporters/catch_reporter_registrars.hpp>

#include <cstdlib>

// 2026-09-28 事故固化：ctest 控制台下 stdin 是 TTY，登录/注册命令族的
// device-flow 呈现路径会真实拉起用户浏览器（FakeApi 的 https://api.test
// 假域名反复弹窗）。listener 在任何测试开始前设 ASTRAL_NO_BROWSER，让
// platform::openInBrowser 全程成 no-op（platform/browser.hpp 契约）。
namespace {

class NoBrowserGuard final : public Catch::EventListenerBase {
public:
    using Catch::EventListenerBase::EventListenerBase;

    void testRunStarting(Catch::TestRunInfo const&) override {
#if defined(_WIN32)
        _putenv_s("ASTRAL_NO_BROWSER", "1");
#else
        ::setenv("ASTRAL_NO_BROWSER", "1", 1);
#endif
    }
};

} // namespace

CATCH_REGISTER_LISTENER(NoBrowserGuard)
