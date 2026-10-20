#include "ia/gpu/ir.hpp"

#include <string_view>

namespace ia::gpu::ir {

std::string_view type_name(Type t) noexcept {
    switch (t) {
    case Type::U32:  return "u32";
    case Type::F32:  return "f32";
    case Type::U64:  return "u64";
    case Type::F64:  return "f64";
    case Type::Void: return "void";
    }
    return "?";
}

std::string_view op_name(Op op) noexcept {
    switch (op) {
    case Op::Mov:        return "Mov";
    case Op::FAdd:       return "FAdd";
    case Op::FMul:       return "FMul";
    case Op::IAdd3:      return "IAdd3";
    case Op::IAdd:       return "IAdd";
    case Op::LOp3:       return "LOp3";
    case Op::Exit:       return "Exit";
    case Op::StoreColor: return "StoreColor";
    case Op::Unknown:    return "Unknown";
    }
    return "?";
}

} // namespace ia::gpu::ir