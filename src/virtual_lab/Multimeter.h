#pragma once

#include <Arduino.h>
#include <vector>

namespace virtual_lab {

class VirtualWorkspace;
class DidacticMenu;

struct MultimeterInputConfig {
  String id;
  String signalId;
  String label;
  bool enabled = true;
};

enum class MultimeterMode {
  DC,
  AC_RMS,
  MIN,
  MAX,
  AVERAGE,
  PEAK_TO_PEAK
};

struct MultimeterMeasurementRequest {
  String inputId;
  MultimeterMode mode = MultimeterMode::DC;
  float startTime = 0.0f;
  float sampleRate = 500.0f;
  size_t sampleCount = 128;
};

struct MultimeterMeasurementResult {
  String inputId;
  MultimeterMode mode = MultimeterMode::DC;
  float value = NAN;
  float minValue = NAN;
  float maxValue = NAN;
};

class Multimeter {
 public:
  explicit Multimeter(VirtualWorkspace &workspace);

  bool configureInput(const MultimeterInputConfig &config);
  bool removeInput(const String &id);
  void replaceInputs(const std::vector<MultimeterInputConfig> &configs);
  const std::vector<MultimeterInputConfig> &inputs() const { return inputs_; }

  bool measure(const MultimeterMeasurementRequest &request,
               MultimeterMeasurementResult &result,
               String &error) const;

  void populateHelp(DidacticMenu &menu) const;

 private:
  VirtualWorkspace &workspace_;
  std::vector<MultimeterInputConfig> inputs_;
};

}  // namespace virtual_lab
