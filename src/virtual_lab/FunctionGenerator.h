#pragma once

#include <Arduino.h>
#include <memory>
#include <vector>

#include "virtual_lab/VirtualSignal.h"

namespace virtual_lab {

class VirtualWorkspace;
class DidacticMenu;

struct FunctionGeneratorOutputConfig {
  String id;
  String name;
  WaveformSettings settings;
  bool enabled = true;
  String units;
};

class FunctionGenerator {
 public:
  explicit FunctionGenerator(VirtualWorkspace &workspace);

  bool configureOutput(const FunctionGeneratorOutputConfig &config,
                       String &error);
  bool removeOutput(const String &id);
  void disableAll();

  struct Output {
    String id;
    String name;
    bool enabled = true;
    WaveformSettings settings;
    String units;
    std::shared_ptr<WaveformSignal> signal;
  };

  const std::vector<Output> &outputs() const { return outputs_; }

  void populateHelp(DidacticMenu &menu) const;

 private:
  VirtualWorkspace &workspace_;
  std::vector<Output> outputs_;

  Output *findOutput(const String &id);
  const Output *findOutput(const String &id) const;
};

}  // namespace virtual_lab
