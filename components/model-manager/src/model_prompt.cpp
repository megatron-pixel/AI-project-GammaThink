#include "model_prompt.hpp"
#include <jinja/parser.h>
#include <common/json.h>

namespace axiom::neural {
namespace {
void require(bool value, knowledge::ErrorCode code, const char* message) {
    if (!value) throw knowledge::Error(code, message);
}
}

struct ModelPrompt::Impl {
    std::string source, bos, eos, system;
    jinja::program compiled;
    bool add_bos, add_eos, thinking;
    std::time_t time;
    Impl(std::string text, std::string begin, std::string end,
         bool add_begin, bool add_end, bool enable_thinking, std::time_t captured_time, std::string policy)
        : source(std::move(text)), bos(std::move(begin)), eos(std::move(end)), system(std::move(policy)),
          compiled(jinja::parse_from_tokens(jinja::lexer{}.tokenize(source))),
          add_bos(add_begin), add_eos(add_end), thinking(enable_thinking), time(captured_time) {}
};

ModelPrompt::ModelPrompt(std::string source, std::string bos, std::string eos,
    bool add_bos, bool add_eos, bool enable_thinking, std::time_t captured_time, std::string trusted_system) {
    require(!source.empty(), knowledge::ErrorCode::Schema, "Missing model chat template");
    knowledge::validate_text(source, 262144);
    if (!trusted_system.empty()) knowledge::validate_text(trusted_system, 4096);
    impl_ = std::make_unique<Impl>(std::move(source), std::move(bos), std::move(eos),
                                 add_bos, add_eos, enable_thinking, captured_time, std::move(trusted_system));
}
ModelPrompt::~ModelPrompt() = default;

std::vector<PromptPart> ModelPrompt::render(std::string_view input, std::size_t maximum_bytes) const {
    require(!input.empty(), knowledge::ErrorCode::Schema, "Empty neural context");
    require(maximum_bytes > 0 && maximum_bytes <= 4 * 1024 * 1024 + 4096,
            knowledge::ErrorCode::Budget, "Invalid prompt byte limit");
    knowledge::validate_text(input, maximum_bytes);
    auto messages = common_json::array();
    if (!impl_->system.empty()) messages.push_back({{"role", "system"}, {"content", impl_->system}});
    messages.push_back({{"role", "user"}, {"content", std::string(input)}});
    const auto variables = common_json{
        {"messages", std::move(messages)},
        {"add_generation_prompt", true}, {"enable_thinking", impl_->thinking}};
    jinja::context context(impl_->source);
    context.current_time = impl_->time;
    jinja::global_from_json(context, variables, true);
    context.set_val("bos_token", jinja::mk_val<jinja::value_string>(impl_->bos));
    context.set_val("eos_token", jinja::mk_val<jinja::value_string>(impl_->eos));
    jinja::runtime runtime(context);
    auto rendered = jinja::runtime::gather_string_parts(runtime.execute(impl_->compiled));
    auto& parts = rendered->val_str.parts;
    if (impl_->add_bos && !parts.empty() && !parts.front().is_input &&
        parts.front().val.starts_with(impl_->bos)) parts.front().val.erase(0, impl_->bos.size());
    if (impl_->add_eos && !parts.empty() && !parts.back().is_input &&
        parts.back().val.ends_with(impl_->eos)) parts.back().val.resize(parts.back().val.size() - impl_->eos.size());
    std::vector<PromptPart> result;
    std::size_t total = 0;
    for (const auto& part : parts) {
        require(part.val.size() <= maximum_bytes - total, knowledge::ErrorCode::Budget,
                "Rendered neural prompt byte limit exceeded");
        total += part.val.size();
        if (!part.val.empty()) result.push_back({part.val, part.is_input});
    }
    require(total > 0, knowledge::ErrorCode::Schema, "Model template produced an empty prompt");
    return result;
}

} // namespace axiom::neural
