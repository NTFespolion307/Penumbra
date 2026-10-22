// pen-embed-binary: writes a binary file as a C++ byte array (build tool; CUDA kernel fatbins can be megabytes,
// too large for a CMake regex). Usage: pen-embed-binary <input> <output.cpp> <Name>
// Defines pen::<Name> (const unsigned char* const) and pen::<Name>Size (size_t).

#include <cstdio>
#include <fstream>
#include <iterator>
#include <vector>

int main(int argc, char** argv) {
  if (argc != 4) {
    std::fprintf(stderr, "usage: pen-embed-binary <input> <output.cpp> <Name>\n");
    return 2;
  }
  std::ifstream in(argv[1], std::ios::binary);
  if (!in) {
    std::fprintf(stderr, "pen-embed-binary: cannot read %s\n", argv[1]);
    return 1;
  }
  const std::vector<unsigned char> data((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
  std::FILE* out = std::fopen(argv[2], "wb");
  if (!out) {
    std::fprintf(stderr, "pen-embed-binary: cannot write %s\n", argv[2]);
    return 1;
  }
  std::fprintf(out, "// Generated from %s by pen-embed-binary; do not edit.\n#include <cstddef>\nnamespace pen {\n", argv[1]);
  std::fprintf(out, "alignas(16) static const unsigned char kBytes[] = {\n");
  for (size_t i = 0; i < data.size(); ++i) std::fprintf(out, "%u,%s", data[i], i % 32 == 31 ? "\n" : "");
  if (data.empty()) std::fprintf(out, "0");
  std::fprintf(out, "};\nextern const unsigned char* const %s = kBytes;\nextern const size_t %sSize = %zu;\n}  // namespace pen\n",
               argv[3], argv[3], data.size());
  return std::fclose(out) == 0 ? 0 : 1;
}
