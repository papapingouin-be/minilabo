#include "virtual_lab/Multimeter.h"

#include <math.h>

#include "virtual_lab/DidacticMenu.h"
#include "virtual_lab/VirtualWorkspace.h"

namespace virtual_lab {

Multimeter::Multimeter(VirtualWorkspace &workspace) : workspace_(workspace) {}

bool Multimeter::configureInput(const MultimeterInputConfig &config) {
  if (config.id.length() == 0 || config.signalId.length() == 0) {
    return false;
  }
  for (auto &input : inputs_) {
    if (input.id == config.id) {
      input = config;
      return true;
    }
  }
  inputs_.push_back(config);
  return true;
}

bool Multimeter::removeInput(const String &id) {
  for (size_t i = 0; i < inputs_.size(); ++i) {
    if (inputs_[i].id == id) {
      inputs_.erase(inputs_.begin() + i);
      return true;
    }
  }
  return false;
}

void Multimeter::replaceInputs(const std::vector<MultimeterInputConfig> &configs) {
  inputs_.clear();
  inputs_.reserve(configs.size());
  for (const auto &config : configs) {
    if (config.id.length() == 0) {
      continue;
    }
    if (config.signalId.length() == 0 && config.enabled) {
      continue;
    }
    inputs_.push_back(config);
  }
}

static bool computeSeriesMetrics(const std::vector<float> &samples, float &minValue,
                                 float &maxValue, float &average, float &rms) {
  if (samples.empty()) {
    return false;
  }
  minValue = samples[0];
  maxValue = samples[0];
  double sum = 0.0;
  double squareSum = 0.0;
  for (float value : samples) {
    if (value < minValue) minValue = value;
    if (value > maxValue) maxValue = value;
    sum += value;
    squareSum += static_cast<double>(value) * static_cast<double>(value);
  }
  average = static_cast<float>(sum / static_cast<double>(samples.size()));
  rms = static_cast<float>(sqrt(squareSum / static_cast<double>(samples.size())));
  return true;
}

bool Multimeter::measure(const MultimeterMeasurementRequest &request,
                         MultimeterMeasurementResult &result,
                         String &error) const {
  error = "";
  result.inputId = request.inputId;
  result.mode = request.mode;
  const MultimeterInputConfig *selected = nullptr;
  for (const auto &input : inputs_) {
    if (input.id == request.inputId) {
      selected = &input;
      break;
    }
  }
  if (!selected) {
    error = String("unknown_input_") + request.inputId;
    return false;
  }
  if (!selected->enabled) {
    error = String("input_disabled_") + request.inputId;
    return false;
  }
  if (request.sampleRate <= 0.0f || request.sampleCount == 0) {
    error = "invalid_sampling";
    return false;
  }
  float interval = 1.0f / request.sampleRate;
  std::vector<float> samples;
  samples.reserve(request.sampleCount);
  for (size_t i = 0; i < request.sampleCount; ++i) {
    float time = request.startTime + interval * static_cast<float>(i);
    float value = NAN;
    if (!workspace_.sampleSignal(selected->signalId, time, value)) {
      error = String("missing_signal_") + selected->signalId;
      return false;
    }
    samples.push_back(value);
  }
  float minValue = NAN;
  float maxValue = NAN;
  float average = NAN;
  float rms = NAN;
  if (!computeSeriesMetrics(samples, minValue, maxValue, average, rms)) {
    error = "metrics_failed";
    return false;
  }
  switch (request.mode) {
    case MultimeterMode::DC:
      result.value = average;
      break;
    case MultimeterMode::AC_RMS: {
      float acComponentRms = 0.0f;
      double sum = 0.0;
      for (float v : samples) {
        double deviation = static_cast<double>(v) - static_cast<double>(average);
        sum += deviation * deviation;
      }
      acComponentRms = static_cast<float>(sqrt(sum / samples.size()));
      result.value = acComponentRms;
      break;
    }
    case MultimeterMode::MIN:
      result.value = minValue;
      break;
    case MultimeterMode::MAX:
      result.value = maxValue;
      break;
    case MultimeterMode::AVERAGE:
      result.value = average;
      break;
    case MultimeterMode::PEAK_TO_PEAK:
      result.value = maxValue - minValue;
      break;
  }
  result.minValue = minValue;
  result.maxValue = maxValue;
  return true;
}

void Multimeter::populateHelp(DidacticMenu &menu) const {
  menu.addEntry("multimeter.overview", "Multimètre virtuel",
                "Mesurez des grandeurs continues ou alternatives sur les "
                "signaux disponibles. Sélectionnez la fonction souhaitée : DC, "
                "RMS, min, max, moyenne ou crête à crête.");
  menu.addEntry(
      "multimeter.inputs", "Entrées du multimètre",
      "Chaque entrée virtuelle peut être reliée à une source mathématique ou "
      "physique simulée. Les mesures sont réalisées sans nécessiter de "
      "redémarrage du système.");
}

}  // namespace virtual_lab
