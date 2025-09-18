#include "virtual_lab/MathZone.h"

#include <memory>

#include "virtual_lab/DidacticMenu.h"
#include "virtual_lab/VirtualWorkspace.h"

namespace virtual_lab {

MathZone::MathZone(VirtualWorkspace &workspace) : workspace_(workspace) {}

bool MathZone::defineExpression(const MathExpressionConfig &config,
                                String &error) {
  error = "";
  if (config.id.length() == 0) {
    error = "missing_id";
    return false;
  }
  if (config.name.length() == 0) {
    error = "missing_name";
    return false;
  }
  if (config.expression.length() == 0) {
    error = "missing_expression";
    return false;
  }
  auto signal = std::make_shared<MathVirtualSignal>(config.id, config.name);
  String compileError;
  if (!signal->configure(config.expression, config.bindings, compileError)) {
    error = String("compile_error:") + compileError;
    return false;
  }
  signal->setUnits(config.units.length() ? config.units : String("V"));
  signal->setHelpKey("math_zone.expression");
  if (!workspace_.registerSignal(signal)) {
    error = "duplicate_signal";
    return false;
  }
  bool found = false;
  for (auto &id : expressionIds_) {
    if (id == config.id) {
      found = true;
      break;
    }
  }
  if (!found) {
    expressionIds_.push_back(config.id);
  }
  return true;
}

bool MathZone::removeExpression(const String &id) {
  for (size_t i = 0; i < expressionIds_.size(); ++i) {
    if (expressionIds_[i] == id) {
      expressionIds_.erase(expressionIds_.begin() + i);
      workspace_.removeSignal(id);
      return true;
    }
  }
  return false;
}

void MathZone::populateHelp(DidacticMenu &menu) const {
  menu.addEntry(
      "math_zone.overview", "Zone mathématique",
      "Créez des équations virtuelles basées sur les signaux disponibles. Les "
      "fonctions mathématiques standard (sin, cos, min, max, etc.) sont "
      "disponibles pour composer vos scénarios pédagogiques.");
  menu.addEntry(
      "math_zone.expression",
      "Équation",
      "Associez un identifiant et une expression. Les variables sont "
      "résolues dynamiquement sur les signaux existants, sans redémarrage de "
      "l'équipement.");
}

}  // namespace virtual_lab
