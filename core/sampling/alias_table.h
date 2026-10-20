#pragma once

// Alias tables (Vose) for O(1) discrete sampling, and a 2D piecewise-constant distribution built from them.

#include <cstdint>
#include <vector>

#include "core/math/vec.h"

namespace pen {

class AliasTable {
 public:
  AliasTable() = default;
  explicit AliasTable(const std::vector<float>& weights);

  bool empty() const { return pmf_.empty(); }
  uint32_t size() const { return static_cast<uint32_t>(pmf_.size()); }
  float sum() const { return sum_; }
  float pmf(uint32_t i) const { return pmf_[i]; }
  // Raw table entries, for GPU backends that sample the same table.
  float prob(uint32_t i) const { return prob_[i]; }
  uint32_t alias(uint32_t i) const { return alias_[i]; }

  // Picks an index with probability pmf(i). `remapped` receives a fresh uniform [0,1) number derived from u,
  // so one random number serves both the choice and a continuous offset.
  uint32_t sample(float u, float* remapped = nullptr) const;

 private:
  std::vector<float> prob_;
  std::vector<uint32_t> alias_;
  std::vector<float> pmf_;
  float sum_ = 0;
};

// Piecewise-constant density on [0,1)^2 over a grid of nu x nv cells (row-major weights, row = v).
class Distribution2D {
 public:
  Distribution2D() = default;
  Distribution2D(const std::vector<float>& weights, uint32_t nu, uint32_t nv);

  bool empty() const { return rows_.empty(); }
  // The tables behind sample() and pdf(), for GPU backends.
  uint32_t nu() const { return nu_; }
  uint32_t nv() const { return nv_; }
  float sum() const { return sum_; }
  const AliasTable& rows() const { return rows_; }
  const AliasTable& row(uint32_t v) const { return cols_[v]; }
  const std::vector<float>& weights() const { return weights_; }
  // Returns a point in [0,1)^2 and its density with respect to area in [0,1)^2.
  Vec2f sample(Vec2f u, float& pdf) const;
  float pdf(Vec2f p) const;

 private:
  uint32_t nu_ = 0, nv_ = 0;
  AliasTable rows_;
  std::vector<AliasTable> cols_;
  float sum_ = 0;
  std::vector<float> weights_;
};

}  // namespace pen
