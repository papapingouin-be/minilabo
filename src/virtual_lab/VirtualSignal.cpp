#include "virtual_lab/VirtualSignal.h"

#include <Arduino.h>
#include <math.h>

#include "virtual_lab/MathExpression.h"
#include "virtual_lab/VirtualWorkspace.h"

namespace virtual_lab {

VirtualSignal::VirtualSignal(const String &id, const String &name, Kind kind)
    : id_(id), name_(name), kind_(kind) {}

ConstantSignal::ConstantSignal(const String &id, const String &name, float value)
    : VirtualSignal(id, name, Kind::Constant), value_(value) {}

float ConstantSignal::sample(const SampleContext &ctx) const {
  (void)ctx;
  return value_;
}

WaveformSignal::WaveformSignal(const String &id, const String &name)
    : VirtualSignal(id, name, Kind::Waveform) {}

void WaveformSignal::configure(const WaveformSettings &settings) {
  settings_ = settings;
}

static float wrapPhase(float phase) {
  while (phase < 0.0f) phase += 1.0f;
  while (phase >= 1.0f) phase -= 1.0f;
  return phase;
}

float WaveformSignal::sample(const SampleContext &ctx) const {
  float t = ctx.time;
  if (settings_.shape == WaveformShape::DC || settings_.frequency == 0.0f) {
    return settings_.offset + settings_.amplitude;
  }
  float cycles = settings_.frequency * t + settings_.phase;
  float phase = wrapPhase(cycles);
  float value = 0.0f;
  switch (settings_.shape) {
    case WaveformShape::DC:
      value = settings_.amplitude;
      break;
    case WaveformShape::Sine:
      value = sinf(2.0f * PI * phase) * settings_.amplitude;
      break;
    case WaveformShape::Square: {
      float duty = settings_.dutyCycle;
      if (duty <= 0.0f) duty = 0.01f;
      if (duty >= 1.0f) duty = 0.99f;
      value = (phase < duty) ? settings_.amplitude : -settings_.amplitude;
      break;
    }
    case WaveformShape::Triangle: {
      if (phase < 0.25f) {
        value = 4.0f * phase * settings_.amplitude;
      } else if (phase < 0.75f) {
        value = (2.0f - 4.0f * phase) * settings_.amplitude;
      } else {
        value = (-4.0f + 4.0f * phase) * settings_.amplitude;
      }
      break;
    }
    case WaveformShape::Sawtooth:
      value = (2.0f * phase - 1.0f) * settings_.amplitude;
      break;
    case WaveformShape::Noise:
      value = (random(-1000, 1001) / 1000.0f) * settings_.amplitude;
      break;
  }
  return value + settings_.offset;
}

MathVirtualSignal::MathVirtualSignal(const String &id, const String &name)
    : VirtualSignal(id, name, Kind::Math) {}

MathVirtualSignal::~MathVirtualSignal() = default;

bool MathVirtualSignal::configure(const String &expression,
                                  const std::vector<VariableBinding> &bindings,
                                  String &error) {
  expression_ = expression;
  bindings_ = bindings;
  compiled_.reset(new MathExpression());
  if (!compiled_->compile(expression, error)) {
    compiled_.reset();
    return false;
  }
  return true;
}

float MathVirtualSignal::sample(const SampleContext &ctx) const {
  if (!compiled_) {
    return NAN;
  }
  auto resolver = [&](const String &variable, float &out) -> bool {
    for (const auto &binding : bindings_) {
      if (binding.variable == variable) {
        if (ctx.workspace == nullptr) {
          return false;
        }
        return ctx.workspace->sampleSignal(binding.signalId, ctx.time, out);
      }
    }
    if (ctx.workspace == nullptr) {
      return false;
    }
    return ctx.workspace->sampleSignal(variable, ctx.time, out);
  };
  float value = NAN;
  if (!compiled_->evaluate(resolver, value)) {
    return NAN;
  }
  return value;
}

}  // namespace virtual_lab
