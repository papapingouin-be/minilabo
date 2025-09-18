#pragma once

#include <Arduino.h>
#include <memory>
#include <vector>

namespace virtual_lab {

class VirtualWorkspace;
class MathExpression;

class VirtualSignal {
 public:
  enum class Kind { Constant, Waveform, Math, External };

  struct SampleContext {
    const VirtualWorkspace *workspace;
    float time;
  };

  VirtualSignal(const String &id, const String &name, Kind kind);
  virtual ~VirtualSignal() = default;

  const String &id() const { return id_; }
  const String &name() const { return name_; }
  void setName(const String &name) { name_ = name; }
  Kind kind() const { return kind_; }

  void setUnits(const String &units) { units_ = units; }
  const String &units() const { return units_; }

  void setHelpKey(const String &key) { helpKey_ = key; }
  const String &helpKey() const { return helpKey_; }

  virtual float sample(const SampleContext &ctx) const = 0;

 protected:
  String id_;
  String name_;
  String units_;
  String helpKey_;
  Kind kind_;
};

class ConstantSignal : public VirtualSignal {
 public:
  ConstantSignal(const String &id, const String &name, float value = 0.0f);
  void setValue(float value) { value_ = value; }
  float value() const { return value_; }
  float sample(const SampleContext &ctx) const override;

 private:
  float value_;
};

enum class WaveformShape { DC, Sine, Square, Triangle, Sawtooth, Noise };

struct WaveformSettings {
  float amplitude = 1.0f;
  float offset = 0.0f;
  float frequency = 1.0f;
  float phase = 0.0f;
  float dutyCycle = 0.5f;
  WaveformShape shape = WaveformShape::Sine;
};

class WaveformSignal : public VirtualSignal {
 public:
  WaveformSignal(const String &id, const String &name);
  void configure(const WaveformSettings &settings);
  const WaveformSettings &settings() const { return settings_; }
  float sample(const SampleContext &ctx) const override;

 private:
  WaveformSettings settings_;
};

struct VariableBinding {
  String variable;
  String signalId;
};

class MathVirtualSignal : public VirtualSignal {
 public:
  MathVirtualSignal(const String &id, const String &name);
  ~MathVirtualSignal();

  bool configure(const String &expression,
                 const std::vector<VariableBinding> &bindings,
                 String &error);

  const String &expression() const { return expression_; }
  const std::vector<VariableBinding> &bindings() const { return bindings_; }

  float sample(const SampleContext &ctx) const override;

 private:
  std::unique_ptr<MathExpression> compiled_;
  String expression_;
  std::vector<VariableBinding> bindings_;
};

}  // namespace virtual_lab
