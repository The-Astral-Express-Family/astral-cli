#include "commands/todo_cmd.hpp"

#include <cstdint>
#include <cstdio>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include <CLI/CLI.hpp>
#include <nlohmann/json.hpp>

#include "auth/api.hpp"
#include "commands/command.hpp"
#include "core/error.hpp"
#include "output/json_output.hpp"
#include "output/style.hpp"

namespace astral::commands {

namespace {

using nlohmann::json;
using output::Painter;

const char* const kTaskStatuses[] = {"open",   "in_progress", "blocked",
                                     "review", "done",        "cancelled"};
const char* const kTaskPriorities[] = {"low", "normal", "high", "urgent"};

// Cuts at a codepoint boundary so CJK titles never get mojibake'd.
std::string truncateUtf8(const std::string& text, std::size_t maxCodepoints) {
    std::size_t codepoints = 0;
    std::size_t offset = 0;
    while (offset < text.size()) {
        if (codepoints == maxCodepoints) {
            return text.substr(0, offset) + "...";
        }
        const unsigned char lead = static_cast<unsigned char>(text[offset]);
        std::size_t len = 1;
        if ((lead & 0xE0) == 0xC0) {
            len = 2;
        } else if ((lead & 0xF0) == 0xE0) {
            len = 3;
        } else if ((lead & 0xF8) == 0xF0) {
            len = 4;
        }
        offset += len;
        ++codepoints;
    }
    return text;
}

std::string padRight(const std::string& text, std::size_t width) {
    return text.size() >= width ? text : text + std::string(width - text.size(), ' ');
}

std::string scalarOr(const json& object, const char* key, const std::string& fallback = "") {
    const auto it = object.find(key);
    if (it == object.end() || it->is_null()) {
        return fallback;
    }
    if (it->is_string()) {
        return it->get<std::string>();
    }
    return it->dump();
}

void appendParam(std::string& query, const std::string& key, const std::string& value) {
    if (value.empty()) {
        return;
    }
    if (!query.empty()) {
        query += '&';
    }
    query += key + '=' + client::urlEncode(value);
}

// Shared read flags of list and search (--limit/--all/--status).
struct PageFlags {
    std::optional<int> limit;
    bool all = false;
    std::string status;
};

void addPageFlags(CLI::App& app, PageFlags& flags) {
    app.add_option("--limit", flags.limit, "Page size (server max 200)");
    app.add_flag("--all", flags.all, "Follow next_cursor until exhausted");
    app.add_option("--status", flags.status, "Filter by task status")
        ->check(CLI::IsMember(
            std::vector<std::string>{std::begin(kTaskStatuses), std::end(kTaskStatuses)}));
}

std::string pageQuery(const PageFlags& flags) {
    std::string query;
    if (!flags.status.empty()) {
        appendParam(query, "status", flags.status);
    }
    if (flags.limit) {
        appendParam(query, "limit", std::to_string(*flags.limit));
    }
    return query;
}

// GETs {path}[?query] page by page, accumulating items. Opaque next_cursor
// values come straight back as &cursor=...; --all follows until exhausted.
// `nextCursor` receives the last observed cursor ("" when exhausted).
json fetchAllPages(const auth::ApiSession& api, const std::string& path, std::string query,
                   bool followAll, const std::string& what, std::string& nextCursor) {
    json items = json::array();
    while (true) {
        client::HttpRequest request;
        request.url = auth::apiUrl(api, query.empty() ? path : path + "?" + query);
        const json body = json::parse(api.requireSuccess(std::move(request), what).body);
        if (auto it = body.find("items"); it != body.end() && it->is_array()) {
            for (const auto& item : *it) {
                items.push_back(item);
            }
        }
        const auto next = body.find("next_cursor");
        const bool hasMore =
            next != body.end() && next->is_string() && !next->get<std::string>().empty();
        nextCursor = hasMore ? next->get<std::string>() : std::string();
        if (!hasMore || !followAll) {
            return items;
        }
        appendParam(query, "cursor", nextCursor);
    }
}

void printTaskTable(std::ostream& out, const json& items, bool withScore) {
    std::size_t idWidth = 2;
    std::size_t statusWidth = 6;
    std::size_t priorityWidth = 3;
    for (const auto& task : items) {
        idWidth = std::max(idWidth, scalarOr(task, "id").size());
        statusWidth = std::max(statusWidth, scalarOr(task, "status").size());
        priorityWidth = std::max(priorityWidth, scalarOr(task, "priority").size());
    }

    auto emitRow = [&](const std::string& id, const std::string& status,
                       const std::string& priority, const std::string& title,
                       const std::string& score) {
        out << padRight(id, idWidth) << "  " << padRight(status, statusWidth) << "  "
            << padRight(priority, priorityWidth) << "  ";
        if (withScore) {
            out << padRight(score, 6) << "  ";
        }
        out << truncateUtf8(title, 48) << '\n';
    };

    emitRow("ID", "STATUS", "PRI", "TITLE", "SCORE");
    for (const auto& task : items) {
        std::string score = "-";
        if (withScore) {
            const auto it = task.find("score");
            if (it != task.end() && it->is_number()) {
                char buffer[32];
                std::snprintf(buffer, sizeof(buffer), "%.3f", it->get<double>());
                score = buffer;
            }
        }
        emitRow(scalarOr(task, "id"), scalarOr(task, "status"), scalarOr(task, "priority"),
                scalarOr(task, "title"), score);
    }
}

void printTaskDetail(std::ostream& out, const Painter& paint, const json& task) {
    auto line = [&out, &paint](const std::string& key, const std::string& value) {
        out << "  " << paint.key(padRight(key + ":", 12)) << value << '\n';
    };
    out << paint.bold(scalarOr(task, "id")) << "  " << scalarOr(task, "title") << '\n';
    line("status", scalarOr(task, "status"));
    line("priority", scalarOr(task, "priority", "normal"));
    line("assignee", scalarOr(task, "assignee_actor_id", "-"));
    line("parent", scalarOr(task, "parent_id", "-"));
    line("revision", scalarOr(task, "revision"));
    if (auto lease = task.find("lease"); lease != task.end() && lease->is_object()) {
        line("lease",
             scalarOr(*lease, "holder_actor_id") + " until " + scalarOr(*lease, "expires_at"));
    } else {
        line("lease", "-");
    }
    std::string tags;
    if (auto array = task.find("tags"); array != task.end() && array->is_array()) {
        for (const auto& tag : *array) {
            if (!tags.empty()) {
                tags += ", ";
            }
            tags += scalarOr(tag, "name");
        }
    }
    line("tags", tags.empty() ? "-" : tags);
    line("created", scalarOr(task, "created_at"));
    line("updated", scalarOr(task, "updated_at"));
    const std::string description = scalarOr(task, "description");
    if (!description.empty()) {
        out << "  " << paint.key("description:") << '\n';
        std::size_t start = 0;
        while (start <= description.size()) {
            const std::size_t end = description.find('\n', start);
            const std::size_t stop = end == std::string::npos ? description.size() : end;
            out << "    " << description.substr(start, stop - start) << '\n';
            if (end == std::string::npos) {
                break;
            }
            start = end + 1;
        }
    }
}

// ---- the noun -----------------------------------------------------------

class TodoCommand final : public Command {
public:
    const char* name() const override { return "todo"; }
    const char* description() const override {
        return "Work with tasks (server task tree is the source of truth)";
    }

    void configure(CLI::App& app) override {
        node_ = &app;
        app.require_subcommand(1);

        CLI::App* list = app.add_subcommand("list", "List tasks (newest first)");
        list->add_option("--assignee", assignee_, "Filter by assignee actor id");
        list->add_option("--parent", parent_, "Only direct children of this task");
        addPageFlags(*list, pageFlags_);

        CLI::App* add = app.add_subcommand("add", "Create a task");
        add->add_option("title", title_, "Task title")->required();
        add->add_option("--parent", parent_, "Parent task id");
        add->add_option("--priority", priority_, "Task priority")
            ->check(CLI::IsMember(
                std::vector<std::string>{std::begin(kTaskPriorities), std::end(kTaskPriorities)}));
        add->add_option("--description", description_, "Task description");
        add->add_option("--tag", tags_, "Existing tag name (repeatable)");

        CLI::App* show = app.add_subcommand("show", "Show one task (includes tags and lease)");
        show->add_option("task_id", taskId_, "Task id")->required();

        CLI::App* claim = app.add_subcommand("claim", "Atomically claim a task (acquire lease)");
        claim->add_option("task_id", taskId_, "Task id")->required();
        claim->add_option("--revision", revision_,
                          "Expected revision (default: read the task's current revision)");
        claim
            ->add_option("--lease-seconds", leaseSeconds_, "Lease duration, 30..3600 (default 300)")
            ->check(CLI::Range(30, 3600));

        CLI::App* done = app.add_subcommand("done", "Mark a task done (optimistic concurrency)");
        done->add_option("task_id", taskId_, "Task id")->required();
        done->add_option("--revision", revision_,
                         "Expected revision (default: read the task's current revision)");

        CLI::App* search = app.add_subcommand("search", "Search tasks (regex filter, fuzzy rank)");
        search->add_option("--regex", regex_, "RE2 regex over title+description (filter)");
        search->add_option("--fuzzy", fuzzy_, "Fuzzy text (ranking)");
        search->add_option("--tag", tag_, "Filter by tag name");
        addPageFlags(*search, pageFlags_);

        listSub_ = list;
        addSub_ = add;
        showSub_ = show;
        claimSub_ = claim;
        doneSub_ = done;
        searchSub_ = search;
    }

    int execute(const CommandContext& context) override {
        if (node_->got_subcommand(listSub_)) {
            return runList(context);
        }
        if (node_->got_subcommand(addSub_)) {
            return runAdd(context);
        }
        if (node_->got_subcommand(showSub_)) {
            return runShow(context);
        }
        if (node_->got_subcommand(claimSub_)) {
            return runClaim(context);
        }
        if (node_->got_subcommand(doneSub_)) {
            return runDone(context);
        }
        if (node_->got_subcommand(searchSub_)) {
            return runSearch(context);
        }
        throw core::AstralError(core::Errc::Usage, "no todo subcommand selected");
    }

private:
    // One session per command run: discovery happens exactly once and the
    // (possibly refreshed) session state stays warm for follow-up requests.
    std::pair<auth::ApiSession, auth::WorkspaceContext>
    openContext(const CommandContext& context) const {
        const auth::LocalTarget local = auth::resolveLocalTarget(context.server, context.workspace);
        auth::ApiSession api{local.serverUrl};
        auth::WorkspaceContext ws = auth::resolveWorkspace(api, local);
        return {std::move(api), std::move(ws)};
    }

    // claim/done need the server's current revision for optimistic
    // concurrency; --revision pins it and skips the extra GET.
    std::int64_t expectedRevision(const auth::ApiSession& api) const {
        return revision_ ? *revision_ : currentRevision(api, taskId_);
    }

    std::int64_t currentRevision(const auth::ApiSession& api, const std::string& taskId) const {
        client::HttpRequest request;
        request.url = auth::apiUrl(api, "/tasks/" + taskId);
        return json::parse(api.requireSuccess(std::move(request), "task lookup").body)
            .value("revision", 0);
    }

    int runList(const CommandContext& context) {
        auto [api, ws] = openContext(context);

        std::string query = pageQuery(pageFlags_);
        appendParam(query, "parent_id", parent_);
        appendParam(query, "assignee", assignee_);
        std::string nextCursor;
        const json items = fetchAllPages(api, "/workspaces/" + ws.workspaceId + "/tasks",
                                         std::move(query), pageFlags_.all, "task list", nextCursor);

        if (context.json) {
            output::printJson(
                context.out,
                {{"workspace_id", ws.workspaceId},
                 {"items", items},
                 {"next_cursor", nextCursor.empty() ? json(nullptr) : json(nextCursor)}});
            return 0;
        }
        if (items.empty()) {
            context.out << "No tasks.\n";
            return 0;
        }
        printTaskTable(context.out, items, /*withScore=*/false);
        if (!nextCursor.empty()) {
            context.out << "(" << items.size()
                        << " shown; more available - pass --all or raise --limit)\n";
        }
        return 0;
    }

    int runSearch(const CommandContext& context) {
        if (regex_.empty() && fuzzy_.empty()) {
            throw core::AstralError(core::Errc::Usage, "todo search needs --regex or --fuzzy");
        }
        auto [api, ws] = openContext(context);

        // Semantics are fixed server-side (architecture section 13):
        // permission -> structured -> regex filter -> fuzzy rank -> paging.
        std::string query = pageQuery(pageFlags_);
        appendParam(query, "regex", regex_);
        appendParam(query, "fuzzy", fuzzy_);
        appendParam(query, "tag", tag_);
        std::string nextCursor;
        const json items =
            fetchAllPages(api, "/workspaces/" + ws.workspaceId + "/tasks/search", std::move(query),
                          pageFlags_.all, "task search", nextCursor);

        if (context.json) {
            output::printJson(
                context.out,
                {{"workspace_id", ws.workspaceId},
                 {"items", items},
                 {"next_cursor", nextCursor.empty() ? json(nullptr) : json(nextCursor)}});
            return 0;
        }
        if (items.empty()) {
            context.out << "No matching tasks.\n";
            return 0;
        }
        printTaskTable(context.out, items, /*withScore=*/!fuzzy_.empty());
        return 0;
    }

    int runAdd(const CommandContext& context) {
        auto [api, ws] = openContext(context);

        json body{{"title", title_}};
        if (!parent_.empty()) {
            body["parent_id"] = parent_;
        }
        if (!priority_.empty()) {
            body["priority"] = priority_;
        }
        if (!description_.empty()) {
            body["description"] = description_;
        }
        if (!tags_.empty()) {
            // Existing tag names only; fresh names need the tags proposal flow.
            body["tags"] = tags_;
        }

        client::HttpRequest request;
        request.method = "POST";
        request.url = auth::apiUrl(api, "/workspaces/" + ws.workspaceId + "/tasks");
        request.body = body.dump();
        request.headers.emplace_back("Content-Type", "application/json");
        const json task = json::parse(api.requireSuccess(std::move(request), "task create").body);

        if (context.json) {
            output::printJson(context.out, task);
            return 0;
        }
        context.out << "Created " << scalarOr(task, "id") << " " << scalarOr(task, "title") << '\n';
        return 0;
    }

    int runShow(const CommandContext& context) {
        auto [api, ws] = openContext(context);

        client::HttpRequest request;
        request.url = auth::apiUrl(api, "/tasks/" + taskId_);
        const json task = json::parse(api.requireSuccess(std::move(request), "task show").body);

        if (context.json) {
            output::printJson(context.out, task);
            return 0;
        }
        printTaskDetail(context.out, Painter(context.color), task);
        return 0;
    }

    int runClaim(const CommandContext& context) {
        auto [api, ws] = openContext(context);

        json body{{"expected_revision", expectedRevision(api)},
                  {"lease_seconds", leaseSeconds_.value_or(300)}};

        client::HttpRequest request;
        request.method = "POST";
        request.url = auth::apiUrl(api, "/tasks/" + taskId_ + "/claim");
        request.body = body.dump();
        request.headers.emplace_back("Content-Type", "application/json");
        const json result = json::parse(api.requireSuccess(std::move(request), "task claim").body);

        if (context.json) {
            output::printJson(context.out, result); // ClaimResult {task, lease} verbatim
            return 0;
        }
        const json& lease = result.at("lease");
        context.out << "Claimed " << taskId_ << " - holder " << scalarOr(lease, "holder_actor_id")
                    << " until " << scalarOr(lease, "expires_at") << '\n';
        return 0;
    }

    int runDone(const CommandContext& context) {
        auto [api, ws] = openContext(context);

        json body{{"expected_revision", expectedRevision(api)}, {"status", "done"}};

        client::HttpRequest request;
        request.method = "PATCH";
        request.url = auth::apiUrl(api, "/tasks/" + taskId_);
        request.body = body.dump();
        request.headers.emplace_back("Content-Type", "application/json");
        const json task = json::parse(api.requireSuccess(std::move(request), "task update").body);

        if (context.json) {
            output::printJson(context.out, task);
            return 0;
        }
        context.out << "Marked " << taskId_ << " done (revision " << scalarOr(task, "revision")
                    << ")\n";
        return 0;
    }

    CLI::App* node_ = nullptr;
    CLI::App* listSub_ = nullptr;
    CLI::App* addSub_ = nullptr;
    CLI::App* showSub_ = nullptr;
    CLI::App* claimSub_ = nullptr;
    CLI::App* doneSub_ = nullptr;
    CLI::App* searchSub_ = nullptr;

    PageFlags pageFlags_;
    std::string assignee_;
    std::string parent_;
    std::string title_;
    std::string priority_;
    std::string description_;
    std::vector<std::string> tags_;
    std::string taskId_;
    std::optional<std::int64_t> revision_;
    std::optional<int> leaseSeconds_;
    std::string regex_;
    std::string fuzzy_;
    std::string tag_;
};

} // namespace

std::unique_ptr<Command> makeTodoCommand() {
    return std::make_unique<TodoCommand>();
}

} // namespace astral::commands
