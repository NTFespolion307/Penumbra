// penumbra-diff: compares renders (docs/GUIDE.md "Testing and validation").
//
//   penumbra-diff a.exr b.exr [--max-relmse X]   relMSE, max absolute difference; fail above X
//   penumbra-diff a.exr b.exr --exact            fail unless every float is bit-identical
//   penumbra-diff a.exr --uniform V --tolerance T   fail unless every pixel is within T (relative) of V
//   --crop x0,y0,x1,y1 compares only that rectangle of A (decoding only its rows: for gigapixel EXRs)
//
// Exit codes: 0 pass, 1 fail, 2 usage or read error. relMSE = mean over pixels and channels of
// (a - b)^2 / (b^2 + 0.01).

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <optional>
#include <fmt/format.h>
#include <string>
#include <vector>

#include "core/image/image_io.h"
#include "core/system/paths.h"

namespace {

int usage() {
  std::fputs("usage: penumbra-diff A B [--max-relmse X | --exact]\n"
             "       penumbra-diff A --uniform V [--tolerance T]\n"
             "       --crop x0,y0,x1,y1 reads only that rectangle of A\n",
             stderr);
  return 2;
}

}  // namespace

int main(int argc, char** argv) {
  std::vector<std::string> files;
  double maxRelMse = -1, uniform = -1, tolerance = 0.005;
  bool exact = false;
  std::optional<pen::ImageCrop> crop;
  for (int i = 1; i < argc; ++i) {
    const std::string a = argv[i];
    if (a == "--exact") exact = true;
    else if (a == "--max-relmse" && i + 1 < argc) maxRelMse = std::atof(argv[++i]);
    else if (a == "--uniform" && i + 1 < argc) uniform = std::atof(argv[++i]);
    else if (a == "--tolerance" && i + 1 < argc) tolerance = std::atof(argv[++i]);
    else if (a == "--crop" && i + 1 < argc) {
      pen::ImageCrop c;
      if (std::sscanf(argv[++i], "%u,%u,%u,%u", &c.x0, &c.y0, &c.x1, &c.y1) != 4) return usage();
      crop = c;
    }
    else if (a.starts_with("--")) return usage();
    else files.push_back(a);
  }

  try {
    if (uniform >= 0) {
      if (files.size() != 1) return usage();
      const pen::ImageRGB img = pen::loadImageRGB(pen::pathFromUtf8(files[0]), crop);
      double worst = 0, sum = 0;
      uint32_t wx = 0, wy = 0, bad = 0;
      for (uint32_t y = 0; y < img.height; ++y) {
        for (uint32_t x = 0; x < img.width; ++x) {
          const pen::Vec3f p = img.at(x, y);
          for (int c = 0; c < 3; ++c) {
            const double e = std::fabs(p[c] - uniform) / uniform;
            sum += p[c];
            if (e > tolerance) ++bad;
            if (e > worst || !std::isfinite(e)) {
              worst = e;
              wx = x;
              wy = y;
            }
          }
        }
      }
      const double mean = sum / (double(img.width) * img.height * 3);
      std::printf("%s: mean %.6f, worst relative error %.6f at (%u, %u), %u values outside %.4f\n", files[0].c_str(),
                  mean, worst, wx, wy, bad, tolerance);
      return bad == 0 && std::isfinite(worst) ? 0 : 1;
    }

    if (files.size() != 2) return usage();
    const pen::ImageRGB a = pen::loadImageRGB(pen::pathFromUtf8(files[0]), crop);
    const pen::ImageRGB b = pen::loadImageRGB(pen::pathFromUtf8(files[1]));
    if (a.width != b.width || a.height != b.height) {
      std::printf("size differs: %ux%u vs %ux%u\n", a.width, a.height, b.width, b.height);
      return 1;
    }
    double relMse = 0, maxAbs = 0;
    uint64_t differentBits = 0;
    for (size_t i = 0; i < a.pixels.size(); ++i) {
      for (int c = 0; c < 3; ++c) {
        const float va = a.pixels[i][c], vb = b.pixels[i][c];
        const double d = double(va) - vb;
        relMse += d * d / (double(vb) * vb + 0.01);
        maxAbs = std::fmax(maxAbs, std::fabs(d));
        if (std::memcmp(&va, &vb, sizeof(float)) != 0) ++differentBits;
      }
    }
    relMse /= double(a.pixels.size()) * 3;
    std::printf("relMSE %.3e, max abs diff %.6g, %llu of %llu values differ bitwise\n", relMse, maxAbs,
                static_cast<unsigned long long>(differentBits), static_cast<unsigned long long>(a.pixels.size() * 3));
    if (exact) return differentBits == 0 ? 0 : 1;
    if (maxRelMse >= 0) return relMse <= maxRelMse ? 0 : 1;
    return 0;
  } catch (const std::exception& e) {
    std::fprintf(stderr, "penumbra-diff: %s\n", e.what());
    return 2;
  }
}
