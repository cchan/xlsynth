// Copyright 2021 The XLS Authors
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//      http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include "xls/passes/union_query_engine.h"

#include <cstdint>
#include <memory>
#include <optional>
#include <utility>
#include <vector>

#include "absl/container/flat_hash_set.h"
#include "absl/log/check.h"
#include "absl/status/statusor.h"
#include "absl/types/span.h"
#include "xls/common/status/status_macros.h"
#include "xls/data_structures/leaf_type_tree.h"
#include "xls/ir/bits.h"
#include "xls/ir/interval_set.h"
#include "xls/ir/node.h"
#include "xls/ir/ternary.h"
#include "xls/ir/type.h"
#include "xls/passes/predicate_state.h"
#include "xls/passes/query_engine.h"

namespace xls {

absl::StatusOr<ReachedFixpoint> UnownedUnionQueryEngine::Populate(
    FunctionBase* f) {
  ReachedFixpoint result = ReachedFixpoint::Unchanged;
  for (QueryEngine* engine : engines_) {
    XLS_ASSIGN_OR_RETURN(ReachedFixpoint rf, engine->Populate(f));
    // Unchanged is the top of the lattice so it's an identity
    if (result == ReachedFixpoint::Unchanged) {
      result = rf;
    }
    // Changed can only degrade to Unknown
    if ((result == ReachedFixpoint::Changed) &&
        (rf == ReachedFixpoint::Unknown)) {
      result = ReachedFixpoint::Unknown;
    }
    // No case needed for ReachedFixpoint::Unknown since it's already the bottom
    // of the lattice
  }
  return result;
}

bool UnownedUnionQueryEngine::IsTracked(Node* node) const {
  for (const auto& engine : engines_) {
    if (engine->IsTracked(node)) {
      return true;
    }
  }
  return false;
}

LeafTypeTree<TernaryVector> UnownedUnionQueryEngine::GetTernary(
    Node* node) const {
  LeafTypeTree<TernaryVector> result =
      LeafTypeTree<TernaryVector>::CreateFromFunction(
          node->GetType(),
          [](Type* leaf_type,
             absl::Span<const int64_t> index) -> absl::StatusOr<TernaryVector> {
            return TernaryVector(leaf_type->GetFlatBitCount(),
                                 TernaryValue::kUnknown);
          })
          .value();
  for (const auto& engine : engines_) {
    if (engine->IsTracked(node)) {
      leaf_type_tree::SimpleUpdateFrom<TernaryVector, TernaryVector>(
          result.AsMutableView(), engine->GetTernary(node).AsView(),
          [](TernaryVector& lhs, const TernaryVector& rhs) {
            CHECK_OK(ternary_ops::UpdateWithUnion(lhs, rhs));
          });
    }
  }
  return result;
}

std::unique_ptr<QueryEngine> UnownedUnionQueryEngine::SpecializeGivenPredicate(
    const absl::flat_hash_set<PredicateState>& state) const {
  std::vector<std::unique_ptr<QueryEngine>> engines;
  engines.reserve(engines_.size());
  for (const auto& engine : engines_) {
    engines.push_back(engine->SpecializeGivenPredicate(state));
  }
  return std::make_unique<UnionQueryEngine>(std::move(engines));
}

LeafTypeTree<IntervalSet> UnownedUnionQueryEngine::GetIntervals(
    Node* node) const {
  LeafTypeTree<IntervalSet> result(node->GetType());
  for (int64_t i = 0; i < result.size(); ++i) {
    result.elements()[i] =
        IntervalSet::Maximal(result.leaf_types()[i]->GetFlatBitCount());
  }
  for (const auto& engine : engines_) {
    if (engine->IsTracked(node)) {
      leaf_type_tree::SimpleUpdateFrom<IntervalSet, IntervalSet>(
          result.AsMutableView(), engine->GetIntervals(node).AsView(),
          [](IntervalSet& lhs, const IntervalSet& rhs) {
            lhs = IntervalSet::Intersect(lhs, rhs);
          });
    }
  }
  return result;
}

bool UnownedUnionQueryEngine::AtMostOneTrue(
    absl::Span<TreeBitLocation const> bits) const {
  for (const auto& engine : engines_) {
    if (engine->AtMostOneTrue(bits)) {
      return true;
    }
  }
  return false;
}

bool UnownedUnionQueryEngine::AtLeastOneTrue(
    absl::Span<TreeBitLocation const> bits) const {
  for (const auto& engine : engines_) {
    if (engine->AtLeastOneTrue(bits)) {
      return true;
    }
  }
  return false;
}

bool UnownedUnionQueryEngine::KnownEquals(const TreeBitLocation& a,
                                          const TreeBitLocation& b) const {
  for (const auto& engine : engines_) {
    if (engine->KnownEquals(a, b)) {
      return true;
    }
  }
  return false;
}

bool UnownedUnionQueryEngine::KnownNotEquals(const TreeBitLocation& a,
                                             const TreeBitLocation& b) const {
  for (const auto& engine : engines_) {
    if (engine->KnownNotEquals(a, b)) {
      return true;
    }
  }
  return false;
}

bool UnownedUnionQueryEngine::Implies(const TreeBitLocation& a,
                                      const TreeBitLocation& b) const {
  for (const auto& engine : engines_) {
    if (engine->Implies(a, b)) {
      return true;
    }
  }
  return false;
}

std::optional<Bits> UnownedUnionQueryEngine::ImpliedNodeValue(
    absl::Span<const std::pair<TreeBitLocation, bool>> predicate_bit_values,
    Node* node) const {
  for (const auto& engine : engines_) {
    if (auto i = engine->ImpliedNodeValue(predicate_bit_values, node)) {
      return i;
    }
  }
  return std::nullopt;
}

std::optional<TernaryVector> UnownedUnionQueryEngine::ImpliedNodeTernary(
    absl::Span<const std::pair<TreeBitLocation, bool>> predicate_bit_values,
    Node* node) const {
  std::optional<TernaryVector> result = std::nullopt;
  for (const auto& engine : engines_) {
    if (std::optional<TernaryVector> implied =
            engine->ImpliedNodeTernary(predicate_bit_values, node);
        implied.has_value()) {
      if (result.has_value()) {
        CHECK_OK(ternary_ops::UpdateWithUnion(*result, *implied));
      } else {
        result = std::move(implied);
      }
    }
  }
  return result;
}

}  // namespace xls
