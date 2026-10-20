# Turns slangc's reflection JSON for a CUDA kernel into a C++ header: the byte offset of every global shader
# parameter inside SLANG_globalParams, and the offset of every field of every struct the kernel reads, so the
# CUDA backend fills the parameter block by name and static_asserts that backends/gpu/gpu_types.h matches
# (DECISIONS D-086). Inputs: INPUT (.json), OUTPUT (.h), NAMESPACE (C++ namespace inside pen::cuda_layout).
file(READ "${INPUT}" json)

set(seen "")
set(field_rows "")
set(struct_rows "")

function(walk_struct t)
  string(JSON name GET "${t}" name)
  if(name IN_LIST seen)
    return()
  endif()
  list(APPEND seen "${name}")
  string(JSON size GET "${t}" sizes 0 value)
  string(APPEND struct_rows "    {\"${name}\", ${size}},\n")
  string(JSON nf LENGTH "${t}" fields)
  math(EXPR last "${nf} - 1")
  foreach(i RANGE ${last})
    string(JSON fname GET "${t}" fields ${i} name)
    string(JSON foff GET "${t}" fields ${i} binding offset)
    string(JSON ftype GET "${t}" fields ${i} type)
    string(JSON fkind GET "${ftype}" kind)
    if(fkind STREQUAL "struct")
      walk_struct("${ftype}")
    endif()
    string(APPEND field_rows "    {\"${name}\", \"${fname}\", ${foff}},\n")
  endforeach()
  set(seen "${seen}" PARENT_SCOPE)
  set(field_rows "${field_rows}" PARENT_SCOPE)
  set(struct_rows "${struct_rows}" PARENT_SCOPE)
endfunction()

# Global parameters.
set(global_rows "")
set(globals_size 0)
string(JSON np LENGTH "${json}" parameters)
math(EXPR last "${np} - 1")
foreach(i RANGE ${last})
  string(JSON name GET "${json}" parameters ${i} name)
  string(JSON off GET "${json}" parameters ${i} binding offset)
  string(JSON size GET "${json}" parameters ${i} binding size)
  string(APPEND global_rows "    {\"${name}\", ${off}, ${size}},\n")
  math(EXPR end "${off} + ${size}")
  if(end GREATER globals_size)
    set(globals_size ${end})
  endif()
  string(JSON type GET "${json}" parameters ${i} type)
  string(JSON kind GET "${type}" kind)
  set(element "")
  if(kind STREQUAL "resource")
    string(JSON element GET "${type}" resultType)
  elseif(kind STREQUAL "constantBuffer")
    string(JSON element GET "${type}" elementVarLayout type)
  endif()
  if(NOT element STREQUAL "")
    string(JSON ekind GET "${element}" kind)
    if(ekind STREQUAL "struct")
      walk_struct("${element}")
    endif()
  endif()
endforeach()

# Uniform entry point parameters (the kernel's arguments).
string(JSON ne LENGTH "${json}" entryPoints)
math(EXPR elast "${ne} - 1")
foreach(e RANGE ${elast})
  string(JSON nparams LENGTH "${json}" entryPoints ${e} parameters)
  math(EXPR plast "${nparams} - 1")
  foreach(i RANGE ${plast})
    string(JSON ptype GET "${json}" entryPoints ${e} parameters ${i} type)
    string(JSON pkind GET "${ptype}" kind)
    if(pkind STREQUAL "struct")
      walk_struct("${ptype}")
    endif()
  endforeach()
endforeach()

file(WRITE "${OUTPUT}" "// Generated from ${INPUT} by cmake/CudaLayout.cmake; do not edit.
#pragma once
#include <cstdint>
#include <string_view>
namespace pen::cuda_layout::${NAMESPACE} {
struct Global {
  std::string_view name;
  uint32_t offset, size;
};
struct Field {
  std::string_view type, name;
  uint32_t offset;
};
struct Struct {
  std::string_view name;
  uint32_t size;
};
inline constexpr uint32_t kGlobalsSize = ${globals_size};
inline constexpr Global kGlobals[] = {
${global_rows}};
inline constexpr Field kFields[] = {
${field_rows}};
inline constexpr Struct kStructs[] = {
${struct_rows}};
}  // namespace pen::cuda_layout::${NAMESPACE}
")
