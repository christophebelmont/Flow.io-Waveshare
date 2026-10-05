#pragma once
#include "Value.h"
#include <stddef.h>

/** Boot/configuration compiler and allocation-free numeric evaluator. */
namespace ValueExpression {
constexpr uint8_t MaxInstructions = 32, MaxStack = 8, MaxDependencies = 16, ParamCount = 4;
constexpr size_t TextCapacity = 192;
enum class Op : uint8_t { Constant, Parameter, Load, Add, Sub, Mul, Div, Neg, Min, Max, Abs, Clamp };
struct Instruction { double number = 0; Op op = Op::Constant; uint8_t operand = 0; };
struct Program {
    Instruction code[MaxInstructions]{};
    ValueId dependencies[MaxDependencies]{};
    uint8_t size = 0, dependencyCount = 0;
};
// delta is the finite difference from the previous observation, not a derivative.
struct Number { double previous = 0, current = 0, delta = 0; };
bool compile(const char* text, Program& output);
bool validate(const Program& program);
Program affine(ValueId source);
// Requires a validated program and arrays sized to ParamCount/dependencyCount.
bool evaluate(const Program& program, const double* parameters, const Number* inputs, Number& output);
}
