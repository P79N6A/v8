// Copyright 2018 the V8 project authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "src/compiler/serializer-for-background-compilation.h"

#include "src/compiler/js-heap-broker.h"
#include "src/handles-inl.h"
#include "src/interpreter/bytecode-array-iterator.h"
#include "src/objects/code.h"
#include "src/objects/shared-function-info-inl.h"
#include "src/zone/zone.h"

namespace v8 {
namespace internal {
namespace compiler {

Hints::Hints(Zone* zone)
    : constants_(zone), maps_(zone), function_blueprints_(zone) {}

const ZoneVector<Handle<Object>>& Hints::constants() const {
  return constants_;
}

const ZoneVector<Handle<Map>>& Hints::maps() const { return maps_; }

const ZoneVector<FunctionBlueprint>& Hints::function_blueprints() const {
  return function_blueprints_;
}

void Hints::AddConstant(Handle<Object> constant) {
  constants_.push_back(constant);
}

void Hints::AddMap(Handle<Map> map) { maps_.push_back(map); }

void Hints::AddFunctionBlueprint(FunctionBlueprint function_blueprint) {
  function_blueprints_.push_back(function_blueprint);
}

void Hints::Add(const Hints& other) {
  for (auto x : other.constants()) AddConstant(x);
  for (auto x : other.maps()) AddMap(x);
  for (auto x : other.function_blueprints()) AddFunctionBlueprint(x);
}

void Hints::Clear() {
  constants_.clear();
  maps_.clear();
  function_blueprints_.clear();
}

class SerializerForBackgroundCompilation::Environment : public ZoneObject {
 public:
  explicit Environment(Zone* zone, Isolate* isolate, int register_count,
                       int parameter_count);

  Environment(SerializerForBackgroundCompilation* serializer, Isolate* isolate,
              int register_count, int parameter_count,
              const HintsVector& arguments);

  int parameter_count() const { return parameter_count_; }
  int register_count() const { return register_count_; }

  Hints& accumulator_hints() { return environment_hints_[accumulator_index()]; }
  Hints& register_hints(interpreter::Register reg) {
    int local_index = RegisterToLocalIndex(reg);
    DCHECK_LT(local_index, environment_hints_.size());
    return environment_hints_[local_index];
  }
  Hints& return_value_hints() { return return_value_hints_; }

  void ClearAccumulatorAndRegisterHints() {
    for (auto& hints : environment_hints_) hints.Clear();
  }

  // Appends the hints for the given register range to {dst} (in order).
  void ExportRegisterHints(interpreter::Register first, size_t count,
                           HintsVector& dst);

 private:
  explicit Environment(Zone* zone)
      : register_count_(0),
        parameter_count_(0),
        environment_hints_(zone),
        return_value_hints_(zone) {}

  Zone* zone() const { return zone_; }

  int RegisterToLocalIndex(interpreter::Register reg) const;

  Zone* zone_;

  // environment_hints_ contains hints for the contents of the registers,
  // the accumulator and the parameters. The layout is as follows:
  // [ receiver | parameters | registers | accumulator | context | closure ]
  const int register_count_;
  const int parameter_count_;
  HintsVector environment_hints_;
  int register_base() const { return parameter_count_; }
  int accumulator_index() const { return register_base() + register_count_; }
  int current_context_index() const { return accumulator_index() + 1; }
  int function_closure_index() const { return current_context_index() + 1; }
  int environment_hints_size() const { return function_closure_index() + 1; }

  Hints return_value_hints_;
};

SerializerForBackgroundCompilation::Environment::Environment(
    Zone* zone, Isolate* isolate, int register_count, int parameter_count)
    : zone_(zone),
      register_count_(register_count),
      parameter_count_(parameter_count),
      environment_hints_(environment_hints_size(), Hints(zone), zone),
      return_value_hints_(zone) {}

SerializerForBackgroundCompilation::Environment::Environment(
    SerializerForBackgroundCompilation* serializer, Isolate* isolate,
    int register_count, int parameter_count, const HintsVector& arguments)
    : Environment(serializer->zone(), isolate, register_count,
                  parameter_count) {
  size_t param_count = static_cast<size_t>(parameter_count);

  // Copy the hints for the actually passed arguments, at most up to
  // the parameter_count.
  for (size_t i = 0; i < std::min(arguments.size(), param_count); ++i) {
    environment_hints_[i] = arguments[i];
  }

  Hints undefined_hint(serializer->zone());
  undefined_hint.AddConstant(
      serializer->broker()->isolate()->factory()->undefined_value());
  // Pad the rest with "undefined".
  for (size_t i = arguments.size(); i < param_count; ++i) {
    environment_hints_[i] = undefined_hint;
  }
}

int SerializerForBackgroundCompilation::Environment::RegisterToLocalIndex(
    interpreter::Register reg) const {
  // TODO(mslekova): We also want to gather hints for the context and
  // we already have data about the closure that we should record.
  if (reg.is_current_context()) return current_context_index();
  if (reg.is_function_closure()) return function_closure_index();
  if (reg.is_parameter()) {
    return reg.ToParameterIndex(parameter_count());
  } else {
    return register_base() + reg.index();
  }
}

SerializerForBackgroundCompilation::SerializerForBackgroundCompilation(
    JSHeapBroker* broker, Zone* zone, Handle<JSFunction> function)
    : broker_(broker),
      zone_(zone),
      shared_(function->shared(), broker->isolate()),
      feedback_(function->feedback_vector(), broker->isolate()),
      environment_(new (zone) Environment(
          zone, broker_->isolate(),
          shared_->GetBytecodeArray()->register_count(),
          shared_->GetBytecodeArray()->parameter_count())) {
  JSFunctionRef(broker, function).Serialize();
}

SerializerForBackgroundCompilation::SerializerForBackgroundCompilation(
    JSHeapBroker* broker, Zone* zone, FunctionBlueprint function,
    const HintsVector& arguments)
    : broker_(broker),
      zone_(zone),
      shared_(function.shared),
      feedback_(function.feedback),
      environment_(new (zone) Environment(
          this, broker->isolate(),
          shared_->GetBytecodeArray()->register_count(),
          shared_->GetBytecodeArray()->parameter_count(), arguments)) {}

Hints SerializerForBackgroundCompilation::Run() {
  SharedFunctionInfoRef shared(broker(), shared_);
  FeedbackVectorRef feedback(broker(), feedback_);
  if (shared.IsSerializedForCompilation(feedback)) {
    return Hints(zone());
  }
  shared.SetSerializedForCompilation(feedback);
  feedback.SerializeSlots();
  TraverseBytecode();
  return environment()->return_value_hints();
}

void SerializerForBackgroundCompilation::TraverseBytecode() {
  BytecodeArrayRef bytecode_array(
      broker(), handle(shared_->GetBytecodeArray(), broker()->isolate()));
  interpreter::BytecodeArrayIterator iterator(bytecode_array.object());

  for (; !iterator.done(); iterator.Advance()) {
    switch (iterator.current_bytecode()) {
#define DEFINE_BYTECODE_CASE(name)     \
  case interpreter::Bytecode::k##name: \
    Visit##name(&iterator);            \
    break;
      SUPPORTED_BYTECODE_LIST(DEFINE_BYTECODE_CASE)
#undef DEFINE_BYTECODE_CASE
      default: {
        environment()->ClearAccumulatorAndRegisterHints();
        break;
      }
    }
  }
}

void SerializerForBackgroundCompilation::VisitIllegal(
    interpreter::BytecodeArrayIterator* iterator) {
  UNREACHABLE();
}

void SerializerForBackgroundCompilation::VisitWide(
    interpreter::BytecodeArrayIterator* iterator) {
  UNREACHABLE();
}

void SerializerForBackgroundCompilation::VisitExtraWide(
    interpreter::BytecodeArrayIterator* iterator) {
  UNREACHABLE();
}

void SerializerForBackgroundCompilation::VisitLdaUndefined(
    interpreter::BytecodeArrayIterator* iterator) {
  environment()->accumulator_hints().Clear();
  environment()->accumulator_hints().AddConstant(
      broker()->isolate()->factory()->undefined_value());
}

void SerializerForBackgroundCompilation::VisitLdaNull(
    interpreter::BytecodeArrayIterator* iterator) {
  environment()->accumulator_hints().Clear();
  environment()->accumulator_hints().AddConstant(
      broker()->isolate()->factory()->null_value());
}

void SerializerForBackgroundCompilation::VisitLdaZero(
    interpreter::BytecodeArrayIterator* iterator) {
  environment()->accumulator_hints().Clear();
  environment()->accumulator_hints().AddConstant(
      handle(Smi::FromInt(0), broker()->isolate()));
}

void SerializerForBackgroundCompilation::VisitLdaSmi(
    interpreter::BytecodeArrayIterator* iterator) {
  environment()->accumulator_hints().Clear();
  environment()->accumulator_hints().AddConstant(handle(
      Smi::FromInt(iterator->GetImmediateOperand(0)), broker()->isolate()));
}

void SerializerForBackgroundCompilation::VisitLdaConstant(
    interpreter::BytecodeArrayIterator* iterator) {
  environment()->accumulator_hints().Clear();
  environment()->accumulator_hints().AddConstant(
      handle(iterator->GetConstantForIndexOperand(0), broker()->isolate()));
}

void SerializerForBackgroundCompilation::VisitLdar(
    interpreter::BytecodeArrayIterator* iterator) {
  environment()->accumulator_hints().Clear();
  environment()->accumulator_hints().Add(
      environment()->register_hints(iterator->GetRegisterOperand(0)));
}

void SerializerForBackgroundCompilation::VisitStar(
    interpreter::BytecodeArrayIterator* iterator) {
  interpreter::Register reg = iterator->GetRegisterOperand(0);
  environment()->register_hints(reg).Clear();
  environment()->register_hints(reg).Add(environment()->accumulator_hints());
}

void SerializerForBackgroundCompilation::VisitMov(
    interpreter::BytecodeArrayIterator* iterator) {
  interpreter::Register src = iterator->GetRegisterOperand(0);
  interpreter::Register dst = iterator->GetRegisterOperand(1);
  environment()->register_hints(dst).Clear();
  environment()->register_hints(dst).Add(environment()->register_hints(src));
}

void SerializerForBackgroundCompilation::VisitCreateClosure(
    interpreter::BytecodeArrayIterator* iterator) {
  Handle<SharedFunctionInfo> shared(
      SharedFunctionInfo::cast(iterator->GetConstantForIndexOperand(0)),
      broker()->isolate());

  FeedbackNexus nexus(feedback_, iterator->GetSlotOperand(1));
  Handle<Object> cell_value(nexus.GetFeedbackCell()->value(),
                            broker()->isolate());

  environment()->accumulator_hints().Clear();
  if (cell_value->IsFeedbackVector()) {
    environment()->accumulator_hints().AddFunctionBlueprint(
        {shared, Handle<FeedbackVector>::cast(cell_value)});
  }
}

void SerializerForBackgroundCompilation::VisitCallUndefinedReceiver(
    interpreter::BytecodeArrayIterator* iterator) {
  ProcessCallVarArgs(iterator, ConvertReceiverMode::kNullOrUndefined);
}

void SerializerForBackgroundCompilation::VisitCallUndefinedReceiver0(
    interpreter::BytecodeArrayIterator* iterator) {
  Hints receiver(zone());
  receiver.AddConstant(broker()->isolate()->factory()->undefined_value());

  const Hints& callee =
      environment()->register_hints(iterator->GetRegisterOperand(0));

  HintsVector parameters(zone());
  parameters.push_back(receiver);
  ProcessCallOrConstruct(callee, parameters);
}

void SerializerForBackgroundCompilation::VisitCallUndefinedReceiver1(
    interpreter::BytecodeArrayIterator* iterator) {
  Hints receiver(zone());
  receiver.AddConstant(broker()->isolate()->factory()->undefined_value());

  const Hints& callee =
      environment()->register_hints(iterator->GetRegisterOperand(0));
  const Hints& arg0 =
      environment()->register_hints(iterator->GetRegisterOperand(1));

  HintsVector parameters(zone());
  parameters.push_back(receiver);
  parameters.push_back(arg0);

  ProcessCallOrConstruct(callee, parameters);
}

void SerializerForBackgroundCompilation::VisitCallUndefinedReceiver2(
    interpreter::BytecodeArrayIterator* iterator) {
  Hints receiver(zone());
  receiver.AddConstant(broker()->isolate()->factory()->undefined_value());

  const Hints& callee =
      environment()->register_hints(iterator->GetRegisterOperand(0));
  const Hints& arg0 =
      environment()->register_hints(iterator->GetRegisterOperand(1));
  const Hints& arg1 =
      environment()->register_hints(iterator->GetRegisterOperand(2));

  HintsVector parameters(zone());
  parameters.push_back(receiver);
  parameters.push_back(arg0);
  parameters.push_back(arg1);

  ProcessCallOrConstruct(callee, parameters);
}

void SerializerForBackgroundCompilation::VisitCallAnyReceiver(
    interpreter::BytecodeArrayIterator* iterator) {
  ProcessCallVarArgs(iterator, ConvertReceiverMode::kAny);
}

void SerializerForBackgroundCompilation::VisitCallNoFeedback(
    interpreter::BytecodeArrayIterator* iterator) {
  ProcessCallVarArgs(iterator, ConvertReceiverMode::kNullOrUndefined);
}

void SerializerForBackgroundCompilation::VisitCallProperty(
    interpreter::BytecodeArrayIterator* iterator) {
  ProcessCallVarArgs(iterator, ConvertReceiverMode::kNullOrUndefined);
}

void SerializerForBackgroundCompilation::VisitCallProperty0(
    interpreter::BytecodeArrayIterator* iterator) {
  const Hints& callee =
      environment()->register_hints(iterator->GetRegisterOperand(0));
  const Hints& receiver =
      environment()->register_hints(iterator->GetRegisterOperand(1));

  HintsVector parameters(zone());
  parameters.push_back(receiver);

  ProcessCallOrConstruct(callee, parameters);
}

void SerializerForBackgroundCompilation::VisitCallProperty1(
    interpreter::BytecodeArrayIterator* iterator) {
  const Hints& callee =
      environment()->register_hints(iterator->GetRegisterOperand(0));
  const Hints& receiver =
      environment()->register_hints(iterator->GetRegisterOperand(1));
  const Hints& arg0 =
      environment()->register_hints(iterator->GetRegisterOperand(2));

  HintsVector parameters(zone());
  parameters.push_back(receiver);
  parameters.push_back(arg0);

  ProcessCallOrConstruct(callee, parameters);
}

void SerializerForBackgroundCompilation::VisitCallProperty2(
    interpreter::BytecodeArrayIterator* iterator) {
  const Hints& callee =
      environment()->register_hints(iterator->GetRegisterOperand(0));
  const Hints& receiver =
      environment()->register_hints(iterator->GetRegisterOperand(1));
  const Hints& arg0 =
      environment()->register_hints(iterator->GetRegisterOperand(2));
  const Hints& arg1 =
      environment()->register_hints(iterator->GetRegisterOperand(3));

  HintsVector parameters(zone());
  parameters.push_back(receiver);
  parameters.push_back(arg0);
  parameters.push_back(arg1);

  ProcessCallOrConstruct(callee, parameters);
}

void SerializerForBackgroundCompilation::VisitCallWithSpread(
    interpreter::BytecodeArrayIterator* iterator) {
  ProcessCallVarArgs(iterator, ConvertReceiverMode::kAny, true);
}

Hints SerializerForBackgroundCompilation::RunChildSerializer(
    FunctionBlueprint function, const HintsVector& arguments,
    bool with_spread) {
  if (with_spread) {
    DCHECK_LT(0, arguments.size());
    // Pad the missing arguments in case we were called with spread operator.
    // Drop the last actually passed argument, which contains the spread.
    // We don't know what the spread element produces. Therefore we pretend
    // that the function is called with the maximal number of parameters and
    // that we have no information about the parameters that were not
    // explicitly provided.
    HintsVector padded = arguments;
    padded.pop_back();  // Remove the spread element.
    // Fill the rest with empty hints.
    padded.resize(function.shared->GetBytecodeArray()->parameter_count(),
                  Hints(zone()));
    return RunChildSerializer(function, padded, false);
  }

  SerializerForBackgroundCompilation child_serializer(broker(), zone(),
                                                      function, arguments);
  return child_serializer.Run();
}

void SerializerForBackgroundCompilation::ProcessCallOrConstruct(
    const Hints& callee, const HintsVector& arguments, bool with_spread) {
  environment()->accumulator_hints().Clear();

  for (auto hint : callee.constants()) {
    if (!hint->IsJSFunction()) continue;

    Handle<JSFunction> function = Handle<JSFunction>::cast(hint);
    if (!function->shared()->IsInlineable()) continue;

    JSFunctionRef(broker(), function).Serialize();

    Handle<SharedFunctionInfo> shared(function->shared(), broker()->isolate());
    Handle<FeedbackVector> feedback(function->feedback_vector(),
                                    broker()->isolate());

    environment()->accumulator_hints().Add(
        RunChildSerializer({shared, feedback}, arguments, with_spread));
  }

  for (auto hint : callee.function_blueprints()) {
    if (!hint.shared->IsInlineable()) continue;
    environment()->accumulator_hints().Add(
        RunChildSerializer(hint, arguments, with_spread));
  }
}

void SerializerForBackgroundCompilation::ProcessCallVarArgs(
    interpreter::BytecodeArrayIterator* iterator,
    ConvertReceiverMode receiver_mode, bool with_spread) {
  const Hints& callee =
      environment()->register_hints(iterator->GetRegisterOperand(0));
  interpreter::Register first_reg = iterator->GetRegisterOperand(1);
  int reg_count = static_cast<int>(iterator->GetRegisterCountOperand(2));

  HintsVector arguments(zone());
  // The receiver is either given in the first register or it is implicitly
  // the {undefined} value.
  if (receiver_mode == ConvertReceiverMode::kNullOrUndefined) {
    Hints receiver(zone());
    receiver.AddConstant(broker()->isolate()->factory()->undefined_value());
    arguments.push_back(receiver);
  }
  environment()->ExportRegisterHints(first_reg, reg_count, arguments);

  ProcessCallOrConstruct(callee, arguments);
}

void SerializerForBackgroundCompilation::VisitReturn(
    interpreter::BytecodeArrayIterator* iterator) {
  environment()->return_value_hints().Add(environment()->accumulator_hints());
  environment()->ClearAccumulatorAndRegisterHints();
}

void SerializerForBackgroundCompilation::Environment::ExportRegisterHints(
    interpreter::Register first, size_t count, HintsVector& dst) {
  dst.resize(dst.size() + count, Hints(zone()));
  int reg_base = first.index();
  for (int i = 0; i < static_cast<int>(count); ++i) {
    dst.push_back(register_hints(interpreter::Register(reg_base + i)));
  }
}

void SerializerForBackgroundCompilation::VisitConstruct(
    interpreter::BytecodeArrayIterator* iterator) {
  const Hints& callee =
      environment()->register_hints(iterator->GetRegisterOperand(0));
  interpreter::Register first_reg = iterator->GetRegisterOperand(1);
  size_t reg_count = iterator->GetRegisterCountOperand(2);

  HintsVector arguments(zone());
  environment()->ExportRegisterHints(first_reg, reg_count, arguments);

  // TODO(mslekova): Support new.target.

  ProcessCallOrConstruct(callee, arguments);
}

void SerializerForBackgroundCompilation::VisitConstructWithSpread(
    interpreter::BytecodeArrayIterator* iterator) {
  const Hints& callee =
      environment()->register_hints(iterator->GetRegisterOperand(0));
  interpreter::Register first_reg = iterator->GetRegisterOperand(1);
  size_t reg_count = iterator->GetRegisterCountOperand(2);

  HintsVector arguments(zone());
  environment()->ExportRegisterHints(first_reg, reg_count, arguments);

  // TODO(mslekova): Support new.target.

  ProcessCallOrConstruct(callee, arguments, true);
}

#define DEFINE_SKIPPED_JUMP(name, ...)                  \
  void SerializerForBackgroundCompilation::Visit##name( \
      interpreter::BytecodeArrayIterator* iterator) {   \
    environment()->ClearAccumulatorAndRegisterHints();  \
  }
CLEAR_ENVIRONMENT_LIST(DEFINE_SKIPPED_JUMP)
#undef DEFINE_SKIPPED_JUMP

#define DEFINE_CLEAR_ACCUMULATOR(name, ...)             \
  void SerializerForBackgroundCompilation::Visit##name( \
      interpreter::BytecodeArrayIterator* iterator) {   \
    environment()->accumulator_hints().Clear();         \
  }
CLEAR_ACCUMULATOR_LIST(DEFINE_CLEAR_ACCUMULATOR)
#undef DEFINE_CLEAR_ACCUMULATOR

}  // namespace compiler
}  // namespace internal
}  // namespace v8
