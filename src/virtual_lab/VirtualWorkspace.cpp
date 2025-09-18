#include "virtual_lab/VirtualWorkspace.h"

#include <ArduinoJson.h>
#include <memory>

#include "virtual_lab/FunctionGenerator.h"
#include "virtual_lab/MathZone.h"
#include "virtual_lab/Multimeter.h"
#include "virtual_lab/Oscilloscope.h"

namespace virtual_lab {

VirtualWorkspace &VirtualWorkspace::Instance() {
  static VirtualWorkspace instance;
  return instance;
}

VirtualWorkspace::VirtualWorkspace() {
  functionGenerator_.reset(new FunctionGenerator(*this));
  oscilloscope_.reset(new Oscilloscope(*this));
  multimeter_.reset(new Multimeter(*this));
  mathZone_.reset(new MathZone(*this));
  functionGenerator_->populateHelp(helpMenu_);
  oscilloscope_->populateHelp(helpMenu_);
  multimeter_->populateHelp(helpMenu_);
  mathZone_->populateHelp(helpMenu_);
}

std::shared_ptr<VirtualSignal> VirtualWorkspace::findSignalInternal(
    const String &id) {
  for (auto &signal : signals_) {
    if (signal->id() == id) {
      return signal;
    }
  }
  return nullptr;
}

std::shared_ptr<const VirtualSignal> VirtualWorkspace::findSignalInternal(
    const String &id) const {
  for (const auto &signal : signals_) {
    if (signal->id() == id) {
      return signal;
    }
  }
  return nullptr;
}

bool VirtualWorkspace::registerSignal(
    const std::shared_ptr<VirtualSignal> &signal) {
  if (!signal) return false;
  for (auto &existing : signals_) {
    if (existing->id() == signal->id()) {
      existing = signal;
      return true;
    }
  }
  signals_.push_back(signal);
  return true;
}

bool VirtualWorkspace::removeSignal(const String &id) {
  for (size_t i = 0; i < signals_.size(); ++i) {
    if (signals_[i]->id() == id) {
      signals_.erase(signals_.begin() + i);
      return true;
    }
  }
  return false;
}

std::shared_ptr<VirtualSignal> VirtualWorkspace::findSignal(
    const String &id) {
  return findSignalInternal(id);
}

std::shared_ptr<const VirtualSignal> VirtualWorkspace::findSignal(
    const String &id) const {
  return findSignalInternal(id);
}

bool VirtualWorkspace::sampleSignal(const String &id, float time,
                                    float &out) const {
  auto signal = findSignalInternal(id);
  if (!signal) {
    return false;
  }
  VirtualSignal::SampleContext ctx;
  ctx.workspace = this;
  ctx.time = time;
  out = signal->sample(ctx);
  return !isnan(out);
}

bool VirtualWorkspace::sampleSignalSeries(const String &id, float startTime,
                                          float interval, size_t count,
                                          std::vector<float> &out) const {
  out.clear();
  out.reserve(count);
  for (size_t i = 0; i < count; ++i) {
    float sampleTime = startTime + interval * static_cast<float>(i);
    float value = NAN;
    if (!sampleSignal(id, sampleTime, value)) {
      return false;
    }
    out.push_back(value);
  }
  return true;
}

FunctionGenerator &VirtualWorkspace::functionGenerator() {
  return *functionGenerator_;
}

Oscilloscope &VirtualWorkspace::oscilloscope() { return *oscilloscope_; }

Multimeter &VirtualWorkspace::multimeter() { return *multimeter_; }

MathZone &VirtualWorkspace::mathZone() { return *mathZone_; }

const FunctionGenerator &VirtualWorkspace::functionGenerator() const {
  return *functionGenerator_;
}

const Oscilloscope &VirtualWorkspace::oscilloscope() const {
  return *oscilloscope_;
}

const Multimeter &VirtualWorkspace::multimeter() const {
  return *multimeter_;
}

const MathZone &VirtualWorkspace::mathZone() const { return *mathZone_; }

void VirtualWorkspace::populateSummaryJson(JsonDocument &doc) const {
  JsonArray signals = doc.createNestedArray("signals");
  for (const auto &signal : signals_) {
    JsonObject obj = signals.createNestedObject();
    obj["id"] = signal->id();
    obj["name"] = signal->name();
    obj["units"] = signal->units();
    switch (signal->kind()) {
      case VirtualSignal::Kind::Constant:
        obj["type"] = "constant";
        break;
      case VirtualSignal::Kind::Waveform:
        obj["type"] = "waveform";
        break;
      case VirtualSignal::Kind::Math:
        obj["type"] = "math";
        break;
      case VirtualSignal::Kind::External:
        obj["type"] = "external";
        break;
    }
  }
  JsonObject instruments = doc.createNestedObject("instruments");
  JsonArray fgOutputs = instruments.createNestedArray("functionGenerator");
  for (const auto &output : functionGenerator_->outputs()) {
    JsonObject obj = fgOutputs.createNestedObject();
    obj["id"] = output.id;
    obj["name"] = output.name;
    obj["enabled"] = output.enabled;
    obj["units"] = output.units;
    obj["amplitude"] = output.settings.amplitude;
    obj["offset"] = output.settings.offset;
    obj["frequency"] = output.settings.frequency;
    obj["phase"] = output.settings.phase;
    obj["dutyCycle"] = output.settings.dutyCycle;
    switch (output.settings.shape) {
      case WaveformShape::DC:
        obj["shape"] = "dc";
        break;
      case WaveformShape::Sine:
        obj["shape"] = "sine";
        break;
      case WaveformShape::Square:
        obj["shape"] = "square";
        break;
      case WaveformShape::Triangle:
        obj["shape"] = "triangle";
        break;
      case WaveformShape::Sawtooth:
        obj["shape"] = "saw";
        break;
      case WaveformShape::Noise:
        obj["shape"] = "noise";
        break;
    }
  }
  JsonArray scopeTraces = instruments.createNestedArray("oscilloscope");
  for (const auto &trace : oscilloscope_->traces()) {
    JsonObject obj = scopeTraces.createNestedObject();
    obj["id"] = trace.id;
    obj["signalId"] = trace.signalId;
    obj["label"] = trace.label;
    obj["enabled"] = trace.enabled;
  }
  JsonArray meterInputs = instruments.createNestedArray("multimeter");
  for (const auto &input : multimeter_->inputs()) {
    JsonObject obj = meterInputs.createNestedObject();
    obj["id"] = input.id;
    obj["signalId"] = input.signalId;
    obj["label"] = input.label;
    obj["enabled"] = input.enabled;
  }
  JsonArray mathExpressions = instruments.createNestedArray("mathZone");
  for (const auto &exprId : mathZone_->expressions()) {
    JsonObject obj = mathExpressions.createNestedObject();
    obj["id"] = exprId;
    auto signal = findSignal(exprId);
    if (!signal) {
      continue;
    }
    auto mathSignal = signal->asMathSignal();
    if (mathSignal) {
      obj["expression"] = mathSignal->expression();
    }
  }
  JsonArray help = doc.createNestedArray("help");
  for (const auto &entry : helpMenu_.entries()) {
    JsonObject obj = help.createNestedObject();
    obj["key"] = entry.key;
    obj["title"] = entry.title;
    obj["text"] = entry.text;
  }
}

}  // namespace virtual_lab
