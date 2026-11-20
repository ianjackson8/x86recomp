#pragma once
// MSVC symbol undecoration with undname's exact formatting (dbghelp.cpp,
// recomp_dbghelp).

#include <cstdint>
#include <string>

namespace recomp::hle {

// UnDecorateSymbolName(mangled, flags) as Microsoft's undname formats it;
// empty if the name does not demangle.
std::string UndnameMsvc(const std::string& mangled, uint32_t flags);

// The text type_info::name() gives for an RTTI type descriptor's decorated
// name (".?AVFoo@@" -> "class Foo"); empty if it does not demangle.
std::string UndnameTypeName(const std::string& decorated);

}  // namespace recomp::hle
