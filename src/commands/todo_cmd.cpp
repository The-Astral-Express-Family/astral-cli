#include "commands/todo_cmd.hpp"

#include <cctype>
#include <cstdint>
#include <cstdio>
#include <fstream>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include <CLI/CLI.hpp>
#include <nlohmann/json.hpp>

#include "auth/api.hpp"
#include "commands/command.hpp"
#include "commands/paging.hpp"
#include "core/content_hash.hpp"
#include "core/error.hpp"
#include "output/json_output.hpp"
#include "output/render.hpp"
#include "output/style.hpp"
#include "platform/stdin.hpp"

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

// add-tree 响应（TaskTreeBatchCreated）的节点统计与缩进渲染：镜像请求
// 树形，子层两空格缩进，行内 id + title。
void countTreeNodes(const json& nodes, std::size_t& total) {
    for (const auto& node : nodes) {
        ++total;
        if (auto children = node.find("children"); children != node.end() && children->is_array()) {
            countTreeNodes(*children, total);
        }
    }
}

void printTreeCreated(std::ostream& out, const json& nodes, int depth) {
    for (const auto& node : nodes) {
        const json& task = node.at("task");
        out << std::string(static_cast<std::size_t>(depth) * 2, ' ') << "- " << scalarOr(task, "id")
            << "  " << scalarOr(task, "title") << '\n';
        if (auto children = node.find("children"); children != node.end() && children->is_array()) {
            printTreeCreated(out, *children, depth + 1);
        }
    }
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

        CLI::App* show = app.add_subcommand("show", "Show one task (includes tags)");
        show->add_option("task_id", taskId_, "Task id")->required();

        CLI::App* claim =
            app.add_subcommand("claim", "Atomically claim a task (held until release/done)");
        claim->add_option("task_id", taskId_, "Task id")->required();
        claim->add_option("--revision", revision_,
                          "Expected revision (default: read the task's current revision)");

        CLI::App* done = app.add_subcommand("done", "Mark task(s) done (1 = PATCH, N = batch)");
        done->add_option("task_id", doneIds_, "Task ids (one or more)")->required()->expected(-1);
        done->add_option("--revision", revision_,
                         "Expected revision (single-id form only; default: read the current one)");

        // 协议 2.1 task_batch：批量树形创建——嵌套 JSON 一次投递整棵子树
        // （--parent 选 task 容器，缺省 workspace 根层）；确定性幂等键使
        // 重跑安全（服务端整批单事务全有或全无）。
        CLI::App* addTree = app.add_subcommand(
            "add-tree", "Create a batch of task trees from a nested JSON file (or '-' for stdin)");
        addTree->add_option("--file", treeFile_, "JSON file: {\"trees\":[...]} or a bare array")
            ->required();
        addTree->add_option("--parent", parent_, "Create the trees under this task");

        // v2.1：移动 = PATCH parent_id 三态；'-' 表示移回根层（null）。
        CLI::App* move =
            app.add_subcommand("move", "Move a task under another container ('-' = root)");
        move->add_option("task_id", taskId_, "Task id")->required();
        move->add_option("--to", moveTo_, "New parent task id, or '-' for the workspace root")
            ->required();

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

        // 协议 2.2：租约时间维度拆除——认领持有至释放/任务完成，无 renew；
        // release 主动让出（204 无响应体），他人认领需 task:override 强制释放。
        CLI::App* release = app.add_subcommand("release", "Release your claim on a task");
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
        addTreeSub_ = addTree;
        moveSub_ = move;
        showSub_ = show;
        claimSub_ = claim;
        doneSub_ = done;
        searchSub_ = search;
        updateSub_ = update;
        releaseSub_ = release;
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
        if (node_->got_subcommand(addTreeSub_)) {
            return runAddTree(context);
        }
        if (node_->got_subcommand(moveSub_)) {
            return runMove(context);
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
        if (node_->got_subcommand(releaseSub_)) {
            return runRelease(context);
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
        // 重试不得双建：确定性 Idempotency-Key（同 actor+endpoint+key 24h
        // 重放首次 2xx；key 源覆盖全部创建入参，与 msg/document 同纪律）。
        std::string keySource =
            ws.workspaceId + "|" + parent_ + "|" + title_ + "|" + description_ + "|" + priority_;
        for (const std::string& tag : tags_) {
            keySource += "|" + tag;
        }
        const json task =
            auth::sendJson(api, "POST", path, body, "task create",
                           {{"Idempotency-Key", "todo-" + core::fnv1aHex(keySource)}});

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

        const json body{{"expected_revision", expectedRevision(api)}};
        const json result =
            auth::sendJson(api, "POST", "/tasks/" + taskId_ + "/claim", body, "task claim");

        if (context.json) {
            output::printJson(context.out, result); // ClaimResult {task} verbatim
            return 0;
        }
        context.out << "Claimed " << taskId_ << " - held until release or done" << '\n';
        return 0;
    }

    int runDone(const CommandContext& context) {
        // One session per run: discovery exactly once; unresolvable targets get the
        // D14 default-workspace hint from openWorkspace.
        auto [api, ws] = auth::openWorkspace(context.server, context.workspace);

        // 单 id：走既有 PATCH（逐任务乐观并发，行为与批量语义一致）。
        if (doneIds_.size() == 1) {
            taskId_ = doneIds_.front();
            const json body{{"expected_revision", expectedRevision(api)}, {"status", "done"}};
            const json task =
                auth::sendJson(api, "PATCH", "/tasks/" + taskId_, body, "task update");

            if (context.json) {
                output::printJson(context.out, task);
                return 0;
            }
            context.out << "Marked " << taskId_ << " done (revision " << scalarOr(task, "revision")
                        << ")\n";
            return 0;
        }

        // 多 id：协议 2.1 batch-update 一次整批（服务端单事务全有或全无，
        // 部分失败时整批不生效，重跑安全——revision 预读自当前状态）。
        json items = json::array();
        for (const std::string& id : doneIds_) {
            items.push_back(json{{"task_id", id}, {"expected_revision", currentRevision(api, id)}});
        }
        const json body{
            {"items", items},
            {"set", json{{"status", "done"}}},
        };
        const json result =
            auth::sendJson(api, "POST", "/workspaces/" + ws.workspaceId + "/tasks/batch-update",
                           body, "task batch update");

        if (context.json) {
            output::printJson(context.out, result); // TaskBatchResult {items} verbatim
            return 0;
        }
        for (const auto& task : result.at("items")) {
            context.out << "Marked " << scalarOr(task, "id") << " done (revision "
                        << scalarOr(task, "revision") << ")\n";
        }
        return 0;
    }

    // runMove：PATCH parent_id 三态（'-' = null = 根层）；移动语义与字段
    // 更新分属不同动词，与 update 共享 revision 节拍。
    int runMove(const CommandContext& context) {
        auto [api, ws] = auth::openWorkspace(context.server, context.workspace);

        json body{{"expected_revision", expectedRevision(api)}};
        if (moveTo_ == "-") {
            body["parent_id"] = nullptr;
        } else {
            body["parent_id"] = moveTo_;
        }
        const json task = auth::sendJson(api, "PATCH", "/tasks/" + taskId_, body, "task move");

        if (context.json) {
            output::printJson(context.out, task);
            return 0;
        }
        context.out << "Moved " << taskId_ << " -> "
                    << (moveTo_ == "-" ? std::string("workspace root") : moveTo_) << " (revision "
                    << scalarOr(task, "revision") << ")\n";
        return 0;
    }

    // runAddTree：嵌套 JSON 一次投递（协议 2.1 task-trees）。输入接受
    // {"trees":[...]} 或裸数组；幂等键 = 内容 hash，重跑同文件服务端重放
    // 首次响应，不会双建。
    int runAddTree(const CommandContext& context) {
        std::string content;
        if (treeFile_ == "-") {
            content = platform::readStdinBinary();
        } else {
            std::ifstream input(treeFile_, std::ios::binary);
            if (!input) {
                throw core::AstralError(core::Errc::Usage, "cannot open '" + treeFile_ + "'");
            }
            // 分块读取（istreambuf_iterator 被 AGENTS.md 禁用）。
            char buffer[8192];
            while (input.read(buffer, sizeof buffer) || input.gcount() > 0) {
                content.append(buffer, static_cast<std::size_t>(input.gcount()));
            }
        }
        json parsed;
        try {
            parsed = json::parse(content);
        } catch (const json::parse_error& error) {
            throw core::AstralError(core::Errc::Usage,
                                    std::string("invalid JSON input: ") + error.what());
        }
        json trees = parsed.contains("trees") ? parsed.at("trees") : parsed;
        if (!trees.is_array() || trees.empty()) {
            throw core::AstralError(
                core::Errc::Usage,
                "input must be {\"trees\":[...]} or a non-empty array of task nodes");
        }

        auto [api, ws] = auth::openWorkspace(context.server, context.workspace);

        const std::string path = parent_.empty() ? "/workspaces/" + ws.workspaceId + "/task-trees"
                                                 : "/tasks/" + parent_ + "/task-trees";
        const json body{{"trees", trees}};
        const json result = auth::sendJson(
            api, "POST", path, body, "task tree create",
            {{"Idempotency-Key",
              "todo-tree-" + core::fnv1aHex(ws.workspaceId + "|" + parent_ + "|" + content)}});

        if (context.json) {
            output::printJson(context.out, result); // TaskTreeBatchCreated verbatim
            return 0;
        }
        const json& created = result.at("items");
        std::size_t total = 0;
        countTreeNodes(created, total);
        context.out << "Created " << total << " task(s) in " << created.size() << " tree(s):\n";
        printTreeCreated(context.out, created, 1);
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

    int runRelease(const CommandContext& context) {
        // One session per run: discovery exactly once; unresolvable targets get the
        // D14 default-workspace hint from openWorkspace.
        auto [api, ws] = auth::openWorkspace(context.server, context.workspace);

        auth::sendNoBody(api, "DELETE", "/tasks/" + taskId_ + "/claim", "claim release");

        if (context.json) {
            // 204 无响应体：--json 侧的确认单对象是 CLI 呈现，非服务端原样。
            output::printJson(context.out, json{{"released", true}, {"task_id", taskId_}});
            return 0;
        }
        context.out << "Released claim on " << taskId_ << '\n';
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
    CLI::App* addTreeSub_ = nullptr;
    CLI::App* moveSub_ = nullptr;
    CLI::App* showSub_ = nullptr;
    CLI::App* claimSub_ = nullptr;
    CLI::App* doneSub_ = nullptr;
    CLI::App* searchSub_ = nullptr;
    CLI::App* updateSub_ = nullptr;
    CLI::App* releaseSub_ = nullptr;
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
    std::vector<std::string> doneIds_;
    std::string treeFile_;
    std::string moveTo_;
    std::optional<std::int64_t> revision_;
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
