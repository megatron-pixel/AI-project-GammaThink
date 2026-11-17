#include "problem_signed_plan.hpp"
#include "problem_semantic.hpp"

#include <json-c/json.h>

#include <cctype>
#include <memory>
#include <set>
#include <stdexcept>

namespace axiom {
namespace {
using J = json_object*;
using Json = std::unique_ptr<json_object, decltype(&json_object_put)>;
void require(bool condition, const char* message) {
    if (!condition) throw std::invalid_argument(message);
}
std::string dump(J value) {
    return json_object_to_json_string_ext(value,JSON_C_TO_STRING_PLAIN);
}
J text(const std::string& value) {
    return json_object_new_string_len(value.data(),static_cast<int>(value.size()));
}
J array(std::initializer_list<J> values) {
    auto result = json_object_new_array();
    for (const auto value : values) json_object_array_add(result,value);
    return result;
}
J field(J value, const char* key) {
    J result = nullptr;
    require(json_object_get_type(value) == json_type_object
        && json_object_object_get_ex(value,key,&result),"Missing signed plan field");
    return result;
}
void fields(J value, std::initializer_list<const char*> keys) {
    require(json_object_get_type(value) == json_type_object
        && static_cast<std::size_t>(json_object_object_length(value)) == keys.size(),
        "Unexpected signed plan fields");
    for (const auto key : keys) (void)field(value,key);
}
std::string string(J value) {
    require(json_object_get_type(value) == json_type_string,"Expected signed plan text");
    std::string result(json_object_get_string(value),json_object_get_string_len(value));
    require(!result.empty() && result.size() <= 4096 && result.find('\0') == std::string::npos,
        "Invalid signed plan text");
    return result;
}
std::size_t count(J value, std::size_t maximum, std::size_t minimum = 0) {
    require(json_object_get_type(value) == json_type_array,"Expected signed plan array");
    const auto size = json_object_array_length(value);
    require(size >= minimum && size <= maximum,"Signed plan array bound exceeded");
    return size;
}
J at(J value, std::size_t index) { return json_object_array_get_idx(value,index); }
std::size_t integer(J value, std::size_t maximum) {
    require(json_object_get_type(value) == json_type_int
        && json_object_get_int64(value) >= 0
        && static_cast<std::uint64_t>(json_object_get_int64(value)) <= maximum,
        "Invalid signed plan integer or reference");
    return static_cast<std::size_t>(json_object_get_int64(value));
}
std::size_t ref(J value, std::size_t size) {
    require(size > 0,"Reference to empty signed plan declaration list");
    return integer(value,size-1);
}

// json-c accepts repeated keys. Scan the already syntax-checked input to reject
// them, including escape-equivalent spellings, before interpreting any fields.
class UniqueKeys {
    const std::string& input;
    std::size_t pos = 0;
    void space() {
        while (pos < input.size() && std::isspace(static_cast<unsigned char>(input[pos]))) ++pos;
    }
    std::string quoted() {
        const auto start = pos++;
        while (pos < input.size()) {
            const char c = input[pos++];
            if (c == '\\') ++pos;
            else if (c == '"') break;
        }
        Json value(json_tokener_parse(input.substr(start,pos-start).c_str()),json_object_put);
        std::string decoded(json_object_get_string(value.get()),json_object_get_string_len(value.get()));
        require(decoded.find('\0') == std::string::npos,"NUL in signed plan JSON string");
        return decoded;
    }
    void value() {
        space();
        if (input[pos] == '{') {
            ++pos; space();
            std::set<std::string> keys;
            if (input[pos] != '}') while (true) {
                space();
                require(keys.insert(quoted()).second,"Repeated signed plan JSON field");
                space(); ++pos;
                value(); space();
                if (input[pos] != ',') break;
                ++pos;
            }
            ++pos;
        } else if (input[pos] == '[') {
            ++pos; space();
            if (input[pos] != ']') while (true) {
                value(); space();
                if (input[pos] != ',') break;
                ++pos;
            }
            ++pos;
        } else if (input[pos] == '"') (void)quoted();
        else while (pos < input.size() && input[pos] != ',' && input[pos] != ']'
            && input[pos] != '}' && !std::isspace(static_cast<unsigned char>(input[pos]))) ++pos;
    }
public:
    explicit UniqueKeys(const std::string& source) : input(source) {}
    void check() { value(); }
};
Json parse(const std::string& input) {
    require(!input.empty() && input.size() <= 1048576 && input.find('\0') == std::string::npos,
        "Invalid signed plan JSON size");
    const auto tokener = std::unique_ptr<json_tokener, decltype(&json_tokener_free)>(
        json_tokener_new_ex(48),json_tokener_free);
    json_tokener_set_flags(tokener.get(),JSON_TOKENER_STRICT | JSON_TOKENER_VALIDATE_UTF8);
    // Include the terminating NUL so scalar policy selections also finish.
    Json result(json_tokener_parse_ex(tokener.get(),input.c_str(),static_cast<int>(input.size()+1)),
        json_object_put);
    require(json_tokener_get_error(tokener.get()) == json_tokener_success,"Malformed signed plan JSON");
    for (auto i = json_tokener_get_parse_end(tokener.get()); i < input.size(); ++i)
        require(input[i] == ' ' || input[i] == '\t' || input[i] == '\r' || input[i] == '\n',
            "Trailing signed plan JSON");
    UniqueKeys(input).check();
    return result;
}

class Lowerer {
    const ProblemSourceCatalog& catalog;
    J plan, declarations, assertions;
    std::vector<std::size_t> arities;
    std::size_t entities = 0, scopes = 0;
    EvidenceSpan span(J value) const {
        if (json_object_get_type(value) == json_type_string)
            return catalog.resolve(catalog.snapshot(),string(value));
        require(count(value,2) == 2,"Invalid signed plan span range");
        return catalog.resolve(catalog.snapshot(),string(at(value,0)),string(at(value,1)));
    }
    std::string label(J value) const {
        const auto resolved = span(value);
        return catalog.context().views.at(resolved.view).text.substr(resolved.offset,resolved.length);
    }
    J supports(J owner, bool question = false) const {
        const auto values = field(owner,"supports");
        for (std::size_t i = 0, n = count(values,16,1); i < n; ++i) {
            const auto resolved = span(at(values,i));
            require(!question || catalog.context().views.at(resolved.view).basis == PremiseBasis::Question,
                "Signed plan goal supports must originate in question");
        }
        return json_object_get(values);
    }
    J list(const char* key) const { return field(declarations,key); }
    J atom(J value, std::size_t variables, std::vector<bool>* used = nullptr) const {
        fields(value,{"predicate","scope","negative","terms"});
        const auto predicate = ref(field(value,"predicate"),arities.size());
        const auto scope = ref(field(value,"scope"),scopes);
        const auto negative = field(value,"negative");
        require(json_object_get_type(negative) == json_type_boolean,"Invalid signed plan sign");
        const auto terms = field(value,"terms");
        require(count(terms,4) == arities[predicate],"Signed plan atom arity mismatch");
        Json lowered(json_object_new_array(),json_object_put);
        for (std::size_t i = 0; i < arities[predicate]; ++i) {
            const auto term = at(terms,i);
            fields(term,{"kind","ref"});
            const auto kind = string(field(term,"kind"));
            require(kind == "constant" || kind == "variable","Invalid signed plan term kind");
            const bool variable = kind == "variable";
            const auto index = ref(field(term,"ref"),variable ? variables : entities);
            if (variable && used) (*used)[index] = true;
            json_object_array_add(lowered.get(),array({text(variable ? "v" : "c"),
                text(std::string(variable ? "v" : "e")+std::to_string(index))}));
        }
        return array({text("p"+std::to_string(predicate)),text("s"+std::to_string(scope)),
            json_object_get(negative),lowered.release()});
    }
public:
    Lowerer(const ProblemSourceCatalog& source, J root, J decl, J assertion)
        : catalog(source), plan(root), declarations(decl), assertions(assertion) {}
    void run(bool complete) {
        fields(plan,{"snapshot","entities","predicates","scopes","clauses","goals"});
        require(string(field(plan,"snapshot")) == catalog.snapshot(),"Stale signed plan snapshot");
        const auto es = field(plan,"entities");
        entities = count(es,64,complete ? 1 : 0);
        for (std::size_t i = 0; i < entities; ++i) {
            const auto item = at(es,i);
            fields(item,{"anchor","supports"});
            json_object_array_add(list("entities"),array({text("e"+std::to_string(i)),
                text(label(field(item,"anchor"))),supports(item)}));
        }
        const auto ps = field(plan,"predicates");
        for (std::size_t i = 0, n = count(ps,64,complete ? 1 : 0); i < n; ++i) {
            const auto item = at(ps,i);
            fields(item,{"anchor","arity","supports"});
            arities.push_back(integer(field(item,"arity"),4));
            json_object_array_add(list("predicates"),array({text("p"+std::to_string(i)),
                text(label(field(item,"anchor"))),json_object_get(field(item,"arity")),supports(item)}));
        }
        const auto ss = field(plan,"scopes");
        scopes = count(ss,64,complete ? 1 : 0);
        for (std::size_t i = 0; i < scopes; ++i) {
            const auto item = at(ss,i);
            fields(item,{"kind","qualifiers","supports"});
            Json evidence(supports(item),json_object_put);
            const auto kind = string(field(item,"kind"));
            require(kind == "global" || kind == "qualified" || kind == "unresolved",
                "Invalid signed plan scope kind");
            const auto qs = field(item,"qualifiers");
            const auto n = count(qs,16);
            require(kind == "unresolved" || (kind == "global" ? n == 0 : n > 0),
                "Signed plan scope contradicts qualifiers");
            Json qualifiers(json_object_new_array(),json_object_put);
            for (std::size_t j = 0; j < n; ++j) {
                const auto q = at(qs,j);
                fields(q,{"key","value"});
                // Typed scope keys must be unique. Labels come from anchors;
                // equal labels are not used to coalesce scope declarations.
                json_object_array_add(qualifiers.get(),array({text(label(field(q,"key"))),
                    text(label(field(q,"value"))),json_object_get(evidence.get())}));
            }
            if (kind == "unresolved")
                throw ProblemDecline(ProblemDeclineKind::Clarify,"Unresolved signed plan scope");
            json_object_array_add(list("scopes"),array({text("s"+std::to_string(i)),
                json_object_new_boolean(true),qualifiers.release()}));
        }
        const auto cs = field(plan,"clauses");
        for (std::size_t i = 0, n = count(cs,128); i < n; ++i) {
            const auto item = at(cs,i);
            fields(item,{"variables","head","premises","supports"});
            const auto vs = field(item,"variables");
            const auto size = count(vs,8);
            Json variables(json_object_new_array(),json_object_put);
            for (std::size_t j = 0; j < size; ++j) {
                (void)span(at(vs,j));
                json_object_array_add(variables.get(),text("v"+std::to_string(j)));
            }
            Json head(atom(field(item,"head"),size),json_object_put);
            Json body(json_object_new_array(),json_object_put);
            std::vector<bool> used(size);
            const auto premises = field(item,"premises");
            for (std::size_t j = 0, m = count(premises,16); j < m; ++j)
                json_object_array_add(body.get(),atom(at(premises,j),size,&used));
            for (const auto present : used) require(present,"Unsafe or unused signed plan variable");
            json_object_array_add(field(assertions,"rules"),array({variables.release(),head.release(),
                body.release(),supports(item)}));
        }
        const auto gs = field(plan,"goals");
        for (std::size_t i = 0, n = count(gs,8,complete ? 1 : 0); i < n; ++i) {
            const auto item = at(gs,i);
            fields(item,{"atom","supports"});
            json_object_array_add(field(assertions,"goals"),array({text("atom"),
                atom(field(item,"atom"),0),supports(item,true)}));
        }
    }
};
Json lower(J root, const ProblemSourceCatalog& catalog, bool complete) {
    Json result(json_object_new_object(),json_object_put);
    json_object_object_add(result.get(),"snapshot",text(catalog.snapshot()));
    auto declarations = json_object_new_object(), assertions = json_object_new_object();
    json_object_object_add(result.get(),"declarations",declarations);
    json_object_object_add(result.get(),"assertions",assertions);
    for (const auto key : {"entities","scopes","properties","quantities","numbers","predicates"})
        json_object_object_add(declarations,key,json_object_new_array());
    for (const auto key : {"bindings","nodes","equations","rules","goals"})
        json_object_object_add(assertions,key,json_object_new_array());
    Lowerer(catalog,root,declarations,assertions).run(complete);
    return result;
}

J object_schema(std::initializer_list<std::pair<const char*,J>> properties) {
    auto result = json_object_new_object(), props = json_object_new_object();
    auto required = json_object_new_array();
    for (const auto& [name,value] : properties) {
        json_object_object_add(props,name,value);
        json_object_array_add(required,text(name));
    }
    json_object_object_add(result,"type",text("object"));
    json_object_object_add(result,"additionalProperties",json_object_new_boolean(false));
    json_object_object_add(result,"required",required);
    json_object_object_add(result,"properties",props);
    return result;
}
J reference(const char* name) {
    auto result = json_object_new_object();
    json_object_object_add(result,"$ref",text(std::string("#/$defs/")+name));
    return result;
}
J list_schema(J items, int minimum, int maximum) {
    auto result = json_object_new_object();
    json_object_object_add(result,"type",text("array"));
    json_object_object_add(result,"items",items);
    json_object_object_add(result,"minItems",json_object_new_int(minimum));
    json_object_object_add(result,"maxItems",json_object_new_int(maximum));
    return result;
}
J int_schema(int maximum) {
    auto result = json_object_new_object();
    json_object_object_add(result,"type",text("integer"));
    json_object_object_add(result,"minimum",json_object_new_int(0));
    json_object_object_add(result,"maximum",json_object_new_int(maximum));
    return result;
}
J enum_schema(std::initializer_list<const char*> values) {
    auto result = json_object_new_object(), options = json_object_new_array();
    for (const auto value : values) json_object_array_add(options,text(value));
    json_object_object_add(result,"enum",options);
    return result;
}
J atom_schema(bool ground) {
    auto sign = json_object_new_object();
    json_object_object_add(sign,"type",text("boolean"));
    return object_schema({{"predicate",int_schema(63)},{"scope",int_schema(63)},
        {"negative",sign},{"terms",list_schema(object_schema({
            {"kind",ground ? enum_schema({"constant"}) : enum_schema({"constant","variable"})},
            {"ref",int_schema(63)}}),0,4)}});
}
}

namespace signed_plan_detail {
void validate_json(const std::string& json) { (void)parse(json); }
void validate_prefix(const std::string& json, const ProblemSourceCatalog& catalog) {
    const auto root = parse(json);
    (void)lower(root.get(),catalog,false);
}
}

CompiledProblemBatch compile_problem_signed_plan(const std::string& json,
    const ProblemSourceCatalog& catalog) {
    const auto root = parse(json);
    const auto typed = lower(root.get(),catalog,true);
    auto result = compile_problem_transaction_batch(dump(typed.get()),catalog);
    auto record = parse(result.interpretation_record);
    require(json_object_get_type(record.get()) == json_type_object,"Invalid batch interpretation record");
    json_object_object_add(record.get(),"source_plan",json_object_get(root.get()));
    result.interpretation_record = dump(record.get());
    return result;
}

std::string problem_signed_plan_schema(const ProblemSourceCatalog& catalog) {
    Json handles(json_object_new_object(),json_object_put);
    auto options = json_object_new_array();
    for (std::size_t i = 0; i < catalog.entries().size(); ++i)
        json_object_array_add(options,text(ProblemSourceCatalog::source_handle(i)));
    json_object_object_add(handles.get(),"enum",options);
    Json defs(json_object_new_object(),json_object_put);
    json_object_object_add(defs.get(),"source_handle",handles.release());
    auto span = json_object_new_object();
    json_object_object_add(span,"anyOf",array({reference("source_handle"),
        list_schema(reference("source_handle"),2,2)}));
    json_object_object_add(defs.get(),"span",span);
    json_object_object_add(defs.get(),"supports",list_schema(reference("span"),1,16));
    json_object_object_add(defs.get(),"entity",object_schema({{"anchor",reference("span")},
        {"supports",reference("supports")}}));
    json_object_object_add(defs.get(),"predicate",object_schema({{"anchor",reference("span")},
        {"arity",int_schema(4)},{"supports",reference("supports")}}));
    json_object_object_add(defs.get(),"scope",object_schema({
        {"kind",enum_schema({"global","qualified","unresolved"})},
        {"qualifiers",list_schema(object_schema({{"key",reference("span")},
            {"value",reference("span")}}),0,16)},{"supports",reference("supports")}}));
    json_object_object_add(defs.get(),"atom",atom_schema(false));
    json_object_object_add(defs.get(),"ground_atom",atom_schema(true));
    json_object_object_add(defs.get(),"clause",object_schema({
        {"variables",list_schema(reference("span"),0,8)},{"head",reference("atom")},
        {"premises",list_schema(reference("atom"),0,16)},{"supports",reference("supports")}}));
    json_object_object_add(defs.get(),"goal",object_schema({{"atom",reference("ground_atom")},
        {"supports",reference("supports")}}));
    auto snapshot = json_object_new_object();
    json_object_object_add(snapshot,"const",text(catalog.snapshot()));
    Json root(object_schema({{"snapshot",snapshot},{"entities",list_schema(reference("entity"),1,64)},
        {"predicates",list_schema(reference("predicate"),1,64)},
        {"scopes",list_schema(reference("scope"),1,64)},
        {"clauses",list_schema(reference("clause"),0,128)},
        {"goals",list_schema(reference("goal"),1,8)}}),json_object_put);
    json_object_object_add(root.get(),"$defs",defs.release());
    return dump(root.get());
}

} // namespace axiom
