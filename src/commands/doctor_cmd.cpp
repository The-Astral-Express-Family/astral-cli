#include "commands/doctor_cmd.hpp"

#include <filesystem>
#include <fstream>
#include <ostream>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "auth/session.hpp"
#include "core/env.hpp"
#include "core/error.hpp"
#include "core/version.hpp"
#include "output/json_output.hpp"
#include "output/style.hpp"
#include "platform/credential_store.hpp"
#include "platform/user_dirs.hpp"
#include "workspace/binding.hpp"

namespace fs = std::filesystem;

namespace astral::commands {

namespace {

struct Check {
    std::string name;
    std::string status; // "ok" | "warn" | "info"
    std::string detail;
};

bool probeWritable(const fs::path& dir) {
    std::error_code ec;
    fs::create_directories(dir, ec);
    if (ec) {
        return false;
    }
    const fs::path probe = dir / ".doctor-write-probe";
    {
        std::ofstream output(probe, std::ios::trunc);
        if (!output) {
            return false;
        }
    }
    fs::remove(probe, ec);
    return true;
}

class DoctorCommand final : public Command {
public:
    const char* name() const override { return "doctor"; }
    const char* description() const override {
        return "Check the local environment (offline; no secrets are printed)";
    }

    void configure(CLI::App& app) override { (void)app; }

    int execute(const CommandContext& context) override {
        const std::vector<Check> checks = runChecks();

        if (context.json) {
            nlohmann::json array = nlohmann::json::array();
            for (const Check& check : checks) {
                array.push_back(nlohmann::json{
                    {"name", check.name},
                    {"status", check.status},
                    {"detail", check.detail},
                });
            }
            nlohmann::json doc = core::identityFields(/*withName=*/false);
            doc["checks"] = array;
            output::printJson(context.out, doc);
            return 0;
        }

        const output::Painter paint(context.color);
        context.out << paint.bold("astral doctor") << " - local environment check\n\n";
        for (const Check& check : checks) {
            std::string marker;
            if (check.status == "ok") {
                marker = paint.ok("  [ok]   ");
            } else if (check.status == "warn") {
                marker = paint.warn("  [warn] ");
            } else {
                marker = paint.dim("  [info] ");
            }
            context.out << marker << paint.key(check.name) << "  " << check.detail << "\n";
        }
        context.out << "\n"
                    << paint.dim("network: not checked (server probes land with the login/init "
                                 "round; HTTP client is ready)")
                    << "\n";
        return 0;
    }

private:
    static std::vector<Check> runChecks() {
        std::vector<Check> checks;

        checks.push_back({"version", "info",
                          std::string(core::kProjectVersion) + " (" + core::kGitDescribe + ")"});
        checks.push_back({"platform", "info", core::buildPlatform()});

        const fs::path configDir = platform::configDir();
        const bool configWritable = probeWritable(configDir);
        checks.push_back({"config-dir", configWritable ? "ok" : "warn",
                          configDir.string() + (configWritable ? "" : " (not writable)")});

        const fs::path cacheDir = platform::cacheDir();
        const bool cacheWritable = probeWritable(cacheDir);
        checks.push_back({"cache-dir", cacheWritable ? "ok" : "warn",
                          cacheDir.string() + (cacheWritable ? "" : " (not writable)")});

        auto store = platform::makeDefaultCredentialStore();
        {
            std::string detail = "backend \"" + std::string(store->backendName()) + "\" -> " +
                                 platform::astralHome().string() + "/credentials.json";
            std::string status = "ok";
            std::error_code ec;
            const fs::path credFile = platform::astralHome() / "credentials.json";
            if (fs::exists(credFile, ec)) {
                const fs::perms perms = fs::status(credFile, ec).permissions();
                if ((perms & (fs::perms::group_read | fs::perms::others_read)) != fs::perms::none) {
                    status = "warn";
                    detail += ", readable by group/others (chmod 600 recommended)";
                }
            }
            try {
                const auto servers = store->list();
                detail += servers.empty()
                              ? ", no stored credentials"
                              : ", " + std::to_string(servers.size()) + " server(s) logged in";
            } catch (const core::AstralError& e) {
                status = "warn";
                detail += std::string(" (") + e.what() + ")";
            }
            checks.push_back({"credential-store", status, detail});
        }

        // Only report presence - never the token value.
        checks.push_back({"env-token", "info",
                          core::env::has("ASTRAL_TOKEN")
                              ? "ASTRAL_TOKEN set (will be used for requests)"
                              : "ASTRAL_TOKEN not set"});

        if (auto bindingDir = workspace::findBindingDir(fs::current_path())) {
            std::string detail = "bound via " + bindingDir->string();
            try {
                if (auto binding = workspace::readBinding(*bindingDir)) {
                    detail += " -> " + binding->workspaceName + " @ " + binding->serverUrl;
                }
            } catch (const core::AstralError& e) {
                detail += " (malformed: " + std::string(e.what()) + ")";
            }
            checks.push_back({"workspace-binding", "ok", detail});
        } else {
            checks.push_back(
                {"workspace-binding", "info", "not bound (no .astral/config.json in this tree)"});
        }

        // Connectivity: probe only when a concrete target exists (env or
        // binding); offline remains a fully valid state for doctor.
        std::optional<std::string> server = core::env::get("ASTRAL_SERVER");
        if (!server) {
            if (auto bindingDir = workspace::findBindingDir(fs::current_path())) {
                if (auto binding = workspace::readBinding(*bindingDir)) {
                    server = binding->serverUrl;
                }
            }
        }
        if (!server) {
            checks.push_back({"server-connectivity", "info",
                              "no server configured (set ASTRAL_SERVER or run astral init)"});
        } else {
            try {
                const auth::ServerInfo info = auth::discoverServer(*server, auth::realHttp());
                checks.push_back({"server-connectivity", "ok",
                                  info.baseUrl + " reachable (" + info.serverId + ")"});
            } catch (const core::AstralError& e) {
                checks.push_back({"server-connectivity", "warn",
                                  *server + " unreachable: " + std::string(e.what())});
            }
        }

        return checks;
    }
};

} // namespace

std::unique_ptr<Command> makeDoctorCommand() {
    return std::make_unique<DoctorCommand>();
}

} // namespace astral::commands
