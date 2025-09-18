#pragma once

#include <Arduino.h>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace virtual_lab {

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
  struct Node;

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
