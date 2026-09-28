#include "commands/todo_cmd.hpp"

#include <cctype>
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
#include "commands/paging.hpp"
#include "core/error.hpp"
#include "output/json_output.hpp"
#include "output/render.hpp"
#include "output/style.hpp"

namespace astral::commands {

namespace {

using nlohmann::json;
using output::Painter;
using output::printMoreHint;
using output::printPageJson;
using output::scalarOr;
using output::truncateUtf8;

const char* const kTaskStatuses[] = {"open",   "in_progress", "blocked",
                                     "review", "done",        "cancelled"};
const char* const kTaskPriorities[] = {"low", "normal", "high", "urgent"};

std::string padRight(const std::string& text, std::size_t width) {
    return text.size() >= width ? text : text + std::string(width - text.size(), ' ');
}

// Task list/search read flags: shared paging (--limit/--all) + the
// task-specific status filter.
struct TaskPageFlags {
    PageFlags paging;
    std::string status;
};

void addPageFlags(CLI::App& app, TaskPageFlags& flags) {
    addPageFlags(app, flags.paging);
    app.add_option("--status", flags.status, "Filter by task status")
        ->check(CLI::IsMember(
            std::vector<std::string>{std::begin(kTaskStatuses), std::end(kTaskStatuses)}));
}

std::string pageQuery(const TaskPageFlags& flags) {
    std::string query;
    if (!flags.status.empty()) {
        appendParam(query, "status", flags.status);
    }
    addPageParams(query, flags.paging);
    return query;
}

void printTaskTable(std::ostream& out, const json& items, bool withScore) {
    std::size_t idWidth = 2;
    std::size_t statusWidth = 6;
    std::size_t priorityWidth = 3;
    std::size_t kidsWidth = 5;
    for (const auto& task : items) {
        idWidth = std::max(idWidth, scalarOr(task, "id").size());
        statusWidth = std::max(statusWidth, scalarOr(task, "status").size());
        priorityWidth = std::max(priorityWidth, scalarOr(task, "priority").size());
    }

    auto emitRow = [&](const std::string& id, const std::string& status,
                       const std::string& priority, const std::string& kids,
                       const std::string& title, const std::string& score) {
        out << padRight(id, idWidth) << "  " << padRight(status, statusWidth) << "  "
            << padRight(priority, priorityWidth) << "  " << padRight(kids, kidsWidth) << "  ";
        if (withScore) {
            out << padRight(score, 6) << "  ";
        }
        out << truncateUtf8(title, 48) << '\n';
    };

    emitRow("ID", "STATUS", "PRI", "KIDS", "TITLE", "SCORE");
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
        std::string kids = "-";
        if (auto it = task.find("children_count"); it != task.end() && it->is_number()) {
            kids = it->dump();
        }
        emitRow(scalarOr(task, "id"), scalarOr(task, "status"), scalarOr(task, "priority"), kids,
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

// tag attach/detach 的 <tag> 参数解析，沿用 tags_cmd 的词典惯例（其
// resolveTag 是文件内私有，此处同规则实现）：`tag_` 前缀直接当 id；否则对
// GET /workspaces/{id}/tags 的词典做规范化名匹配（服务端 NFKC+lowercase，
// 覆盖纯 ASCII 的 CLI 输入），找不到本地报 NotFound。
struct TagRef {
    std::string id;
    std::string name;
};

TagRef resolveTagRef(const auth::ApiSession& api, const auth::WorkspaceContext& ws,
                     const std::string& nameOrId) {
    if (nameOrId.rfind("tag_", 0) == 0) {
        return TagRef{nameOrId, ""};
    }
    client::HttpRequest request;
    request.url = auth::apiUrl(api, "/workspaces/" + ws.workspaceId + "/tags");
    const json page = json::parse(api.requireSuccess(std::move(request), "tag list").body);
    std::string lower;
    lower.reserve(nameOrId.size());
    for (char c : nameOrId) {
        lower += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    if (const auto it = page.find("items"); it != page.end() && it->is_array()) {
        for (const auto& tag : *it) {
            const std::string name = scalarOr(tag, "name");
            std::string candidate;
            candidate.reserve(name.size());
            for (char c : name) {
                candidate += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
            }
            if (candidate == lower) {
                return TagRef{scalarOr(tag, "id"), name};
            }
        }
    }
    throw core::AstralError(core::Errc::NotFound,
                            "tag '" + nameOrId + "' not found in this workspace");
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

        // v2（协议 2，D15）：list = 容器子任务集合——默认 workspace 根层，
        // --parent <id> 切到 task 容器；只回传直接子层。
        CLI::App* list =
            app.add_subcommand("list", "List children of a container (workspace root by default)");
        list->add_option("--parent", parent_,
                         "List children of this task instead of the workspace root");
        list->add_option("--assignee", assignee_, "Filter by assignee actor id");
        list->add_option("--tag", tag_, "Filter by tag name");
        addPageFlags(*list, pageFlags_);

        // v2：创建即投递进容器；无 --parent 落 workspace 根层。
        CLI::App* add =
            app.add_subcommand("add", "Create a task in a container (workspace root by default)");
        add->add_option("title", title_, "Task title")->required();
        add->add_option("--parent", parent_, "Create as a child of this task");
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

        CLI::App* update =
            app.add_subcommand("update", "Update task fields (optimistic concurrency)");
        update->add_option("task_id", taskId_, "Task id")->required();
        update->add_option("--title", title_, "New task title");
        update->add_option("--description", description_, "New task description");
        update->add_option("--status", status_, "New task status")
            ->check(CLI::IsMember(
                std::vector<std::string>{std::begin(kTaskStatuses), std::end(kTaskStatuses)}));
        update->add_option("--priority", priority_, "New task priority")
            ->check(CLI::IsMember(
                std::vector<std::string>{std::begin(kTaskPriorities), std::end(kTaskPriorities)}));
        update->add_option("--assignee", assigneeUpdate_,
                           "New assignee actor id ('-' clears the assignee)");
        update->add_option("--revision", revision_,
                           "Expected revision (default: read the task's current revision)");

        // v2 lease 管理：renew 仅 holder 可续；release 主动让出（204 无响应体）。
        CLI::App* lease = app.add_subcommand("lease", "Manage the lease on a claimed task");
        lease->require_subcommand(1);
        CLI::App* renew = lease->add_subcommand("renew", "Renew a lease you hold (holder only)");
        renew->add_option("task_id", taskId_, "Task id")->required();
        CLI::App* release = lease->add_subcommand("release", "Release a lease you hold");
        release->add_option("task_id", taskId_, "Task id")->required();

        // v2 tag 挂载/摘除：幂等语义服务端保证（D11），<tag> 为词典名或 tag_ id。
        CLI::App* tag = app.add_subcommand("tag", "Attach or detach workspace tags on a task");
        tag->require_subcommand(1);
        CLI::App* attach = tag->add_subcommand("attach", "Attach a tag to a task (idempotent)");
        attach->add_option("task_id", taskId_, "Task id")->required();
        attach->add_option("tag", tagArg_, "Tag name or tag_ id")->required();
        CLI::App* detach = tag->add_subcommand("detach", "Detach a tag from a task (idempotent)");
        detach->add_option("task_id", taskId_, "Task id")->required();
        detach->add_option("tag", tagArg_, "Tag name or tag_ id")->required();

        // v2：平面查询走 task-search，结构化（tag/status/assignee）与内容
        // （regex/fuzzy）平权，至少一个条件。
        CLI::App* search = app.add_subcommand(
            "search", "Flat workspace-wide task query (structured and/or content)");
        search->add_option("--regex", regex_, "RE2 regex over title+description (filter)");
        search->add_option("--fuzzy", fuzzy_, "Fuzzy text (ranking)");
        search->add_option("--tag", tag_, "Filter by tag name");
        search->add_option("--assignee", assignee_, "Filter by assignee actor id");
        addPageFlags(*search, pageFlags_);

        listSub_ = list;
        addSub_ = add;
        showSub_ = show;
        claimSub_ = claim;
        doneSub_ = done;
        searchSub_ = search;
        updateSub_ = update;
        leaseSub_ = lease;
        leaseRenewSub_ = renew;
        leaseReleaseSub_ = release;
        tagSub_ = tag;
        tagAttachSub_ = attach;
        tagDetachSub_ = detach;
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
        if (node_->got_subcommand(updateSub_)) {
            return runUpdate(context);
        }
        if (node_->got_subcommand(leaseSub_)) {
            if (leaseSub_->got_subcommand(leaseRenewSub_)) {
                return runLeaseRenew(context);
            }
            if (leaseSub_->got_subcommand(leaseReleaseSub_)) {
                return runLeaseRelease(context);
            }
            throw core::AstralError(core::Errc::Usage, "no todo lease subcommand selected");
        }
        if (node_->got_subcommand(tagSub_)) {
            if (tagSub_->got_subcommand(tagAttachSub_)) {
                return runTagAttach(context);
            }
            if (tagSub_->got_subcommand(tagDetachSub_)) {
                return runTagDetach(context);
            }
            throw core::AstralError(core::Errc::Usage, "no todo tag subcommand selected");
        }
        throw core::AstralError(core::Errc::Usage, "no todo subcommand selected");
    }

private:
    // claim/done need the server's current revision for optimistic
    // concurrency; --revision pins it and skips the extra GET.
    std::int64_t expectedRevision(const auth::ApiSession& api) const {
        return revision_ ? *revision_ : currentRevision(api, taskId_);
    }

    std::int64_t currentRevision(const auth::ApiSession& api, const std::string& taskId) const {
        return auth::getJson(api, "/tasks/" + taskId, "task lookup").value("revision", 0);
    }

    int runList(const CommandContext& context) {
        // One session per run: discovery exactly once; unresolvable targets get the
        // D14 default-workspace hint from openWorkspace.
        auto [api, ws] = auth::openWorkspace(context.server, context.workspace);

        // v2 容器集合：--parent 切到 task 容器，否则 workspace 容器（根层）。
        const std::string path = parent_.empty() ? "/workspaces/" + ws.workspaceId + "/children"
                                                 : "/tasks/" + parent_ + "/children";
        std::string query = pageQuery(pageFlags_);
        appendParam(query, "tag", tag_);
        appendParam(query, "assignee", assignee_);
        std::string nextCursor;
        const json items = auth::fetchPageItems(api, path, std::move(query), pageFlags_.paging.all,
                                                "task list", nextCursor);

        if (context.json) {
            printPageJson(context.out, ws.workspaceId, items, nextCursor);
            return 0;
        }
        if (items.empty()) {
            context.out << "No tasks.\n";
            return 0;
        }
        printTaskTable(context.out, items, /*withScore=*/false);
        if (!nextCursor.empty()) {
            printMoreHint(context.out, items.size(), "--all or raise --limit");
        }
        return 0;
    }

    int runSearch(const CommandContext& context) {
        // v2：结构化与内容过滤平权，至少其一（与服务端 400 守卫同语义）。
        if (regex_.empty() && fuzzy_.empty() && tag_.empty() && pageFlags_.status.empty() &&
            assignee_.empty()) {
            throw core::AstralError(
                core::Errc::Usage,
                "todo search needs at least one of --regex/--fuzzy/--tag/--status/--assignee");
        }
        // One session per run: discovery exactly once; unresolvable targets get the
        // D14 default-workspace hint from openWorkspace.
        auto [api, ws] = auth::openWorkspace(context.server, context.workspace);

        // Semantics are fixed server-side (architecture section 13):
        // permission -> structured -> regex filter -> fuzzy rank -> paging.
        std::string query = pageQuery(pageFlags_);
        appendParam(query, "regex", regex_);
        appendParam(query, "fuzzy", fuzzy_);
        appendParam(query, "tag", tag_);
        appendParam(query, "assignee", assignee_);
        std::string nextCursor;
        const json items = auth::fetchPageItems(
            api, "/workspaces/" + ws.workspaceId + "/task-search", std::move(query),
            pageFlags_.paging.all, "task search", nextCursor);

        if (context.json) {
            printPageJson(context.out, ws.workspaceId, items, nextCursor);
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
        // One session per run: discovery exactly once; unresolvable targets get the
        // D14 default-workspace hint from openWorkspace.
        auto [api, ws] = auth::openWorkspace(context.server, context.workspace);

        // v2：创建即投递进容器——无 --parent 落 workspace 根层集合，
        // 有 --parent 投递进该 task 的 children 集合（body 无 parent_id 字段）。
        json body{{"title", title_}};
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

        const std::string path = parent_.empty() ? "/workspaces/" + ws.workspaceId + "/children"
                                                 : "/tasks/" + parent_ + "/children";
        const json task = auth::sendJson(api, "POST", path, body, "task create");

        if (context.json) {
            output::printJson(context.out, task);
            return 0;
        }
        context.out << "Created " << scalarOr(task, "id") << " " << scalarOr(task, "title") << '\n';
        return 0;
    }

    int runShow(const CommandContext& context) {
        // One session per run: discovery exactly once; unresolvable targets get the
        // D14 default-workspace hint from openWorkspace.
        auto [api, ws] = auth::openWorkspace(context.server, context.workspace);

        const json task = auth::getJson(api, "/tasks/" + taskId_, "task show");

        if (context.json) {
            output::printJson(context.out, task);
            return 0;
        }
        printTaskDetail(context.out, Painter(context.color), task);
        return 0;
    }

    int runClaim(const CommandContext& context) {
        // One session per run: discovery exactly once; unresolvable targets get the
        // D14 default-workspace hint from openWorkspace.
        auto [api, ws] = auth::openWorkspace(context.server, context.workspace);

        const json body{{"expected_revision", expectedRevision(api)},
                        {"lease_seconds", leaseSeconds_.value_or(300)}};
        const json result =
            auth::sendJson(api, "POST", "/tasks/" + taskId_ + "/claim", body, "task claim");

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
        // One session per run: discovery exactly once; unresolvable targets get the
        // D14 default-workspace hint from openWorkspace.
        auto [api, ws] = auth::openWorkspace(context.server, context.workspace);

        const json body{{"expected_revision", expectedRevision(api)}, {"status", "done"}};
        const json task = auth::sendJson(api, "PATCH", "/tasks/" + taskId_, body, "task update");

        if (context.json) {
            output::printJson(context.out, task);
            return 0;
        }
        context.out << "Marked " << taskId_ << " done (revision " << scalarOr(task, "revision")
                    << ")\n";
        return 0;
    }

    int runUpdate(const CommandContext& context) {
        // --revision 只是并发参数：至少要一个可变字段，否则本地 USAGE。
        if (title_.empty() && description_.empty() && status_.empty() && priority_.empty() &&
            !assigneeUpdate_) {
            throw core::AstralError(core::Errc::Usage,
                                    "todo update needs at least one of "
                                    "--title/--description/--status/--priority/--assignee");
        }
        // One session per run: discovery exactly once; unresolvable targets get the
        // D14 default-workspace hint from openWorkspace.
        auto [api, ws] = auth::openWorkspace(context.server, context.workspace);

        json body{{"expected_revision", expectedRevision(api)}};
        if (!title_.empty()) {
            body["title"] = title_;
        }
        if (!description_.empty()) {
            body["description"] = description_;
        }
        if (!status_.empty()) {
            body["status"] = status_;
        }
        if (!priority_.empty()) {
            body["priority"] = priority_;
        }
        if (assigneeUpdate_) {
            // 字面 "-" 表示置空（assignee_actor_id: null）。
            body["assignee_actor_id"] =
                *assigneeUpdate_ == "-" ? json(nullptr) : json(*assigneeUpdate_);
        }
        const json task = auth::sendJson(api, "PATCH", "/tasks/" + taskId_, body, "task update");

        if (context.json) {
            output::printJson(context.out, task);
            return 0;
        }
        context.out << "Updated " << taskId_ << " (revision " << scalarOr(task, "revision")
                    << ")\n";
        return 0;
    }

    int runLeaseRenew(const CommandContext& context) {
        // One session per run: discovery exactly once; unresolvable targets get the
        // D14 default-workspace hint from openWorkspace.
        auto [api, ws] = auth::openWorkspace(context.server, context.workspace);

        // 协议无 requestBody，带 body 反而不洁：走无 body 请求。
        const client::HttpResponse response =
            auth::sendNoBody(api, "POST", "/tasks/" + taskId_ + "/lease/renew", "lease renew");
        const json lease = json::parse(response.body);

        if (context.json) {
            output::printJson(context.out, lease); // Lease verbatim
            return 0;
        }
        context.out << "Renewed " << taskId_ << " lease - holder "
                    << scalarOr(lease, "holder_actor_id") << " until "
                    << scalarOr(lease, "expires_at") << '\n';
        return 0;
    }

    int runLeaseRelease(const CommandContext& context) {
        // One session per run: discovery exactly once; unresolvable targets get the
        // D14 default-workspace hint from openWorkspace.
        auto [api, ws] = auth::openWorkspace(context.server, context.workspace);

        auth::sendNoBody(api, "DELETE", "/tasks/" + taskId_ + "/lease", "lease release");

        if (context.json) {
            // 204 无响应体：--json 侧的确认单对象是 CLI 呈现，非服务端原样。
            output::printJson(context.out, json{{"released", true}, {"task_id", taskId_}});
            return 0;
        }
        context.out << "Released lease on " << taskId_ << '\n';
        return 0;
    }

    int runTagAttach(const CommandContext& context) {
        // One session per run: discovery exactly once; unresolvable targets get the
        // D14 default-workspace hint from openWorkspace.
        auto [api, ws] = auth::openWorkspace(context.server, context.workspace);

        const TagRef tag = resolveTagRef(api, ws, tagArg_);
        // 不带 body（expected_revision 可选）：幂等挂载由服务端保证，不 bump revision。
        const client::HttpResponse response = auth::sendNoBody(
            api, "PUT", "/tasks/" + taskId_ + "/tags/" + tag.id, "task tag attach");
        const json task = json::parse(response.body);

        if (context.json) {
            output::printJson(context.out, task); // Task（含 tags）verbatim
            return 0;
        }
        context.out << "Attached " << (tag.name.empty() ? tag.id : tag.name) << " to " << taskId_
                    << '\n';
        return 0;
    }

    int runTagDetach(const CommandContext& context) {
        // One session per run: discovery exactly once; unresolvable targets get the
        // D14 default-workspace hint from openWorkspace.
        auto [api, ws] = auth::openWorkspace(context.server, context.workspace);

        const TagRef tag = resolveTagRef(api, ws, tagArg_);
        auth::sendNoBody(api, "DELETE", "/tasks/" + taskId_ + "/tags/" + tag.id, "task tag detach");

        if (context.json) {
            // 204 无响应体（含未挂载的幂等路径）：确认单对象为 CLI 呈现。
            output::printJson(context.out,
                              json{{"detached", true}, {"task_id", taskId_}, {"tag_id", tag.id}});
            return 0;
        }
        context.out << "Detached " << (tag.name.empty() ? tag.id : tag.name) << " from " << taskId_
                    << '\n';
        return 0;
    }

    CLI::App* node_ = nullptr;
    CLI::App* listSub_ = nullptr;
    CLI::App* addSub_ = nullptr;
    CLI::App* showSub_ = nullptr;
    CLI::App* claimSub_ = nullptr;
    CLI::App* doneSub_ = nullptr;
    CLI::App* searchSub_ = nullptr;
    CLI::App* updateSub_ = nullptr;
    CLI::App* leaseSub_ = nullptr;
    CLI::App* leaseRenewSub_ = nullptr;
    CLI::App* leaseReleaseSub_ = nullptr;
    CLI::App* tagSub_ = nullptr;
    CLI::App* tagAttachSub_ = nullptr;
    CLI::App* tagDetachSub_ = nullptr;

    TaskPageFlags pageFlags_;
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
    std::string status_;
    std::optional<std::string> assigneeUpdate_;
    std::string tagArg_;
};

} // namespace

std::unique_ptr<Command> makeTodoCommand() {
    return std::make_unique<TodoCommand>();
}

} // namespace astral::commands
