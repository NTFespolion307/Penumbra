# Writes a SPIR-V binary as a C++ array. Inputs: INPUT (.spv), OUTPUT (.cpp), NAME (C++ identifier).
file(READ "${INPUT}" hex HEX)
string(LENGTH "${hex}" len)
math(EXPR words "${len} / 8")
string(REGEX REPLACE "([0-9a-f][0-9a-f])" "0x\\1," bytes "${hex}")
string(REGEX REPLACE "((0x..,)(0x..,)(0x..,)(0x..,)(0x..,)(0x..,)(0x..,)(0x..,)(0x..,)(0x..,)(0x..,)(0x..,)(0x..,)(0x..,)(0x..,)(0x..,))" "\\1\n" bytes "${bytes}")
file(WRITE "${OUTPUT}" "// Generated from ${INPUT}; do not edit.\n#include <cstddef>\n#include <cstdint>\nnamespace pen {\nalignas(4) static const unsigned char kBytes[] = {\n${bytes}};\nextern const uint32_t* const ${NAME} = reinterpret_cast<const uint32_t*>(kBytes);\nextern const size_t ${NAME}Words = ${words};\n}  // namespace pen\n")
