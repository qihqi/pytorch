#include <torch/csrc/jit/serialization/export.h>

#include <c10/util/Exception.h>
#include <torch/csrc/jit/backends/backend_debug_handler.h>
#include <torch/csrc/jit/backends/backend_debug_info.h>
#include <torch/csrc/jit/frontend/source_range.h>
#include <torch/csrc/jit/ir/attributes.h>
#include <torch/csrc/jit/ir/ir.h>
#include <torch/csrc/jit/ir/type_hashing.h>
#include <torch/csrc/jit/mobile/function.h>
#include <torch/csrc/jit/mobile/interpreter.h>
#include <torch/csrc/jit/mobile/method.h>
#include <torch/csrc/jit/mobile/module.h>
#include <torch/csrc/jit/passes/inliner.h>
#include <torch/csrc/jit/runtime/instruction.h>
#include <torch/csrc/jit/serialization/callstack_debug_info_serialization.h>
#include <torch/csrc/jit/serialization/ivalue_serialization.h>
#include <torch/csrc/jit/serialization/import_export_constants.h>
#include <torch/csrc/jit/serialization/import_export_helpers.h>
#include <torch/csrc/jit/serialization/pickle.h>
#include <torch/csrc/jit/serialization/python_print.h>
#include <torch/csrc/jit/serialization/source_range_serialization.h>
#include <torch/csrc/jit/serialization/type_name_uniquer.h>
#include <torch/csrc/jit/serialization/mobile_bytecode_generated.h>
#include <third_party/flatbuffers/include/flatbuffers/flatbuffers.h>
// #include <third_party/flatbuffers/include/flatbuffers/minireflect.h>

#include <caffe2/serialize/inline_container.h>

#include <ATen/ATen.h>

#include <ATen/core/jit_type.h>
#include <ATen/core/qualified_name.h>
#include <string>
#include <unordered_set>
#include <vector>
#include <fstream>


namespace torch {
namespace jit {

char const* toString(OpCode op);

namespace {

using mobile::serialization::CreateArg;
using mobile::serialization::CreateOperator;
using mobile::serialization::CreateFunctionDirect;
using mobile::serialization::CreateDebugInfo;
using mobile::serialization::CreateObjectTypeDirect;
using flatbuffers::FlatBufferBuilder;

// Only compress these records if they're not tiny.
// The cpu cost of generating zip datastructs and compressing isn't
// well-spent for very small records.
static constexpr size_t kMinToCompress = 200;

void CreateAndAppendOperator(
    flatbuffers::FlatBufferBuilder& fbb,
    const std::string& name,
    const std::string& overload,
    int num_args,
    std::vector<flatbuffers::Offset<mobile::serialization::Operator>>* operators) {
  operators->push_back(CreateOperator(
      fbb, fbb.CreateString(name), fbb.CreateString(overload), num_args));
}

flatbuffers::Offset<
    flatbuffers::Vector<flatbuffers::Offset<flatbuffers::String>>>
CreateTypes(
    flatbuffers::FlatBufferBuilder& fbb,
    const std::vector<std::string>& type_strs) {
  return fbb.CreateVectorOfStrings(type_strs);
}

flatbuffers::Offset<jit::mobile::serialization::Schema> CreateFBSchema(
    flatbuffers::FlatBufferBuilder& fbb,
    const std::vector<Argument>& args,
    const std::vector<Argument>& returns,
    c10::TypePrinter type_printer,
    IValueFlatbufferSerializer* serializer) {
  std::vector<flatbuffers::Offset<jit::mobile::serialization::Arg>> arg_vec;
  arg_vec.reserve(args.size());
  std::vector<flatbuffers::Offset<jit::mobile::serialization::Arg>> return_vec;
  return_vec.reserve(returns.size());
  for (const auto& arg : args) {
    mobile::serialization::IValue type;
    flatbuffers::Offset<void> offset;
    std::tie(type, offset) = serializer->iValueToFB(fbb, arg.default_value());
    arg_vec.emplace_back(CreateArg(
        fbb,
        fbb.CreateString(arg.name()),
        fbb.CreateString(arg.type()->annotation_str(type_printer)),
        type, offset));
  }

  for (const auto& ret : returns) {
    mobile::serialization::IValue type;
    flatbuffers::Offset<void> offset;
    std::tie(type, offset) = serializer->iValueToFB(fbb, ret.default_value());
    return_vec.emplace_back(CreateArg(
        fbb,
        fbb.CreateString(ret.name()),
        fbb.CreateString(ret.type()->annotation_str(type_printer)),
        type, offset));
  }

  return CreateSchema(fbb, fbb.CreateVector(arg_vec), fbb.CreateVector(return_vec));
}

flatbuffers::Offset<jit::mobile::serialization::DebugInfo> CreateFBDebugInfo(
  flatbuffers::FlatBufferBuilder& fbb,
  const std::vector<int64_t>& debug_handles) {
  return CreateDebugInfo(fbb, fbb.CreateVector(debug_handles));
}


ExportModuleExtraFilesHook& GetExtraFilesHook() {
  static ExportModuleExtraFilesHook func = nullptr;
  return func;
}

static IValue Tup(std::vector<IValue> ivalues) {
  return c10::ivalue::Tuple::create(std::move(ivalues));
}

static IValue Table(
    const std::vector<std::pair<std::string, IValue>>& entries) {
  std::vector<IValue> ivalue_entries;
  ivalue_entries.reserve(entries.size());
  for (const auto& e : entries) {
    ivalue_entries.push_back(Tup({e.first, e.second}));
  }
  return Tup(std::move(ivalue_entries));
}

std::pair<IValue, IValue> getFunctionTuple(
    const Module& module,
    const Function& func,
    BackendDebugInfoRecorder& debug_info_recorder,
    const std::basic_string<char>& qn,
    TypeNameUniquer& type_name_uniquer_) {
        auto graph = func.graph()->copy();

  Inline(*graph);

  std::shared_ptr<MobileCode> code;
  code = std::make_shared<MobileCode>(
      graph,
      func.name(),
      BytecodeEmitDefaultValueForUnspecifiedArgMode::
          is_enabled() /* emit_default_input_instructions */);
  auto instructions_copy = code->instructions();

  // operator names
  std::vector<c10::OperatorName> opnames;
  std::vector<std::string> method_names;
  std::vector<int64_t> op_debug_handles;
  for (size_t i = 0; i < instructions_copy.size(); ++i) {
    Instruction ins = instructions_copy[i];
    if (ins.op == OP || ins.op == OPN) {
      auto node = code->instructions_source()[i];
      opnames.emplace_back(node->schema().operator_name());
    }
    // CALL nodes at this point represent built-in (i.e. non-Graph)
    // functions that were not inlined. Here we convert the CALL
    // instructions for these functions into INTERFACE_CALL instructions
    // s.t. at runtime, we will look up the Function* on the Type of the
    // 0th argument in the stack and call that directly.
    if (ins.op == CALL) {
      auto node = code->instructions_source()[i];
      if (node->kind() == prim::CallMethod) {
        // NB: replacing instruction
        auto method_name_idx =
            code->constant_table().size() + method_names.size();
        method_names.emplace_back(node->s(attr::name));
        Instruction new_instr{
            INTERFACE_CALL,
            static_cast<int32_t>(method_name_idx),
            static_cast<uint16_t>(node->inputs().size())};
        instructions_copy[i] = new_instr;
      } else {
        TORCH_INTERNAL_ASSERT(
            false, "Unsupported node kind on CALL opcode for mobile");
      }
    } else if (ins.op == RET) {
      auto node = code->instructions_source()[i];
      for (const auto& input : node->inputs()) {
        const auto& input_type = input->type();
        if (input_type->kind() == TypeKind::TupleType) {
          if (const auto& name_typed_input =
                  input_type->cast<at::NamedType>()) {
            TORCH_CHECK(
                !name_typed_input->name(),
                "A named tuple type is not supported in mobile module. ",
                "Workaround: instead of using a named tuple type's fields, ",
                "use a dictionary type's key-value pair itmes or ",
                "a pytorch class (class Foo(torch.nn.Module))'s attributes.'");
          }
        } else if (
            input_type->kind() == TypeKind::ListType ||
            input_type->kind() == TypeKind::DictType) {
          for (const TypePtr& element_type : input_type->containedTypes()) {
            TORCH_CHECK(
                element_type->kind() != TypeKind::ClassType,
                "Returining a list or dictionary with pytorch class type ",
                "is not supported in mobile module "
                "(List[Foo] or Dict[int, Foo] for class Foo(torch.nn.Module)). "
                "Workaround: instead of using pytorch class as their element type, ",
                "use a combination of list, dictionary, and single types.");
          }
        }
      }
    } else {
      TORCH_CHECK(
          isOpSupportedInMobile(ins.op),
          toString(ins.op),
          " is not supported in mobile module.");
    }
    auto node = code->instructions_source()[i];
    int64_t debug_handle = debug_info_recorder.getNextDebugHandle(node);
    // Note 1-to-1 correspondence between instructions and debug handles
    op_debug_handles.emplace_back(debug_handle);
  }

  // instructions
  std::vector<IValue> instructions;
  instructions.reserve(instructions_copy.size());
  for (Instruction ins : instructions_copy) {
    instructions.emplace_back(Tup({toString(ins.op), ins.X, ins.N}));
  }

  // operators
  std::vector<IValue> operators;
  auto op_to_specified_args = code->op_to_num_specified_args();
  operators.reserve(opnames.size());
  for (const auto& opname : opnames) {
    auto unique_name = c10::toString(opname);
    // For operator with vararg, adding default arguments would be confusing and
    // is not allowed. For an operator with num_args = -1, it means the number
    // of arguments is not available for this operator, we don't do any backward
    // compatibility adaptation at runtime.
    int num_args = -1;
    auto it = op_to_specified_args.find(unique_name);
    if (it != op_to_specified_args.end()) {
      num_args = it->second;
    }
    if (BytecodeEmitDefaultValueForUnspecifiedArgMode::is_enabled()) {
      operators.emplace_back(Tup({opname.name, opname.overload_name}));
    } else {
      operators.emplace_back(
          Tup({opname.name, opname.overload_name, num_args}));
    }
  }

  // constants
  //
  // Make a copy of the constants and append the method names
  // that we emitted for the converted INTERFACE_CALL nodes above.
  auto constants = code->constant_table();
  for (auto& method_name : method_names) {
    constants.emplace_back(std::move(method_name));
  }

  // types
  std::vector<IValue> types;
  types.reserve(code->type_table().size());
  static const std::string torch_prefix("__torch__");
  static const std::string class_prefix("__torch__.torch.classes");
  for (const TypePtr& t : code->type_table()) {
    auto type_str = t->annotation_str();
    if (type_str.find(torch_prefix) == 0) {
      TORCH_CHECK(
          type_str.find(class_prefix) == 0,
          "__torch__ types other than torchbind (__torch__.torch.classes)"
          "are not supported in lite interpreter. ",
          "Workaround: instead of using arbitrary class type (class Foo()), ",
          "define a pytorch class (class Foo(torch.nn.Module)).");
    }
    types.emplace_back(type_str);
  }

  // since the register location is embedded into the bytecode, pass the
  // register size
  auto register_size = static_cast<int>(code->register_size());

  auto codeTable = Table(
      {{"instructions", Tup(instructions)},
       {"operators", Tup(operators)},
       {"constants", Tup(constants)},
       {"types", Tup(types)},
       {"register_size", register_size}});

  // schema
  const auto& schema = func.getSchema();
  auto type_printer =
      [&](const c10::ConstTypePtr& t) -> c10::optional<std::string> {
    auto namedType = t->cast<c10::NamedType>();
    if (namedType && namedType->name()) {
      return type_name_uniquer_.getUniqueName(namedType).qualifiedName();
    }
    return c10::nullopt;
  };
  TORCH_CHECK(
      schema.overload_name().empty(), // @TODO: is this check correct?
      "Overloads are not supported in mobile modules.");
  TORCH_CHECK(
      !schema.is_vararg(), "Python *args are not supported in mobile modules.");
  TORCH_CHECK(
      !schema.is_varret(),
      "A variable number of return values is not supported in mobile modules.");
  auto makeArgTuple = [&](const std::vector<Argument>& args) {
    std::vector<IValue> argTables;
    for (auto&& arg : args) {
      TORCH_CHECK(
          !arg.N(),
          "Arguments with known list lengths are not supported in mobile modules.");
      TORCH_CHECK(
          !arg.kwarg_only(),
          "Keyword-only arguments are not supported in mobile modules.");
      /*
        This part adds the argument's name, type and default_value in
        `bytecode.pkl` This has to be consistent with the `code/` directory
        which has annotated py code of the entire module. `type_printer` uses
        `TypeNameUniquer` to get the managled name of the argument. This helps
        in having the right object reference when a class method is called using
        the `self` argument.
        arg.type()->annotation_str(type_printer) => mangled unique name of the
        module/submodule
      */
      argTables.emplace_back(Table({
          {"name", arg.name()},
          {"type", arg.type()->annotation_str(type_printer)},
          {"default_value", arg.default_value()},
      }));
    }
    return Tup(argTables);
  };
  auto schemaTable = Table({
      {"arguments", makeArgTuple(schema.arguments())},
      {"returns", makeArgTuple(schema.returns())},
  });

  // function tuple
  auto bytecode_vals = Tup({qn, codeTable, schemaTable});

  c10::optional<IValue> debug_info_vals;
  // module debug info
  // This is just a set of debug handles.
  // We always save debug handles.
  // debug handles generated by debug_handle_manager
  // will correspond to {source_range, inlinedCallStackPtr} which we will
  // serialize separately.
  IValue module_debug_tuple = c10::ivalue::Tuple::create(op_debug_handles);
  auto function_debug_info =
      Table({{"function_debug_handles", module_debug_tuple}});
  debug_info_vals = Tup({qn, function_debug_info});
  return std::make_pair(bytecode_vals, debug_info_vals);
}

void setstateTuple(
    const Module& module,
    const IValue& ivalue,
    std::vector<c10::IValue>& elements,
    std::unordered_set<std::string>& qn_cache,
    std::vector<c10::IValue>& debug_info_elements,
    BackendDebugInfoRecorder& debug_info_recorder,
    TypeNameUniquer& type_name_uniquer_) {
  if (!ivalue.isObject())
    return;
  auto obj = ivalue.toObject();
  auto type = obj->type();
  if (checkHasValidSetGetState(type)) {
    Function& setstate = type->getMethod("__setstate__");
    const auto qn =
        type_name_uniquer_.getUniqueName(obj->type()).qualifiedName() + "." +
        setstate.name();
    if (qn_cache.find(qn) != qn_cache.end()) {
      return;
    }
    if (setstate.isGraphFunction()) {
      auto func_tuple = getFunctionTuple(
          module, setstate, debug_info_recorder, qn, type_name_uniquer_);
      elements.push_back(func_tuple.first);
      qn_cache.emplace(qn);
      debug_info_elements.push_back(func_tuple.second);
    }
  } else {
    for (size_t i = 0, n = type->numAttributes(); i < n; ++i) {
      setstateTuple(
          module,
          obj->getSlot(i),
          elements,
          qn_cache,
          debug_info_elements,
          debug_info_recorder,
          type_name_uniquer_);
    }
  }
}

bool isLoweredModule(const Module& m) {
  c10::QualifiedName type_name;
  if (m.type()->name()) {
    type_name = m.type()->name().value();
  }
  bool isLoweredModule = false;
  for (const auto& atom : type_name.atoms()) {
    if (atom == "LoweredModule") {
      isLoweredModule = true;
      break;
    }
  }
  return isLoweredModule;
}

// Check if the global static map of backend debug info
// contains debug info for this module and any of its children.
// If so combine all the maps together and return one.
void getBackendDebugInfoMap(
    const Module& m,
    BackendDebugInfoMapType& debug_map) {
  if (isLoweredModule(m)) {
    auto backend_debug_info =
        m.attr("__backend_debug_info").toCustomClass<PyTorchBackendDebugInfo>();
    const auto& map = backend_debug_info->getDebugInfoMap();
    if (map) {
      debug_map.insert(map.value().begin(), map.value().end());
    }
  }
  for (const auto& c : m.children()) {
    getBackendDebugInfoMap(c, debug_map);
  }
}

SourceRangeRecords getBackendSourceRanges(const Module& m) {
  SourceRangeRecords sr_records;
  if (isLoweredModule(m)) {
    constexpr size_t kSourceRange = 1;
    auto backend_debug_info =
        m.attr("__backend_debug_info").toCustomClass<PyTorchBackendDebugInfo>();
    const auto& map = backend_debug_info->getDebugInfoMap();
    if (map) {
      const auto& map_val = map.value();
      // This map is map of debug handle-to-DebugInfoTuple
      // DebugInfoTuple= <source range, op name, inlined_cs_ptr>
      for (const auto& it : map_val) {
        auto& source_range =
            std::get<kDebugInfoTupleSourceRangeIndex>(it.second);
        sr_records.emplace_back(
            std::numeric_limits<size_t>::max(), source_range);
        // NOLINTNEXTLINE(performance-unnecessary-copy-initialization)
        auto cs_ptr = std::get<kDebugInfoTupleInlinedCSIndex>(it.second);
        if (cs_ptr) {
          for (const auto& e : cs_ptr->vec()) {
            // NOLINTNEXTLINE(performance-unnecessary-copy-initialization)
            const auto sr = std::get<kSourceRange>(e);
            sr_records.emplace_back(std::numeric_limits<size_t>::max(), sr);
          }
        }
      }
    }
  }
  for (const auto& c : m.children()) {
    const auto& child_sr_records = getBackendSourceRanges(c);
    sr_records.reserve(sr_records.size() + child_sr_records.size());
    std::move(
        child_sr_records.begin(),
        child_sr_records.end(),
        std::back_inserter(sr_records));
  }
  return sr_records;
}

} // namespace

void moduleMethodsTuple(
    const Module& module,
    std::vector<c10::IValue>& elements, // note: appended to in-place
    std::vector<c10::IValue>& debug_info_elements,
    BackendDebugInfoRecorder& debug_info_recorder,
    TypeNameUniquer& type_name_uniquer_) {
  auto methods = module.get_methods();
  std::unordered_set<std::string> qn_cache;
  // top level methods
  for (const auto& method : methods) {
    const auto qn = method.function().qualname().qualifiedName();
    if (qn_cache.find(qn) != qn_cache.end()) {
      continue;
    }
    auto func_tuple = getFunctionTuple(
        module, method.function(), debug_info_recorder, qn, type_name_uniquer_);
    elements.push_back(func_tuple.first);
    qn_cache.emplace(qn);
    debug_info_elements.push_back(func_tuple.second);
  }

  // __setstate__ of all components
  setstateTuple(
      module,
      module._ivalue(),
      elements,
      qn_cache,
      debug_info_elements,
      debug_info_recorder,
      type_name_uniquer_);
}

std::tuple<
    std::vector<c10::OperatorName>,
    std::vector<std::string>,
    std::vector<int64_t>>
convertInstructionsForMobile(
  const MobileCode* code,
  std::vector<Instruction>* instructions,
  BackendDebugInfoRecorder& debug_info_recorder
) {
  std::vector<c10::OperatorName> opnames;
  std::vector<std::string> method_names;
  std::vector<int64_t> op_debug_handles;
  for (size_t i = 0; i < instructions->size(); ++i) {
    Instruction ins = instructions->at(i);
    if (ins.op == OP || ins.op == OPN) {
      auto node = code->instructions_source()[i];
      opnames.emplace_back(node->schema().operator_name());
    }
    // CALL nodes at this point represent built-in (i.e. non-Graph)
    // functions that were not inlined. Here we convert the CALL
    // instructions for these functions into INTERFACE_CALL instructions
    // s.t. at runtime, we will look up the Function* on the Type of the
    // 0th argument in the stack and call that directly.
    if (ins.op == CALL) {
      auto node = code->instructions_source()[i];
      if (node->kind() == prim::CallMethod) {
        // NB: replacing instruction
        auto method_name_idx =
            code->constant_table().size() + method_names.size();
        method_names.emplace_back(node->s(attr::name));
        Instruction new_instr{
            INTERFACE_CALL,
            static_cast<int32_t>(method_name_idx),
            static_cast<uint16_t>(node->inputs().size())};
        instructions->at(i) = new_instr;
      } else {
        TORCH_INTERNAL_ASSERT(
            false, "Unsupported node kind on CALL opcode for mobile");
      }
    } else if (ins.op == RET) {
      auto node = code->instructions_source()[i];
      for (const auto& input : node->inputs()) {
        const auto& input_type = input->type();
        if (input_type->kind() == TypeKind::TupleType) {
          if (const auto& name_typed_input =
                  input_type->cast<at::NamedType>()) {
            TORCH_CHECK(
                !name_typed_input->name(),
                "A named tuple type is not supported in mobile module. ",
                "Workaround: instead of using a named tuple type's fields, ",
                "use a dictionary type's key-value pair itmes or ",
                "a pytorch class (class Foo(torch.nn.Module))'s attributes.'");
          }
        } else if (
            input_type->kind() == TypeKind::ListType ||
            input_type->kind() == TypeKind::DictType) {
          for (const TypePtr& element_type : input_type->containedTypes()) {
            TORCH_CHECK(
                element_type->kind() != TypeKind::ClassType,
                "Returining a list or dictionary with pytorch class type ",
                "is not supported in mobile module "
                "(List[Foo] or Dict[int, Foo] for class Foo(torch.nn.Module)). "
                "Workaround: instead of using pytorch class as their element type, ",
                "use a combination of list, dictionary, and single types.");
          }
        }
      }
    } else {
      TORCH_CHECK(
          isOpSupportedInMobile(ins.op),
          toString(ins.op),
          " is not supported in mobile module.");
    }
    auto node = code->instructions_source()[i];
    int64_t debug_handle = debug_info_recorder.getNextDebugHandle(node);
    // Note 1-to-1 correspondence between instructions and debug handles
    op_debug_handles.emplace_back(debug_handle);
  }
  return std::make_tuple(opnames, method_names, op_debug_handles);
}

flatbuffers::Offset<mobile::serialization::Function>
functionToFlatbuffers(
    FlatBufferBuilder& fbb,
    const Module& module,
    const Function& func,
    BackendDebugInfoRecorder& debug_info_recorder,
    const std::basic_string<char>& qn,
    TypeNameUniquer& type_name_uniquer_,
    IValueFlatbufferSerializer* serializer
) {

  auto graph = func.graph()->copy();
  Inline(*graph);

  std::shared_ptr<MobileCode> code;
  code = std::make_shared<MobileCode>(
      graph,
      func.name(),
      BytecodeEmitDefaultValueForUnspecifiedArgMode::
          is_enabled() /* emit_default_input_instructions */);
  auto instructions_copy = code->instructions();

  std::vector<c10::OperatorName> opnames;
  std::vector<std::string> method_names;
  std::vector<int64_t> op_debug_handles;
  std::tie(opnames, method_names, op_debug_handles) = convertInstructionsForMobile(
    code.get(), &instructions_copy, debug_info_recorder);

  // instructions
  std::vector<mobile::serialization::Instruction> instruction_vector;
  for (const auto& inst: instructions_copy) {
    instruction_vector.emplace_back(inst.op, inst.N, inst.X);
  }

  // operators
  std::vector<flatbuffers::Offset<mobile::serialization::Operator>> operator_vector;
  auto op_to_specified_args = code->op_to_num_specified_args();
  operator_vector.reserve(opnames.size());
  for (const auto& opname : opnames) {
    auto unique_name = c10::toString(opname);
    // For operator with vararg, adding default arguments would be confusing and
    // is not allowed. For an operator with num_args = -1, it means the number
    // of arguments is not available for this operator, we don't do any backward
    // compatibility adaptation at runtime.
    int num_args = -1;
    auto it = op_to_specified_args.find(unique_name);
    if (it != op_to_specified_args.end()) {
      num_args = it->second;
    }
    CreateAndAppendOperator(fbb, opname.name, opname.overload_name,
        num_args, &operator_vector);
  }

  const auto& constants = code->constant_table();
  std::vector<uint8_t> constant_types;
  std::vector<flatbuffers::Offset<void>> constant_offsets;
  std::tie(constant_types, constant_offsets) = serializer->iValueIteratorToFB(
      fbb, constants.begin(), constants.end());

  // types
  std::vector<flatbuffers::Offset<flatbuffers::String>> type_str_offsets;
  type_str_offsets.reserve(code->type_table().size());
  static const std::string torch_prefix("__torch__");
  static const std::string class_prefix("__torch__.torch.classes");
  for (const TypePtr& t : code->type_table()) {
    auto type_str = t->annotation_str();
    if (type_str.find(torch_prefix) == 0) {
      TORCH_CHECK(
          type_str.find(class_prefix) == 0,
          "__torch__ types other than torchbind (__torch__.torch.classes)"
          "are not supported in lite interpreter. ",
          "Workaround: instead of using arbitrary class type (class Foo()), ",
          "define a pytorch class (class Foo(torch.nn.Module)).");
    }
    type_str_offsets.emplace_back(fbb.CreateString(type_str));
  }

  // since the register location is embedded into the bytecode, pass the
  // register size
  auto register_size = static_cast<int>(code->register_size());

  // schema
  const auto& schema = func.getSchema();
  auto type_printer =
      [&](const c10::ConstTypePtr& t) -> c10::optional<std::string> {
    auto namedType = t->cast<c10::NamedType>();
    if (namedType && namedType->name()) {
      return type_name_uniquer_.getUniqueName(namedType).qualifiedName();
    }
    return c10::nullopt;
  };
  TORCH_CHECK(
      schema.overload_name().empty(), // @TODO: is this check correct?
      "Overloads are not supported in mobile modules.");
  TORCH_CHECK(
      !schema.is_vararg(), "Python *args are not supported in mobile modules.");
  TORCH_CHECK(
      !schema.is_varret(),
      "A variable number of return values is not supported in mobile modules.");

  auto schema_offset = CreateFBSchema(fbb, schema.arguments(), schema.returns(), type_printer, serializer);
  auto debug_info_offset = CreateFBDebugInfo(fbb, op_debug_handles);

  auto function_offset = CreateFunctionDirect(
    fbb,
    qn.c_str(),
    &instruction_vector,
    &operator_vector,
    &constant_types,
    &constant_offsets,
    &type_str_offsets,
    register_size,
    schema_offset,
    debug_info_offset);
  return function_offset;
}

flatbuffers::DetachedBuffer
moduleToFlatbuffers(
    const Module& module,
    std::vector<c10::IValue>& debug_info_elements,
    BackendDebugInfoRecorder& debug_info_recorder,
    TypeNameUniquer& type_name_uniquer_,
    IValueFlatbufferSerializer* serializer) {
  auto methods = module.get_methods();
  std::unordered_set<std::string> qn_cache;
  // top level methods
  std::vector<flatbuffers::Offset<mobile::serialization::Function>> functions;
  FlatBufferBuilder fbb;
  for (const auto& method : methods) {
    const auto qn = method.function().qualname().qualifiedName();
    if (qn_cache.find(qn) != qn_cache.end()) {
      continue;
    }
    auto func_offset = functionToFlatbuffers(
        fbb, module, method.function(), debug_info_recorder, qn, type_name_uniquer_, serializer);
    functions.push_back(func_offset);
    qn_cache.emplace(qn);
  }


  auto functions_offset = fbb.CreateVector(functions);
  flatbuffers::Offset<mobile::serialization::Object> ivalue_offset = serializer->objectToFB(fbb, module._ivalue());

  // at this point, serializer contains all type infos used:

  std::vector<flatbuffers::Offset<mobile::serialization::ObjectType>> obj_types;
  for (const auto& classptr : serializer->memoized_class_types_) {
    std::vector<flatbuffers::Offset<flatbuffers::String>> attr_names(classptr->numAttributes());
    for (size_t i = 0, n = classptr->numAttributes(); i < n; ++i) {
      attr_names[i] = fbb.CreateString(classptr->getAttributeName(i));
    }
    obj_types.push_back(
      CreateObjectTypeDirect(fbb, classptr->name()->qualifiedName().c_str(), &attr_names));
  }
  auto obj_types_offset = fbb.CreateVector(obj_types);

  int i = 0;
  std::vector<flatbuffers::Offset<mobile::serialization::StorageData>> storage_data;
  for (const auto& td : serializer->tensor_data_) {
    WriteableTensorData writable_td = getWriteableTensorData(td);
    auto storage_offset = mobile::serialization::CreateStorageData(
      fbb, fbb.CreateVector(reinterpret_cast<const int8_t*>(writable_td.data()), writable_td.sizeInBytes()));
    storage_data.push_back(storage_offset);
  }


  auto mod = CreateModule(fbb, functions_offset, obj_types_offset, ivalue_offset,
    fbb.CreateVector(storage_data));
  fbb.Finish(mod);
  return fbb.Release();
}

void SetExportModuleExtraFilesHook(ExportModuleExtraFilesHook hook) {
  GetExtraFilesHook() = std::move(hook);
}

void ScriptModuleSerializer::serialize(
    const Module& module,
    const ExtraFilesMap& extra_files,
    bool bytecode_format,
    bool save_mobile_debug_info,
    bool use_flatbuffer) {
  C10_LOG_API_USAGE_ONCE("torch.script.save");
  writeExtraFiles(module, extra_files);
  // Serialize the model object
  // Then we serialize all code info.
  convertTypes(module.type());
  writeFiles("code/");
  // The tensor constants from the code are written to a separate archive
  // so loading the code does not depend on loading the data
  std::vector<IValue> ivalue_constants(
      constant_table_.begin(), constant_table_.end());
  if (bytecode_format) {
    writeArchive(
        c10::ivalue::Tuple::create(ivalue_constants),
        /*archive_name=*/"constants",
        /*archive_dir=*/"",
        /*tensor_dir=*/"constants/",
        /*use_storage_context=*/true);

    writeByteCode(module, save_mobile_debug_info, use_flatbuffer);
  } else {
    std::cerr << " Why am i here " << std::endl;
    writeArchive(
        module._ivalue(),
        /*archive_name=*/"data",
        /*archive_dir=*/"",
        /*tensor_dir=*/"data/");
    writeArchive(
        c10::ivalue::Tuple::create(ivalue_constants),
        /*archive_name=*/"constants",
        /*archive_dir=*/"",
        /*tensor_dir=*/"constants/");
  }
  // Acquires and sets minimum (dynamic) version
  for (auto& item : file_streams_) {
    writer_.setMinVersion(item.value().minVersion());
  }
}

void ScriptModuleSerializer::writeArchive(
    const IValue& value,
    const std::string& archive_name,
    const std::string& archive_dir,
    const std::string& tensor_dir,
    bool use_storage_context) {
  std::vector<char> data;
  // Vector to capture the run-time class types during pickling the IValues
  std::vector<c10::ClassTypePtr> memoizedClassTypes;
  std::vector<std::string> tensor_names;
  // tensors that are already serialized in use_storage_context
  std::unordered_set<std::string> serialized_tensors;
  Pickler data_pickle(
      [&](const char* buf, size_t size) {
        data.insert(data.end(), buf, buf + size);
      },
      nullptr,
      [&](const c10::ClassTypePtr& t) {
        return type_name_uniquer_.getUniqueName(t);
      },
      &memoizedClassTypes,
      [&](const at::Tensor& tensor) {
        // returns a string to use in picker.cpp as storage obj key
        if (use_storage_context) {
          bool already_serialized =
              storage_context_.hasStorage(tensor.storage());
          std::string tensor_name =
              std::to_string(
                  storage_context_.getOrAddStorage(tensor.storage())) +
              ".storage";
          if (already_serialized) {
            // this case is hit when storage has been serialized already
            // from a torch.package context
            serialized_tensors.insert(tensor_name);
          }
          tensor_names.push_back(tensor_name);
        } else {
          tensor_names.push_back(std::to_string(tensor_names.size()));
        }
        return tensor_names.back();
      });
  data_pickle.protocol();
  data_pickle.pushIValue(value);
  data_pickle.stop();
  // write out tensor data
  size_t i = 0;
  std::string prefix = archive_name + "/";

  TORCH_INTERNAL_ASSERT(tensor_names.size() == data_pickle.tensorData().size());

  for (const auto& td : data_pickle.tensorData()) {
    WriteableTensorData writable_td = getWriteableTensorData(td);
    std::string tensor_name = tensor_names[i++];
    if (use_storage_context && serialized_tensors.count(tensor_name)) {
      // storage has been serialzed already, skip
      continue;
    }
    writer_.writeRecord(
        tensor_dir + tensor_name,
        writable_td.data(),
        writable_td.sizeInBytes());
  }

  std::string fname = archive_dir + archive_name + ".pkl";
  writer_.writeRecord(fname, data.data(), data.size());

  // serialize all the captured run-time class types
  for (const c10::ClassTypePtr& wroteType : memoizedClassTypes) {
    convertNamedType(wroteType);
  }
}

void ScriptModuleSerializer::writeExtraFiles(
    const Module& module,
    const ExtraFilesMap& extra_files) {
  // Write out extra files.
  for (const auto& kv : extra_files) {
    const std::string key = "extra/" + kv.first;
    writer_.writeRecord(key, kv.second.data(), kv.second.size());
  }
  auto hook = GetExtraFilesHook();
  if (hook) {
    ExtraFilesMap hook_files = hook(module);
    for (const auto& kv : hook_files) {
      // Checks if the hooked file is already written in extra files,
      //   if so, skips it and warns
      if (extra_files.find(kv.first) != extra_files.end()) {
        TORCH_WARN_ONCE(
            "An extra files hook attempted to write ",
            kv.first,
            " but ",
            "this is already written in extra files and so will be skipped. ",
            "This warning will only appear once per process.");
        continue;
      }
      const std::string key = "extra/" + kv.first;
      writer_.writeRecord(key, kv.second.data(), kv.second.size());
    }
  }
}

void ScriptModuleSerializer::updateSourceRangeTags(
    const SourceRangeRecords& ranges) {
  for (const auto& range : ranges) {
    if (source_range_tags_.find(range.range) == source_range_tags_.end()) {
      source_range_tags_[range.range] = current_source_range_tag_;
      current_source_range_tag_++;
    }
  }
}

void ScriptModuleSerializer::convertTypes(const at::NamedTypePtr& root_type) {
  class_deps_.add(root_type);
  for (size_t i = 0; i < class_deps_.size(); ++i) {
    // note: convertNameType may extend class_deps_, so re-checking .size() is
    // necessary
    convertNamedType(class_deps_[i]);
  }
}

void ScriptModuleSerializer::writeFiles(const std::string& code_dir) {
  current_source_range_tag_ = 0;
  // Mapping of filename => src. We need this because multiple classes may go
  // in the same file (e.g. foo.bar.Baz and foo.bar.Qux)
  for (auto& item : file_streams_) {
    const std::string filename = qualifierToArchivePath(item.key(), code_dir);

    std::string src = item.value().str();

    writer_.writeRecord(
        filename,
        src.c_str(),
        src.size(),
        src.size() > kMinToCompress /*compress*/);

    // Write out the debug information
    std::string debugFilename = filename + ".debug_pkl";
    SourceRangePickler source_range_pickler;
    updateSourceRangeTags(item.value().ranges());
    auto range_data =
        source_range_pickler.pickle(item.value().ranges(), source_range_tags_);
    writer_.writeRecord(
        debugFilename,
        range_data.data(),
        range_data.size(),
        range_data.size() > kMinToCompress /*compress*/);
  }
}


void ScriptModuleSerializer::writeByteCode(
    const Module& module,
    const bool save_mobile_debug_info,
    const bool use_flatbuffers) {
  BackendDebugInfoRecorder debug_info_recorder;
  int64_t version_to_write = caffe2::serialize::kProducedBytecodeVersion;
  std::vector<c10::IValue> debug_info_elements;
  // Always save debug handles
  debug_info_elements.emplace_back(static_cast<int64_t>(version_to_write));

  if (use_flatbuffers) {
    IValueFlatbufferSerializer serializer;
    flatbuffers::DetachedBuffer buffer = moduleToFlatbuffers(
        module,
        debug_info_elements,
        debug_info_recorder,
        type_name_uniquer_,
        &serializer);

    std::fstream outfile( writer_.archiveName() + ".ff", std::ios::out | std::ios::binary);
    outfile.write((char*)buffer.data(), buffer.size());
    outfile.close();
  } else {
      writeArchive(
      module._ivalue(),
      /*archive_name=*/"data",
      /*archive_dir=*/"",
      /*tensor_dir=*/"data/");
    std::vector<c10::IValue> elements;
    elements.emplace_back(static_cast<int64_t>(version_to_write));
    moduleMethodsTuple(
        module,
        elements,
        debug_info_elements,
        debug_info_recorder,
        type_name_uniquer_);
    auto telements = Tup(std::move(elements));
    writeArchive(
        telements,
        /*archive_name=*/"bytecode",
        /*archive_dir=*/"",
        /*tensor_dir=*/"constants/",
        /*use_storage_context=*/true);

    auto debug_info_telements = Tup(std::move(debug_info_elements));

    // At the moment keeping this feature experimental
    // since we have not evaluated how this affect model size
    // and we have not build any utility to strip off debug info
    // when desired
    // TODO: Build utility to strip off debug map. It should also do the
    // same for debug_pkl files
    if (save_mobile_debug_info) {
      // Note that stripping off debug map will not strip off
      // debug handles.
      // The reason we save debug handles conditionally is so that
      // we dont end up with a model that has debug handles but has not
      // debug map to correlate debug handels with.
      // Once we have a model with both handles and debug map, we can
      // strip off debug map and have a lean model served to production.
      // If exception ocurrs we have a model with debug map that can be
      // used to symbolicate debug handles
      writeArchive(
          debug_info_telements,
          /*archive_name=*/"mobile_debug_handles",
          /*archive_dir=*/"",
          /*tensor_dir=*/"mobile_debug_handles/");
      static constexpr size_t kMinToCompress = 200;
      // For delegated backends get source ranges that are in the debug info
      // map. Since delegated backend replace original module with lowered
      // module we will not serialize original module's code which is what would
      // have contained source range. Since we dont have that anymore, extract
      // source ranges out of delegated module and store in a separate archive.
      // Note that we must do this first because in order to serialize inlined
      // CS appropriate source_range_tags must have been generated.
      auto backend_source_range_records = getBackendSourceRanges(module);
      SourceRangePickler source_range_pickler;
      updateSourceRangeTags(backend_source_range_records);
      auto range_data = source_range_pickler.pickle(
          backend_source_range_records, source_range_tags_);
      std::string debugFilename = "delegated_backends.debug_pkl";
      writer_.writeRecord(
          debugFilename,
          range_data.data(),
          range_data.size(),
          range_data.size() > kMinToCompress /*compress*/);

      // For delegated backends get debug_info_map
      // This is merged with other debug_info_map of other modules
      // which were not delegated.
      BackendDebugInfoMapType backend_debug_info_map;
      getBackendDebugInfoMap(module, backend_debug_info_map);
      // Now get the debug-handles-to-inlined-cs-ptr-map
      // And serialize that in a separate archive
      auto debug_handle_cs_ptr_map = debug_info_recorder.stopRecording();
      debug_handle_cs_ptr_map.insert(
          backend_debug_info_map.begin(), backend_debug_info_map.end());
      CallStackDebugInfoPickler cs_debug_info_pickler;
      auto cs_data = cs_debug_info_pickler.pickle(
          debug_handle_cs_ptr_map, source_range_tags_);
      // Write out map: [debug-handle, {source range, InlinedCallStack}]
      std::string filename = "callstack_debug_map.pkl";
      writer_.writeRecord(
          filename,
          cs_data.data(),
          cs_data.size(),
          cs_data.size() > kMinToCompress /*compress*/);
    }
  }
}

void ScriptModuleSerializer::convertNamedType(
    const c10::NamedTypePtr& class_type) {
  if (converted_types_.count(class_type)) {
    return;
  }
  converted_types_.insert(class_type);
  auto qualname = type_name_uniquer_.getUniqueName(class_type);
  std::string qualifier = qualname.prefix();
  PythonPrint* pp = file_streams_.find(qualifier);

  auto type_printer =
      [&](const c10::ConstTypePtr& t) -> c10::optional<std::string> {
    auto namedType = t->cast<c10::NamedType>();
    if (namedType && namedType->name()) {
      return type_name_uniquer_.getUniqueName(namedType).qualifiedName();
    }
    return c10::nullopt;
  };
  if (!pp) {
    pp = &file_streams_.insert(
        std::move(qualifier),
        PythonPrint(
            constant_table_,
            class_deps_,
            type_printer,
            /*enforce_importable=*/true));
  }
  pp->printNamedType(class_type);
}

void ScriptModuleSerializer::serialize_unified_format(
    Module& module,
    uint64_t script_module_id) {
  const std::string archive_dir =
      ".data/ts_code/" + std::to_string(script_module_id) + "/";

  // Serialize the model object
  writeArchive(
      module._ivalue(),
      "data",
      archive_dir,
      /*tensor_dir=*/".data/",
      /*use_storage_context=*/true);
  // Then we serialize all code info.
  convertTypes(module.type());
  // The tensor constants from the code are written to a separate archive
  // so loading the code does not depend on loading the data
  std::vector<IValue> ivalue_constants(
      constant_table_.begin(), constant_table_.end());
  writeArchive(
      c10::ivalue::Tuple::create(ivalue_constants),
      "constants",
      archive_dir,
      /*tensor_dir=*/".data/",
      /*use_storage_context=*/true);

  // Note: writeFiles() call needs to be made in addition to calling this
  // function to have the code actually saved (tensors are saved)
}

SerializationStorageContext& ScriptModuleSerializer::storage_context() {
  return storage_context_;
}

void ExportModule(
    const Module& module,
    std::ostream& out,
    const ExtraFilesMap& extra_files,
    bool bytecode_format,
    bool save_mobile_debug_info,
    bool use_flatbuffer) {
  caffe2::serialize::PyTorchStreamWriter writer(
      [&](const void* buf, size_t nbytes) -> size_t {
        out.write(static_cast<const char*>(buf), nbytes);
        return !out ? 0 : nbytes;
      });
  ScriptModuleSerializer serializer(writer);
  serializer.serialize(
      module, extra_files, bytecode_format, save_mobile_debug_info, use_flatbuffer);
}

void ExportModule(
    const Module& module,
    const std::string& filename,
    const ExtraFilesMap& extra_files,
    bool bytecode_format,
    bool save_mobile_debug_info,
    bool use_flatbuffer) {
  caffe2::serialize::PyTorchStreamWriter writer(filename);
  ScriptModuleSerializer serializer(writer);
  serializer.serialize(
      module, extra_files, bytecode_format, save_mobile_debug_info, use_flatbuffer);
}

void ExportModule(
    const Module& module,
    const std::function<size_t(const void*, size_t)>& writer_func,
    const ExtraFilesMap& extra_files,
    bool bytecode_format,
    bool save_mobile_debug_info,
    bool use_flatbuffer) {
  caffe2::serialize::PyTorchStreamWriter writer(writer_func);
  ScriptModuleSerializer serializer(writer);
  serializer.serialize(
      module, extra_files, bytecode_format, save_mobile_debug_info, use_flatbuffer);
}

namespace {
void export_opnames(const script::Module& m, std::set<std::string>& opnames) {
  std::vector<c10::IValue> elements;
  std::vector<c10::IValue> debug_info_elements;
  BackendDebugInfoRecorder dummy;
  TypeNameUniquer dummy_uniquer = TypeNameUniquer();
  moduleMethodsTuple(m, elements, debug_info_elements, dummy, dummy_uniquer);
  for (const auto& element : elements) {
    auto table = element.toTuple()->elements()[1];
    auto row =
        table.toTuple()->elements().at(BYTECODE_INDEX_OPERATOR).toTuple();
    TORCH_INTERNAL_ASSERT(
        row->elements().at(0).toStringRef() == "operators",
        "Expected operators but found ",
        row->elements().at(0).toStringRef());
    const auto& ops_list = row->elements().at(1).toTuple()->elements();
    for (const auto& op : ops_list) {
      auto op_item = op.toTuple()->elements();
      TORCH_CHECK(
          op_item.size() >= 2,
          "There should be either two parts (name and overload name), ",
          "or three parts (name, overload name and number of specified args) ",
          "for an operator.");
      auto opname = op_item[0].toString()->string();
      auto overload = op_item[1].toString()->string();
      // NOLINTNEXTLINE(performance-inefficient-string-concatenation)
      opnames.emplace(overload.empty() ? opname : opname + "." + overload);
    }
  }
}
} // namespace

std::vector<std::string> export_opnames(const script::Module& m) {
  std::set<std::string> names;
  export_opnames(m, names);
  return std::vector<std::string>(names.begin(), names.end());
}

// Thread local flag (only happens in export, i.e. on server side)
// to control if instructions for bytecode default inputs are emitted
// or not. It's the major difference between bytecode v5 and v6.
thread_local bool emitBytecodeDefaultInputs =
    caffe2::serialize::kProducedBytecodeVersion <= 5 ? true : false;
bool BytecodeEmitDefaultValueForUnspecifiedArgMode::is_enabled() {
  return emitBytecodeDefaultInputs;
}
void BytecodeEmitDefaultValueForUnspecifiedArgMode::set_enabled(bool enabled) {
  emitBytecodeDefaultInputs = enabled;
}

} // namespace jit
} // namespace torch
