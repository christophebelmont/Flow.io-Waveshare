#include "ValueExpression.h"
#include <string.h>
#include <stdlib.h>
#include <algorithm>

namespace ValueExpression {
namespace {
uint8_t arity(Op op) {
    switch (op) {
        case Op::Constant: case Op::Parameter: case Op::Load: return 0;
        case Op::Neg: case Op::Abs: return 1;
        case Op::Clamp: return 3;
        default: return 2;
    }
}
bool digit(char c) { return c >= '0' && c <= '9'; }
bool letter(char c) { return c >= 'a' && c <= 'z'; }
class Parser {
public:
    Parser(const char* text, Program& output) : cursor(text), p(output) {}
    bool run() { return expression(0) && (space(), *cursor == '\0') && validate(p); }
private:
    const char* cursor;
    Program& p;
    void space() { while (*cursor == ' ' || *cursor == '\t' || *cursor == '\r' || *cursor == '\n') ++cursor; }
    bool take(char c) { space(); if (*cursor != c) return false; ++cursor; return true; }
    bool emit(Op op, double number = 0, uint8_t operand = 0) {
        if (p.size == MaxInstructions) return false;
        p.code[p.size++] = {number, op, operand}; return true;
    }
    bool expression(unsigned depth) {
        if (!product(depth)) return false;
        for (;;) {
            if (take('+')) { if (!product(depth) || !emit(Op::Add)) return false; }
            else if (take('-')) { if (!product(depth) || !emit(Op::Sub)) return false; }
            else return true;
        }
    }
    bool product(unsigned depth) {
        if (!primary(depth)) return false;
        for (;;) {
            if (take('*')) { if (!primary(depth) || !emit(Op::Mul)) return false; }
            else if (take('/')) { if (!primary(depth) || !emit(Op::Div)) return false; }
            else return true;
        }
    }
    bool primary(unsigned depth) {
        if (depth >= MaxInstructions) return false;
        if (take('-')) return primary(depth + 1) && emit(Op::Neg);
        if (take('(')) return expression(depth + 1) && take(')');
        space();
        if (digit(*cursor) || *cursor == '.') {
            // Explicit decimal grammar excludes hex, NaN, infinity and locale syntax.
            const char* start = cursor;
            bool digits = false;
            while (digit(*cursor)) { ++cursor; digits = true; }
            if (*cursor == '.') { ++cursor; while (digit(*cursor)) { ++cursor; digits = true; } }
            if (!digits) return false;
            if (*cursor == 'e' || *cursor == 'E') {
                ++cursor;
                if (*cursor == '+' || *cursor == '-') ++cursor;
                if (!digit(*cursor)) return false;
                while (digit(*cursor)) ++cursor;
            }
            char* end = nullptr;
            const double value = strtod(start, &end);
            return end == cursor && isfinite(value) && emit(Op::Constant, value);
        }
        const char* start = cursor;
        while (letter(*cursor) || digit(*cursor) || *cursor == '.') ++cursor;
        const size_t length = cursor - start;
        if (!length) return false;
        if (length == 2 && start[0] == 'k' && start[1] >= '0' && start[1] <= '3')
            return emit(Op::Parameter, 0, start[1] - '0');
        if (length >= 3 && digit(start[1]) && digit(start[2])) {
            const unsigned index = (start[1] - '0') * 10 + start[2] - '0';
            ValueId id = VALUE_INVALID;
            if (length == 3 && start[0] == 'a' && index < 32) id = ValueIds::Analog + index;
            if (length == 3 && start[0] == 'v' && index < ValueIds::DerivedCapacity) id = ValueIds::Derived + index;
            if (start[0] == 'i' && index < 16) {
                if (length == 9 && memcmp(start + 3, ".count", 6) == 0) id = ValueIds::Digital + index;
                if (length == 9 && memcmp(start + 3, ".total", 6) == 0) id = ValueIds::Total + index;
                if (length == 8 && memcmp(start + 3, ".rate", 5) == 0) id = ValueIds::PulseRate + index;
                if (length == 8 && memcmp(start + 3, ".flow", 5) == 0) id = ValueIds::ConvertedRate + index;
            }
            if (id == VALUE_INVALID) return false;
            uint8_t dep = 0;
            while (dep < p.dependencyCount && p.dependencies[dep] != id) ++dep;
            if (dep == p.dependencyCount) {
                if (dep == MaxDependencies) return false;
                p.dependencies[p.dependencyCount++] = id;
            }
            return emit(Op::Load, 0, dep);
        }
        struct Function { const char* name; Op op; uint8_t args; };
        static constexpr Function functions[] = {{"min", Op::Min, 2}, {"max", Op::Max, 2},
            {"abs", Op::Abs, 1}, {"clamp", Op::Clamp, 3}};
        for (const auto& f : functions) {
            if (length != strlen(f.name) || memcmp(start, f.name, length)) continue;
            if (!take('(')) return false;
            for (uint8_t i = 0; i < f.args; ++i) {
                if (i && !take(',')) return false;
                if (!expression(depth + 1)) return false;
            }
            return take(')') && emit(f.op);
        }
        return false;
    }
};
Number select(const Number& a, const Number& b, bool minimum) {
    const bool oldA = minimum ? a.previous <= b.previous : a.previous >= b.previous;
    const bool newA = minimum ? a.current <= b.current : a.current >= b.current;
    const auto& old = oldA ? a : b;
    const auto& now = newA ? a : b;
    return {old.previous, now.current, oldA == newA ? now.delta : (now.previous - old.previous) + now.delta};
}
}
bool compile(const char* text, Program& output) {
    output = Program{};
    if (!text) return false;
    size_t length = 0;
    while (length < TextCapacity && text[length]) ++length;
    if (length == TextCapacity) return false;
    if (Parser(text, output).run()) return true;
    output = Program{}; return false;
}
bool validate(const Program& p) {
    if (!p.size || p.size > MaxInstructions || p.dependencyCount > MaxDependencies) return false;
    for (uint8_t i = 0; i < p.dependencyCount; ++i) {
        if (p.dependencies[i] >= ValueIds::Capacity) return false;
        for (uint8_t j = 0; j < i; ++j) if (p.dependencies[i] == p.dependencies[j]) return false;
    }
    unsigned stack = 0;
    for (uint8_t i = 0; i < p.size; ++i) {
        const auto& instruction = p.code[i];
        if (instruction.op > Op::Clamp || !isfinite(instruction.number) ||
            (instruction.op == Op::Load && instruction.operand >= p.dependencyCount) ||
            (instruction.op == Op::Parameter && instruction.operand >= ParamCount)) return false;
        const auto args = arity(instruction.op);
        if (stack < args) return false;
        stack = stack - args + 1;
        if (stack > MaxStack) return false;
    }
    return stack == 1;
}
Program affine(ValueId source) {
    Program p;
    p.dependencies[0] = source; p.dependencyCount = 1; p.size = 5;
    p.code[0] = {0, Op::Load, 0}; p.code[1] = {0, Op::Parameter, 0};
    p.code[2] = {0, Op::Mul, 0}; p.code[3] = {0, Op::Parameter, 1};
    p.code[4] = {0, Op::Add, 0};
    return p;
}
bool evaluate(const Program& p, const double* params, const Number* inputs, Number& output) {
    // Programs are validated before registration; execution only touches numeric tables.
    Number stack[MaxStack];
    uint8_t size = 0;
    for (uint8_t i = 0; i < p.size; ++i) {
        const auto& instruction = p.code[i];
        const auto op = instruction.op;
        Number result;
        if (op == Op::Constant || op == Op::Parameter) {
            result.current = result.previous = op == Op::Constant ? instruction.number : params[instruction.operand];
        } else if (op == Op::Load) result = inputs[instruction.operand];
        else if (op == Op::Neg || op == Op::Abs) {
            const auto a = stack[--size];
            if (op == Op::Neg) result = {-a.previous, -a.current, -a.delta};
            else {
                const Number negative{-a.previous, -a.current, -a.delta};
                result = select(a, negative, false);
            }
        } else if (op == Op::Clamp) {
            const auto high = stack[--size], low = stack[--size], a = stack[--size];
            if (low.current > high.current) return false;
            result = select(select(a, low, false), high, true);
        } else {
            const auto b = stack[--size], a = stack[--size];
            switch (op) {
                case Op::Add: result = {a.previous+b.previous, a.current+b.current, a.delta+b.delta}; break;
                case Op::Sub: result = {a.previous-b.previous, a.current-b.current, a.delta-b.delta}; break;
                case Op::Mul: result = {a.previous*b.previous, a.current*b.current,
                    a.previous*b.delta + b.previous*a.delta + a.delta*b.delta}; break;
                case Op::Div:
                    if (b.current == 0) return false;
                    result = {a.previous/b.previous, a.current/b.current,
                        (a.delta - (a.previous/b.previous)*b.delta)/b.current}; break;
                case Op::Min: result = select(a, b, true); break;
                case Op::Max: result = select(a, b, false); break;
                default: return false;
            }
        }
        if (!isfinite(result.current)) return false;
        stack[size++] = result;
    }
    output = stack[0]; return true;
}
}
