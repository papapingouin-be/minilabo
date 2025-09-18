#include "virtual_lab/Oscilloscope.h"

#include "virtual_lab/DidacticMenu.h"
#include "virtual_lab/VirtualSignal.h"
#include "virtual_lab/VirtualWorkspace.h"

namespace virtual_lab {

Oscilloscope::Oscilloscope(VirtualWorkspace &workspace)
    : workspace_(workspace) {}

bool Oscilloscope::configureTrace(const OscilloscopeTraceConfig &config) {
  if (config.id.length() == 0 || config.signalId.length() == 0) {
    return false;
  }
  for (auto &trace : traces_) {
    if (trace.id == config.id) {
      trace = config;
      return true;
    }
  }
  traces_.push_back(config);
  return true;
}

bool Oscilloscope::removeTrace(const String &id) {
  for (size_t i = 0; i < traces_.size(); ++i) {
    if (traces_[i].id == id) {
      traces_.erase(traces_.begin() + i);
      return true;
    }
  }
  return false;
}

bool Oscilloscope::capture(const OscilloscopeCaptureRequest &request,
                           OscilloscopeCaptureResult &result,
                           String &error) const {
  error = "";
  if (request.sampleRate <= 0.0f || request.sampleCount == 0) {
    error = "invalid_sampling";
    return false;
  }
  float interval = 1.0f / request.sampleRate;
  result.sampleRate = request.sampleRate;
  result.traces.clear();
  for (const auto &trace : traces_) {
    if (!trace.enabled) {
      continue;
    }
    OscilloscopeTraceData data;
    data.id = trace.id;
    data.label = trace.label;
    data.enabled = trace.enabled;
    data.samples.reserve(request.sampleCount);
    for (size_t i = 0; i < request.sampleCount; ++i) {
      float time = request.startTime + interval * static_cast<float>(i);
      float value = NAN;
      if (!workspace_.sampleSignal(trace.signalId, time, value)) {
        error = String("missing_signal_") + trace.signalId;
        return false;
      }
      data.samples.push_back(value);
    }
    result.traces.push_back(data);
  }
  return true;
}

void Oscilloscope::populateHelp(DidacticMenu &menu) const {
  menu.addEntry(
      "oscilloscope.overview", "Oscilloscope virtuel",
      "Affichez plusieurs traces simultanément. La base de temps est définie "
      "par la fréquence d'échantillonnage. Configurez les canaux pour "
      "observer les signaux générés ou calculés.");
  menu.addEntry(
      "oscilloscope.trigger",
      "Capture", "Chaque acquisition retourne un ensemble d'échantillons "
                  "pour les traces actives. Les données peuvent être "
                  "transmises via l'API pour affichage dans l'interface.");
}

}  // namespace virtual_lab
