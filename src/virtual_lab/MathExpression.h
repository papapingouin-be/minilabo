#pragma once

#include <Arduino.h>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace virtual_lab {

class MathExpressionParser;

class MathExpression {
 public:
  MathExpression();
  ~MathExpression();

  bool compile(const String &expression, String &errorMessage);

  bool evaluate(const std::function<bool(const String &, float &)> &resolver,
                float &out) const;

  const std::vector<String> &variables() const { return variables_; }
  const String &expression() const { return expression_; }

 private:
  friend class MathExpressionParser;

  struct Node {
    enum class Type { Constant, Variable, Unary, Binary, Function };
    Type type;
    float constant = 0.0f;
    char op = '\0';
    String identifier;
    std::vector<std::unique_ptr<Node>> children;
  };

  std::unique_ptr<Node> parseExpression(String &errorMessage);

  std::unique_ptr<Node> root_;
  std::vector<String> variables_;
  String expression_;
  std::string asciiExpression_;

  void registerVariable(const String &name);

  bool evaluateNode(const Node *node,
                    const std::function<bool(const String &, float &)> &resolver,
                    float &out) const;
};

}  // namespace virtual_lab
