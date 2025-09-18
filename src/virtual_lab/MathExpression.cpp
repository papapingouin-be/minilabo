#include "virtual_lab/MathExpression.h"

#include <algorithm>
#include <ctype.h>
#include <math.h>
#include <stdlib.h>

#ifndef M_E
#define M_E 2.71828182845904523536
#endif

namespace virtual_lab {

namespace {

struct Node {
  enum class Type { Constant, Variable, Unary, Binary, Function };
  Type type;
  float constant = 0.0f;
  char op = '\0';
  String identifier;
  std::vector<std::unique_ptr<Node>> children;
};

class Parser {
 public:
  Parser(const std::string &source, MathExpression *owner, String &error)
      : source_(source), owner_(owner), error_(error) {}

  std::unique_ptr<Node> parse() {
    skipWhitespace();
    auto expr = parseExpression();
    skipWhitespace();
    if (!expr || hasError()) {
      return nullptr;
    }
    if (position_ != source_.size()) {
      setError(String("Unexpected token at position ") + String(position_ + 1));
      return nullptr;
    }
    return expr;
  }

 private:
  std::unique_ptr<Node> parseExpression() {
    auto node = parseTerm();
    if (!node) return nullptr;
    while (true) {
      skipWhitespace();
      if (match('+')) {
        auto rhs = parseTerm();
        if (!rhs) return nullptr;
        node = makeBinary('+', std::move(node), std::move(rhs));
      } else if (match('-')) {
        auto rhs = parseTerm();
        if (!rhs) return nullptr;
        node = makeBinary('-', std::move(node), std::move(rhs));
      } else {
        break;
      }
    }
    return node;
  }

  std::unique_ptr<Node> parseTerm() {
    auto node = parsePower();
    if (!node) return nullptr;
    while (true) {
      skipWhitespace();
      if (match('*')) {
        auto rhs = parsePower();
        if (!rhs) return nullptr;
        node = makeBinary('*', std::move(node), std::move(rhs));
      } else if (match('/')) {
        auto rhs = parsePower();
        if (!rhs) return nullptr;
        node = makeBinary('/', std::move(node), std::move(rhs));
      } else {
        break;
      }
    }
    return node;
  }

  std::unique_ptr<Node> parsePower() {
    auto node = parseUnary();
    if (!node) return nullptr;
    skipWhitespace();
    if (match('^')) {
      auto rhs = parsePower();
      if (!rhs) return nullptr;
      node = makeBinary('^', std::move(node), std::move(rhs));
    }
    return node;
  }

  std::unique_ptr<Node> parseUnary() {
    skipWhitespace();
    if (match('+')) {
      return parseUnary();
    }
    if (match('-')) {
      auto operand = parseUnary();
      if (!operand) return nullptr;
      auto node = std::unique_ptr<Node>(new Node());
      node->type = Node::Type::Unary;
      node->op = '-';
      node->children.push_back(std::move(operand));
      return node;
    }
    return parsePrimary();
  }

  std::unique_ptr<Node> parsePrimary() {
    skipWhitespace();
    if (match('(')) {
      auto node = parseExpression();
      if (!node) return nullptr;
      skipWhitespace();
      if (!match(')')) {
        setError(String("Missing closing parenthesis at position ") +
                 String(position_ + 1));
        return nullptr;
      }
      return node;
    }

    if (peek() == '\0') {
      setError("Unexpected end of expression");
      return nullptr;
    }

    if (isdigit(peek()) || peek() == '.') {
      return parseNumber();
    }

    if (isalpha(peek()) || peek() == '_') {
      return parseIdentifier();
    }

    setError(String("Unexpected character '") + String(peek()) + "'");
    return nullptr;
  }

  std::unique_ptr<Node> parseNumber() {
    const char *start = source_.c_str() + position_;
    char *end = nullptr;
    double value = strtod(start, &end);
    if (end == start) {
      setError(String("Invalid number at position ") + String(position_ + 1));
      return nullptr;
    }
    position_ = static_cast<size_t>(end - source_.c_str());
    auto node = std::unique_ptr<Node>(new Node());
    node->type = Node::Type::Constant;
    node->constant = static_cast<float>(value);
    return node;
  }

  std::unique_ptr<Node> parseIdentifier() {
    size_t start = position_;
    while (isalnum(peek()) || peek() == '_') {
      advance();
    }
    std::string ident = source_.substr(start, position_ - start);
    std::string lowered = ident;
    std::transform(lowered.begin(), lowered.end(), lowered.begin(), ::tolower);
    skipWhitespace();
    if (match('(')) {
      auto node = std::unique_ptr<Node>(new Node());
      node->type = Node::Type::Function;
      node->identifier = String(lowered.c_str());
      if (match(')')) {
        return node;
      }
      while (true) {
        auto arg = parseExpression();
        if (!arg) return nullptr;
        node->children.push_back(std::move(arg));
        skipWhitespace();
        if (match(')')) {
          break;
        }
        if (!match(',')) {
          setError(String("Expected ',' or ')' in argument list at position ") +
                   String(position_ + 1));
          return nullptr;
        }
      }
      return node;
    }

    if (lowered == "pi") {
      auto node = std::unique_ptr<Node>(new Node());
      node->type = Node::Type::Constant;
      node->constant = PI;
      return node;
    }
    if (lowered == "e") {
      auto node = std::unique_ptr<Node>(new Node());
      node->type = Node::Type::Constant;
      node->constant = static_cast<float>(M_E);
      return node;
    }

    auto node = std::unique_ptr<Node>(new Node());
    node->type = Node::Type::Variable;
    node->identifier = String(ident.c_str());
    owner_->registerVariable(node->identifier);
    return node;
  }

  bool match(char expected) {
    if (peek() == expected) {
      advance();
      return true;
    }
    return false;
  }

  char peek() const {
    if (position_ >= source_.size()) {
      return '\0';
    }
    return source_[position_];
  }

  void advance() {
    if (position_ < source_.size()) {
      ++position_;
    }
  }

  void skipWhitespace() {
    while (isspace(peek())) {
      advance();
    }
  }

  bool hasError() const { return error_.length() > 0; }

  void setError(const String &message) {
    if (error_.length() == 0) {
      error_ = message;
    }
  }

  std::unique_ptr<Node> makeBinary(char op, std::unique_ptr<Node> left,
                                   std::unique_ptr<Node> right) {
    auto node = std::unique_ptr<Node>(new Node());
    node->type = Node::Type::Binary;
    node->op = op;
    node->children.push_back(std::move(left));
    node->children.push_back(std::move(right));
    return node;
  }

  const std::string &source_;
  MathExpression *owner_;
  String &error_;
  size_t position_ = 0;
};

float applyBinary(char op, float lhs, float rhs) {
  switch (op) {
    case '+':
      return lhs + rhs;
    case '-':
      return lhs - rhs;
    case '*':
      return lhs * rhs;
    case '/':
      return rhs == 0.0f ? NAN : lhs / rhs;
    case '^':
      return powf(lhs, rhs);
    default:
      return NAN;
  }
}

float applyUnary(char op, float value) {
  switch (op) {
    case '-':
      return -value;
    default:
      return value;
  }
}

bool evaluateFunction(const String &name, const std::vector<float> &args,
                      float &out) {
  String lower = name;
  lower.toLowerCase();
  if (lower == "sin") {
    if (args.size() != 1) return false;
    out = sinf(args[0]);
    return true;
  }
  if (lower == "cos") {
    if (args.size() != 1) return false;
    out = cosf(args[0]);
    return true;
  }
  if (lower == "tan") {
    if (args.size() != 1) return false;
    out = tanf(args[0]);
    return true;
  }
  if (lower == "asin") {
    if (args.size() != 1) return false;
    out = asinf(args[0]);
    return true;
  }
  if (lower == "acos") {
    if (args.size() != 1) return false;
    out = acosf(args[0]);
    return true;
  }
  if (lower == "atan") {
    if (args.size() != 1) return false;
    out = atanf(args[0]);
    return true;
  }
  if (lower == "sqrt") {
    if (args.size() != 1) return false;
    out = args[0] < 0.0f ? NAN : sqrtf(args[0]);
    return true;
  }
  if (lower == "abs") {
    if (args.size() != 1) return false;
    out = fabsf(args[0]);
    return true;
  }
  if (lower == "exp") {
    if (args.size() != 1) return false;
    out = expf(args[0]);
    return true;
  }
  if (lower == "ln" || lower == "log") {
    if (args.size() != 1) return false;
    out = args[0] <= 0.0f ? NAN : logf(args[0]);
    return true;
  }
  if (lower == "log10") {
    if (args.size() != 1) return false;
    out = args[0] <= 0.0f ? NAN : log10f(args[0]);
    return true;
  }
  if (lower == "floor") {
    if (args.size() != 1) return false;
    out = floorf(args[0]);
    return true;
  }
  if (lower == "ceil") {
    if (args.size() != 1) return false;
    out = ceilf(args[0]);
    return true;
  }
  if (lower == "round") {
    if (args.size() != 1) return false;
    out = roundf(args[0]);
    return true;
  }
  if (lower == "min") {
    if (args.empty()) return false;
    float value = args[0];
    for (size_t i = 1; i < args.size(); ++i) {
      value = (args[i] < value) ? args[i] : value;
    }
    out = value;
    return true;
  }
  if (lower == "max") {
    if (args.empty()) return false;
    float value = args[0];
    for (size_t i = 1; i < args.size(); ++i) {
      value = (args[i] > value) ? args[i] : value;
    }
    out = value;
    return true;
  }
  if (lower == "avg" || lower == "mean") {
    if (args.empty()) return false;
    float sum = 0.0f;
    for (float v : args) sum += v;
    out = sum / static_cast<float>(args.size());
    return true;
  }
  if (lower == "sum") {
    float sum = 0.0f;
    for (float v : args) sum += v;
    out = sum;
    return true;
  }
  if (lower == "clamp") {
    if (args.size() != 3) return false;
    float value = args[0];
    float lo = args[1];
    float hi = args[2];
    if (lo > hi) {
      float tmp = lo;
      lo = hi;
      hi = tmp;
    }
    if (value < lo) value = lo;
    if (value > hi) value = hi;
    out = value;
    return true;
  }
  if (lower == "pow") {
    if (args.size() != 2) return false;
    out = powf(args[0], args[1]);
    return true;
  }
  return false;
}

}  // namespace

MathExpression::MathExpression() = default;
MathExpression::~MathExpression() = default;

bool MathExpression::compile(const String &expression, String &errorMessage) {
  expression_ = expression;
  asciiExpression_ = std::string(expression.c_str());
  variables_.clear();
  errorMessage = "";
  Parser parser(asciiExpression_, this, errorMessage);
  root_ = parser.parse();
  if (!root_) {
    return false;
  }
  return true;
}

bool MathExpression::evaluate(
    const std::function<bool(const String &, float &)> &resolver,
    float &out) const {
  if (!root_) {
    return false;
  }
  return evaluateNode(root_.get(), resolver, out);
}

void MathExpression::registerVariable(const String &name) {
  for (const auto &existing : variables_) {
    if (existing == name) {
      return;
    }
  }
  variables_.push_back(name);
}

bool MathExpression::evaluateNode(
    const Node *node,
    const std::function<bool(const String &, float &)> &resolver,
    float &out) const {
  switch (node->type) {
    case Node::Type::Constant:
      out = node->constant;
      return true;
    case Node::Type::Variable: {
      float value = NAN;
      if (!resolver(node->identifier, value)) {
        return false;
      }
      out = value;
      return true;
    }
    case Node::Type::Unary: {
      float operand = NAN;
      if (!evaluateNode(node->children[0].get(), resolver, operand)) {
        return false;
      }
      out = applyUnary(node->op, operand);
      return true;
    }
    case Node::Type::Binary: {
      float lhs = NAN;
      float rhs = NAN;
      if (!evaluateNode(node->children[0].get(), resolver, lhs)) {
        return false;
      }
      if (!evaluateNode(node->children[1].get(), resolver, rhs)) {
        return false;
      }
      out = applyBinary(node->op, lhs, rhs);
      return true;
    }
    case Node::Type::Function: {
      std::vector<float> args;
      args.reserve(node->children.size());
      for (const auto &child : node->children) {
        float value = NAN;
        if (!evaluateNode(child.get(), resolver, value)) {
          return false;
        }
        args.push_back(value);
      }
      float value = NAN;
      if (!evaluateFunction(node->identifier, args, value)) {
        return false;
      }
      out = value;
      return true;
    }
  }
  return false;
}

}  // namespace virtual_lab
