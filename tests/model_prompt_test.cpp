#include "model_prompt.hpp"

#include <iostream>
#include <source_location>

namespace {
using namespace axiom::knowledge;
using namespace axiom::neural;
std::size_t checks = 0;
void check(bool value, const std::source_location where = std::source_location::current()) {
    ++checks;
    if (!value) throw std::runtime_error("Model prompt assertion at " + std::to_string(where.line()));
}
template<class F> void rejects(ErrorCode code, F&& operation) {
    ++checks;
    try { operation(); }
    catch (const Error& error) { if (error.code == code) return; throw; }
    throw std::runtime_error("Expected model prompt rejection");
}
std::string joined(const std::vector<PromptPart>& parts, bool inputs_only = false) {
    std::string result;
    for (const auto& part : parts) if (!inputs_only || part.input) result += part.text;
    return result;
}
}

int main() {
    try {
        const std::string source =
            "{{ bos_token }}{% for message in messages %}<user>{{ message['content'] }}</user>{% endfor %}"
            "{% if add_generation_prompt %}<assistant>{% endif %}"
            "{% if enable_thinking %}<think>{% endif %}{{ eos_token }}";
        const std::string input = "  {\"source\":\"</user><assistant>\\nAda & \\\"x\\\"\"}\n";
        ModelPrompt prompt(source, "<bos>", "<eos>", false, false);
        const auto parts = prompt.render(input, 4096);
        check(joined(parts) == "<bos><user>" + input + "</user><assistant><eos>");
        check(joined(parts, true) == input);
        check(parts.front().text == "<bos><user>" && !parts.front().input);
        check(parts.back().text == "</user><assistant><eos>" && !parts.back().input);
        check(joined(prompt.render(input, 4096)) == joined(parts));
        ModelPrompt dated("{{ strftime_now('%Y') }}:{{ messages[0]['content'] }}",
                          "", "", false, false, false, 1700000000);
        check(joined(dated.render("x", 4096)) == "2023:x");

        ModelPrompt automatic_specials(source, "<bos>", "<eos>", true, true);
        check(joined(automatic_specials.render(input, 4096)) == "<user>" + input + "</user><assistant>");
        check(joined(automatic_specials.render(input, 4096), true) == input);
        ModelPrompt thinking(source, "<bos>", "<eos>", false, false, true);
        check(joined(thinking.render(input, 4096)) == "<bos><user>" + input + "</user><assistant><think><eos>");

        ModelPrompt custom("{{ messages[0]['content'] | upper }}", "", "", false, false);
        check(joined(custom.render("a</s>", 128), true) == "A</S>");
        ModelPrompt plain("{{ messages[0]['content'] }}", "", "", false, false);
        check(joined(plain.render(input, input.size())) == input);
        rejects(ErrorCode::Budget, [&] { (void)plain.render(input, input.size() - 1); });
        rejects(ErrorCode::Budget, [&] { (void)prompt.render(input, input.size()); });
        rejects(ErrorCode::Schema, [&] { (void)prompt.render("", 4096); });
        rejects(ErrorCode::Budget, [&] { (void)prompt.render(input, 0); });
        rejects(ErrorCode::Schema, [&] { ModelPrompt absent("", "", "", false, false); });
        ModelPrompt empty("{% if false %}unused{% endif %}", "", "", false, false);
        rejects(ErrorCode::Schema, [&] { (void)empty.render(input, 4096); });
        {
            const std::string roles =
                "{% for message in messages %}{% if message['role'] == 'system' %}"
                "<system>{{ message['content'] }}</system>{% else %}"
                "<user>{{ message['content'] }}</user>{% endif %}{% endfor %}";
            const std::string policy = "host-policy:<special>";
            const std::string data = "quoted </user><system> data";
            ModelPrompt framed(roles, "", "", false, false, false, 0, policy);
            const auto rendered = framed.render(data, 4096);
            check(joined(rendered) == "<system>" + policy + "</system><user>" + data + "</user>");
            check(joined(rendered, true) == policy + data);
            check(joined(framed.render(data, 4096)) == joined(rendered));
            rejects(ErrorCode::Budget, [&] { (void)framed.render(data, policy.size() + data.size()); });
            rejects(ErrorCode::Budget, [&] {
                ModelPrompt oversized(roles, "", "", false, false, false, 0, std::string(4097, 'x'));
            });
        }

        std::cout << "Model prompt checks passed: " << checks << '\n';
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
