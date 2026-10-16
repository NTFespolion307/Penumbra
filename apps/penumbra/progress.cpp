#include "apps/penumbra/progress.h"

#include <cstdio>
#include <fmt/format.h>
#include <memory>

#include "core/settings/parse.h"

#if defined(_WIN32)
#include <io.h>
#define PEN_ISATTY(f) _isatty(_fileno(f))
#else
#include <unistd.h>
#define PEN_ISATTY(f) isatty(fileno(f))
#endif

namespace pen::app {

namespace {

struct Numbers {
  double percent, rate, eta;
};

Numbers numbers(const RenderProgress& p) {
  const double fraction = p.samples_total ? double(p.samples_done) / double(p.samples_total) : 0.0;
  const double rate = p.elapsed_seconds > 0 ? p.samples_done / p.elapsed_seconds : 0.0;
  const double eta = rate > 0 ? (p.samples_total - p.samples_done) / rate : -1.0;
  return {fraction * 100, rate, eta};
}

std::string etaText(double eta) { return eta < 0 ? "?" : formatDuration(static_cast<uint64_t>(eta + 0.5)); }

}  // namespace

static std::function<void(const RenderProgress&)> makeRawPrinter(ProgressStyle style);

std::function<void(const RenderProgress&)> makeProgressPrinter(ProgressStyle style) {
  if (style == ProgressStyle::Bar && !PEN_ISATTY(stderr)) style = ProgressStyle::Plain;
  return [print = makeRawPrinter(style), finished = false](const RenderProgress& p) mutable {
    if (finished) return;  // the controller reports the final state once more after the loop
    finished = p.samples_done == p.samples_total;
    print(p);
  };
}

static std::function<void(const RenderProgress&)> makeRawPrinter(ProgressStyle style) {
  switch (style) {
    case ProgressStyle::Json:
      return [](const RenderProgress& p) {
        const Numbers n = numbers(p);
        std::fputs(fmt::format("{{\"percent\":{:.3f},\"samples_done\":{},\"samples_total\":{},\"samples_per_s\":{:.0f},"
                               "\"elapsed_s\":{:.1f},\"eta_s\":{:.1f},\"tiles_done\":{},\"tiles_total\":{}}}\n",
                               n.percent, p.samples_done, p.samples_total, n.rate, p.elapsed_seconds, n.eta,
                               p.items_done, p.items_total)
                       .c_str(),
                   stdout);
        std::fflush(stdout);
      };
    case ProgressStyle::Plain: {
      auto last = std::make_shared<double>(-1e9);
      return [last](const RenderProgress& p) {
        const bool finished = p.samples_done == p.samples_total;
        if (!finished && p.elapsed_seconds - *last < 5) return;
        *last = p.elapsed_seconds;
        const Numbers n = numbers(p);
        std::fputs(fmt::format("progress {:5.1f}%  tiles {}/{}  {:.2f} M samples/s  elapsed {}  ETA {}\n", n.percent,
                               p.items_done, p.items_total, n.rate / 1e6,
                               formatDuration(static_cast<uint64_t>(p.elapsed_seconds)), etaText(n.eta))
                       .c_str(),
                   stderr);
      };
    }
    case ProgressStyle::Bar:
    default:
      return [](const RenderProgress& p) {
        const Numbers n = numbers(p);
        constexpr int kWidth = 30;
        const int filled = static_cast<int>(n.percent / 100 * kWidth);
        const std::string bar = std::string(filled, '#') + std::string(kWidth - filled, '-');
        std::fputs(fmt::format("\r[{}] {:5.1f}%  {:.2f} M samples/s  ETA {:<10}", bar, n.percent, n.rate / 1e6,
                               etaText(n.eta))
                       .c_str(),
                   stderr);
        if (p.samples_done == p.samples_total) std::fputs("\n", stderr);
        std::fflush(stderr);
      };
  }
}

}  // namespace pen::app
