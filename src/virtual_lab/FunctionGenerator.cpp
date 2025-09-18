#include "virtual_lab/FunctionGenerator.h"

#include "virtual_lab/DidacticMenu.h"
#include "virtual_lab/VirtualWorkspace.h"

namespace virtual_lab {

FunctionGenerator::FunctionGenerator(VirtualWorkspace &workspace)
    : workspace_(workspace) {}

FunctionGenerator::Output *FunctionGenerator::findOutput(const String &id) {
  for (auto &output : outputs_) {
    if (output.id == id) {
      return &output;
    }
  }
  return nullptr;
}

const FunctionGenerator::Output *FunctionGenerator::findOutput(
    const String &id) const {
  for (const auto &output : outputs_) {
    if (output.id == id) {
      return &output;
    }
  }
  return nullptr;
}

bool FunctionGenerator::configureOutput(
    const FunctionGeneratorOutputConfig &config, String &error) {
  error = "";
  if (config.id.length() == 0) {
    error = "missing_id";
    return false;
  }
  if (config.name.length() == 0) {
    error = "missing_name";
    return false;
  }
  auto *existing = findOutput(config.id);
  if (existing) {
    existing->name = config.name;
    existing->settings = config.settings;
    existing->enabled = config.enabled;
    existing->units = config.units;
    if (existing->signal) {
      existing->signal->setName(config.name);
      existing->signal->configure(config.settings);
      existing->signal->setUnits(config.units);
    }
    return true;
  }
  auto signal = std::make_shared<WaveformSignal>(config.id, config.name);
  signal->configure(config.settings);
  signal->setUnits(config.units.length() ? config.units : String("V"));
  signal->setHelpKey("function_generator.output");
  if (!workspace_.registerSignal(signal)) {
    error = "duplicate_signal";
    return false;
  }
  Output output;
  output.id = config.id;
  output.name = config.name;
  output.enabled = config.enabled;
  output.settings = config.settings;
  output.units = signal->units();
  output.signal = signal;
  outputs_.push_back(output);
  return true;
}

bool FunctionGenerator::removeOutput(const String &id) {
  for (size_t i = 0; i < outputs_.size(); ++i) {
    if (outputs_[i].id == id) {
      workspace_.removeSignal(outputs_[i].id);
      outputs_.erase(outputs_.begin() + i);
      return true;
    }
  }
  return false;
}

void FunctionGenerator::disableAll() {
  for (auto &output : outputs_) {
    output.enabled = false;
  }
}

void FunctionGenerator::populateHelp(DidacticMenu &menu) const {
  menu.addEntry(
      "function_generator.overview", "Générateur de fonctions",
      "Configurez des sorties virtuelles sinusoïdales, carrées, triangulaires, "
      "dent de scie ou bruit blanc. Chaque sortie peut être reliée à des "
      "équations mathématiques ou à des instruments. Réglez amplitude, "
      "fréquence, décalage et facteur de service pour explorer différents "
      "signaux pédagogiques.");
  menu.addEntry(
      "function_generator.output", "Sortie du générateur",
      "Une sortie virtuelle peut être utilisée par l'oscilloscope ou le "
      "multimètre. Activez ou désactivez la sortie selon le scénario "
      "pédagogique souhaité.");
}

}  // namespace virtual_lab
