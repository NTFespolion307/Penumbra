#include "core/sampling/alias_table.h"

#include <algorithm>

namespace pen {

AliasTable::AliasTable(const std::vector<float>& weights) {
  const size_t n = weights.size();
  if (n == 0) return;
  double total = 0;
  for (float w : weights) total += std::max(w, 0.0f);
  sum_ = static_cast<float>(total);
  pmf_.resize(n);
  prob_.assign(n, 1.0f);
  alias_.resize(n);
  for (size_t i = 0; i < n; ++i) alias_[i] = static_cast<uint32_t>(i);
  if (total <= 0) {
    for (auto& p : pmf_) p = 1.0f / static_cast<float>(n);
    return;
  }

  std::vector<double> scaled(n);
  std::vector<uint32_t> small, large;
  for (size_t i = 0; i < n; ++i) {
    const double p = std::max(weights[i], 0.0f) / total;
    pmf_[i] = static_cast<float>(p);
    scaled[i] = p * static_cast<double>(n);
    (scaled[i] < 1.0 ? small : large).push_back(static_cast<uint32_t>(i));
  }
  while (!small.empty() && !large.empty()) {
    const uint32_t s = small.back();
    small.pop_back();
    const uint32_t l = large.back();
    prob_[s] = static_cast<float>(scaled[s]);
    alias_[s] = l;
    scaled[l] = (scaled[l] + scaled[s]) - 1.0;
    if (scaled[l] < 1.0) {
      large.pop_back();
      small.push_back(l);
    }
  }
  // Leftovers are 1 up to rounding.
  for (uint32_t i : small) prob_[i] = 1.0f;
  for (uint32_t i : large) prob_[i] = 1.0f;
}

uint32_t AliasTable::sample(float u, float* remapped) const {
  const uint32_t n = size();
  const float x = u * static_cast<float>(n);
  const uint32_t i = std::min(static_cast<uint32_t>(x), n - 1);
  const float f = std::min(x - static_cast<float>(i), 0.99999994f);
  if (f < prob_[i]) {
    if (remapped) *remapped = std::min(f / prob_[i], 0.99999994f);
    return i;
  }
  if (remapped) *remapped = std::min((f - prob_[i]) / (1.0f - prob_[i]), 0.99999994f);
  return alias_[i];
}

Distribution2D::Distribution2D(const std::vector<float>& weights, uint32_t nu, uint32_t nv)
    : nu_(nu), nv_(nv), weights_(weights) {
  std::vector<float> rowSums(nv);
  cols_.reserve(nv);
  double total = 0;
  for (uint32_t v = 0; v < nv; ++v) {
    std::vector<float> row(weights.begin() + v * nu, weights.begin() + (v + 1) * nu);
    cols_.emplace_back(row);
    rowSums[v] = cols_.back().sum();
    total += rowSums[v];
  }
  rows_ = AliasTable(rowSums);
  sum_ = static_cast<float>(total);
}

Vec2f Distribution2D::sample(Vec2f u, float& pdf) const {
  float fv = 0, fu = 0;
  const uint32_t v = rows_.sample(u.y, &fv);
  const uint32_t c = cols_[v].sample(u.x, &fu);
  pdf = this->pdf({(c + 0.5f) / nu_, (v + 0.5f) / nv_});
  return {(c + fu) / static_cast<float>(nu_), (v + fv) / static_cast<float>(nv_)};
}

float Distribution2D::pdf(Vec2f p) const {
  if (sum_ <= 0) return 0;
  const uint32_t c = std::min(static_cast<uint32_t>(p.x * nu_), nu_ - 1);
  const uint32_t v = std::min(static_cast<uint32_t>(p.y * nv_), nv_ - 1);
  return weights_[v * nu_ + c] / sum_ * static_cast<float>(nu_) * static_cast<float>(nv_);
}

}  // namespace pen
