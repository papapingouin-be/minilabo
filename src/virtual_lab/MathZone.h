#pragma once

#include <Arduino.h>
#include <vector>

#include "virtual_lab/VirtualSignal.h"

namespace virtual_lab {

class VirtualWorkspace;
class DidacticMenu;

struct MathExpressionConfig {
  String id;
  String name;
  String expression;
  std::vector<VariableBinding> bindings;
  String units;
};

class MathZone {
 public:
  explicit MathZone(VirtualWorkspace &workspace);

  bool defineExpression(const MathExpressionConfig &config, String &error);
  bool removeExpression(const String &id);
  const std::vector<String> &expressions() const { return expressionIds_; }

  void populateHelp(DidacticMenu &menu) const;

 private:
  VirtualWorkspace &workspace_;
  std::vector<String> expressionIds_;
};

}  // namespace virtual_lab
