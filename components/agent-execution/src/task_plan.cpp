#include "task_plan.hpp"
#include "problem_semantic.hpp"

#include <json-c/json.h>

#include <algorithm>
#include <charconv>
#include <memory>
#include <sstream>

namespace axiom::knowledge {
namespace {
using Json = std::unique_ptr<json_object, decltype(&json_object_put)>;
void require(bool value, ErrorCode code, const char* message) {
    if (!value) throw Error(code, message);
}
json_object* allocated(json_object* value) {
    require(value != nullptr, ErrorCode::Storage, "Cannot allocate Task IR JSON"); return value;
}
Json array() { return {allocated(json_object_new_array()), json_object_put}; }
json_object* text_json(std::string_view value) {
    return allocated(json_object_new_string_len(value.data(), static_cast<int>(value.size())));
}
void append(json_object* value, json_object* item) {
    require(json_object_array_add(value, item) == 0, ErrorCode::Storage, "Cannot encode Task IR tuple");
}
std::string dump(json_object* value) {
    const auto* result = json_object_to_json_string_ext(value, JSON_C_TO_STRING_PLAIN);
    require(result != nullptr, ErrorCode::Storage, "Cannot serialize Task IR"); return result;
}
std::string_view trim(std::string_view value) {
    const auto first = value.find_first_not_of(" \t\r\n");
    if (first == std::string_view::npos) return {};
    return value.substr(first, value.find_last_not_of(" \t\r\n") - first + 1);
}
Json parse(std::string_view input) {
    // This tuple grammar has integer metadata only. Check original lexemes
    // before json-c can saturate an out-of-range signed integer.
    for (std::size_t i = 0; i < input.size();) {
        if (input[i] == '"') {
            ++i;
            while (i < input.size() && input[i] != '"') {
                if (input[i] == '\\' && i + 1 < input.size()) {
                    if (input[i + 1] == 'u') {
                        const auto codepoint = [&](std::size_t position) {
                            require(position + 4 <= input.size(), ErrorCode::Schema, "Incomplete Task IR Unicode escape");
                            std::uint32_t value = 0;
                            const auto [end, code] = std::from_chars(input.data() + position, input.data() + position + 4, value, 16);
                            require(code == std::errc{} && end == input.data() + position + 4,
                                ErrorCode::Schema, "Invalid Task IR Unicode escape"); return value;
                        };
                        const auto high = codepoint(i + 2);
                        if (high >= 0xd800 && high <= 0xdbff) {
                            require(i + 12 <= input.size() && input.substr(i + 6, 2) == "\\u",
                                ErrorCode::Schema, "Unpaired Task IR Unicode surrogate");
                            const auto low = codepoint(i + 8);
                            require(low >= 0xdc00 && low <= 0xdfff, ErrorCode::Schema, "Unpaired Task IR Unicode surrogate");
                            i += 12; continue;
                        }
                        require(high < 0xdc00 || high > 0xdfff, ErrorCode::Schema, "Unpaired Task IR Unicode surrogate");
                        i += 6; continue;
                    }
                    ++i;
                }
                ++i;
            }
            if (i < input.size()) ++i;
        } else if (input[i] == '-' || (input[i] >= '0' && input[i] <= '9')) {
            const auto first = i;
            while (i < input.size() && std::string_view(",]} \t\r\n").find(input[i]) == std::string_view::npos) ++i;
            std::int64_t value = 0;
            const auto [end, code] = std::from_chars(input.data() + first, input.data() + i, value);
            require(code == std::errc{} && end == input.data() + i, ErrorCode::Schema,
                "Task IR metadata requires exact signed 64-bit integers");
        } else ++i;
    }
    std::unique_ptr<json_tokener, decltype(&json_tokener_free)> parser(json_tokener_new_ex(32), json_tokener_free);
    require(bool(parser), ErrorCode::Storage, "Cannot allocate Task IR parser");
    json_tokener_set_flags(parser.get(), JSON_TOKENER_STRICT | JSON_TOKENER_VALIDATE_UTF8);
    Json value(json_tokener_parse_ex(parser.get(), input.data(), static_cast<int>(input.size())), json_object_put);
    require(value && json_tokener_get_error(parser.get()) == json_tokener_success
        && json_tokener_get_parse_end(parser.get()) == input.size(), ErrorCode::Schema, "Invalid complete Task IR JSON");
    return value;
}
std::size_t count(json_object* value, std::size_t maximum) {
    require(value && json_object_is_type(value, json_type_array), ErrorCode::Schema, "Expected Task IR array");
    const auto size = json_object_array_length(value);
    require(size <= maximum, ErrorCode::Budget, "Task IR array exceeds its bound"); return size;
}
void tuple(json_object* value, std::size_t size) {
    require(value && json_object_is_type(value, json_type_array) && json_object_array_length(value) == size,
        ErrorCode::Schema, "Task IR tuple has wrong arity");
}
json_object* at(json_object* value, std::size_t index) { return json_object_array_get_idx(value, index); }
std::string text(json_object* value, std::size_t maximum = 4096) {
    require(value && json_object_is_type(value, json_type_string), ErrorCode::Schema, "Expected Task IR text");
    std::string result(json_object_get_string(value), static_cast<std::size_t>(json_object_get_string_len(value)));
    validate_text(result, maximum); return result;
}
std::uint64_t number(json_object* value, std::uint64_t maximum = INT64_MAX) {
    require(value && json_object_is_type(value, json_type_int) && json_object_get_int64(value) >= 0
        && json_object_get_uint64(value) <= maximum, ErrorCode::Schema, "Invalid Task IR unsigned integer");
    return json_object_get_uint64(value);
}
void identifier(std::string_view value) {
    require(!value.empty() && value.size() <= 64 && !value.starts_with("__task_"),
        ErrorCode::Schema, "Invalid or reserved Task IR identifier");
    for (const auto ch : value) require((ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z')
        || (ch >= '0' && ch <= '9') || ch == '_' || ch == '-', ErrorCode::Schema, "Invalid Task IR identifier byte");
}
std::string id(json_object* value) { auto result = text(value, 64); identifier(result); return result; }
GoalValue scalar(json_object* value) {
    require(value != nullptr, ErrorCode::Schema, "Null Task IR condition value");
    if (json_object_is_type(value, json_type_boolean)) return json_object_get_boolean(value) != 0;
    if (json_object_is_type(value, json_type_int)) return static_cast<std::int64_t>(json_object_get_int64(value));
    if (json_object_is_type(value, json_type_string)) return text(value);
    throw Error(ErrorCode::Schema, "Task IR conditions require bool, integer or text");
}
json_object* scalar_json(const GoalValue& value) {
    if (const auto* item = std::get_if<bool>(&value)) return allocated(json_object_new_boolean(*item));
    if (const auto* item = std::get_if<std::int64_t>(&value)) return allocated(json_object_new_int64(*item));
    return text_json(std::get<std::string>(value));
}
std::string_view comparison_name(GoalComparison comparison) {
    switch (comparison) {
    case GoalComparison::Equal: return "eq";
    case GoalComparison::NotEqual: return "ne";
    case GoalComparison::Less: return "lt";
    case GoalComparison::LessEqual: return "le";
    case GoalComparison::Greater: return "gt";
    case GoalComparison::GreaterEqual: return "ge";
    }
    throw Error(ErrorCode::Schema, "Unknown Task IR comparison");
}
std::vector<GoalPredicate> predicates(json_object* value) {
    std::vector<GoalPredicate> result;
    for (std::size_t i = 0, size = count(value, maximum_items); i < size; ++i) {
        auto* row = at(value, i); tuple(row, 3);
        const auto operation = text(at(row, 1), 16);
        std::optional<GoalComparison> comparison;
        for (int code = 1; code <= 6; ++code)
            if (operation == comparison_name(static_cast<GoalComparison>(code))) comparison = static_cast<GoalComparison>(code);
        require(comparison.has_value(), ErrorCode::Schema, "Unknown Task IR comparison");
        result.push_back({text(at(row, 0), 128), *comparison, scalar(at(row, 2))});
    }
    validate_goal_predicates(result); return result;
}
Json predicate_json(std::span<const GoalPredicate> values) {
    auto result = array();
    for (const auto& value : values) {
        auto row = array(); append(row.get(), text_json(value.field));
        append(row.get(), text_json(comparison_name(value.comparison)));
        append(row.get(), scalar_json(value.value)); append(result.get(), row.release());
    }
    return result;
}
EvidenceSpan span(json_object* value, const TaskPlanContext& context) {
    const auto& source = context.sources();
    if (value && json_object_is_type(value, json_type_string)) return source.resolve(source.snapshot(), text(value, 128));
    tuple(value, 2);
    const auto first = text(at(value, 0), 128), last = text(at(value, 1), 128);
    return source.resolve(source.snapshot(), first, std::string_view(last));
}
std::vector<EvidenceSpan> supports(json_object* value, const TaskPlanContext& context, bool current = false) {
    const auto size = count(value, 16);
    require(size > 0, ErrorCode::Schema, "Task IR requires source support");
    std::vector<EvidenceSpan> result;
    for (std::size_t i = 0; i < size; ++i) result.push_back(span(at(value, i), context));
    require(!current || std::any_of(result.begin(), result.end(),
        [&](const auto& item) { return item.view == context.current_question(); }),
        ErrorCode::Schema, "Executable intent must cite the current question");
    return result;
}
std::vector<SourceReference> references(const ProblemContext& context, std::span<const EvidenceSpan> spans) {
    std::vector<SourceReference> result;
    for (const auto& item : spans) {
        const auto& view = context.views.at(item.view);
        result.push_back({view.archive_id, "", view.entry_path, view.extractor_version,
            digest(std::span(reinterpret_cast<const std::uint8_t*>(view.text.data()), view.text.size())),
            item.offset, item.length});
    }
    return result;
}
std::vector<std::string> ids(json_object* value, std::size_t maximum) {
    std::vector<std::string> result;
    for (std::size_t i = 0, size = count(value, maximum); i < size; ++i) result.push_back(id(at(value, i)));
    return result;
}
const TaskObject& object(json_object* value, const TaskPlanContext& context) {
    const auto handle = text(value, 128);
    const auto found = context.objects().find(handle);
    require(found != context.objects().end(), ErrorCode::Schema, "Unknown host object handle"); return found->second;
}
Atom atom(json_object* value) {
    tuple(value, 2);
    require(text(at(value, 0), 16) == "atom", ErrorCode::Schema, "Expected existing atom goal tuple");
    auto* item = at(value, 1); tuple(item, 3);
    require(at(item, 1) && json_object_is_type(at(item, 1), json_type_boolean),
        ErrorCode::Schema, "Expected explicit atom sign");
    Atom result{static_cast<std::uint32_t>(number(at(item, 0), UINT32_MAX)), json_object_get_boolean(at(item, 1)) != 0, {}};
    for (std::size_t i = 0, size = count(at(item, 2), 4); i < size; ++i) {
        auto* term = at(at(item, 2), i); tuple(term, 2);
        require(text(at(term, 0), 8) == "c", ErrorCode::Schema, "Task source query must be ground");
        result.terms.push_back({TermKind::Constant, static_cast<std::uint32_t>(number(at(term, 1), UINT32_MAX))});
    }
    return result;
}
KnowledgeQuery query(json_object* value, const TaskPlanContext& context) {
    const auto size = count(value, 3);
    require(size > 0, ErrorCode::Schema, "Empty Task IR knowledge query");
    const auto kind = text(at(value, 0), 32);
    KnowledgeQuery result;
    if (kind == "all") { tuple(value, 1); result.kind = QueryKind::All; }
    else if (kind == "kind") {
        tuple(value, 2); result.kind = QueryKind::Kind;
        const auto wanted = text(at(value, 1), 32);
        bool found = false;
        for (int i = 1; i <= 12; ++i) if (wanted == name(static_cast<ObjectKind>(i))) {
            result.object_kind = static_cast<ObjectKind>(i); found = true;
        }
        require(found, ErrorCode::Schema, "Unknown queried object kind");
    } else if (kind == "concept" || kind == "text") {
        tuple(value, 2); result.kind = kind == "concept" ? QueryKind::Concept : QueryKind::Text;
        result.value = text(at(value, 1));
    } else if (kind == "source") {
        tuple(value, 3); result.kind = QueryKind::Source; result.value = text(at(value, 1));
        if (at(value, 2)) result.source_version = text(at(value, 2));
    } else if (kind == "related" || kind == "required-by") {
        tuple(value, kind == "related" ? 3 : 2); result.target = object(at(value, 1), context).id;
        result.kind = kind == "related" ? QueryKind::Related : QueryKind::RequiredBy;
        if (kind == "related") {
            const auto wanted = text(at(value, 2), 32);
            bool found = false;
            for (int i = 1; i <= 8; ++i) if (wanted == name(static_cast<Relationship>(i))) {
                result.relationship = static_cast<Relationship>(i); found = true;
            }
            require(found, ErrorCode::Schema, "Unknown queried relationship");
        }
    } else throw Error(ErrorCode::Schema, "Unknown Task IR knowledge query");
    validate_knowledge_query(result, 64); return result;
}
TaskActionKind action_kind(json_object* value) {
    const auto wanted = text(value, 32);
    for (int i = 1; i <= 9; ++i) if (wanted == name(static_cast<TaskActionKind>(i))) return static_cast<TaskActionKind>(i);
    throw Error(ErrorCode::Schema, "Unknown Task IR action kind");
}
}

std::string_view name(TaskActionKind kind) {
    switch (kind) {
    case TaskActionKind::Reason: return "reason";
    case TaskActionKind::SourceReason: return "source-reason";
    case TaskActionKind::Retrieve: return "retrieve";
    case TaskActionKind::CompileSource: return "compile-source";
    case TaskActionKind::GenerateCode: return "generate-code";
    case TaskActionKind::ExecuteCode: return "execute-code";
    case TaskActionKind::Explain: return "explain";
    case TaskActionKind::Visualize: return "visualize";
    case TaskActionKind::RequestPublication: return "request-publication";
    }
    throw Error(ErrorCode::Schema, "Unknown Task IR action kind");
}

TaskPlanContext::TaskPlanContext(ProblemSourceCatalog sources, std::size_t current_question,
    std::map<std::string, TaskObject> objects, std::vector<GoalPredicate> active, TaskPlanLimits limits)
    : sources_(std::move(sources)), current_question_(current_question), objects_(std::move(objects)),
      active_(std::move(active)), limits_(limits) {
    require(current_question_ < sources_.context().views.size()
        && sources_.context().views[current_question_].basis == PremiseBasis::Question
        && !sources_.context().views[current_question_].text.empty(), ErrorCode::Schema, "Missing explicit current question");
    require(limits_.wire_bytes > 0 && limits_.wire_bytes <= 1024 * 1024
        && limits_.entities > 0 && limits_.entities <= 128 && limits_.goals > 0 && limits_.goals <= 128
        && limits_.actions > 0 && limits_.actions <= 128 && limits_.planner.nodes > 0 && limits_.planner.nodes <= 256
        && limits_.planner.edges > 0 && limits_.planner.edges <= 4096 && objects_.size() <= 256,
        ErrorCode::Budget, "Invalid Task IR context bounds");
    validate_goal_predicates(active_);
    require(active_.size() <= 128, ErrorCode::Budget, "Too many active host constraints");
    const auto source = serialize_problem_context(sources_.context());
    require(source.size() <= 4 * 1024 * 1024, ErrorCode::Budget, "Task IR context exceeds byte budget");
    auto wire = array();
    append(wire.get(), text_json("gammathink.task-ir-context.v1"));
    append(wire.get(), text_json(source));
    auto source_limits = array();
    append(source_limits.get(), allocated(json_object_new_uint64(sources_.limits().work)));
    append(source_limits.get(), allocated(json_object_new_uint64(sources_.limits().steps)));
    append(wire.get(), source_limits.release());
    append(wire.get(), allocated(json_object_new_uint64(current_question_)));
    auto handles = array();
    for (const auto& [handle, value] : objects_) {
        validate_text(handle, 128); validate_text(value.format, 256);
        require(!handle.empty() && !value.format.empty() && value.kind >= ObjectKind::Claim
            && value.kind <= ObjectKind::Contradiction, ErrorCode::Schema, "Invalid advertised task object");
        auto row = array(); append(row.get(), text_json(handle)); append(row.get(), text_json(hex(value.id)));
        append(row.get(), allocated(json_object_new_int(static_cast<int>(value.kind))));
        append(row.get(), text_json(value.format)); append(handles.get(), row.release());
    }
    append(wire.get(), handles.release());
    append(wire.get(), predicate_json(active_).release());
    auto bounds = array();
    for (const auto value : {limits_.wire_bytes, limits_.entities, limits_.goals, limits_.actions,
                             limits_.planner.nodes, limits_.planner.edges})
        append(bounds.get(), allocated(json_object_new_uint64(value)));
    append(wire.get(), bounds.release());
    wire_ = dump(wire.get());
    require(wire_.size() <= 4 * 1024 * 1024, ErrorCode::Budget, "Task IR context exceeds byte budget");
    snapshot_ = "task-ir-context-v1:" + hex(ObjectId{digest(std::span(
        reinterpret_cast<const std::uint8_t*>(wire_.data()), wire_.size()))});
}
TaskPlanContext restore_task_plan_context(std::string_view wire) try {
    require(!wire.empty() && wire.size() <= 4 * 1024 * 1024, ErrorCode::Budget, "Task IR context exceeds byte budget");
    const auto root = parse(wire); tuple(root.get(), 7);
    require(text(at(root.get(), 0), 64) == "gammathink.task-ir-context.v1", ErrorCode::Schema, "Unknown Task IR context profile");
    const auto context = parse_problem_context(text(at(root.get(), 1), 4 * 1024 * 1024));
    auto* native = at(root.get(), 2); tuple(native, 2);
    ProblemLimits source_limits{number(at(native, 0)), static_cast<std::uint32_t>(number(at(native, 1), UINT32_MAX))};
    const auto current = number(at(root.get(), 3));
    std::map<std::string, TaskObject> objects;
    auto* rows = at(root.get(), 4);
    for (std::size_t i = 0, size = count(rows, 256); i < size; ++i) {
        auto* row = at(rows, i); tuple(row, 4);
        const auto handle = text(at(row, 0), 128);
        const auto kind = number(at(row, 2), 12);
        require(kind > 0 && objects.emplace(handle, TaskObject{parse_id(text(at(row, 1), 64)),
            static_cast<ObjectKind>(kind), text(at(row, 3), 256)}).second,
            ErrorCode::Schema, "Duplicate or invalid advertised task object");
    }
    auto* bounds = at(root.get(), 6); tuple(bounds, 6);
    TaskPlanLimits limits{number(at(bounds, 0)), number(at(bounds, 1)), number(at(bounds, 2)),
        number(at(bounds, 3)), {number(at(bounds, 4)), number(at(bounds, 5))}};
    TaskPlanContext result(ProblemSourceCatalog(context, source_limits), current, std::move(objects),
        predicates(at(root.get(), 5)), limits);
    require(result.wire() == wire, ErrorCode::Integrity, "Stored Task IR context is not canonical"); return result;
} catch (const Error&) {
    throw;
} catch (const std::runtime_error& error) {
    throw Error(ErrorCode::Schema, error.what());
}

GoalConditions parse_task_environment(std::string_view wire) {
    require(!wire.empty() && wire.size() <= 1024 * 1024, ErrorCode::Budget, "Task environment exceeds byte budget");
    const auto values = parse(wire);
    GoalConditions result;
    for (std::size_t i = 0, size = count(values.get(), maximum_items); i < size; ++i) {
        auto* row = at(values.get(), i); tuple(row, 2);
        const auto key = text(at(row, 0), 128);
        require(!key.empty() && result.emplace(key, scalar(at(row, 1))).second,
            ErrorCode::Schema, "Empty or duplicate task environment field");
    }
    return result;
}

AdmittedTaskPlan::AdmittedTaskPlan(std::string snapshot, std::string dsl, std::string context,
    std::vector<TaskEntity> entities, std::vector<TaskAction> actions, std::vector<GoalIR> definitions,
    GoalGraph graph, std::vector<GoalPredicate> active, std::map<std::string, std::string> artifact_goals)
    : snapshot_(std::move(snapshot)), dsl_(std::move(dsl)), context_wire_(std::move(context)),
      entities_(std::move(entities)), actions_(std::move(actions)), definitions_(std::move(definitions)),
      graph_(std::move(graph)), active_(std::move(active)), artifact_goals_(std::move(artifact_goals)) {}

TaskPlanInterpretation admit_task_plan(const TaskPlanContext& context, std::string_view snapshot, std::string_view dsl) {
    require(snapshot == context.snapshot(), ErrorCode::Integrity, "Task IR input snapshot changed");
    require(!dsl.empty() && dsl.size() <= context.limits().wire_bytes, ErrorCode::Budget, "Task IR exceeds wire budget");
    validate_text(dsl, context.limits().wire_bytes);
    std::vector<TaskEntity> entities;
    std::vector<TaskAction> actions;
    std::vector<GoalIR> definitions;
    std::map<std::string, std::size_t> entity_index, goal_index;
    std::map<std::string, std::vector<EvidenceSpan>> goal_spans;
    std::set<std::string> action_ids;
    std::map<std::string, std::vector<std::string>> producers;
    std::optional<TaskPlanDecline> decline;
    std::string root;
    bool header = false, captured = false, ended = false;
    std::size_t edges = 0;
    std::istringstream lines{std::string(dsl)};
    std::string line;
    while (std::getline(lines, line)) {
        const auto row = trim(line);
        if (row.empty()) continue;
        require(!ended, ErrorCode::Schema, "Task IR contains content after END");
        if (!header) { require(row == "GAMMATHINK_TASK_IR 1", ErrorCode::Schema, "Unknown Task IR header"); header = true; continue; }
        if (row == "END") { ended = true; continue; }
        const auto separator = row.find_first_of(" \t");
        require(separator != std::string_view::npos, ErrorCode::Schema, "Task IR record requires JSON");
        const auto tag = row.substr(0, separator);
        const auto value = parse(trim(row.substr(separator)));
        if (!captured) {
            require(tag == "CONTEXT" && text(value.get(), 128) == snapshot, ErrorCode::Integrity, "Task IR context echo differs");
            captured = true; continue;
        }
        require(tag != "CONTEXT", ErrorCode::Schema, "Repeated Task IR context");
        if (tag == "CLARIFY" || tag == "UNSUPPORTED") {
            tuple(value.get(), 2);
            require(!decline && entities.empty() && definitions.empty() && actions.empty() && root.empty(),
                ErrorCode::Schema, "Task IR decline cannot mix executable work");
            auto reason = text(at(value.get(), 0));
            require(!reason.empty(), ErrorCode::Schema, "Empty Task IR decline reason");
            decline = TaskPlanDecline{tag == "CLARIFY" ? TaskDeclineKind::Clarify : TaskDeclineKind::Unsupported,
                std::move(reason), std::string(snapshot), std::string(dsl), supports(at(value.get(), 1), context, true)};
            continue;
        }
        require(!decline, ErrorCode::Schema, "Task IR decline cannot mix executable work");
        if (tag == "ROOT") {
            require(root.empty(), ErrorCode::Schema, "Repeated Task IR root"); root = id(value.get());
        } else if (tag == "ENTITY") {
            tuple(value.get(), 4);
            require(entities.size() < context.limits().entities, ErrorCode::Budget, "Task IR entity budget exceeded");
            TaskEntity entity{id(at(value.get(), 0)), TaskArtifact{}, supports(at(value.get(), 3), context)};
            require(entity_index.emplace(entity.id, entities.size()).second, ErrorCode::Schema, "Duplicate Task IR entity");
            const auto kind = text(at(value.get(), 1), 32);
            auto* payload = at(value.get(), 2);
            if (kind == "text") {
                const auto selected = span(payload, context);
                entity.value = TaskText{selected, context.sources().context().views.at(selected.view).text.substr(selected.offset, selected.length)};
            } else if (kind == "object") entity.value = object(payload, context);
            else if (kind == "artifact") require(payload == nullptr, ErrorCode::Schema, "Unproduced artifacts cannot carry values");
            else if (kind == "problem") {
                auto admitted = admit_task_dsl(context.sources(), context.sources().snapshot(), text(payload, context.limits().wire_bytes));
                require(std::holds_alternative<AdmittedTask>(admitted), ErrorCode::Schema, "Problem entity is not admitted requested work");
                entity.value = std::get<AdmittedTask>(std::move(admitted));
                (void)supports(at(value.get(), 3), context, true);
            } else if (kind == "query") entity.value = query(payload, context);
            else if (kind == "source-query") {
                tuple(payload, 2); const auto& module = object(at(payload, 0), context);
                require(module.kind == ObjectKind::Claim &&
                    (module.format == "gammathink.archive-dsl.v1" || module.format == "gammathink.archive-dsl.v2"),
                    ErrorCode::Schema, "Source query requires an advertised archive module");
                auto* goal = at(payload, 1);
                if (count(goal, 3) == 3 && text(at(goal, 0), 16) == "q") {
                    entity.value = TaskSourceQuery{module.id, ProblemQuantityGoal{
                        static_cast<std::uint32_t>(number(at(goal, 1), UINT32_MAX)),
                        static_cast<std::uint32_t>(number(at(goal, 2), UINT32_MAX))}};
                } else entity.value = TaskSourceQuery{module.id, ProblemAtomGoal{atom(goal)}};
            } else throw Error(ErrorCode::Schema, "Unknown Task IR entity kind");
            entities.push_back(std::move(entity));
        } else if (tag == "GOAL") {
            tuple(value.get(), 6);
            require(definitions.size() < context.limits().goals, ErrorCode::Budget, "Task IR goal budget exceeded");
            GoalIR goal; goal.id = id(at(value.get(), 0));
            require(goal_index.emplace(goal.id, definitions.size()).second, ErrorCode::Schema, "Duplicate Task IR goal");
            auto* methods = at(value.get(), 1);
            for (std::size_t i = 0, size = count(methods, context.limits().planner.edges); i < size; ++i) {
                auto method = ids(at(methods, i), context.limits().planner.edges);
                require(!method.empty() && method.size() <= context.limits().planner.edges - edges,
                    ErrorCode::Budget, "Empty method or Task IR edge budget exceeded");
                edges += method.size(); goal.methods.push_back(std::move(method));
            }
            goal.requirements = predicates(at(value.get(), 2));
            goal.success_requirements = predicates(at(value.get(), 3));
            auto* assumptions = at(value.get(), 4);
            for (std::size_t i = 0, size = count(assumptions, 128); i < size; ++i) {
                const auto& assumption = object(at(assumptions, i), context);
                require(assumption.kind == ObjectKind::Assumption, ErrorCode::Schema, "Goal assumption requires an advertised assumption");
                goal.assumptions.push_back(assumption.id);
            }
            auto selected = supports(at(value.get(), 5), context);
            goal.sources = references(context.sources().context(), selected);
            goal_spans.emplace(goal.id, std::move(selected));
            definitions.push_back(std::move(goal));
        } else if (tag == "ACTION") {
            tuple(value.get(), 6);
            require(actions.size() < context.limits().actions, ErrorCode::Budget, "Task IR action budget exceeded");
            TaskAction action{id(at(value.get(), 0)), id(at(value.get(), 1)), action_kind(at(value.get(), 2)),
                ids(at(value.get(), 3), 16), ids(at(value.get(), 4), 1), supports(at(value.get(), 5), context, true)};
            require(action_ids.insert(action.id).second && action.outputs.size() == 1,
                ErrorCode::Schema, "Duplicate action or missing single artifact output");
            actions.push_back(std::move(action));
        } else throw Error(ErrorCode::Schema, "Unknown Task IR record");
    }
    require(header && captured && ended, ErrorCode::Schema, "Incomplete Task IR envelope");
    if (decline) return *decline;
    require(!root.empty() && goal_index.contains(root) && !actions.empty(), ErrorCode::Schema, "Missing executable Task IR root");
    const auto& root_spans = goal_spans.at(root);
    require(std::any_of(root_spans.begin(), root_spans.end(), [&](const auto& source) {
        return source.view == context.current_question();
    }), ErrorCode::Schema, "Task IR root must cite the current question");
    for (const auto& definition : definitions) for (const auto& method : definition.methods)
        for (const auto& child : method) require(goal_index.contains(child), ErrorCode::Schema, "Unknown Task IR dependency goal");
    std::set<std::string> action_goals;
    for (const auto& action : actions) {
        require(goal_index.contains(action.goal) && action_goals.insert(action.goal).second,
            ErrorCode::Schema, "Unknown goal or more than one action per goal");
        for (const auto& input : action.inputs) require(entity_index.contains(input), ErrorCode::Schema, "Unknown action input entity");
        for (const auto& output : action.outputs) {
            require(entity_index.contains(output)
                && std::holds_alternative<TaskArtifact>(entities[entity_index.at(output)].value),
                ErrorCode::Schema, "Action output must be an unproduced artifact");
            producers[output].push_back(action.goal);
        }
        require(!action.inputs.empty(), ErrorCode::Schema, "Action requires typed input");
        const auto& first = entities[entity_index.at(action.inputs.front())].value;
        const bool deferred = std::holds_alternative<TaskArtifact>(first);
        bool valid = false;
        switch (action.kind) {
        case TaskActionKind::Reason:
            valid = action.inputs.size() == 1 && (std::holds_alternative<AdmittedTask>(first)
                || (std::holds_alternative<TaskObject>(first) && std::get<TaskObject>(first).kind == ObjectKind::Goal
                    && std::get<TaskObject>(first).format == native_request_format)); break;
        case TaskActionKind::SourceReason: valid = action.inputs.size() == 1 && std::holds_alternative<TaskSourceQuery>(first); break;
        case TaskActionKind::Retrieve: valid = action.inputs.size() == 1 && std::holds_alternative<KnowledgeQuery>(first); break;
        case TaskActionKind::CompileSource:
            valid = action.inputs.size() == 1 && (deferred || (std::holds_alternative<TaskObject>(first)
                && std::get<TaskObject>(first).kind == ObjectKind::Evidence
                && std::get<TaskObject>(first).format.starts_with("source.zim.raw.v1:"))); break;
        case TaskActionKind::GenerateCode: valid = action.inputs.size() == 1 && std::holds_alternative<TaskText>(first); break;
        case TaskActionKind::ExecuteCode: valid = action.inputs.size() <= 2 && (deferred
            || std::holds_alternative<TaskText>(first) || std::holds_alternative<TaskObject>(first))
            && (action.inputs.size() == 1 || std::holds_alternative<TaskText>(
                entities[entity_index.at(action.inputs[1])].value)); break;
        case TaskActionKind::Explain:
            valid = action.inputs.size() <= 2 && (deferred || std::holds_alternative<TaskObject>(first))
                && (action.inputs.size() == 1 || std::holds_alternative<TaskText>(entities[entity_index.at(action.inputs[1])].value)); break;
        case TaskActionKind::Visualize: valid = action.inputs.size() == 1 && (deferred || std::holds_alternative<TaskObject>(first)); break;
        case TaskActionKind::RequestPublication:
            valid = std::all_of(action.inputs.begin(), action.inputs.end(), [&](const auto& input) {
                const auto& value = entities[entity_index.at(input)].value;
                return std::holds_alternative<TaskObject>(value) || std::holds_alternative<TaskArtifact>(value);
            }); break;
        }
        require(valid, ErrorCode::Schema, "Action input types differ from its declared operation");
    }
    std::map<std::string, std::string> artifact_goals;
    std::set<std::string> consumed;
    for (const auto& action : actions) for (const auto& input : action.inputs) consumed.insert(input);
    for (const auto& entity : entities) if (std::holds_alternative<TaskArtifact>(entity.value)) {
        require(producers.contains(entity.id), ErrorCode::Schema, "Artifact entity has no declared producer");
        if (!consumed.contains(entity.id)) continue;
        GoalIR artifact; artifact.id = "__task_artifact_" + entity.id;
        artifact.sources = references(context.sources().context(), entity.supports);
        for (const auto& producer : producers.at(entity.id)) {
            require(edges < context.limits().planner.edges, ErrorCode::Budget, "Task IR dataflow edge budget exceeded");
            ++edges; artifact.methods.push_back({producer});
        }
        artifact.success_requirements = {{"artifact_present", GoalComparison::Equal, true}};
        artifact_goals.emplace(entity.id, artifact.id);
        definitions.push_back(std::move(artifact));
    }
    for (const auto& action : actions) {
        auto& goal = definitions[goal_index.at(action.goal)];
        goal.success_requirements.push_back({"__task_outputs_present", GoalComparison::Equal, true});
        std::set<std::string> needed;
        for (const auto& input : action.inputs) if (artifact_goals.contains(input)) needed.insert(artifact_goals.at(input));
        if (needed.empty()) continue;
        if (goal.methods.empty()) goal.methods.emplace_back();
        for (auto& method : goal.methods) for (const auto& dependency : needed) {
            require(edges < context.limits().planner.edges, ErrorCode::Budget, "Task IR dataflow edge budget exceeded");
            ++edges; method.push_back(dependency);
        }
    }
    const auto graph = decompose_goals(root, definitions, context.limits().planner);
    require(graph.nodes.size() == definitions.size(), ErrorCode::Schema, "Task IR contains unreachable work");
    return AdmittedTaskPlan(std::string(snapshot), std::string(dsl), context.wire(), std::move(entities),
        std::move(actions), std::move(definitions), graph, context.active_constraints(), std::move(artifact_goals));
}

TaskPlanEvaluation evaluate_task_plan(const TaskPlanContext& current_context, const AdmittedTaskPlan& plan,
    const std::map<std::string, GoalOutcome>& verified_outcomes, const GoalConditions& environment,
    const std::map<std::string, ObjectId>& artifacts, const std::set<ObjectId>& accepted_assumptions) {
    require(current_context.snapshot() == plan.snapshot(), ErrorCode::Integrity, "Task IR evaluation context changed");
    auto outcomes = verified_outcomes;
    for (const auto& [id, object] : artifacts) {
        (void)object;
        require(std::any_of(plan.entities().begin(), plan.entities().end(), [&](const auto& entity) {
            return entity.id == id && std::holds_alternative<TaskArtifact>(entity.value);
        }), ErrorCode::Schema, "Produced artifact is not declared");
    }
    for (const auto& action : plan.actions()) {
        const auto found = outcomes.find(action.goal);
        if (found == outcomes.end()) continue;
        found->second.observed.erase("__task_outputs_present");
        if (std::all_of(action.outputs.begin(), action.outputs.end(),
            [&](const auto& output) { return artifacts.contains(output); }))
            found->second.observed.emplace("__task_outputs_present", true);
    }
    auto runtime_graph = plan.graph();
    std::map<std::string, std::size_t> nodes;
    for (std::size_t i = 0; i < runtime_graph.nodes.size(); ++i) nodes.emplace(runtime_graph.nodes[i].id, i);
    for (const auto& [entity, goal] : plan.artifact_goals()) {
        outcomes.erase(goal);
        const bool verified_producer = std::any_of(plan.actions().begin(), plan.actions().end(), [&](const auto& action) {
            const auto producer = outcomes.find(action.goal);
            return producer != outcomes.end() && producer->second.verified &&
                std::find(action.outputs.begin(), action.outputs.end(), entity) != action.outputs.end();
        });
        if (artifacts.contains(entity) && verified_producer) {
            outcomes.emplace(goal, GoalOutcome{true, true, {{"artifact_present", true}}, {}});
            // The verified operation completed even if its requested success
            // condition failed. Add this event only to the evaluation copy:
            // canonical planning/cycle edges and stored manifest IDs stay fixed.
            auto& availability = runtime_graph.nodes.at(nodes.at(goal));
            availability.definition->methods.emplace_back();
            availability.methods.emplace_back();
        }
    }
    TaskPlanEvaluation result;
    result.goals = evaluate_goals(runtime_graph, outcomes, environment, accepted_assumptions, plan.active_constraints());
    for (const auto& action : plan.actions()) {
        const bool missing = std::any_of(action.inputs.begin(), action.inputs.end(), [&](const auto& input) {
            return plan.artifact_goals().contains(input) && !artifacts.contains(input);
        });
        for (const auto& input : action.inputs) if (plan.artifact_goals().contains(input) && !artifacts.contains(input)
            && std::find(result.missing_artifacts.begin(), result.missing_artifacts.end(), input) == result.missing_artifacts.end())
            result.missing_artifacts.push_back(input);
        const bool produced = std::all_of(action.outputs.begin(), action.outputs.end(), [&](const auto& output) { return artifacts.contains(output); });
        if (!missing && !produced && result.goals.states.at(nodes.at(action.goal)) == GoalState::Ready) result.ready_actions.push_back(action.id);
    }
    return result;
}

namespace {
struct TaskPlanRecords {
    PersistedTaskPlan ids;
    std::vector<Object> objects;
};
TaskPlanRecords task_plan_records(const AdmittedTaskPlan& plan) {
    // Re-admit the complete immutable capture before any knowledge write.
    const auto context = restore_task_plan_context(plan.context_wire());
    const auto checked = admit_task_plan(context, plan.snapshot(), plan.dsl());
    require(std::holds_alternative<AdmittedTaskPlan>(checked), ErrorCode::Integrity, "Stored Task IR is not executable intent");
    TaskPlanRecords materialized;
    auto& result = materialized.ids;
    const auto put = [&](ObjectKind kind, std::string format, std::string payload,
                         std::vector<Link> links, std::vector<SourceReference> sources) {
        Object object; object.kind = kind; object.format = std::move(format);
        object.payload.assign(payload.begin(), payload.end()); object.links = std::move(links);
        object.sources = std::move(sources);
        const auto id = identify(object); materialized.objects.push_back(std::move(object)); return id;
    };
    result.context = put(ObjectKind::Assumption, "gammathink.task-ir-context.v1", plan.context_wire(), {}, {});
    result.interpretation = put(ObjectKind::Action, "gammathink.task-ir.v1", plan.dsl(),
        {{Relationship::DependsOn, result.context}}, {});
    for (const auto& entity : plan.entities()) {
        auto row = array(); append(row.get(), text_json(entity.id));
        std::vector<Link> links{{Relationship::DependsOn, result.interpretation}};
        if (const auto* object = std::get_if<TaskObject>(&entity.value)) links.push_back({Relationship::DependsOn, object->id});
        if (const auto* source = std::get_if<TaskSourceQuery>(&entity.value)) links.push_back({Relationship::DependsOn, source->module});
        if (const auto* query = std::get_if<KnowledgeQuery>(&entity.value);
            query && (query->kind == QueryKind::Related || query->kind == QueryKind::RequiredBy))
            links.push_back({Relationship::DependsOn, query->target});
        result.entities.push_back(put(ObjectKind::Assumption, "gammathink.task-ir.entity.v1", dump(row.get()),
            std::move(links), references(context.sources().context(), entity.supports)));
    }
    for (const auto& goal : plan.definitions()) {
        auto row = array(); append(row.get(), text_json(goal.id));
        std::vector<Link> links{{Relationship::DependsOn, result.interpretation}};
        for (const auto& assumption : goal.assumptions) links.push_back({Relationship::DependsOn, assumption});
        result.goals.push_back(put(ObjectKind::Goal, "gammathink.task-ir.goal.v1", dump(row.get()),
            std::move(links), goal.sources));
    }
    for (const auto& action : plan.actions()) {
        auto row = array(); append(row.get(), text_json(action.id)); append(row.get(), text_json(action.goal));
        append(row.get(), text_json(name(action.kind)));
        result.actions.push_back(put(ObjectKind::Action, "gammathink.task-ir.action.v1", dump(row.get()),
            {{Relationship::DependsOn, result.interpretation}}, references(context.sources().context(), action.supports)));
    }
    auto manifest = array(); append(manifest.get(), text_json(plan.snapshot()));
    append(manifest.get(), text_json(plan.graph().nodes.at(plan.graph().root).id));
    std::vector<Link> links{{Relationship::DependsOn, result.context}, {Relationship::DependsOn, result.interpretation}};
    for (const auto* group : {&result.entities, &result.goals, &result.actions}) {
        auto values = array();
        for (const auto& id : *group) { append(values.get(), text_json(hex(id))); links.push_back({Relationship::DependsOn, id}); }
        append(manifest.get(), values.release());
    }
    result.manifest = put(ObjectKind::Goal, "gammathink.task-ir.manifest.v1", dump(manifest.get()), std::move(links), {});
    return materialized;
}
}
PersistedTaskPlan persist_task_plan(Journal& journal, const AdmittedTaskPlan& plan) {
    const auto records = task_plan_records(plan);
    for (const auto& object : records.objects) journal.put(object);
    return records.ids;
}
AdmittedTaskPlan restore_task_plan(Journal& journal, const ObjectId& id) {
    std::optional<Object> manifest, context_record, interpretation;
    journal.read_snapshot([&] {
        manifest = journal.get(id);
        require(manifest && manifest->kind == ObjectKind::Goal && manifest->format == "gammathink.task-ir.manifest.v1"
            && manifest->links.size() >= 2, ErrorCode::Integrity, "Invalid Task IR manifest");
        context_record = journal.get(manifest->links[0].target);
        interpretation = journal.get(manifest->links[1].target);
        require(context_record && context_record->kind == ObjectKind::Assumption
            && context_record->format == "gammathink.task-ir-context.v1" && interpretation
            && interpretation->kind == ObjectKind::Action && interpretation->format == "gammathink.task-ir.v1",
            ErrorCode::Integrity, "Task IR capture dependencies differ");
    });
    const auto context = restore_task_plan_context(std::string(context_record->payload.begin(), context_record->payload.end()));
    auto checked = admit_task_plan(context, context.snapshot(), std::string(interpretation->payload.begin(), interpretation->payload.end()));
    require(std::holds_alternative<AdmittedTaskPlan>(checked), ErrorCode::Integrity, "Stored Task IR is not executable intent");
    auto result = std::get<AdmittedTaskPlan>(std::move(checked));
    const auto expected = task_plan_records(result);
    require(expected.ids.manifest == id, ErrorCode::Integrity, "Task IR manifest identities differ");
    journal.read_snapshot([&] {
        for (const auto& object : expected.objects) {
            const auto stored = journal.get(identify(object));
            require(stored && *stored == object, ErrorCode::Integrity, "Task IR record is missing or differs");
        }
    });
    return result;
}

} // namespace axiom::knowledge
