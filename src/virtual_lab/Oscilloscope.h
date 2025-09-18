#pragma once

#include <Arduino.h>
#include <vector>

namespace virtual_lab {

class VirtualWorkspace;
class DidacticMenu;

struct OscilloscopeTraceConfig {
  String id;
  String signalId;
  String label;
  bool enabled = true;
};

struct OscilloscopeCaptureRequest {
  float startTime = 0.0f;
  float sampleRate = 1000.0f;
  size_t sampleCount = 512;
};

struct OscilloscopeTraceData {
  String id;
  String label;
  bool enabled = true;
  std::vector<float> samples;
};

struct OscilloscopeCaptureResult {
  float sampleRate = 0.0f;
  std::vector<OscilloscopeTraceData> traces;
};

class Oscilloscope {
 public:
  explicit Oscilloscope(VirtualWorkspace &workspace);

  bool configureTrace(const OscilloscopeTraceConfig &config);
  bool removeTrace(const String &id);
  const std::vector<OscilloscopeTraceConfig> &traces() const { return traces_; }

  bool capture(const OscilloscopeCaptureRequest &request,
               OscilloscopeCaptureResult &result,
               String &error) const;

  void populateHelp(DidacticMenu &menu) const;

 private:
  VirtualWorkspace &workspace_;
  std::vector<OscilloscopeTraceConfig> traces_;
};

}  // namespace virtual_lab
