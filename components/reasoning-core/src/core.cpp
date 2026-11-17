#include "core.hpp"

#include <algorithm>
#include <cctype>
#include <fstream>
#include <iomanip>
#include <set>
#include <sstream>
#include <stdexcept>
#include <utility>

namespace axiom {
namespace {
constexpr std::size_t max_bits = 4096;
constexpr std::size_t max_dimension = 16;
constexpr std::size_t max_symbols = 1024;
constexpr std::size_t max_rules = 512;
constexpr std::size_t max_premises = 16;
constexpr std::size_t max_expression = 512;
constexpr std::size_t max_events = 4096;

void require(bool condition, const char* message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}

void bounded(const Rational& q) {
    require(q.get_den() > 0, "rational denominator must be positive");
    require(mpz_sizeinbase(q.get_num_mpz_t(), 2) <= max_bits &&
            mpz_sizeinbase(q.get_den_mpz_t(), 2) <= max_bits, "rational bit limit exceeded");
}

Rational checked(Rational q) {
    q.canonicalize();
    bounded(q);
    return q;
}

bool identifier(const std::string& text) {
    if (text.empty() || text.size() > 128 ||
        !(std::isalpha(static_cast<unsigned char>(text.front())) || text.front() == '_')) {
        return false;
    }
    return std::all_of(text.begin(), text.end(), [](unsigned char c) {
        return c < 128 && (std::isalnum(c) || c == '_' || c == '.');
    });
}

std::string trim(const std::string& text) {
    const auto start = text.find_first_not_of(" \t\r\n");
    if (start == std::string::npos) {
        return {};
    }
    return text.substr(start, text.find_last_not_of(" \t\r\n") - start + 1);
}

std::vector<std::string> split(const std::string& text, char delimiter) {
    std::vector<std::string> result;
    std::size_t start = 0;
    for (;;) {
        const auto end = text.find(delimiter, start);
        result.push_back(trim(text.substr(start, end - start)));
        if (end == std::string::npos) {
            return result;
        }
        start = end + 1;
    }
}

void validate_module(const Module& module) {
    require(identifier(module.name), "invalid module name");
    require(!module.version.empty() && module.version.size() <= 128, "invalid module version");
    require(module.symbols.size() <= max_symbols && module.rules.size() <= max_rules,
            "module count limit exceeded");
    std::set<std::string> unique;
    for (const auto& symbol : module.symbols) {
        require(identifier(symbol) && unique.insert(symbol).second, "invalid or duplicate symbol");
    }
    for (const auto& rule : module.rules) {
        require(rule.head < module.symbols.size() * 2, "invalid rule head");
        require(rule.premises.size() <= max_premises, "too many rule premises");
        require(!rule.provenance.empty() && rule.provenance.size() <= 1024, "missing or oversized provenance");
        for (auto premise : rule.premises) {
            require(premise < module.symbols.size() * 2, "invalid premise");
        }
    }
}

void validate_matrix(const Matrix& matrix) {
    require(!matrix.empty() && matrix.size() <= max_dimension, "matrix row count must be 1..16");
    const auto columns = matrix.front().size();
    require(columns > 0 && columns <= max_dimension, "matrix column count must be 1..16");
    for (const auto& row : matrix) {
        require(row.size() == columns, "ragged matrix");
        for (const auto& value : row) {
            bounded(value);
        }
    }
}

struct Affine {
    Rational coefficient = 0;
    Rational constant = 0;
};

Affine evaluate(const std::vector<Instruction>& expression, bool allow_variable) {
    require(!expression.empty() && expression.size() <= max_expression, "expression size invalid");
    std::vector<Affine> stack;
    for (const auto& instruction : expression) {
        if (instruction.code == Code::Constant) {
            bounded(instruction.value);
            stack.push_back({0, instruction.value});
        } else if (instruction.code == Code::Variable) {
            require(allow_variable, "variables require solve");
            stack.push_back({1, 0});
        } else if (instruction.code == Code::Negate) {
            require(!stack.empty(), "invalid unary expression");
            stack.back().coefficient = -stack.back().coefficient;
            stack.back().constant = -stack.back().constant;
        } else {
            require(stack.size() >= 2, "invalid binary expression");
            const auto right = stack.back();
            stack.pop_back();
            const auto left = stack.back();
            stack.pop_back();
            Affine out;
            switch (instruction.code) {
            case Code::Add:
                out = {checked(left.coefficient + right.coefficient), checked(left.constant + right.constant)};
                break;
            case Code::Subtract:
                out = {checked(left.coefficient - right.coefficient), checked(left.constant - right.constant)};
                break;
            case Code::Multiply:
                require(left.coefficient == 0 || right.coefficient == 0, "nonlinear multiplication unsupported");
                out = {checked(left.coefficient * right.constant + right.coefficient * left.constant),
                       checked(left.constant * right.constant)};
                break;
            case Code::Divide:
                require(right.coefficient == 0, "variable denominator unsupported");
                require(right.constant != 0, "division by zero");
                out = {checked(left.coefficient / right.constant), checked(left.constant / right.constant)};
                break;
            default:
                throw std::runtime_error("invalid expression opcode");
            }
            stack.push_back(std::move(out));
        }
    }
    require(stack.size() == 1, "expression stack not singular");
    return stack.front();
}

Rational determinant(Matrix matrix) {
    validate_matrix(matrix);
    const auto n = matrix.size();
    require(matrix.front().size() == n, "determinant requires square matrix");
    Rational result = 1;
    for (std::size_t column = 0; column < n; ++column) {
        std::size_t pivot = column;
        while (pivot < n && matrix[pivot][column] == 0) {
            ++pivot;
        }
        if (pivot == n) {
            return 0;
        }
        if (pivot != column) {
            std::swap(matrix[pivot], matrix[column]);
            result = -result;
        }
        result = checked(result * matrix[column][column]);
        for (std::size_t row = column + 1; row < n; ++row) {
            const Rational factor = checked(matrix[row][column] / matrix[column][column]);
            for (std::size_t j = column + 1; j < n; ++j) {
                matrix[row][j] = checked(matrix[row][j] - factor * matrix[column][j]);
            }
            matrix[row][column] = 0;
        }
    }
    return result;
}

void linear(const Matrix& input, const Matrix& rhs, Result& result) {
    validate_matrix(input);
    validate_matrix(rhs);
    require(rhs.size() == input.size() && rhs.front().size() == 1, "right side must be matching column vector");
    auto a = input;
    auto b = rhs;
    const auto rows = a.size();
    const auto columns = a.front().size();
    std::vector<std::size_t> pivots;
    for (std::size_t column = 0, row = 0; column < columns && row < rows; ++column) {
        auto pivot = row;
        while (pivot < rows && a[pivot][column] == 0) {
            ++pivot;
        }
        if (pivot == rows) {
            continue;
        }
        std::swap(a[row], a[pivot]);
        std::swap(b[row], b[pivot]);
        const Rational divisor = a[row][column];
        for (auto& value : a[row]) {
            value = checked(value / divisor);
        }
        b[row][0] = checked(b[row][0] / divisor);
        for (std::size_t i = 0; i < rows; ++i) {
            if (i == row) {
                continue;
            }
            const Rational factor = a[i][column];
            for (std::size_t j = 0; j < columns; ++j) {
                a[i][j] = checked(a[i][j] - factor * a[row][j]);
            }
            b[i][0] = checked(b[i][0] - factor * b[row][0]);
        }
        pivots.push_back(column);
        ++row;
    }
    for (std::size_t row = pivots.size(); row < rows; ++row) {
        if (b[row][0] != 0) {
            result.status = Status::Inconsistent;
            return;
        }
    }
    result.values.assign(columns, 0);
    for (std::size_t row = 0; row < pivots.size(); ++row) {
        result.values[pivots[row]] = b[row][0];
    }
    for (std::size_t column = 0; column < columns; ++column) {
        if (std::find(pivots.begin(), pivots.end(), column) != pivots.end()) {
            continue;
        }
        std::vector<Rational> basis(columns, 0);
        basis[column] = 1;
        for (std::size_t row = 0; row < pivots.size(); ++row) {
            basis[pivots[row]] = -a[row][column];
        }
        result.basis.push_back(std::move(basis));
    }
    // Independently substitute both the particular solution and nullspace basis.
    for (std::size_t row = 0; row < rows; ++row) {
        Rational sum = 0;
        for (std::size_t j = 0; j < columns; ++j) {
            sum = checked(sum + input[row][j] * result.values[j]);
        }
        require(sum == rhs[row][0], "linear solution verification failed");
        for (const auto& basis : result.basis) {
            sum = 0;
            for (std::size_t j = 0; j < columns; ++j) {
                sum = checked(sum + input[row][j] * basis[j]);
            }
            require(sum == 0, "nullspace verification failed");
        }
    }
    result.status = result.basis.empty() ? Status::Ok : Status::Infinite;
}

Result mathematical(const Request& request) {
    Result result;
    result.operation = request.operation;
    result.status = Status::Ok;
    switch (request.operation) {
    case Operation::Evaluate:
        result.values.push_back(evaluate(request.expression, false).constant);
        break;
    case Operation::Solve: {
        const auto expression = evaluate(request.expression, true);
        if (expression.coefficient == 0) {
            result.status = expression.constant == 0 ? Status::Infinite : Status::Inconsistent;
        } else {
            const Rational solution = checked(-expression.constant / expression.coefficient);
            require(checked(expression.coefficient * solution + expression.constant) == 0,
                    "equation substitution failed");
            result.values.push_back(solution);
        }
        break;
    }
    case Operation::Determinant:
        result.values.push_back(determinant(request.matrix));
        break;
    case Operation::Linear:
        linear(request.matrix, request.rhs, result);
        break;
    case Operation::Born: {
        validate_matrix(request.matrix);
        require(request.matrix.front().size() == 2, "Born amplitudes require real,imaginary pairs");
        Rational norm = 0;
        for (const auto& amplitude : request.matrix) {
            Rational probability = checked(amplitude[0] * amplitude[0] + amplitude[1] * amplitude[1]);
            norm = checked(norm + probability);
            result.values.push_back(std::move(probability));
        }
        require(norm == 1, "Born state must have exact unit norm");
        break;
    }
    default:
        throw std::runtime_error("unknown mathematical operation");
    }
    return result;
}

struct Search {
    const Module& module;
    Limits limits;
    Result& result;
    std::vector<bool> active;
    std::vector<std::uint32_t> proved;

    bool event(EventKind kind, std::uint32_t literal, std::uint32_t rule, std::uint32_t depth) {
        if (result.history.size() == limits.events) {
            result.limited = true;
            return false;
        }
        result.history.push_back({kind, literal, rule, depth});
        return true;
    }

    std::uint32_t derive(std::uint32_t literal, std::uint32_t depth) {
        if (!event(EventKind::Visit, literal, no_step, depth)) {
            return no_step;
        }
        if (depth > limits.depth) {
            result.limited = true;
            event(EventKind::Bound, literal, no_step, depth);
            return no_step;
        }
        if (proved[literal] != no_step) {
            event(EventKind::Cached, literal, no_step, depth);
            return proved[literal];
        }
        if (active[literal]) {
            event(EventKind::Cycle, literal, no_step, depth);
            return no_step;
        }
        active[literal] = true;
        for (std::uint32_t id = 0; id < module.rules.size(); ++id) {
            const auto& rule = module.rules[id];
            if (rule.head != literal) {
                continue;
            }
            if (!event(EventKind::TryRule, literal, id, depth)) {
                break;
            }
            std::vector<std::uint32_t> dependencies;
            bool success = true;
            for (auto premise : rule.premises) {
                const auto step = derive(premise, depth + 1);
                if (step == no_step) {
                    success = false;
                    break;
                }
                dependencies.push_back(step);
            }
            if (success) {
                // Every retained step is closed over previously proved facts.
                // Failed branches never introduce assumptions into this cache.
                if (!event(EventKind::Derived, literal, id, depth)) {
                    break;
                }
                const auto index = static_cast<std::uint32_t>(result.proof.size());
                result.proof.push_back({literal, id, std::move(dependencies)});
                proved[literal] = index;
                active[literal] = false;
                return index;
            }
            if (!event(EventKind::Backtrack, literal, id, depth)) {
                break;
            }
        }
        active[literal] = false;
        // Failed results are deliberately not memoized: a path-local cycle
        // does not establish global failure when another branch can succeed.
        return no_step;
    }
};

void gpu_search(Search& search, const RuleMatcher& matcher) {
    require(static_cast<bool>(matcher), "GPU requested but no GPU backend supplied");
    require(search.module.symbols.size() * 2 <= 64, "GPU proof supports at most 64 signed literals");
    require(!search.module.rules.empty(), "GPU proof requires a nonempty rule batch");
    std::vector<std::uint64_t> masks;
    for (const auto& rule : search.module.rules) {
        std::uint64_t mask = 0;
        for (auto literal : rule.premises) {
            mask |= std::uint64_t{1} << literal;
        }
        masks.push_back(mask);
    }
    std::uint64_t known = 0;
    for (std::uint32_t round = 0;; ++round) {
        if (round > search.limits.depth || search.result.history.size() == search.limits.events) {
            search.result.limited = true;
            break;
        }
        const auto matches = matcher(masks, known);
        require(matches.size() == masks.size(), "GPU returned wrong eligibility count");
        // Check proposals, including false negatives, against the scalar contract.
        // GPU eligibility is never trusted as a proof certificate.
        for (std::size_t i = 0; i < masks.size(); ++i) {
            require(matches[i] == static_cast<std::uint8_t>((masks[i] & known) == masks[i]),
                    "GPU eligibility failed independent CPU validation");
        }
        bool changed = false;
        for (std::uint32_t i = 0; i < search.module.rules.size(); ++i) {
            const auto& rule = search.module.rules[i];
            if (!search.event(EventKind::TryRule, rule.head, i, round)) {
                break;
            }
            if (!matches[i] || search.proved[rule.head] != no_step) {
                continue;
            }
            std::vector<std::uint32_t> dependencies;
            for (auto premise : rule.premises) {
                require(search.proved[premise] != no_step, "GPU proposal lacks checked premise");
                dependencies.push_back(search.proved[premise]);
            }
            if (!search.event(EventKind::Derived, rule.head, i, round)) {
                break;
            }
            const auto step = static_cast<std::uint32_t>(search.result.proof.size());
            search.result.proof.push_back({rule.head, i, std::move(dependencies)});
            search.proved[rule.head] = step;
            known |= std::uint64_t{1} << rule.head;
            changed = true;
        }
        if (search.result.limited || !changed) {
            break;
        }
    }
    search.result.positive_root = search.proved[search.result.goal];
    search.result.negative_root = search.proved[search.result.goal ^ 1U];
}

void validate_proof_result(const Result& result) {
    validate_module(result.module);
    require(result.goal < result.module.symbols.size() * 2, "invalid proof goal");
    require(verify_proof(result.module, result.proof), "proof replay failed");
    const auto root_valid = [&](std::uint32_t root, std::uint32_t literal) {
        return root == no_step ||
               (root < result.proof.size() && result.proof[root].literal == literal);
    };
    require(root_valid(result.positive_root, result.goal) &&
            root_valid(result.negative_root, result.goal ^ 1U), "proof root mismatch");
    const bool positive = result.positive_root != no_step;
    const bool negative = result.negative_root != no_step;
    switch (result.status) {
    case Status::Contradiction:
        require(positive && negative, "contradiction requires two proofs");
        break;
    case Status::Proved:
        require(positive && !negative && !result.limited, "invalid proved status");
        break;
    case Status::Disproved:
        require(negative && !positive && !result.limited, "invalid disproved status");
        break;
    case Status::Unknown:
        require(!(positive && negative), "two proofs require contradiction");
        break;
    default:
        throw std::runtime_error("invalid logical status");
    }
    // An independent monotone closure guards status against search/controller
    // errors. This checker is finite and never contributes candidate proofs.
    if (result.status == Status::Proved || result.status == Status::Disproved) {
        std::vector<bool> closure(result.module.symbols.size() * 2, false);
        bool changed = true;
        while (changed) {
            changed = false;
            for (const auto& rule : result.module.rules) {
                if (!closure[rule.head] &&
                    std::all_of(rule.premises.begin(), rule.premises.end(),
                                [&](auto premise) { return closure[premise]; })) {
                    closure[rule.head] = true;
                    changed = true;
                }
            }
        }
        require(!(closure[result.goal] && closure[result.goal ^ 1U]),
                "status hides derivable contradiction");
    }
    require(result.history.size() <= max_events, "history limit exceeded");
    for (const auto& event : result.history) {
        require(event.literal < result.module.symbols.size() * 2 &&
                (event.rule == no_step || event.rule < result.module.rules.size()) &&
                event.depth <= 129 &&
                event.kind >= EventKind::Visit && event.kind <= EventKind::Bound,
                "invalid search history entry");
    }
}

} // namespace

bool verify_proof(const Module& module, const std::vector<ProofStep>& proof) {
    try {
        validate_module(module);
        require(proof.size() <= max_symbols * 2, "proof step limit exceeded");
        for (std::size_t i = 0; i < proof.size(); ++i) {
            const auto& step = proof[i];
            require(step.rule < module.rules.size(), "invalid proof rule");
            const auto& rule = module.rules[step.rule];
            require(step.literal == rule.head && step.dependencies.size() == rule.premises.size(),
                    "proof rule mismatch");
            for (std::size_t j = 0; j < step.dependencies.size(); ++j) {
                const auto dependency = step.dependencies[j];
                require(dependency < i && proof[dependency].literal == rule.premises[j],
                        "invalid proof dependency");
            }
        }
        return true;
    } catch (const std::runtime_error&) {
        return false;
    }
}

Result execute(const Request& request, const RuleMatcher& gpu_matcher) {
    if (request.operation != Operation::Prove) {
        require(!request.gpu, "GPU option is supported only for prove");
        return mathematical(request);
    }
    validate_module(request.module);
    require(request.goal < request.module.symbols.size() * 2, "invalid goal");
    require(request.limits.events > 0 && request.limits.events <= max_events &&
            request.limits.depth > 0 && request.limits.depth <= 128, "invalid search limits");
    Result result;
    result.operation = Operation::Prove;
    result.module = request.module;
    result.goal = request.goal;
    result.gpu = request.gpu;
    const auto literal_count = request.module.symbols.size() * 2;
    Search search{request.module, request.limits, result,
                  std::vector<bool>(literal_count, false),
                  std::vector<std::uint32_t>(literal_count, no_step)};
    if (request.gpu) {
        gpu_search(search, gpu_matcher);
    } else {
        result.positive_root = search.derive(request.goal, 0);
        result.negative_root = search.derive(request.goal ^ 1U, 0);
    }
    const bool positive = result.positive_root != no_step;
    const bool negative = result.negative_root != no_step;
    if (positive && negative) {
        result.status = Status::Contradiction;
    } else if (result.limited) {
        result.status = Status::Unknown;
    } else if (positive) {
        result.status = Status::Proved;
    } else if (negative) {
        result.status = Status::Disproved;
    } else {
        result.status = Status::Unknown;
    }
    validate_proof_result(result);
    return result;
}

namespace {

struct Writer {
    Bytes bytes;
    void u8(std::uint8_t value) {
        require(bytes.size() < max_wire_bytes, "binary record too large");
        bytes.push_back(value);
    }
    void u32(std::uint32_t value) {
        for (unsigned shift = 0; shift < 32; shift += 8) {
            u8(static_cast<std::uint8_t>(value >> shift));
        }
    }
    void count(std::size_t value, std::size_t maximum) {
        require(value <= maximum, "binary count limit exceeded");
        u32(static_cast<std::uint32_t>(value));
    }
    void text(const std::string& text) {
        count(text.size(), 1024);
        for (unsigned char c : text) {
            u8(c);
        }
    }
    void magnitude(const mpz_class& integer) {
        const mpz_class absolute = integer < 0 ? -integer : integer;
        require(mpz_sizeinbase(absolute.get_mpz_t(), 2) <= max_bits, "integer bit limit exceeded");
        Bytes buffer((mpz_sizeinbase(absolute.get_mpz_t(), 2) + 7) / 8);
        std::size_t size = 0;
        mpz_export(buffer.data(), &size, -1, 1, 0, 0, absolute.get_mpz_t());
        count(size, max_bits / 8);
        for (std::size_t i = 0; i < size; ++i) {
            u8(buffer[i]);
        }
    }
    void rational(const Rational& value) {
        bounded(value);
        const auto q = checked(value);
        u8(q < 0 ? 1 : 0);
        magnitude(q.get_num());
        magnitude(q.get_den());
    }
    void matrix(const Matrix& matrix, bool optional = false) {
        if (!(optional && matrix.empty())) {
            validate_matrix(matrix);
        }
        count(matrix.size(), max_dimension);
        if (matrix.empty()) {
            return;
        }
        count(matrix.front().size(), max_dimension);
        for (const auto& row : matrix) {
            for (const auto& value : row) {
                rational(value);
            }
        }
    }
    void module(const Module& module) {
        validate_module(module);
        text(module.name);
        text(module.version);
        count(module.symbols.size(), max_symbols);
        for (const auto& symbol : module.symbols) {
            text(symbol);
        }
        count(module.rules.size(), max_rules);
        for (const auto& rule : module.rules) {
            u32(rule.head);
            count(rule.premises.size(), max_premises);
            for (auto premise : rule.premises) {
                u32(premise);
            }
            text(rule.provenance);
        }
    }
};

struct Reader {
    std::span<const std::uint8_t> bytes;
    std::size_t position = 0;
    std::uint8_t u8() {
        require(position < bytes.size(), "truncated binary record");
        return bytes[position++];
    }
    std::uint32_t u32() {
        std::uint32_t result = 0;
        for (unsigned shift = 0; shift < 32; shift += 8) {
            result |= static_cast<std::uint32_t>(u8()) << shift;
        }
        return result;
    }
    std::size_t count(std::size_t maximum) {
        const auto size = u32();
        require(size <= maximum, "binary count limit exceeded");
        return size;
    }
    std::string text() {
        const auto size = count(1024);
        require(size <= bytes.size() - position, "truncated binary string");
        std::string result;
        result.reserve(size);
        for (std::size_t i = 0; i < size; ++i) {
            const auto c = u8();
            require(c >= 32 && c != 127, "control byte in metadata");
            result.push_back(static_cast<char>(c));
        }
        return result;
    }
    mpz_class magnitude() {
        const auto size = count(max_bits / 8);
        require(size <= bytes.size() - position, "truncated integer");
        require(size == 0 || bytes[position + size - 1] != 0, "noncanonical integer");
        mpz_class result;
        mpz_import(result.get_mpz_t(), size, -1, 1, 0, 0, bytes.data() + position);
        position += size;
        return result;
    }
    Rational rational() {
        const auto sign = u8();
        require(sign <= 1, "invalid rational sign");
        auto numerator = magnitude();
        const auto denominator = magnitude();
        require(denominator > 0 && !(numerator == 0 && sign), "invalid rational representation");
        if (sign) {
            numerator = -numerator;
        }
        Rational result(numerator, denominator);
        result.canonicalize();
        require(result.get_num() == numerator && result.get_den() == denominator,
                "noncanonical rational");
        bounded(result);
        return result;
    }
    Matrix matrix(bool optional = false) {
        const auto rows = count(max_dimension);
        if (optional && rows == 0) {
            return {};
        }
        const auto columns = count(max_dimension);
        require(rows > 0 && columns > 0, "empty matrix");
        Matrix result(rows, std::vector<Rational>(columns));
        for (auto& row : result) {
            for (auto& value : row) {
                value = rational();
            }
        }
        return result;
    }
    Module module() {
        Module result;
        result.name = text();
        result.version = text();
        const auto symbols = count(max_symbols);
        for (std::size_t i = 0; i < symbols; ++i) {
            result.symbols.push_back(text());
        }
        const auto rules = count(max_rules);
        for (std::size_t i = 0; i < rules; ++i) {
            Rule rule;
            rule.head = u32();
            const auto premises = count(max_premises);
            for (std::size_t j = 0; j < premises; ++j) {
                rule.premises.push_back(u32());
            }
            rule.provenance = text();
            result.rules.push_back(std::move(rule));
        }
        validate_module(result);
        return result;
    }
    void end() const {
        require(position == bytes.size(), "trailing binary data");
    }
};

Bytes envelope(std::uint8_t type, const Writer& payload) {
    require(payload.bytes.size() <= max_wire_bytes - 12, "binary payload too large");
    Writer writer;
    for (auto byte : Bytes{'A', 'X', 'R', 'M', 1, 0, type, 0}) {
        writer.u8(byte);
    }
    writer.u32(static_cast<std::uint32_t>(payload.bytes.size()));
    for (auto byte : payload.bytes) {
        writer.u8(byte);
    }
    return writer.bytes;
}

Reader payload(std::span<const std::uint8_t> bytes, std::uint8_t type) {
    require(bytes.size() <= max_wire_bytes, "binary record too large");
    Reader reader{bytes};
    require(reader.u8() == 'A' && reader.u8() == 'X' && reader.u8() == 'R' && reader.u8() == 'M',
            "invalid binary magic");
    require(reader.u8() == 1 && reader.u8() == 0, "unsupported binary version");
    require(reader.u8() == type && reader.u8() == 0, "wrong binary record type or flags");
    const auto length = reader.count(max_wire_bytes - 12);
    require(length == bytes.size() - reader.position, "binary length mismatch");
    return Reader{bytes.subspan(reader.position)};
}

Operation read_operation(Reader& reader) {
    const auto operation = static_cast<Operation>(reader.u8());
    require(operation >= Operation::Evaluate && operation <= Operation::Prove, "invalid operation");
    return operation;
}

void validate_math_result(const Result& result) {
    require(result.values.size() <= max_dimension, "too many result values");
    for (const auto& value : result.values) {
        bounded(value);
    }
    require(result.status == Status::Ok || result.status == Status::Infinite ||
            result.status == Status::Inconsistent, "invalid mathematical status");
    require(!result.limited && !result.gpu, "invalid mathematical execution flag");
    switch (result.operation) {
    case Operation::Evaluate:
    case Operation::Determinant:
        require(result.status == Status::Ok && result.values.size() == 1 && result.basis.empty(),
                "invalid scalar result");
        break;
    case Operation::Solve:
        require(result.basis.empty() &&
                result.values.size() == (result.status == Status::Ok ? 1U : 0U), "invalid solve result");
        break;
    case Operation::Born: {
        require(result.status == Status::Ok && !result.values.empty() && result.basis.empty(),
                "invalid Born result");
        Rational sum = 0;
        for (const auto& value : result.values) {
            require(value >= 0 && value <= 1, "invalid probability");
            sum = checked(sum + value);
        }
        require(sum == 1, "probabilities do not normalize");
        break;
    }
    case Operation::Linear:
        if (result.status == Status::Inconsistent) {
            require(result.values.empty() && result.basis.empty(), "inconsistent result has solution");
        } else {
            require(!result.values.empty(), "missing linear solution");
            require((result.status == Status::Infinite) == !result.basis.empty(), "invalid nullspace status");
            if (!result.basis.empty()) {
                validate_matrix(result.basis);
                require(result.basis.front().size() == result.values.size(), "invalid nullspace shape");
            }
        }
        break;
    default:
        throw std::runtime_error("invalid result operation");
    }
}

} // namespace

Bytes encode_request(const Request& request) {
    Writer writer;
    writer.u8(static_cast<std::uint8_t>(request.operation));
    switch (request.operation) {
    case Operation::Evaluate:
    case Operation::Solve:
        writer.count(request.expression.size(), max_expression);
        for (const auto& instruction : request.expression) {
            require(instruction.code >= Code::Constant && instruction.code <= Code::Negate,
                    "invalid instruction");
            writer.u8(static_cast<std::uint8_t>(instruction.code));
            if (instruction.code == Code::Constant) {
                writer.rational(instruction.value);
            }
        }
        break;
    case Operation::Determinant:
    case Operation::Born:
        writer.matrix(request.matrix);
        break;
    case Operation::Linear:
        writer.matrix(request.matrix);
        writer.matrix(request.rhs);
        break;
    case Operation::Prove:
        writer.module(request.module);
        writer.u32(request.goal);
        writer.u32(request.limits.events);
        writer.u32(request.limits.depth);
        writer.u8(request.gpu ? 1 : 0);
        break;
    default:
        throw std::runtime_error("invalid operation");
    }
    return envelope(1, writer);
}

Request decode_request(std::span<const std::uint8_t> bytes) {
    auto reader = payload(bytes, 1);
    Request request;
    request.operation = read_operation(reader);
    switch (request.operation) {
    case Operation::Evaluate:
    case Operation::Solve: {
        const auto count = reader.count(max_expression);
        require(count > 0, "empty expression");
        std::size_t stack = 0;
        for (std::size_t i = 0; i < count; ++i) {
            Instruction instruction;
            instruction.code = static_cast<Code>(reader.u8());
            require(instruction.code >= Code::Constant && instruction.code <= Code::Negate,
                    "invalid instruction");
            if (instruction.code == Code::Constant) {
                instruction.value = reader.rational();
                ++stack;
            } else if (instruction.code == Code::Variable) {
                require(request.operation == Operation::Solve, "variable in evaluation request");
                ++stack;
            } else if (instruction.code == Code::Negate) {
                require(stack > 0, "invalid expression stack");
            } else {
                require(stack >= 2, "invalid expression stack");
                --stack;
            }
            request.expression.push_back(std::move(instruction));
        }
        require(stack == 1, "invalid expression stack");
        break;
    }
    case Operation::Determinant:
    case Operation::Born:
        request.matrix = reader.matrix();
        break;
    case Operation::Linear:
        request.matrix = reader.matrix();
        request.rhs = reader.matrix();
        break;
    case Operation::Prove:
        request.module = reader.module();
        request.goal = reader.u32();
        request.limits.events = reader.u32();
        request.limits.depth = reader.u32();
        {
            const auto gpu = reader.u8();
            require(gpu <= 1, "invalid GPU flag");
            request.gpu = gpu != 0;
        }
        require(request.goal < request.module.symbols.size() * 2, "invalid goal");
        require(request.limits.events > 0 && request.limits.events <= max_events &&
                request.limits.depth > 0 && request.limits.depth <= 128, "invalid search limits");
        break;
    }
    reader.end();
    return request;
}

Bytes encode_result(const Result& result) {
    Writer writer;
    writer.u8(static_cast<std::uint8_t>(result.operation));
    writer.u8(static_cast<std::uint8_t>(result.status));
    writer.u8(result.limited ? 1 : 0);
    writer.u8(result.gpu ? 1 : 0);
    if (result.operation == Operation::Prove) {
        validate_proof_result(result);
        writer.module(result.module);
        writer.u32(result.goal);
        writer.u32(result.positive_root);
        writer.u32(result.negative_root);
        writer.count(result.proof.size(), max_symbols * 2);
        for (const auto& step : result.proof) {
            writer.u32(step.literal);
            writer.u32(step.rule);
            writer.count(step.dependencies.size(), max_premises);
            for (auto dependency : step.dependencies) {
                writer.u32(dependency);
            }
        }
        writer.count(result.history.size(), max_events);
        for (const auto& event : result.history) {
            writer.u8(static_cast<std::uint8_t>(event.kind));
            writer.u32(event.literal);
            writer.u32(event.rule);
            writer.u32(event.depth);
        }
    } else {
        validate_math_result(result);
        writer.count(result.values.size(), max_dimension);
        for (const auto& value : result.values) {
            writer.rational(value);
        }
        writer.matrix(result.basis, true);
    }
    return envelope(2, writer);
}

Result decode_result(std::span<const std::uint8_t> bytes) {
    auto reader = payload(bytes, 2);
    Result result;
    result.operation = read_operation(reader);
    result.status = static_cast<Status>(reader.u8());
    require(result.status >= Status::Ok && result.status <= Status::Contradiction, "invalid status");
    const auto limited = reader.u8();
    require(limited <= 1, "invalid limit flag");
    result.limited = limited != 0;
    const auto gpu = reader.u8();
    require(gpu <= 1, "invalid GPU flag");
    result.gpu = gpu != 0;
    if (result.operation == Operation::Prove) {
        result.module = reader.module();
        result.goal = reader.u32();
        result.positive_root = reader.u32();
        result.negative_root = reader.u32();
        const auto count = reader.count(max_symbols * 2);
        for (std::size_t i = 0; i < count; ++i) {
            ProofStep step;
            step.literal = reader.u32();
            step.rule = reader.u32();
            const auto dependencies = reader.count(max_premises);
            for (std::size_t j = 0; j < dependencies; ++j) {
                step.dependencies.push_back(reader.u32());
            }
            result.proof.push_back(std::move(step));
        }
        const auto history = reader.count(max_events);
        for (std::size_t i = 0; i < history; ++i) {
            SearchEvent event;
            event.kind = static_cast<EventKind>(reader.u8());
            event.literal = reader.u32();
            event.rule = reader.u32();
            event.depth = reader.u32();
            result.history.push_back(event);
        }
        validate_proof_result(result);
    } else {
        const auto count = reader.count(max_dimension);
        for (std::size_t i = 0; i < count; ++i) {
            result.values.push_back(reader.rational());
        }
        result.basis = reader.matrix(true);
        validate_math_result(result);
    }
    reader.end();
    return result;
}

Bytes run_binary(std::span<const std::uint8_t> request, const RuleMatcher& gpu_matcher) {
    return encode_result(execute(decode_request(request), gpu_matcher));
}

Bytes read_binary(std::istream& input) {
    Bytes result;
    char c = 0;
    while (input.get(c)) {
        require(result.size() < max_wire_bytes, "binary stream too large");
        result.push_back(static_cast<std::uint8_t>(static_cast<unsigned char>(c)));
    }
    require(input.eof(), "binary stream read failure");
    return result;
}

void write_binary(std::ostream& output, const Bytes& bytes) {
    output.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    require(static_cast<bool>(output), "binary stream write failure");
}

namespace {

class ExpressionParser {
    const std::string& text;
    std::size_t position = 0;
    std::vector<Instruction> code;

    void space() {
        while (position < text.size() && std::isspace(static_cast<unsigned char>(text[position]))) {
            ++position;
        }
    }
    bool take(char c) {
        space();
        if (position < text.size() && text[position] == c) {
            ++position;
            return true;
        }
        return false;
    }
    void emit(Code opcode, Rational value = 0) {
        require(code.size() < max_expression, "expression instruction limit exceeded");
        code.push_back({opcode, std::move(value)});
    }
    void atom(std::size_t depth) {
        require(depth <= 128, "expression nesting limit exceeded");
        if (take('+')) {
            atom(depth + 1);
        } else if (take('-')) {
            atom(depth + 1);
            emit(Code::Negate);
        } else if (take('(')) {
            sum(depth + 1);
            require(take(')'), "missing closing parenthesis");
        } else if (take('x')) {
            emit(Code::Variable);
        } else {
            space();
            const auto start = position;
            while (position < text.size() && text[position] >= '0' && text[position] <= '9') {
                ++position;
            }
            require(position > start, "expected integer, x, or parenthesized expression");
            require(position - start <= 1234, "integer digit limit exceeded");
            Rational value(mpz_class(text.substr(start, position - start), 10));
            bounded(value);
            emit(Code::Constant, std::move(value));
        }
    }
    void product(std::size_t depth) {
        atom(depth);
        for (;;) {
            if (take('*')) {
                atom(depth);
                emit(Code::Multiply);
            } else if (take('/')) {
                atom(depth);
                emit(Code::Divide);
            } else {
                return;
            }
        }
    }
    void sum(std::size_t depth) {
        product(depth);
        for (;;) {
            if (take('+')) {
                product(depth);
                emit(Code::Add);
            } else if (take('-')) {
                product(depth);
                emit(Code::Subtract);
            } else {
                return;
            }
        }
    }

public:
    explicit ExpressionParser(const std::string& input) : text(input) {}
    std::vector<Instruction> parse() {
        require(text.size() <= max_text_bytes, "expression text too large");
        sum(0);
        space();
        require(position == text.size(), "unsupported or trailing expression syntax");
        return code;
    }
};

Matrix parse_matrix(const std::string& source) {
    require(source.size() <= max_text_bytes, "matrix text too large");
    const auto text = trim(source);
    require(text.size() >= 2 && text.front() == '[' && text.back() == ']', "matrix requires brackets");
    Matrix result;
    const auto rows = split(text.substr(1, text.size() - 2), ';');
    require(rows.size() <= max_dimension, "matrix row limit exceeded");
    for (const auto& row : rows) {
        std::vector<Rational> values;
        const auto fields = split(row, ',');
        require(fields.size() <= max_dimension, "matrix column limit exceeded");
        for (const auto& field : fields) {
            values.push_back(evaluate(ExpressionParser(field).parse(), false).constant);
        }
        result.push_back(std::move(values));
    }
    validate_matrix(result);
    return result;
}

std::uint32_t intern(Module& module, const std::string& source) {
    auto name = trim(source);
    const bool negative = !name.empty() && name.front() == '!';
    if (negative) {
        name.erase(0, 1);
    }
    require(identifier(name), "literal must be an identifier, optionally prefixed by !");
    auto found = std::find(module.symbols.begin(), module.symbols.end(), name);
    if (found == module.symbols.end()) {
        require(module.symbols.size() < max_symbols, "symbol limit exceeded");
        module.symbols.push_back(name);
        return static_cast<std::uint32_t>((module.symbols.size() - 1) * 2 + (negative ? 1 : 0));
    }
    return static_cast<std::uint32_t>(static_cast<std::size_t>(found - module.symbols.begin()) * 2 +
                                      (negative ? 1 : 0));
}

std::string literal_name(const Module& module, std::uint32_t literal) {
    require(literal < module.symbols.size() * 2, "invalid literal display");
    return (literal % 2 ? "!" : "") + module.symbols[literal / 2];
}

const char* status_name(Status status) {
    switch (status) {
    case Status::Ok: return "OK";
    case Status::Infinite: return "INFINITE_SOLUTIONS";
    case Status::Inconsistent: return "INCONSISTENT";
    case Status::Proved: return "PROVED";
    case Status::Disproved: return "DISPROVED";
    case Status::Unknown: return "UNKNOWN";
    case Status::Contradiction: return "CONTRADICTION";
    }
    throw std::runtime_error("invalid status");
}

} // namespace

Module parse_module(const std::string& source) {
    require(source.size() <= max_text_bytes, "module source too large");
    std::istringstream input(source);
    std::string line;
    Module module;
    bool header = false;
    while (std::getline(input, line)) {
        line = trim(line);
        if (line.empty() || line.front() == '#') {
            continue;
        }
        if (!header) {
            std::istringstream fields(line);
            std::string marker, schema, extra;
            require(static_cast<bool>(fields >> marker >> schema >> module.name >> module.version) &&
                    !(fields >> extra) && marker == "axioms" && schema == "1",
                    "module header must be: axioms 1 NAME VERSION");
            header = true;
            continue;
        }
        require(module.rules.size() < max_rules, "module rule limit exceeded");
        const auto bar = line.find('|');
        require(bar != std::string::npos && line.find('|', bar + 1) == std::string::npos,
                "every module statement requires one | provenance field");
        const auto statement = trim(line.substr(0, bar));
        Rule rule;
        rule.provenance = trim(line.substr(bar + 1));
        if (statement.starts_with("fact ")) {
            rule.head = intern(module, statement.substr(5));
        } else if (statement.starts_with("rule ")) {
            const auto arrow = statement.find("->", 5);
            require(arrow != std::string::npos && statement.find("->", arrow + 2) == std::string::npos,
                    "rule requires exactly one ->");
            const auto premises = split(statement.substr(5, arrow - 5), '&');
            require(!premises.empty() && premises.size() <= max_premises, "invalid premise count");
            for (const auto& premise : premises) {
                rule.premises.push_back(intern(module, premise));
            }
            rule.head = intern(module, statement.substr(arrow + 2));
        } else {
            throw std::runtime_error("module statement must start with fact or rule");
        }
        module.rules.push_back(std::move(rule));
    }
    require(header, "missing module header");
    validate_module(module);
    return module;
}

Module load_module(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    require(static_cast<bool>(input), "cannot open module");
    std::string source;
    char c = 0;
    while (input.get(c)) {
        require(source.size() < max_text_bytes, "module source too large");
        source.push_back(c);
    }
    require(input.eof(), "module read failure");
    return parse_module(source);
}

Request compile_proof(Module module, const std::string& goal) {
    validate_module(module);
    Request request;
    request.operation = Operation::Prove;
    request.goal = intern(module, goal);
    request.module = std::move(module);
    return request;
}

Request compile(const std::vector<std::string>& arguments) {
    require(!arguments.empty(), "missing controlled-language command");
    for (const auto& argument : arguments) {
        require(argument.size() <= max_text_bytes, "argument too large");
    }
    Request request;
    const auto& command = arguments.front();
    if (command == "eval" || command == "evaluate" || command == "solve") {
        require(arguments.size() == 2, "eval/solve requires one quoted expression");
        if (command == "solve") {
            request.operation = Operation::Solve;
            const auto equals = arguments[1].find('=');
            require(equals != std::string::npos && arguments[1].find('=', equals + 1) == std::string::npos,
                    "solve requires exactly one equals sign");
            const auto left = arguments[1].substr(0, equals);
            const auto right = arguments[1].substr(equals + 1);
            request.expression = ExpressionParser(left).parse();
            const auto rhs = ExpressionParser(right).parse();
            request.expression.insert(request.expression.end(), rhs.begin(), rhs.end());
            request.expression.push_back({Code::Subtract, 0});
        } else {
            request.expression = ExpressionParser(arguments[1]).parse();
        }
        // Reject unsupported affine operations at the text boundary as well.
        static_cast<void>(evaluate(request.expression, request.operation == Operation::Solve));
    } else if (command == "det" || command == "born") {
        require(arguments.size() == 2, "det/born requires one quoted matrix");
        request.operation = command == "det" ? Operation::Determinant : Operation::Born;
        request.matrix = parse_matrix(arguments[1]);
    } else if (command == "linear") {
        require(arguments.size() == 3, "linear requires coefficient matrix and column right side");
        request.operation = Operation::Linear;
        request.matrix = parse_matrix(arguments[1]);
        request.rhs = parse_matrix(arguments[2]);
    } else if (command == "prove") {
        require(arguments.size() == 3 || (arguments.size() == 4 && arguments[3] == "--gpu"),
                "prove requires module path, literal goal, and optional --gpu");
        request = compile_proof(load_module(arguments[1]), arguments[2]);
        request.gpu = arguments.size() == 4;
    } else {
        throw std::runtime_error("unsupported controlled-language command; use --help");
    }
    return request;
}

std::string render(const Result& result) {
    std::ostringstream output;
    output << "status: " << status_name(result.status) << '\n';
    if (result.operation == Operation::Prove) {
        validate_proof_result(result);
        output << "goal: " << literal_name(result.module, result.goal)
               << "\nmodule: " << std::quoted(result.module.name) << " version "
               << std::quoted(result.module.version)
               << "\nsemantics: ground Horn rules; explicit negation; no explosion"
               << "\nproofs are conditional on the declared module axioms"
               << "\nbackend: " << (result.gpu ? "GPU eligibility; CPU certificate replay" : "CPU backward search")
               << "\nsearch: " << result.history.size() << " recorded events; limit reached: "
               << (result.limited ? "yes" : "no") << '\n';
        for (std::size_t i = 0; i < result.proof.size(); ++i) {
            const auto& step = result.proof[i];
            output << "step " << i << ": " << literal_name(result.module, step.literal)
                   << " by rule " << step.rule << " using [";
            for (std::size_t j = 0; j < step.dependencies.size(); ++j) {
                output << (j ? "," : "") << step.dependencies[j];
            }
            output << "] source " << std::quoted(result.module.rules[step.rule].provenance) << '\n';
        }
        if (result.positive_root != no_step) {
            output << "goal proof root: " << result.positive_root << '\n';
        }
        if (result.negative_root != no_step) {
            output << "opposite proof root: " << result.negative_root << '\n';
        }
    } else {
        validate_math_result(result);
        if (result.operation == Operation::Born) {
            output << "scope: finite rational-complex pure state; computational-basis Born probabilities\n";
        }
        if (result.operation == Operation::Solve && result.status == Status::Ok) {
            output << "x = " << result.values.front().get_str() << '\n';
        } else if (!result.values.empty()) {
            output << (result.operation == Operation::Linear ? "particular solution: [" : "values: [");
            for (std::size_t i = 0; i < result.values.size(); ++i) {
                output << (i ? "," : "") << result.values[i].get_str();
            }
            output << "]\n";
        }
        for (std::size_t i = 0; i < result.basis.size(); ++i) {
            output << "free parameter " << i << " times [";
            for (std::size_t j = 0; j < result.basis[i].size(); ++j) {
                output << (j ? "," : "") << result.basis[i][j].get_str();
            }
            output << "]\n";
        }
    }
    return output.str();
}

std::string help() {
    return R"HELP(axiom: bounded C++20 symbolic reasoning; no trained neural model exists.
Commands (quote expressions/matrices as one shell argument):
  eval "1/3 + 2*(5-1)"       exact rational arithmetic (evaluate is an alias)
  solve "2*x+3=11"           affine equation in x over the rationals
  det "[1,2;3,4]"           square matrix determinant
  linear "[1,2;3,4]" "[5;6]"  rectangular A*x=b; particular solution + nullspace
  born "[3/5,0;0,4/5]"      amplitudes as real,imaginary rows; exact unit norm
  prove MODULE GOAL [--gpu] [--library ROOT]
  compile COMMAND ARGS...   binary request on stdout (no informational text)
  run                       binary request on stdin, binary result on stdout
  decode                    binary result on stdin, deterministic text on stdout
  search ROOT QUERY         cited library evidence, never automatic axioms
  library-check ROOT        inspect archive availability
  gpu-test                  actual GPU rule-kernel parity test; failure is nonzero
  --help                    this grammar and scope

Scalar grammar: decimal integer literals, x (solve only), parentheses, unary
+/-, binary + - * /; multiplication/division bind tighter and associate left.
No implicit multiplication, decimals, powers, radicals, inequalities, units,
arbitrary English, nonlinear products, or variable denominators.
Matrices: [EXPR,EXPR;EXPR,EXPR], nonempty rectangular, dimensions 1..16.
Born task: 1..16 rational-complex amplitudes, sum(re^2+im^2)=1 exactly.
This is a finite measurement model, not arbitrary quantum physics.

Module text grammar (one statement per line; # starts a whole-line comment):
  axioms 1 NAME VERSION
  fact LITERAL | nonempty provenance
  rule LITERAL & LITERAL -> LITERAL | nonempty provenance
LITERAL = optional ! followed by [A-Za-z_][A-Za-z0-9_.]* (max 128 bytes).
Rules are ground/propositional: no implicit variables or quantifiers.
Rule order controls deterministic search. Cycles backtrack, proved facts cache,
failed path-local goals do not cache. Both polarities are searched and replayed.
PROVED/DISPROVED require a checked goal/opposite proof and completed search.
CONTRADICTION means both goal polarities were proved; unrelated conflicts do not
explode. Neither proof means UNKNOWN, never false. Bounds also yield UNKNOWN.
Proofs are conditional on module axioms, not claims that their sources are true.
Library lookup after UNKNOWN is evidence only and cannot change proof status.
--gpu selects forward closure with actual CUDA eligibility proposals and CPU
validation/replay. At most 64 signed literals (32 symbols); no silent fallback.
GPU selection is preserved by compile/run. Exact mathematics remains CPU-only.

Limits: 64 KiB text/module; 512 instructions; expression nesting 128; rational
numerator/denominator 4096 bits; 1024 symbols; 512 rules; 16 premises per rule;
4096 search/history events; proof depth 128; 1 MiB binary record.
Binary v1: AXRM magic, u16 little-endian version=1, u8 type (1 request/2 result),
u8 reserved=0, u32 little-endian payload length, typed payload. No trailing bytes.
Counts/IDs are u32 little-endian; rational sign is u8 followed by length-prefixed
little-endian numerator magnitude and positive denominator, reduced/canonical.
See examples/FORMAT.md for the complete operation-specific wire schema.
Exit codes: 0 valid result (including UNKNOWN), 2 invalid input/operational error;
search returns 1 for incomplete coverage. Binary errors go only to stderr.
)HELP";
}

} // namespace axiom
