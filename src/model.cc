/**
 * Copyright (c) 2016-present, Facebook, Inc.
 * All rights reserved.
 *
 * This source code is licensed under the BSD-style license found in the
 * LICENSE file in the root directory of this source tree. An additional grant
 * of patent rights can be found in the PATENTS file in the same directory.
 */

#include "model.h"

#include <assert.h>

#include <algorithm>

#include "args.h"
#include "utils.h"

extern Args args;

real Model::lr_ = MIN_LR;

Model::Model(Matrix& wi, Matrix& wo, int32_t hsz, real lr, int32_t seed)
            : wi_(wi), wo_(wo), hidden_(hsz), output_(wo.m_),
              grad_(hsz), rng(seed) {
  isz_ = wi.m_;
  osz_ = wo.m_;
  hsz_ = hsz;
  lr_ = lr;
  negpos = 0;
}

void Model::setLearningRate(real lr) {
  lr_ = (lr < MIN_LR) ? MIN_LR : lr;
}

real Model::getLearningRate() {
  return lr_;
}

real Model::binaryLogistic(int32_t target, bool label) {
  real score = utils::sigmoid(wo_.dotRow(hidden_, target));
  real alpha = lr_ * (real(label) - score);
  grad_.addRow(wo_, target, alpha);
  wo_.addRow(hidden_, target, alpha);
  if (label) {
    return -utils::log(score);
  } else {
    return -utils::log(1.0 - score);
  }
}

real Model::negativeSampling(int32_t target) {
  real loss = 0.0;
  grad_.zero();
  for (int32_t n = 0; n <= args.neg; n++) {
    if (n == 0) {
      loss += binaryLogistic(target, true);
    } else {
      loss += binaryLogistic(getNegative(target), false);
    }
  }
  return loss;
}

real Model::hierarchicalSoftmax(int32_t target) {
  real loss = 0.0;
  grad_.zero();
  const std::vector<bool>& binaryCode = codes[target];
  const std::vector<int32_t>& pathToRoot = paths[target];
  for (int32_t i = 0; i < pathToRoot.size(); i++) {
    loss += binaryLogistic(pathToRoot[i], binaryCode[i]);
  }
  return loss;
}

real Model::softmax(int32_t target) {
  grad_.zero();
  output_.mul(wo_, hidden_);
  real max = output_[0], z = 0.0;
  for (int32_t i = 0; i < osz_; i++) {
    max = std::max(output_[i], max);
  }
  for (int32_t i = 0; i < osz_; i++) {
    output_[i] = exp(output_[i] - max);
    z += output_[i];
  }
  for (int32_t i = 0; i < osz_; i++) {
    real label = (i == target) ? 1.0 : 0.0;
    output_[i] /= z;
    real alpha = lr_ * (label - output_[i]);
    grad_.addRow(wo_, i, alpha);
    wo_.addRow(hidden_, i, alpha);
  }
  return -utils::log(output_[target]);
}

namespace {
  void computeHidden(Vector& hidden, const Matrix& wi, const std::vector<int32_t>& input) {
    hidden.zero();
    for (auto it = input.cbegin(); it != input.cend(); ++it) {
      hidden.addRow(wi, *it);
    }
    hidden.mul(1.0 / input.size());
  }

  bool compScoreNodePairs(const std::pair<real, int32_t> &l, const std::pair<real, int32_t> &r) {
    return l.first > r.first;
  }

  std::vector<int32_t> findKLargestIndices(const Vector& v, int32_t k) {
    auto cmp = [&v] (int32_t i, int32_t j) {
      return v[i] > v[j];
    };
    std::vector<int32_t> indices;
    indices.reserve(k + 1);
    for (int32_t i = 0; i < v.m_; i += 1) {
      if (indices.size() == k && v[i] < v[indices.front()]) {
        continue;
      }
      indices.push_back(i);
      std::push_heap(indices.begin(), indices.end(), cmp);
      if (indices.size() > k) {
        std::pop_heap(indices.begin(), indices.end(), cmp);
        indices.pop_back();
      }
    }
    std::sort_heap(indices.begin(), indices.end(), cmp);
    return indices;
  }
}

int32_t Model::predict(const std::vector<int32_t>& input) {
  computeHidden(hidden_, wi_, input);
  if (args.loss == loss_name::hs) {
    real max = -1e10;
    int32_t argmax = -1;
    dfs(2 * osz_ - 2, 0.0, max, argmax);
    return argmax;
  } else {
    output_.mul(wo_, hidden_);
    return output_.argmax();
  }
}

std::vector<int32_t> Model::predict(int32_t top_k, const std::vector<int32_t>& input) {
  assert(top_k > 0);
  computeHidden(hidden_, wi_, input);
  if (args.loss == loss_name::hs) {
    std::vector<std::pair<real, int32_t>> heap;
    heap.reserve(top_k + 1);
    dfs(top_k, 2 * osz_ - 2, 0.0, heap);
    std::sort_heap(heap.begin(), heap.end(), compScoreNodePairs);
    std::vector<int32_t> label_indices;
    label_indices.reserve(top_k);
    for (auto iter = heap.cbegin(); iter != heap.cend(); iter++) {
      label_indices.push_back(iter->second);
    }
    return label_indices;
  } else {
    output_.mul(wo_, hidden_);
    return findKLargestIndices(output_, top_k);
  }
}

void Model::dfs(int32_t node, real score, real& max, int32_t& argmax) {
  if (score < max) return;
  if (tree[node].left == -1 && tree[node].right == -1) {
    max = score;
    argmax = node;
    return;
  }
  real f = utils::sigmoid(wo_.dotRow(hidden_, node - osz_));
  dfs(tree[node].left, score + utils::log(1.0 - f), max, argmax);
  dfs(tree[node].right, score + utils::log(f), max, argmax);
}

void Model::dfs(int32_t top_k, int32_t node, real score, std::vector<std::pair<real, int32_t>>& heap) {
  if (heap.size() == top_k && score < heap.front().first) {
    return;
  }

  if (tree[node].left == -1 && tree[node].right == -1) {
    heap.push_back(std::make_pair(score, node));
    std::push_heap(heap.begin(), heap.end(), compScoreNodePairs);
    if (heap.size() > top_k) {
      std::pop_heap(heap.begin(), heap.end(), compScoreNodePairs);
      heap.pop_back();
    }
    return;
  }

  real f = utils::sigmoid(wo_.dotRow(hidden_, node - osz_));
  dfs(top_k, tree[node].left, score + utils::log(1.0 - f), heap);
  dfs(top_k, tree[node].right, score + utils::log(f), heap);
}

real Model::update(const std::vector<int32_t>& input, int32_t target) {
  assert(target >= 0);
  assert(target < osz_);
  if (input.size() == 0) return 0.0;
  hidden_.zero();
  for (auto it = input.cbegin(); it != input.cend(); ++it) {
    hidden_.addRow(wi_, *it);
  }
  hidden_.mul(1.0 / input.size());

  real loss;
  if (args.loss == loss_name::ns) {
    loss = negativeSampling(target);
  } else if (args.loss == loss_name::hs) {
    loss = hierarchicalSoftmax(target);
  } else {
    loss = softmax(target);
  }

  if (args.model == model_name::sup) {
    grad_.mul(1.0 / input.size());
  }
  for (auto it = input.cbegin(); it != input.cend(); ++it) {
    wi_.addRow(grad_, *it, 1.0);
  }
  return loss;
}

void Model::setTargetCounts(const std::vector<int64_t>& counts) {
  assert(counts.size() == osz_);
  if (args.loss == loss_name::ns) {
    initTableNegatives(counts);
  }
  if (args.loss == loss_name::hs) {
    buildTree(counts);
  }
}

void Model::initTableNegatives(const std::vector<int64_t>& counts) {
  real z = 0.0;
  for (size_t i = 0; i < counts.size(); i++) {
    z += pow(counts[i], 0.5);
  }
  for (size_t i = 0; i < counts.size(); i++) {
    real c = pow(counts[i], 0.5);
    for (size_t j = 0; j < c * NEGATIVE_TABLE_SIZE / z; j++) {
      negatives.push_back(i);
    }
  }
  std::shuffle(negatives.begin(), negatives.end(), rng);
}

int32_t Model::getNegative(int32_t target) {
  int32_t negative;
  do {
    negative = negatives[negpos];
    negpos = (negpos + 1) % negatives.size();
  } while (target == negative);
  return negative;
}

void Model::buildTree(const std::vector<int64_t>& counts) {
  tree.resize(2 * osz_ - 1);
  for (int32_t i = 0; i < 2 * osz_ - 1; i++) {
    tree[i].parent = -1;
    tree[i].left = -1;
    tree[i].right = -1;
    tree[i].count = 1e15;
    tree[i].binary = false;
  }
  for (int32_t i = 0; i < osz_; i++) {
    tree[i].count = counts[i];
  }
  int32_t leaf = osz_ - 1;
  int32_t node = osz_;
  for (int32_t i = osz_; i < 2 * osz_ - 1; i++) {
    int32_t mini[2];
    for (int32_t j = 0; j < 2; j++) {
      if (leaf >= 0 && tree[leaf].count < tree[node].count) {
        mini[j] = leaf--;
      } else {
        mini[j] = node++;
      }
    }
    tree[i].left = mini[0];
    tree[i].right = mini[1];
    tree[i].count = tree[mini[0]].count + tree[mini[1]].count;
    tree[mini[0]].parent = i;
    tree[mini[1]].parent = i;
    tree[mini[1]].binary = true;
  }
  for (int32_t i = 0; i < osz_; i++) {
    std::vector<int32_t> path;
    std::vector<bool> code;
    int32_t j = i;
    while (tree[j].parent != -1) {
      path.push_back(tree[j].parent - osz_);
      code.push_back(tree[j].binary);
      j = tree[j].parent;
    }
    paths.push_back(path);
    codes.push_back(code);
  }
}
