#pragma once

#include <Arduino.h>
#include <memory>
#include <vector>

#include "virtual_lab/DidacticMenu.h"
#include "virtual_lab/VirtualSignal.h"

class JsonDocument;

namespace virtual_lab {

class FunctionGenerator;
class Oscilloscope;
class Multimeter;
class MathZone;

class VirtualWorkspace {
 public:
  static VirtualWorkspace &Instance();

  VirtualWorkspace();

  bool registerSignal(const std::shared_ptr<VirtualSignal> &signal);
  bool removeSignal(const String &id);
  std::shared_ptr<VirtualSignal> findSignal(const String &id);
  std::shared_ptr<const VirtualSignal> findSignal(const String &id) const;

  bool sampleSignal(const String &id, float time, float &out) const;
  bool sampleSignalSeries(const String &id, float startTime, float interval,
                          size_t count, std::vector<float> &out) const;

  FunctionGenerator &functionGenerator();
  Oscilloscope &oscilloscope();
  Multimeter &multimeter();
  MathZone &mathZone();

  const FunctionGenerator &functionGenerator() const;
  const Oscilloscope &oscilloscope() const;
  const Multimeter &multimeter() const;
  const MathZone &mathZone() const;

  DidacticMenu &helpMenu() { return helpMenu_; }
  const DidacticMenu &helpMenu() const { return helpMenu_; }

  void populateSummaryJson(JsonDocument &doc) const;

 private:
  std::shared_ptr<VirtualSignal> findSignalInternal(const String &id);
  std::shared_ptr<const VirtualSignal> findSignalInternal(const String &id) const;

  std::vector<std::shared_ptr<VirtualSignal>> signals_;
  std::unique_ptr<FunctionGenerator> functionGenerator_;
  std::unique_ptr<Oscilloscope> oscilloscope_;
  std::unique_ptr<Multimeter> multimeter_;
  std::unique_ptr<MathZone> mathZone_;
  DidacticMenu helpMenu_;
};

}  // namespace virtual_lab
