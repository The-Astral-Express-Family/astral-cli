#include <algorithm>
#include <cctype>
#include <exception>
#include <iostream>
#include <memory>
#include <optional>
#include <ostream>
#include <string>
#include <utility>

#include <CLI/CLI.hpp>
#include <nlohmann/json.hpp>

#include "auth/api.hpp"
#include "auth/device_flow.hpp"
#include "auth/session.hpp"
#include "client/http_client.hpp"
#include "commands/command.hpp"
#include "commands/register_cmd.hpp"
#include "core/error.hpp"
#include "output/json_output.hpp"
#include "output/render.hpp"
#include "platform/browser.hpp"
#include "platform/credential_store.hpp"
#include "platform/stdin.hpp"

namespace astral::commands {

namespace {

using nlohmann::json;

// `astral register <server_url>` — 邀请码注册（modulator TODO §3 P3 移交项；
// 契约：POST /auth/register，docs/registration.md §5.2）。带 invite_code 走
// 邀请兑换，--bootstrap 走冷启动分支（服务器无 human 时才成功）；码的归一化
// 在服务端做，CLI 原样传。注册成功只建立 web cookie 会话，CLI 无法消费，
// 因此缺省自动衔接 device-flow login 换取可落盘的 token 对（--no-login 跳过）。

std::string trimAscii(const std::string& value) {
    const auto notSpace = [](unsigned char ch) { return std::isspace(ch) == 0; };
    const auto begin = std::find_if(value.begin(), value.end(), notSpace);
    const auto end = std::find_if(value.rbegin(), value.rend(), notSpace).base();
    return begin < end ? std::string(begin, end) : std::string();
}

// Prompts go to stderr so --json keeps stdout a single object. Only reachable
// on an interactive stdin (see stdinIsTty gate in execute).
std::string promptLine(const char* text) {
    std::cerr << text;
    std::cerr.flush();
    std::string line;
    std::getline(std::cin, line);
    return trimAscii(line);
}

std::string promptPassword(const char* text) {
    std::cerr << text;
    std::cerr.flush();
    // 密码不做 trim：空格可能是合法字符；空答案由 execute 的用法检查兜底。
    return platform::readLineNoEcho();
}

// Device-flow presentation (same shape as login): the browser is only attempted
// on an interactive stdin — JSON/scripted callers must not get windows opened
// (platform/browser.hpp contract); the manual URL/code path always goes to
// stderr.
void presentUserCode(bool interactive, const std::string& url, const std::string& code,
                     std::ostream& err) {
    const bool opened = interactive && platform::openInBrowser(url);
    err << (opened ? "browser opened for approval"
                   : (interactive ? "browser unavailable" : "device approval required"))
        << "\napprove at: " << url << "\nuser code:  " << code << "\n";
}

struct ServerRejection {
    std::string code;    // protocol error.code（"" = body 不是 JSON envelope）
    std::string message; // protocol error.message
    std::string details; // error.details 对象的紧凑 dump（"" = 缺省）
    std::optional<std::string> requestId;
    std::optional<bool> retryable;
};

ServerRejection parseRejection(const client::HttpResponse& response) {
    ServerRejection rejection;
    try {
        const json envelope = json::parse(response.body).at("error");
        rejection.code = envelope.value("code", std::string());
        rejection.message = envelope.value("message", std::string());
        if (const auto it = envelope.find("details");
            it != envelope.end() && it->is_object() && it->size() > 0) {
            rejection.details = it->dump();
        }
        if (const auto it = envelope.find("request_id"); it != envelope.end() && it->is_string()) {
            rejection.requestId = it->get<std::string>();
        }
        if (const auto it = envelope.find("retryable"); it != envelope.end() && it->is_boolean()) {
            rejection.retryable = it->get<bool>();
        }
    } catch (const std::exception&) {
        // Non-JSON body: status-only phrasing below still applies.
    }
    return rejection;
}

// 注册流的契约性拒绝（400 INVITE_INVALID/VALIDATION_FAILED、403 bootstrap
// 关闭、409 EMAIL_TAKEN、429）恒 exit 1（Errc::RegistrationRejected）：调用者
// 是尚未持有任何凭证的人，这类失败没有可机器分支的动作（换码/换邮箱/
// 等待后人重试），按 HTTP 状态映射反而会把 403 误报成鉴权失败、400 误报成
// 协议不兼容。服务端 message 与 details 原样并入人话；协议码经 withProtocol
// 透传给 --json。
[[noreturn]] void throwRegisterRejection(const client::HttpResponse& response) {
    const ServerRejection rejection = parseRejection(response);
    std::string message;
    if (response.status == 429) {
        message = "registration rate-limited";
        if (const auto retryAfter = response.header("Retry-After")) {
            message += "; retry after " + *retryAfter + "s";
        } else {
            message += "; retry later";
        }
    } else if (rejection.code == "INVITE_INVALID") {
        message = "invite code rejected (unknown, already used, revoked or expired)";
    } else if (rejection.code == "EMAIL_TAKEN") {
        message = "email is already registered (the invite code was not consumed)";
    } else if (response.status == 403) {
        message = "registration closed: the server already has an account, an invite code is "
                  "required (--invite-code)";
    } else {
        message = "registration rejected";
    }
    if (!rejection.message.empty()) {
        message += ": " + rejection.message;
    }
    if (!rejection.details.empty()) {
        message += " (details: " + rejection.details + ")";
    }

    core::AstralError error{core::Errc::RegistrationRejected, std::move(message)};
    if (!rejection.code.empty()) {
        error.withProtocol(rejection.code, rejection.requestId, rejection.retryable);
    }
    throw error;
}

void renderRegistered(std::ostream& out, const std::string& baseUrl, const json& me) {
    const json& actor = me.at("actor");
    const std::string displayName = output::scalarOr(actor, "display_name");
    const std::string actorId = output::scalarOr(actor, "id");
    out << "Registered " << (displayName.empty() ? actorId : displayName + " (" + actorId + ")")
        << " on " << baseUrl << "\n";
    if (const std::string email = me.value("email", std::string()); !email.empty()) {
        out << "email:  " << email << "\n";
    }
}

class RegisterCommand final : public Command {
public:
    const char* name() const override { return "register"; }
    const char* description() const override {
        return "Create an account with an invite code (or bootstrap the first one)";
    }

    void configure(CLI::App& app) override {
        app.add_option("server_url", serverUrl_, "Server base URL, e.g. https://astral.example.com")
            ->required();
        app.add_option("--email", email_, "Account email (prompted when interactive)");
        app.add_option("--password", password_,
                       "Account password, 8-72 chars with letters and digits (prompted with echo "
                       "off when interactive)");
        app.add_option("--display-name", displayName_,
                       "Display name (optional; set later with `astral profile set`)");
        app.add_option(
            "--invite-code", inviteCode_,
            "One-time invite code XXXXX-XXXXX-XXXXX-XXXXX (required unless --bootstrap)");
        app.add_flag("--bootstrap", bootstrap_,
                     "No invite code: create the first account on an empty server")
            ->excludes("--invite-code");
        app.add_flag("--no-login", noLogin_,
                     "Only create the account; skip the automatic device-flow login");
    }

    int execute(const CommandContext& context) override {
        const std::string baseUrl = auth::resolveServerUrl(serverUrl_, context.server);
        // 空串视为「未提供」：invite-code/display-name 的空值没有独立语义
        // （profile set 的 --bio "" 清空语义不适用于此）。
        std::string email = email_;
        std::string password = password_;
        std::string displayName = displayName_;
        std::string inviteCode = inviteCode_;

        // Interactive fill-ins: only the required fields, and only when stdin
        // is a keyboard. Non-TTY callers (pipes, agents) get a usage error
        // instead of a blocked read. display_name stays a pure flag: prompting
        // for optional data would hang scripted flows on an idle TTY, and the
        // name is editable later via `astral profile set --display-name`.
        const bool interactive = platform::stdinIsTty();
        if (interactive) {
            if (email.empty()) {
                email = promptLine("email: ");
            }
            if (password.empty()) {
                password = promptPassword("password (8-72 chars, letters and digits): ");
            }
            if (!bootstrap_ && inviteCode.empty()) {
                inviteCode = promptLine("invite code: ");
            }
        }

        if (email.empty()) {
            throw core::AstralError(core::Errc::Usage,
                                    "--email is required when stdin is not interactive");
        }
        if (password.empty()) {
            throw core::AstralError(core::Errc::Usage,
                                    "--password is required when stdin is not interactive");
        }
        if (!bootstrap_ && inviteCode.empty()) {
            throw core::AstralError(core::Errc::Usage,
                                    "no invite code given: pass --invite-code <code>, or "
                                    "--bootstrap to create the first account on an empty server");
        }

        json body = json{{"email", email}, {"password", password}};
        if (!displayName.empty()) {
            body["display_name"] = displayName;
        }
        if (!bootstrap_) {
            body["invite_code"] = inviteCode;
        }

        // Anonymous endpoint: discovery for api base + protocol gate, then the
        // register POST without any bearer (ASTRAL_TOKEN must not leak in here).
        const auth::ServerInfo server = auth::discoverServer(baseUrl, auth::commandHttp());
        client::HttpRequest request;
        request.method = "POST";
        request.url = server.origin() + server.apiBase + "/auth/register";
        request.body = body.dump();
        request.headers.emplace_back("Content-Type", "application/json");
        auth::stampClientHeaders(request);
        const client::HttpResponse response = auth::commandHttp()(request);

        if (response.status != 201) {
            if (response.status == 400 || response.status == 403 || response.status == 409 ||
                response.status == 429) {
                throwRegisterRejection(response);
            }
            // Contract-external status: shared mapper (5xx -> 6, odd 4xx -> 9...).
            auth::throwApiError(response, "register");
        }
        json me;
        try {
            me = json::parse(response.body);
        } catch (const std::exception&) {
            throw core::AstralError(core::Errc::ProtocolIncompatible,
                                    "register: response is not valid JSON");
        }
        if (!me.contains("actor") || !me.at("actor").is_object()) {
            throw core::AstralError(core::Errc::ProtocolIncompatible,
                                    "register: response missing the actor object");
        }

        if (!context.json) {
            renderRegistered(context.out, server.baseUrl, me);
        }
        json payload = json{{"registered", me}, {"logged_in", false}};
        if (noLogin_) {
            if (context.json) {
                output::printJson(context.out, payload);
            }
            return 0;
        }

        // Auto-login through the same device flow as `astral login`: the
        // register response grants a web cookie session the CLI cannot store.
        platform::LoginSession session;
        try {
            session = auth::runDeviceFlow(
                server.baseUrl, auth::commandHttp(), auth::realSleep,
                [interactive, &context](const std::string& url, const std::string& code) {
                    presentUserCode(interactive, url, code, context.err);
                });
        } catch (const core::AstralError& error) {
            // 账号已建，只有登录半程失败：保留原退出码与协议字段，附上重试提示。
            core::AstralError retry{error.code(), std::string(error.what()) +
                                                      " (account created; run 'astral login " +
                                                      server.baseUrl + "' to log in)"};
            if (error.protocolCode()) {
                retry.withProtocol(*error.protocolCode(), error.requestId(), error.retryable());
            }
            throw retry;
        }
        platform::makeDefaultCredentialStore()->saveSession(session.serverUrl, session);

        payload["logged_in"] = true;
        payload["session"] = json{{"server_url", session.serverUrl},
                                  {"server_id", session.serverId},
                                  {"principal_id", session.principalId}};
        if (context.json) {
            output::printJson(context.out, payload);
            return 0;
        }
        context.out << "Logged in as "
                    << (session.principalId.empty() ? "<unknown>" : session.principalId) << " on "
                    << session.serverId << " (" << session.serverUrl << ")\n";
        return 0;
    }

private:
    std::string serverUrl_;
    std::string email_;
    std::string password_;
    std::string displayName_;
    std::string inviteCode_;
    bool bootstrap_ = false;
    bool noLogin_ = false;
};

} // namespace

std::unique_ptr<Command> makeRegisterCommand() {
    return std::make_unique<RegisterCommand>();
}

} // namespace astral::commands
