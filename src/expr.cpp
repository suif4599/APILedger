#include "expr.h"

#include <cctype>
#include <cmath>
#include <cstdlib>

namespace {

struct Parser {
    const std::string& text;
    const std::function<double(const std::string&)>& lookup;
    size_t position = 0;
    bool ok = true;

    void skip_space() {
        while (position < text.size() &&
               std::isspace(static_cast<unsigned char>(text[position]))) {
            ++position;
        }
    }

    bool expression(double& value) {
        if (!term(value)) return false;
        while (true) {
            skip_space();
            if (position >= text.size() || (text[position] != '+' && text[position] != '-')) {
                break;
            }
            const char op = text[position++];
            double right = 0.0;
            if (!term(right)) return false;
            value = op == '+' ? value + right : value - right;
        }
        return true;
    }

    bool term(double& value) {
        if (!factor(value)) return false;
        while (true) {
            skip_space();
            if (position >= text.size() || (text[position] != '*' && text[position] != '/')) {
                break;
            }
            const char op = text[position++];
            double right = 0.0;
            if (!factor(right)) return false;
            if (op == '*') {
                value *= right;
            } else {
                if (right == 0.0) {
                    ok = false;
                    return false;
                }
                value /= right;
            }
        }
        return true;
    }

    bool factor(double& value) {
        skip_space();
        if (position >= text.size()) return false;

        const char ch = text[position];
        if (ch == '+' || ch == '-') {
            ++position;
            if (!factor(value)) return false;
            if (ch == '-') value = -value;
            return true;
        }

        if (ch == '(') {
            ++position;
            if (!expression(value)) return false;
            skip_space();
            if (position >= text.size() || text[position] != ')') return false;
            ++position;
            return true;
        }

        if (std::isdigit(static_cast<unsigned char>(ch)) || ch == '.') {
            const size_t start = position;
            while (position < text.size() &&
                   (std::isdigit(static_cast<unsigned char>(text[position])) ||
                    text[position] == '.')) {
                ++position;
            }
            const std::string literal = text.substr(start, position - start);
            char* end = nullptr;
            value = std::strtod(literal.c_str(), &end);
            if (end == literal.c_str() || *end != '\0') return false;
            return true;
        }

        if (std::isalpha(static_cast<unsigned char>(ch)) || ch == '_') {
            const size_t start = position;
            while (position < text.size() &&
                   (std::isalnum(static_cast<unsigned char>(text[position])) ||
                    text[position] == '_')) {
                ++position;
            }
            value = lookup(text.substr(start, position - start));
            return true;
        }

        return false;
    }
};

}  // namespace

std::optional<double> compute_field(
    const std::string& expression,
    const std::function<double(const std::string&)>& lookup) {
    if (expression.empty()) return std::nullopt;

    Parser parser{expression, lookup};
    double value = 0.0;
    if (!parser.expression(value)) return std::nullopt;
    parser.skip_space();
    if (parser.position != expression.size() || !parser.ok) return std::nullopt;

    return std::round(value * 1e6) / 1e6;
}
