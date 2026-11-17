#include "problem_semantic.hpp"
#include "semantic.hpp"

#include <json-c/json.h>

#include <algorithm>
#include <limits>
#include <memory>
#include <tuple>

namespace axiom {
namespace {
using Json = std::unique_ptr<json_object, decltype(&json_object_put)>;
using J = json_object*;
void require(bool value, const char* message = "Invalid problem proposal JSON") {
    if (!value) throw std::runtime_error(message);
}
Json parse(const std::string& text) {
    require(!text.empty() && text.size() <= 262144 && text.find('\0') == std::string::npos);
    const auto token = std::unique_ptr<json_tokener, decltype(&json_tokener_free)>(
        json_tokener_new_ex(48), json_tokener_free);
    require(token != nullptr);
    json_tokener_set_flags(token.get(), JSON_TOKENER_STRICT);
    Json value(json_tokener_parse_ex(token.get(), text.data(), static_cast<int>(text.size())),
               json_object_put);
    require(json_tokener_get_error(token.get()) == json_tokener_success && value
        && json_object_get_type(value.get()) == json_type_object);
    for (auto i = json_tokener_get_parse_end(token.get()); i < text.size(); ++i) {
        require(text[i] == ' ' || text[i] == '\r' || text[i] == '\n' || text[i] == '\t');
    }
    return value;
}
J field(J value, const char* key) {
    J result = nullptr;
    require(json_object_object_get_ex(value,key,&result));
    return result;
}
void fields(J value, std::initializer_list<const char*> keys) {
    require(json_object_get_type(value) == json_type_object
        && static_cast<std::size_t>(json_object_object_length(value)) == keys.size());
    for (const auto key : keys) (void)field(value,key);
}
std::size_t count(J value, std::size_t limit) {
    require(json_object_get_type(value) == json_type_array);
    const auto size = json_object_array_length(value);
    require(size <= limit);
    return size;
}
void tuple(J value, std::size_t size) { require(count(value,size) == size); }
J at(J value, std::size_t i) { return json_object_array_get_idx(value,i); }
std::string string(J value, bool allow_empty = false, std::size_t limit = 1024) {
    require(json_object_get_type(value) == json_type_string);
    std::string result(json_object_get_string(value),json_object_get_string_len(value));
    require((allow_empty || !result.empty()) && result.size() <= limit
        && result.find('\0') == std::string::npos);
    return result;
}
std::uint32_t integer(J value) {
    require(json_object_get_type(value) == json_type_int);
    const auto number = json_object_get_int64(value);
    require(number >= 0 && number <= std::numeric_limits<std::uint32_t>::max());
    return static_cast<std::uint32_t>(number);
}
bool boolean(J value) {
    require(json_object_get_type(value) == json_type_boolean);
    return json_object_get_boolean(value);
}
bool null(J value) { return json_object_get_type(value) == json_type_null; }
std::optional<std::size_t> unique_quote(std::string_view source, std::string_view quote,
                                       std::uint64_t& work) {
    require(!quote.empty() && quote.size() <= 4096,"Invalid quotation size");
    std::optional<std::size_t> found;
    for (std::size_t i = 0; i + quote.size() <= source.size(); ++i) {
        std::size_t j = 0;
        while (j < quote.size()) {
            require(++work <= 4000000,"Quotation search work bound exceeded");
            if (source[i+j] != quote[j]) break;
            ++j;
        }
        if (j == quote.size()) {
            if (found) return std::nullopt;
            found = i;
        }
    }
    return found;
}
std::pair<std::size_t,std::size_t> selector_range(const ProblemContext& context,
                                                std::uint32_t view, std::uint32_t locator) {
    require(view < context.views.size() && context.views[view].text.size() <= 65536,
            "Invalid quote view");
    const auto& source = context.views[view];
    if (locator == no_step) {
        require(source.locators.empty(),"Quotation requires the declared structural locator");
        return {0,source.text.size()};
    }
    require(locator < source.locators.size(),"Invalid quote locator");
    const auto& range = source.locators[locator];
    require(range.offset <= source.text.size() && range.length > 0
        && range.length <= source.text.size()-range.offset,"Invalid quote locator range");
    return {range.offset,range.length};
}
EvidenceSpan span(J value, const ProblemContext& context, ProblemSelectorMode mode,
                  std::uint64_t& work) try {
    tuple(value,4);
    if (mode == ProblemSelectorMode::OffsetV1) {
        return {integer(at(value,0)),integer(at(value,1)),integer(at(value,2)),
            null(at(value,3)) ? no_step : integer(at(value,3))};
    }
    require(mode == ProblemSelectorMode::QuoteV1,"Unknown problem selector mode");
    const auto view = integer(at(value,0));
    const auto locator = null(at(value,1)) ? no_step : integer(at(value,1));
    const auto [begin,length] = selector_range(context,view,locator);
    auto source = std::string_view(context.views[view].text).substr(begin,length);
    std::size_t offset = begin;
    if (!null(at(value,3))) {
        const auto enclosing = string(at(value,3),false,4096);
        const auto position = unique_quote(source,enclosing,work);
        require(position.has_value(),"Missing or ambiguous enclosing quotation");
        offset += *position;
        source = source.substr(*position,enclosing.size());
    }
    const auto quote = string(at(value,2),false,4096);
    const auto position = unique_quote(source,quote,work);
    require(position.has_value(),"Missing or ambiguous selected quotation");
    return {view,static_cast<std::uint32_t>(offset+*position),
            static_cast<std::uint32_t>(quote.size()),locator};
} catch (const std::runtime_error& error) {
    if (mode == ProblemSelectorMode::QuoteV1) throw ProblemSelectorError(error.what());
    throw;
}
ProblemScope scope(J value) {
    tuple(value,2);
    ProblemScope result;
    result.resolved = boolean(at(value,0));
    const auto items = at(value,1);
    for (std::size_t i = 0, n = count(items,16); i < n; ++i) {
        tuple(at(items,i),2);
        result.qualifiers.emplace_back(string(at(at(items,i),0)),string(at(at(items,i),1)));
    }
    return result;
}
ProblemOrigin origin(J value, const ProblemContext& context, std::string label,
                     ProblemSelectorMode mode, std::uint64_t& work) {
    ProblemOrigin result{PremiseBasis::Question,std::move(label),{}};
    for (std::size_t i = 0, n = count(value,16); i < n; ++i) {
        const auto support = span(at(value,i),context,mode,work);
        require(support.view < context.views.size());
        const auto basis = context.views[support.view].basis;
        require(basis == PremiseBasis::Question || basis == PremiseBasis::Source,
                "Learned proposal cannot assert formal premises");
        if (basis == PremiseBasis::Source) result.basis = basis;
        result.supports.push_back(support);
    }
    require(!result.supports.empty(), "Uncited learned premise");
    return result;
}
Term term(J value) {
    tuple(value,2);
    const auto kind = string(at(value,0));
    require(kind == "c" || kind == "v");
    return {kind == "c" ? TermKind::Constant : TermKind::Variable, integer(at(value,1))};
}
Atom atom(J value) {
    tuple(value,3);
    Atom result{integer(at(value,0)),boolean(at(value,1)),{}};
    const auto terms = at(value,2);
    for (std::size_t i = 0, n = count(terms,16); i < n; ++i) {
        result.terms.push_back(term(at(terms,i)));
    }
    return result;
}
ProblemNode problem_node(J item) {
    require(count(item,3) >= 2);
    const auto tag = string(at(item,0));
    if (tag == "q" || tag == "n" || tag == "neg") {
        tuple(item,2);
        const auto index = integer(at(item,1));
        if (tag == "q") return ProblemQuantityNode{index};
        if (tag == "n") return ProblemNumberNode{index};
        return ProblemNegateNode{index};
    }
    tuple(item,3);
    require(tag == "add" || tag == "sub" || tag == "mul" || tag == "div");
    const auto op = tag == "add" ? Code::Add : tag == "sub" ? Code::Subtract
        : tag == "mul" ? Code::Multiply : Code::Divide;
    return ProblemBinaryNode{op,integer(at(item,1)),integer(at(item,2))};
}
J array() { return json_object_new_array(); }
J text(const std::string& value) { return json_object_new_string_len(value.data(),value.size()); }
J number(std::uint32_t value) { return json_object_new_int64(value); }
J yes(bool value) { return json_object_new_boolean(value); }
void add(J value, J item) { json_object_array_add(value,item); }
void put(J value, const char* key, J item) { json_object_object_add(value,key,item); }
J pack(std::initializer_list<J> values) {
    auto result = array();
    for (const auto value : values) add(result,value);
    return result;
}
J encode(const EvidenceSpan& value) {
    return pack({number(value.view),number(value.offset),number(value.length),
                 value.locator == no_step ? nullptr : number(value.locator)});
}
J encode(const EvidenceSpan& value, const ProblemContext& context,
         ProblemSelectorMode mode, std::uint64_t& work) {
    if (mode == ProblemSelectorMode::OffsetV1) return encode(value);
    require(mode == ProblemSelectorMode::QuoteV1,"Unknown problem selector mode");
    const auto [begin,length] = selector_range(context,value.view,value.locator);
    const auto& text_view = context.views[value.view].text;
    const auto source = std::string_view(text_view).substr(begin,length);
    require(value.offset >= begin && value.length > 0
        && value.offset-begin <= length && value.length <= length-(value.offset-begin),
        "Invalid quoted span");
    const auto relative = value.offset-begin;
    const auto quote = source.substr(relative,value.length);
    const auto result = [&](J enclosure) {
        return pack({number(value.view),value.locator == no_step ? nullptr : number(value.locator),
                     text(std::string(quote)),enclosure});
    };
    if (const auto position = unique_quote(source,quote,work); position && *position == relative) {
        return result(nullptr);
    }
    const auto boundary = [&](std::size_t i) {
        return i == source.size() || (static_cast<unsigned char>(source[i])&0xc0) != 0x80;
    };
    const auto anchor = [&](std::size_t left, std::size_t right) -> std::optional<std::string> {
        if (!boundary(left) || !boundary(right)) return std::nullopt;
        const auto enclosing = source.substr(left,right-left);
        const auto selected = unique_quote(enclosing,quote,work);
        if (!selected || left+*selected != relative) return std::nullopt;
        const auto located = unique_quote(source,enclosing,work);
        if (!located || *located != left) return std::nullopt;
        return std::string(enclosing);
    };
    for (std::size_t extra = 1; extra+quote.size() <= std::min<std::size_t>(4096,length); ++extra) {
        if (relative+quote.size()+extra <= source.size()) {
            if (auto enclosure = anchor(relative,relative+quote.size()+extra)) return result(text(*enclosure));
        }
        if (relative >= extra) {
            if (auto enclosure = anchor(relative-extra,relative+quote.size())) return result(text(*enclosure));
        }
        const auto left_extra = extra/2, right_extra = extra-left_extra;
        if (relative >= left_extra && relative+quote.size()+right_extra <= source.size()) {
            if (auto enclosure = anchor(relative-left_extra,relative+quote.size()+right_extra)) {
                return result(text(*enclosure));
            }
        }
    }
    throw std::runtime_error("Span has no bounded unique quotation selector");
}
J encode(const ProblemScope& value) {
    auto items = array();
    for (const auto& [key,item] : value.qualifiers) add(items,pack({text(key),text(item)}));
    return pack({yes(value.resolved),items});
}
J encode(const ProblemOrigin& value, const ProblemContext& context,
         ProblemSelectorMode mode, std::uint64_t& work) {
    require(value.basis != PremiseBasis::Formal, "Formal premises cannot be serialized as learned proposals");
    auto result = array();
    for (const auto& support : value.supports) add(result,encode(support,context,mode,work));
    return result;
}
J encode(const Atom& value) {
    auto terms = array();
    for (const auto& term : value.terms) {
        add(terms,pack({text(term.kind == TermKind::Constant ? "c" : "v"),number(term.index)}));
    }
    return pack({number(value.predicate),yes(value.negative),terms});
}
const char* precision_name(QuantityPrecision value) {
    switch (value) {
    case QuantityPrecision::Exact: return "exact";
    case QuantityPrecision::Approximate: return "approximate";
    case QuantityPrecision::Bound: return "bound";
    case QuantityPrecision::Unspecified: return "unspecified";
    }
    throw std::runtime_error("Invalid quantity precision");
}
QuantityPrecision precision(const std::string& value) {
    for (const auto item : {QuantityPrecision::Exact,QuantityPrecision::Approximate,
                            QuantityPrecision::Bound,QuantityPrecision::Unspecified}) {
        if (value == precision_name(item)) return item;
    }
    throw std::runtime_error("Invalid quantity precision");
}
std::string dump(J value) { return json_object_to_json_string_ext(value,JSON_C_TO_STRING_PLAIN); }
}

namespace {
ProblemRequest parse_problem_proposal_impl(const std::string& json, const ProblemContext& context,
                                           ProblemLimits limits, ProblemSelectorMode mode,
                                           bool with_goal) {
    require(mode == ProblemSelectorMode::OffsetV1 || mode == ProblemSelectorMode::QuoteV1);
    require(json.size() <= 131072);
    const auto root = parse(json);
    J status = nullptr;
    if (with_goal && json_object_object_get_ex(root.get(),"status",&status)) {
        fields(root.get(),{"status","reason"});
        const auto kind = string(status);
        require(kind == "clarify" || kind == "unsupported");
        throw ProblemDecline(kind == "clarify" ? ProblemDeclineKind::Clarify
                                               : ProblemDeclineKind::Unsupported,
                             string(field(root.get(),"reason")));
    }
    if (with_goal) {
        fields(root.get(),{"entities","quantities","numbers","bindings","nodes","equations",
                           "predicates","rules","goal"});
    } else {
        fields(root.get(),{"entities","quantities","numbers","bindings","nodes","equations",
                           "predicates","rules"});
    }
    ProblemRequest result{context,{},limits};
    std::uint64_t selector_work = 0;
    auto& p = result.proposal;
    const auto each = [&](const char* key, std::size_t limit, auto action) {
        const auto items = field(root.get(),key);
        for (std::size_t i = 0, n = count(items,limit); i < n; ++i) action(at(items,i),i);
    };
    each("entities",64,[&](J item, std::size_t) { p.entities.push_back(string(item)); });
    each("quantities",64,[&](J item, std::size_t) {
        tuple(item,4);
        p.quantities.push_back({string(at(item,0)),integer(at(item,1)),scope(at(item,2)),integer(at(item,3))});
    });
    each("numbers",128,[&](J item, std::size_t) {
        tuple(item,5);
        p.numbers.push_back({span(at(item,0),context,mode,selector_work),null(at(item,1)) ? std::nullopt
            : std::optional{span(at(item,1),context,mode,selector_work)},integer(at(item,2)),precision(string(at(item,3))),
            null(at(item,4)) ? std::nullopt : std::optional{span(at(item,4),context,mode,selector_work)}});
    });
    each("bindings",128,[&](J item, std::size_t i) {
        tuple(item,3);
        p.bindings.push_back({integer(at(item,0)),integer(at(item,1)),
            origin(at(item,2),context,"binding_"+std::to_string(i),mode,selector_work)});
    });
    each("nodes",256,[&](J item, std::size_t) {
        p.nodes.push_back(problem_node(item));
    });
    each("equations",64,[&](J item, std::size_t i) {
        tuple(item,5);
        p.equations.push_back({integer(at(item,0)),integer(at(item,1)),scope(at(item,2)),
            null(at(item,3)) ? std::nullopt : std::optional{atom(at(item,3))},
            origin(at(item,4),context,"equation_"+std::to_string(i),mode,selector_work)});
    });
    each("predicates",64,[&](J item, std::size_t) {
        tuple(item,2);
        p.logic.predicates.push_back({string(at(item,0)),integer(at(item,1))});
    });
    each("rules",128,[&](J item, std::size_t i) {
        tuple(item,4);
        RelationalRule rule{integer(at(item,0)),atom(at(item,1)),{},"rule_"+std::to_string(i)};
        const auto premises = at(item,2);
        for (std::size_t j = 0, n = count(premises,64); j < n; ++j) {
            rule.premises.push_back(atom(at(premises,j)));
        }
        p.logic.rules.push_back(rule);
        p.logic_origins.push_back(origin(at(item,3),context,rule.provenance,mode,selector_work));
    });
    if (!p.logic.predicates.empty() || !p.logic.rules.empty()) {
        p.logic.name = "interpreted_problem";
        p.logic.version = "1";
        p.logic.constants = p.entities;
    }
    if (with_goal) {
        const auto goal = field(root.get(),"goal");
        require(count(goal,3) >= 2);
        const auto tag = string(at(goal,0));
        if (tag == "q") {
            tuple(goal,3);
            p.goal = ProblemQuantityGoal{integer(at(goal,1)),integer(at(goal,2))};
        } else {
            require(tag == "atom");
            tuple(goal,2);
            p.goal = ProblemAtomGoal{atom(at(goal,1))};
        }
        (void)admit_problem(result);
    } else {
        (void)admit_problem_theory(result);
    }
    return result;
}
}

ProblemRequest parse_problem_proposal(const std::string& json, const ProblemContext& context,
                                      ProblemLimits limits, ProblemSelectorMode mode) {
    return parse_problem_proposal_impl(json, context, limits, mode, true);
}

void validate_problem_theory_proposal(const std::string& json, const ProblemContext& context,
                                      ProblemLimits limits, ProblemSelectorMode mode) {
    (void)parse_problem_theory_proposal(json, context, limits, mode);
}

ProblemProposal parse_problem_theory_proposal(const std::string& json, const ProblemContext& context,
                                             ProblemLimits limits, ProblemSelectorMode mode) {
    return parse_problem_proposal_impl(json, context, limits, mode, false).proposal;
}

ParsedQuantifiedTheory parse_quantified_problem_theory_proposal(const std::string& json,
    const ProblemContext& context, ProblemLimits limits, ProblemSelectorMode mode) {
    require(json.size() <= 131072);
    const auto root = parse(json);
    fields(root.get(),{"entities","quantities","numbers","bindings","nodes","equations",
                       "predicates","rules","quantified_equations"});
    Json basic(json_object_new_object(), json_object_put);
    require(bool(basic), "Cannot allocate base theory");
    for (const auto key : {"entities","quantities","numbers","bindings","nodes","equations",
                           "predicates","rules"})
        put(basic.get(), key, json_object_get(field(root.get(),key)));
    ParsedQuantifiedTheory result{
        parse_problem_theory_proposal(
            json_object_to_json_string_ext(basic.get(),JSON_C_TO_STRING_PLAIN), context, limits, mode), {}};
    std::uint64_t selector_work = 0;
    const auto declarations = field(root.get(),"quantified_equations");
    for (std::size_t i = 0, n = count(declarations,64); i < n; ++i) {
        const auto item = at(declarations,i);
        tuple(item,8);
        QuantifiedEquation schema;
        schema.variables = integer(at(item,0));
        const auto quantities = at(item,1);
        for (std::size_t j = 0, m = count(quantities,64); j < m; ++j) {
            const auto value = at(quantities,j);
            tuple(value,4);
            schema.quantities.push_back({string(at(value,0)),term(at(value,1)),
                scope(at(value,2)),integer(at(value,3))});
        }
        const auto nodes = at(item,2);
        for (std::size_t j = 0, m = count(nodes,256); j < m; ++j)
            schema.nodes.push_back(problem_node(at(nodes,j)));
        schema.left = integer(at(item,3));
        schema.right = integer(at(item,4));
        schema.scope = scope(at(item,5));
        if (!null(at(item,6))) schema.guard = atom(at(item,6));
        schema.origin = origin(at(item,7), context, "quantified_equation_"+std::to_string(i),
            mode, selector_work);
        result.schemas.push_back(std::move(schema));
    }
    return result;
}

std::string serialize_problem_proposal(const ProblemRequest& request, ProblemSelectorMode mode) {
    require(mode == ProblemSelectorMode::OffsetV1 || mode == ProblemSelectorMode::QuoteV1);
    (void)admit_problem(request);
    const auto& p = request.proposal;
    Json root(json_object_new_object(),json_object_put);
    std::uint64_t selector_work = 0;
    const auto quoted = [&](const auto& value) { return encode(value,request.context,mode,selector_work); };
    const auto list = [&](const char* key, const auto& values, auto action) {
        auto items = array();
        for (const auto& value : values) add(items,action(value));
        put(root.get(),key,items);
    };
    list("entities",p.entities,[](const auto& value) { return text(value); });
    list("quantities",p.quantities,[](const auto& q) {
        return pack({text(q.property),number(q.entity),encode(q.scope),number(q.unit)});
    });
    list("numbers",p.numbers,[&](const auto& n) {
        return pack({quoted(n.numeral),n.unit_span ? quoted(*n.unit_span) : nullptr,
            number(n.unit),text(precision_name(n.precision)),n.qualifier ? quoted(*n.qualifier) : nullptr});
    });
    list("bindings",p.bindings,[&](const auto& b) {
        return pack({number(b.quantity),number(b.number),quoted(b.origin)});
    });
    list("nodes",p.nodes,[](const auto& node) {
        if (const auto q = std::get_if<ProblemQuantityNode>(&node)) return pack({text("q"),number(q->quantity)});
        if (const auto n = std::get_if<ProblemNumberNode>(&node)) return pack({text("n"),number(n->number)});
        if (const auto n = std::get_if<ProblemNegateNode>(&node)) return pack({text("neg"),number(n->operand)});
        const auto& b = std::get<ProblemBinaryNode>(node);
        const auto tag = b.operation == Code::Add ? "add" : b.operation == Code::Subtract ? "sub"
            : b.operation == Code::Multiply ? "mul" : "div";
        return pack({text(tag),number(b.left),number(b.right)});
    });
    list("equations",p.equations,[&](const auto& e) {
        return pack({number(e.left),number(e.right),encode(e.scope),
                     e.guard ? encode(*e.guard) : nullptr,quoted(e.origin)});
    });
    list("predicates",p.logic.predicates,[](const auto& value) {
        return pack({text(value.name),number(value.arity)});
    });
    auto rules = array();
    for (std::size_t i = 0; i < p.logic.rules.size(); ++i) {
        const auto& rule = p.logic.rules[i];
        auto premises = array();
        for (const auto& premise : rule.premises) add(premises,encode(premise));
        add(rules,pack({number(rule.variables),encode(rule.head),premises,quoted(p.logic_origins[i])}));
    }
    put(root.get(),"rules",rules);
    if (const auto q = std::get_if<ProblemQuantityGoal>(&p.goal)) {
        put(root.get(),"goal",pack({text("q"),number(q->quantity),number(q->unit)}));
    } else put(root.get(),"goal",pack({text("atom"),encode(std::get<ProblemAtomGoal>(p.goal).atom)}));
    return dump(root.get());
}

std::string problem_semantic_input(const ProblemContext& context) {
    Json root(json_object_new_object(),json_object_put);
    auto units = array();
    for (std::size_t i = 0; i < context.units.size(); ++i) {
        const auto& unit = context.units[i];
        auto spellings = array();
        for (const auto& spelling : unit.spellings) add(spellings,text(spelling));
        add(units,pack({number(i),text(unit.id),text(unit.symbol),spellings}));
    }
    put(root.get(),"units",units);
    auto views = array();
    for (std::size_t i = 0; i < context.views.size(); ++i) {
        const auto& view = context.views[i];
        require(view.basis == PremiseBasis::Question || view.basis == PremiseBasis::Source,
                "Learned context must contain only question and source views");
        auto locators = array();
        for (const auto& locator : view.locators) {
            auto headers = array();
            for (const auto header : locator.headers) add(headers,number(header));
            add(locators,pack({number(static_cast<std::uint32_t>(locator.role)),number(locator.offset),
                number(locator.length),text(locator.table),text(locator.row),text(locator.column),headers}));
        }
        add(views,pack({number(i),text(view.id),
            text(view.basis == PremiseBasis::Question ? "question" : "source"),text(view.text),locators}));
    }
    put(root.get(),"views",views);
    const auto result = dump(root.get());
    require(result.size() <= 131072,"Problem interpretation input exceeds bound");
    return result;
}

ProblemContext parse_problem_context(const std::string& json) {
    const auto root = parse(json);
    fields(root.get(),{"registry","units","views"});
    const auto registry = field(root.get(),"registry");
    tuple(registry,2);
    ProblemContext result;
    result.unit_registry_id = string(at(registry,0));
    result.unit_registry_version = string(at(registry,1));
    const auto units = field(root.get(),"units");
    for (std::size_t i = 0, n = count(units,64); i < n; ++i) {
        const auto item = at(units,i);
        tuple(item,5);
        ProblemUnit unit;
        unit.id = string(at(item,0)); unit.symbol = string(at(item,1));
        const auto dimensions = at(item,2);
        tuple(dimensions,7);
        for (std::size_t j = 0; j < 7; ++j) {
            const auto power = at(dimensions,j);
            require(json_object_get_type(power) == json_type_int);
            const auto exponent = json_object_get_int64(power);
            require(exponent >= -64 && exponent <= 64);
            unit.dimension[j] = static_cast<std::int32_t>(exponent);
        }
        const auto scale = string(at(item,3));
        require(unit.scale.set_str(scale,10) == 0,"Invalid host unit scale");
        unit.scale = checked_problem_rational(unit.scale);
        require(unit.scale > 0 && unit.scale.get_str() == scale,"Noncanonical host unit scale");
        const auto spellings = at(item,4);
        for (std::size_t j = 0, m = count(spellings,16); j < m; ++j) {
            unit.spellings.push_back(string(at(spellings,j)));
        }
        result.units.push_back(unit);
    }
    const auto views = field(root.get(),"views");
    std::size_t total = 0;
    for (std::size_t i = 0, n = count(views,16); i < n; ++i) {
        const auto item = at(views,i);
        tuple(item,7);
        const auto basis = string(at(item,1));
        require(basis == "question" || basis == "source","Invalid host evidence basis");
        auto view = make_evidence_view(string(at(item,0)),
            basis == "source" ? PremiseBasis::Source : PremiseBasis::Question,
            string(at(item,5),false,65536),string(at(item,2),true),
            string(at(item,3),true),string(at(item,4)));
        total += view.text.size();
        require(total <= 131072);
        const auto locators = at(item,6);
        for (std::size_t j = 0, m = count(locators,256); j < m; ++j) {
            const auto loc = at(locators,j);
            tuple(loc,7);
            const auto role = integer(at(loc,0));
            require(role >= 1 && role <= 3);
            EvidenceLocator locator{static_cast<LocatorRole>(role),integer(at(loc,1)),integer(at(loc,2)),
                string(at(loc,3),true),string(at(loc,4),true),string(at(loc,5),true),{}};
            const auto headers = at(loc,6);
            for (std::size_t k = 0, h = count(headers,16); k < h; ++k) {
                locator.headers.push_back(integer(at(headers,k)));
            }
            view.locators.push_back(locator);
        }
        result.views.push_back(view);
    }
    // Full span, registry and graph admission happens when this host context
    // is combined with a proposal; no learned field can replace this context.
    return result;
}

std::string serialize_problem_context(const ProblemContext& context) {
    Json root(json_object_new_object(),json_object_put);
    put(root.get(),"registry",pack({text(context.unit_registry_id),text(context.unit_registry_version)}));
    auto units = array();
    for (const auto& unit : context.units) {
        auto dimensions = array(), spellings = array();
        for (const auto power : unit.dimension) add(dimensions,json_object_new_int(power));
        for (const auto& spelling : unit.spellings) add(spellings,text(spelling));
        add(units,pack({text(unit.id),text(unit.symbol),dimensions,text(unit.scale.get_str()),spellings}));
    }
    put(root.get(),"units",units);
    auto views = array();
    for (const auto& view : context.views) {
        require(view.basis == PremiseBasis::Question || view.basis == PremiseBasis::Source);
        require(view.digest == evidence_digest(view.text),"Host evidence digest mismatch");
        auto locators = array();
        for (const auto& locator : view.locators) {
            auto headers = array();
            for (const auto header : locator.headers) add(headers,number(header));
            add(locators,pack({number(static_cast<std::uint32_t>(locator.role)),number(locator.offset),
                number(locator.length),text(locator.table),text(locator.row),text(locator.column),headers}));
        }
        add(views,pack({text(view.id),text(view.basis == PremiseBasis::Source ? "source" : "question"),
            text(view.archive_id),text(view.entry_path),text(view.extractor_version),text(view.text),locators}));
    }
    put(root.get(),"views",views);
    const auto result = dump(root.get());
    require(parse_problem_context(result) == context,"Host context does not roundtrip");
    return result;
}

std::string problem_proposal_schema(ProblemSelectorMode mode) {
    require(mode == ProblemSelectorMode::OffsetV1 || mode == ProblemSelectorMode::QuoteV1,
            "Unknown problem selector mode");
    const auto ref = [](const char* name) {
        auto value = json_object_new_object();
        put(value,"$ref",text(std::string("#/$defs/")+name));
        return value;
    };
    const auto tuple_schema = [](std::initializer_list<J> items) {
        auto value = json_object_new_object();
        put(value,"type",text("array"));
        put(value,"prefixItems",pack(items));
        put(value,"minItems",json_object_new_int64(items.size()));
        put(value,"maxItems",json_object_new_int64(items.size()));
        return value;
    };
    const auto list_schema = [&](const char* name, unsigned maximum, unsigned minimum = 0) {
        auto value = json_object_new_object();
        put(value,"type",text("array"));
        put(value,"items",ref(name));
        put(value,"minItems",json_object_new_int64(minimum));
        put(value,"maxItems",json_object_new_int64(maximum));
        return value;
    };
    const auto alternatives = [](std::initializer_list<J> items) {
        auto value = json_object_new_object();
        put(value,"anyOf",pack(items));
        return value;
    };
    const auto nullable = [&](const char* name) {
        return alternatives({ref(name),parse(R"({"type":"null"})").release()});
    };
    Json root(json_object_new_object(),json_object_put);
    auto definitions = json_object_new_object();
    put(root.get(),"$defs",definitions);
    put(definitions,"index",parse(R"({"type":"integer","minimum":0,"maximum":4294967295})").release());
    put(definitions,"text",parse(R"({"type":"string","minLength":1,"maxLength":1024})").release());
    put(definitions,"quote",parse(R"({"type":"string","minLength":1,"maxLength":4096})").release());
    put(definitions,"nullable_index",nullable("index"));
    put(definitions,"nullable_quote",nullable("quote"));
    put(definitions,"span",mode == ProblemSelectorMode::QuoteV1
        ? tuple_schema({ref("index"),ref("nullable_index"),ref("quote"),ref("nullable_quote")})
        : tuple_schema({ref("index"),ref("index"),ref("index"),ref("nullable_index")}));
    put(definitions,"nullable_span",nullable("span"));
    put(definitions,"qualifier",tuple_schema({ref("text"),ref("text")}));
    put(definitions,"scope",tuple_schema({parse(R"({"type":"boolean"})").release(),
                                          list_schema("qualifier",16)}));
    put(definitions,"quantity",tuple_schema({ref("text"),ref("index"),ref("scope"),ref("index")}));
    put(definitions,"number",tuple_schema({ref("span"),ref("nullable_span"),ref("index"),
        parse(R"({"enum":["exact","approximate","bound","unspecified"]})").release(),ref("nullable_span")}));
    put(definitions,"origin",list_schema("span",16,1));
    put(definitions,"binding",tuple_schema({ref("index"),ref("index"),ref("origin")}));
    put(definitions,"term",tuple_schema({parse(R"({"enum":["c","v"]})").release(),ref("index")}));
    put(definitions,"atom",tuple_schema({ref("index"),parse(R"({"type":"boolean"})").release(),
                                         list_schema("term",16)}));
    put(definitions,"nullable_atom",nullable("atom"));
    put(definitions,"node",alternatives({
        tuple_schema({parse(R"({"enum":["q","n","neg"]})").release(),ref("index")}),
        tuple_schema({parse(R"({"enum":["add","sub","mul","div"]})").release(),ref("index"),ref("index")})}));
    put(definitions,"equation",tuple_schema({ref("index"),ref("index"),ref("scope"),
                                              ref("nullable_atom"),ref("origin")}));
    put(definitions,"predicate",tuple_schema({ref("text"),ref("index")}));
    put(definitions,"rule",tuple_schema({ref("index"),ref("atom"),list_schema("atom",64),ref("origin")}));
    put(definitions,"goal",alternatives({
        tuple_schema({parse(R"({"const":"q"})").release(),ref("index"),ref("index")}),
        tuple_schema({parse(R"({"const":"atom"})").release(),ref("atom")})}));
    auto proposal = json_object_new_object();
    put(proposal,"type",text("object"));
    put(proposal,"additionalProperties",json_object_new_boolean(false));
    auto properties = json_object_new_object();
    auto required = array();
    for (const auto& [name,type,maximum] : {
             std::tuple{"entities","text",64U},std::tuple{"quantities","quantity",64U},
             std::tuple{"numbers","number",128U},std::tuple{"bindings","binding",128U},
             std::tuple{"nodes","node",256U},std::tuple{"equations","equation",64U},
             std::tuple{"predicates","predicate",64U},std::tuple{"rules","rule",128U}}) {
        put(properties,name,list_schema(type,maximum));
        add(required,text(name));
    }
    put(properties,"goal",ref("goal"));
    add(required,text("goal"));
    put(proposal,"properties",properties);
    put(proposal,"required",required);
    auto decline = parse(R"({"type":"object","additionalProperties":false,"required":["status","reason"],"properties":{"status":{"enum":["clarify","unsupported"]},"reason":{"$ref":"#/$defs/text"}}})").release();
    put(root.get(),"oneOf",pack({proposal,decline}));
    return dump(root.get());
}

std::string interpret_problem_json(const ProblemContext& context, ProblemSelectorMode mode) {
    return interpret_local_proposal_json(problem_semantic_prompt(mode),
        problem_semantic_input(context),problem_proposal_schema(mode),1536);
}
} // namespace axiom
