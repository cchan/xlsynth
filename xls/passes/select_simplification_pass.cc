// Copyright 2020 The XLS Authors
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

#include "xls/passes/select_simplification_pass.h"

#include <algorithm>
#include <cstdint>
#include <deque>
#include <functional>
#include <iterator>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <variant>
#include <vector>

#include "absl/algorithm/container.h"
#include "absl/container/flat_hash_set.h"
#include "absl/log/check.h"
#include "absl/log/log.h"
#include "absl/status/status.h"
#include "absl/status/statusor.h"
#include "absl/strings/str_cat.h"
#include "absl/strings/str_format.h"
#include "absl/types/span.h"
#include "absl/types/variant.h"
#include "xls/common/status/ret_check.h"
#include "xls/common/status/status_macros.h"
#include "xls/common/visitor.h"
#include "xls/data_structures/algorithm.h"
#include "xls/data_structures/inline_bitmap.h"
#include "xls/ir/bits.h"
#include "xls/ir/bits_ops.h"
#include "xls/ir/lsb_or_msb.h"
#include "xls/ir/node.h"
#include "xls/ir/node_util.h"
#include "xls/ir/nodes.h"
#include "xls/ir/op.h"
#include "xls/ir/ternary.h"
#include "xls/ir/topo_sort.h"
#include "xls/ir/value.h"
#include "xls/ir/value_utils.h"
#include "xls/passes/optimization_pass.h"
#include "xls/passes/optimization_pass_registry.h"
#include "xls/passes/pass_base.h"
#include "xls/passes/query_engine.h"
#include "xls/passes/stateless_query_engine.h"
#include "xls/passes/ternary_query_engine.h"
#include "xls/passes/union_query_engine.h"

namespace xls {
namespace {

// Given a SelectT node (either OneHotSelect or Select), squeezes the const_msb
// and const_lsb values out of the output, and slices all the operands to
// correspond to the non-const run of bits in the center.
template <typename SelectT>
absl::StatusOr<bool> SqueezeSelect(
    const Bits& const_msb, const Bits& const_lsb,
    const std::function<absl::StatusOr<SelectT*>(SelectT*, std::vector<Node*>)>&
        make_select,
    SelectT* select) {
  FunctionBase* f = select->function_base();
  int64_t bit_count = select->BitCountOrDie();
  auto slice = [&](Node* n) -> absl::StatusOr<Node*> {
    int64_t new_width =
        bit_count - const_msb.bit_count() - const_lsb.bit_count();
    return f->MakeNode<BitSlice>(select->loc(), n,
                                 /*start=*/const_lsb.bit_count(),
                                 /*width=*/new_width);
  };
  std::vector<Node*> new_cases;
  absl::Span<Node* const> cases = select->operands().subspan(1);
  for (Node* old_case : cases) {
    XLS_ASSIGN_OR_RETURN(Node * new_case, slice(old_case));
    new_cases.push_back(new_case);
  }
  XLS_ASSIGN_OR_RETURN(Node * msb_literal,
                       f->MakeNode<Literal>(select->loc(), Value(const_msb)));
  XLS_ASSIGN_OR_RETURN(Node * lsb_literal,
                       f->MakeNode<Literal>(select->loc(), Value(const_lsb)));
  XLS_ASSIGN_OR_RETURN(Node * new_select, make_select(select, new_cases));
  Node* select_node = select;
  VLOG(2) << absl::StrFormat("Squeezing select: %s", select->ToString());
  XLS_RETURN_IF_ERROR(select_node
                          ->ReplaceUsesWithNew<Concat>(std::vector<Node*>{
                              msb_literal, new_select, lsb_literal})
                          .status());
  return true;
}

// The source of a bit. Can be either a literal 0/1 or a bit at a particular
// index of a Node.
using BitSource = std::variant<bool, std::pair<Node*, int64_t>>;

// Traces the bit at the given node and bit index through bit slices and concats
// and returns its source.
// TODO(meheff): Combine this into TernaryQueryEngine.
BitSource GetBitSource(Node* node, int64_t bit_index,
                       const QueryEngine& query_engine) {
  if (node->Is<BitSlice>()) {
    return GetBitSource(node->operand(0),
                        bit_index + node->As<BitSlice>()->start(),
                        query_engine);
  }
  if (node->Is<Concat>()) {
    int64_t offset = 0;
    for (int64_t i = node->operand_count() - 1; i >= 0; --i) {
      Node* operand = node->operand(i);
      if (bit_index - offset < operand->BitCountOrDie()) {
        return GetBitSource(operand, bit_index - offset, query_engine);
      }
      offset += operand->BitCountOrDie();
    }
    LOG(FATAL) << "Bit index " << bit_index << " too large for "
               << node->ToString();
  } else if (node->Is<Literal>()) {
    return node->As<Literal>()->value().bits().Get(bit_index);
  } else if (node->GetType()->IsBits() &&
             query_engine.IsKnown(TreeBitLocation(node, bit_index))) {
    return query_engine.IsOne(TreeBitLocation(node, bit_index));
  }
  return std::make_pair(node, bit_index);
}

std::string ToString(const BitSource& bit_source) {
  return absl::visit(Visitor{[](bool value) { return absl::StrCat(value); },
                             [](const std::pair<Node*, int64_t>& p) {
                               return absl::StrCat(p.first->GetName(), "[",
                                                   p.second, "]");
                             }},
                     bit_source);
}

using MatchedPairs = std::vector<std::pair<int64_t, int64_t>>;

// Returns the pairs of indices into 'nodes' for which the indexed Nodes have
// the same of bits sources at the given bit index. The returned indices are
// indices into the given 'nodes' span. For example, given the following:
//
//  GetBitSource(a, 42) = BitSource{true}
//  GetBitSource(b, 42) = BitSource{foo, 7}
//  GetBitSource(c, 42) = BitSource{foo, 7}
//  GetBitSource(d, 42) = BitSource{true}
//  GetBitSource(e, 42) = BitSource{false}
//
// PairsOfBitsWithSameSource({a, b, c, d, e}, 42) would return [(0, 3), (1, 2)]
MatchedPairs PairsOfBitsWithSameSource(absl::Span<Node* const> nodes,
                                       int64_t bit_index,
                                       const QueryEngine& query_engine) {
  std::vector<BitSource> bit_sources;
  for (Node* node : nodes) {
    bit_sources.push_back(GetBitSource(node, bit_index, query_engine));
  }
  MatchedPairs matching_pairs;
  for (int64_t i = 0; i < bit_sources.size(); ++i) {
    for (int64_t j = i + 1; j < bit_sources.size(); ++j) {
      if (bit_sources[i] == bit_sources[j]) {
        matching_pairs.push_back({i, j});
      }
    }
  }
  return matching_pairs;
}

std::string ToString(const MatchedPairs& pairs) {
  std::string ret;
  for (const auto& p : pairs) {
    absl::StrAppend(&ret, "(", p.first, ", ", p.second, ") ");
  }
  return ret;
}

// Returns a OneHotSelect instruction which selects a slice of the given
// OneHotSelect's cases. The cases are sliced with the given start and width and
// then selected with a new OnehotSelect which is returned.
absl::StatusOr<OneHotSelect*> SliceOneHotSelect(OneHotSelect* ohs,
                                                int64_t start, int64_t width) {
  std::vector<Node*> case_slices;
  for (Node* cas : ohs->cases()) {
    XLS_ASSIGN_OR_RETURN(Node * case_slice,
                         ohs->function_base()->MakeNode<BitSlice>(
                             ohs->loc(), cas, /*start=*/start,
                             /*width=*/width));
    case_slices.push_back(case_slice);
  }
  return ohs->function_base()->MakeNode<OneHotSelect>(
      ohs->loc(), ohs->selector(), case_slices);
}

// Returns the length of the run of bit indices starting at 'start' for which
// there exists at least one pair of elements in 'cases' which have the same bit
// source at the respective bit indices in the entire run. For example, given
// the following
//
//   a = Literal(value=0b110011)
//   b = Literal(value=0b100010)
//   c = Literal(value=0b101010)
//
// RunOfNonDistinctCaseBits({a, b, c}, 1) returns 3 because bits 1, 2, and 3 of
// 'a', and 'b' are the same (have the same BitSource).
int64_t RunOfNonDistinctCaseBits(absl::Span<Node* const> cases, int64_t start,
                                 const QueryEngine& query_engine) {
  VLOG(5) << "Finding runs of non-distinct bits starting at " << start;
  // Do a reduction via intersection of the set of matching pairs within
  // 'cases'. When the intersection is empty, the run is over.
  MatchedPairs matches;
  int64_t i = start;
  while (i < cases.front()->BitCountOrDie()) {
    if (i == start) {
      matches = PairsOfBitsWithSameSource(cases, i, query_engine);
    } else {
      MatchedPairs new_matches;
      absl::c_set_intersection(
          PairsOfBitsWithSameSource(cases, i, query_engine), matches,
          std::back_inserter(new_matches));
      matches = std::move(new_matches);
    }

    VLOG(5) << "  " << i << ": " << ToString(matches);
    if (matches.empty()) {
      break;
    }
    ++i;
  }
  VLOG(5) << " run of " << i - start;
  return i - start;
}

// Returns the length of the run of bit indices starting at 'start' for which
// the indexed bits of the given cases are distinct at each
// bit index. For example:
int64_t RunOfDistinctCaseBits(absl::Span<Node* const> cases, int64_t start,
                              const QueryEngine& query_engine) {
  VLOG(5) << "Finding runs of distinct case bit starting at " << start;
  int64_t i = start;
  while (i < cases.front()->BitCountOrDie() &&
         PairsOfBitsWithSameSource(cases, i, query_engine).empty()) {
    ++i;
  }
  VLOG(5) << " run of " << i - start << " bits";
  return i - start;
}

// Try to split OneHotSelect instructions into separate OneHotSelect
// instructions which have common cases. For example, if some of the cases of a
// OneHotSelect have the same first three bits, then this transformation will
// slice off these three bits (and the remainder) into separate OneHotSelect
// operation and replace the original OneHotSelect with a concat of thes sharded
// OneHotSelects.
//
// Returns the newly created OneHotSelect instructions if the transformation
// succeeded.
absl::StatusOr<std::vector<OneHotSelect*>> MaybeSplitOneHotSelect(
    OneHotSelect* ohs, const QueryEngine& query_engine) {
  // For *very* wide one-hot-selects this optimization can be very slow and make
  // a mess of the graph so limit it to 64 bits.
  if (!ohs->GetType()->IsBits() || ohs->GetType()->GetFlatBitCount() > 64) {
    return std::vector<OneHotSelect*>();
  }

  VLOG(4) << "Trying to split: " << ohs->ToString();
  if (VLOG_IS_ON(4)) {
    for (int64_t i = 0; i < ohs->cases().size(); ++i) {
      Node* cas = ohs->get_case(i);
      VLOG(4) << "  case (" << i << "): " << cas->ToString();
      for (int64_t j = 0; j < cas->BitCountOrDie(); ++j) {
        VLOG(4) << "    bit " << j << ": "
                << ToString(GetBitSource(cas, j, query_engine));
      }
    }
  }

  int64_t start = 0;
  std::vector<Node*> ohs_slices;
  std::vector<OneHotSelect*> new_ohses;
  while (start < ohs->BitCountOrDie()) {
    int64_t run = RunOfDistinctCaseBits(ohs->cases(), start, query_engine);
    if (run == 0) {
      run = RunOfNonDistinctCaseBits(ohs->cases(), start, query_engine);
    }
    XLS_RET_CHECK_GT(run, 0);
    if (run == ohs->BitCountOrDie()) {
      // If all the cases are distinct (or have a matching pair) then just
      // return as there is nothing to slice.
      return std::vector<OneHotSelect*>();
    }
    XLS_ASSIGN_OR_RETURN(OneHotSelect * ohs_slice,
                         SliceOneHotSelect(ohs,
                                           /*start=*/start,
                                           /*width=*/run));
    new_ohses.push_back(ohs_slice);
    ohs_slices.push_back(ohs_slice);
    start += run;
  }
  std::reverse(ohs_slices.begin(), ohs_slices.end());
  VLOG(2) << absl::StrFormat("Splitting one-hot-select: %s", ohs->ToString());
  XLS_RETURN_IF_ERROR(ohs->ReplaceUsesWithNew<Concat>(ohs_slices).status());
  return new_ohses;
}

// Any type of select with only one non-literal-zero arm can be replaced with
// an AND.
//
//  sel(p, cases=[x, 0]) => and(sign_ext(p == 0), x)
//  sel(p, cases=[0, x]) => and(sign_ext(p == 1), x)
//  one_hot_select(p, cases=[x, 0]) => and(sign_ext(p[0]), x)
//  one_hot_select(p, cases=[0, x]) => and(sign_ext(p[1]), x)
//  priority_select(p, cases=[x, 0]) => and(sign_ext(p[0]), x)
//  priority_select(p, cases=[0, x]) => and(sign_ext(p == 2), x)
//
//  sel(p, cases=[x], default_value=0) => and(sign_ext(p == 0), x)
//  one_hot_select(p, cases=[x])       => and(sign_ext(p[0]), x)
//  priority_select(p, cases=[x])      => and(sign_ext(p), x)
//
//  sel(p, cases=[0], default_value=x) => and(sign_ext(p != 0), x)
//
// If the result is not bits-typed, we can still reduce it to a two-arm select
// against a literal zero. (If a non-bits-typed select only has two arms,
// there's no benefit, so we won't simplify the node.)
//
absl::StatusOr<bool> MaybeConvertSelectToMask(Node* node,
                                              const QueryEngine& query_engine) {
  if (!node->OpIn({Op::kSel, Op::kOneHotSel, Op::kPrioritySel})) {
    return false;
  }
  if (!node->GetType()->IsBits() && node->operands().size() <= 3) {
    // We already have a select with at most two arms; we can't simplify this
    // any further for non-bits-typed operands.
    return false;
  }

  std::optional<Node*> only_nonzero_value = std::nullopt;
  Node* nonzero_condition = nullptr;
  switch (node->op()) {
    default:
      return false;
    case Op::kSel: {
      Select* sel = node->As<Select>();
      std::optional<int64_t> nonzero_arm = std::nullopt;
      if (sel->default_value().has_value() &&
          !query_engine.IsAllZeros(*sel->default_value())) {
        nonzero_arm = -1;
        only_nonzero_value = sel->default_value();
      }
      for (int64_t arm = 0; arm < sel->cases().size(); ++arm) {
        Node* case_value = sel->get_case(arm);
        if (query_engine.IsAllZeros(case_value)) {
          continue;
        }
        if (only_nonzero_value.has_value()) {
          // More than one non-zero value;
          return false;
        }

        nonzero_arm = arm;
        only_nonzero_value = case_value;
      }
      if (nonzero_arm.has_value()) {
        VLOG(2) << absl::StrFormat("Select with one non-zero case: %s",
                                   node->ToString());
        if (*nonzero_arm == -1) {
          XLS_ASSIGN_OR_RETURN(
              Node * num_cases,
              node->function_base()->MakeNode<Literal>(
                  node->loc(), Value(UBits(sel->cases().size(),
                                           sel->selector()->BitCountOrDie()))));
          XLS_ASSIGN_OR_RETURN(
              nonzero_condition,
              node->function_base()->MakeNode<CompareOp>(
                  sel->loc(), sel->selector(), num_cases, Op::kUGe));
        } else if (sel->selector()->BitCountOrDie() == 1) {
          if (*nonzero_arm == 0) {
            XLS_ASSIGN_OR_RETURN(nonzero_condition,
                                 node->function_base()->MakeNode<UnOp>(
                                     sel->loc(), sel->selector(), Op::kNot));
          } else {
            XLS_RET_CHECK_EQ(*nonzero_arm, 1);
            nonzero_condition = sel->selector();
          }
        } else {
          XLS_ASSIGN_OR_RETURN(
              Node * arm_number,
              node->function_base()->MakeNode<Literal>(
                  node->loc(), Value(UBits(*nonzero_arm,
                                           sel->selector()->BitCountOrDie()))));
          XLS_ASSIGN_OR_RETURN(
              nonzero_condition,
              node->function_base()->MakeNode<CompareOp>(
                  sel->loc(), sel->selector(), arm_number, Op::kEq));
        }
      }
      break;
    }
    case Op::kOneHotSel: {
      OneHotSelect* sel = node->As<OneHotSelect>();
      std::optional<int64_t> nonzero_arm = std::nullopt;
      for (int64_t arm = 0; arm < sel->cases().size(); ++arm) {
        Node* case_value = sel->get_case(arm);
        if (query_engine.IsAllZeros(case_value)) {
          continue;
        }
        if (only_nonzero_value.has_value()) {
          // More than one non-zero value;
          return false;
        }

        nonzero_arm = arm;
        only_nonzero_value = case_value;
      }
      if (nonzero_arm.has_value()) {
        VLOG(2) << absl::StrFormat("One-hot select with one non-zero case: %s",
                                   node->ToString());
        if (sel->selector()->BitCountOrDie() == 1) {
          XLS_RET_CHECK_EQ(*nonzero_arm, 0);
          nonzero_condition = sel->selector();
        } else {
          XLS_ASSIGN_OR_RETURN(
              nonzero_condition,
              node->function_base()->MakeNode<BitSlice>(
                  sel->loc(), sel->selector(), /*start=*/*nonzero_arm,
                  /*width=*/1));
        }
      }
      break;
    }
    case Op::kPrioritySel: {
      PrioritySelect* sel = node->As<PrioritySelect>();
      std::optional<int64_t> nonzero_arm = std::nullopt;
      for (int64_t arm = 0; arm < sel->cases().size(); ++arm) {
        Node* case_value = sel->get_case(arm);
        if (query_engine.IsAllZeros(case_value)) {
          continue;
        }
        if (only_nonzero_value.has_value()) {
          // More than one non-zero value;
          return false;
        }

        nonzero_arm = arm;
        only_nonzero_value = case_value;
      }
      if (nonzero_arm.has_value()) {
        VLOG(2) << absl::StrFormat("Priority select with one non-zero case: %s",
                                   node->ToString());
        Node* truncated_selector;
        if (sel->selector()->BitCountOrDie() == 1) {
          truncated_selector = sel->selector();
        } else {
          XLS_ASSIGN_OR_RETURN(truncated_selector,
                               node->function_base()->MakeNode<BitSlice>(
                                   sel->loc(), sel->selector(), /*start=*/0,
                                   /*width=*/*nonzero_arm + 1));
        }
        if (*nonzero_arm == 0) {
          nonzero_condition = truncated_selector;
        } else {
          XLS_ASSIGN_OR_RETURN(
              Node * matching_value,
              node->function_base()->MakeNode<Literal>(
                  sel->loc(),
                  Value(Bits::PowerOfTwo(*nonzero_arm, *nonzero_arm + 1))));
          XLS_ASSIGN_OR_RETURN(
              nonzero_condition,
              node->function_base()->MakeNode<CompareOp>(
                  sel->loc(), truncated_selector, matching_value, Op::kEq));
        }
      }
      break;
    }
  }

  if (!only_nonzero_value.has_value()) {
    // The select can't return any non-zero value.
    VLOG(2) << absl::StrFormat("select with no non-zero cases: %s",
                               node->ToString());
    XLS_RETURN_IF_ERROR(
        node->ReplaceUsesWithNew<Literal>(ZeroOfType(node->GetType()))
            .status());
    return true;
  }

  XLS_RET_CHECK_NE(nonzero_condition, nullptr);
  if (node->GetType()->IsBits()) {
    Node* mask;
    if (node->BitCountOrDie() == 1) {
      mask = nonzero_condition;
    } else {
      XLS_ASSIGN_OR_RETURN(
          mask, node->function_base()->MakeNode<ExtendOp>(
                    node->loc(), nonzero_condition,
                    /*new_bit_count=*/node->BitCountOrDie(), Op::kSignExt));
    }
    XLS_RETURN_IF_ERROR(
        node->ReplaceUsesWithNew<NaryOp>(
                std::vector<Node*>{*only_nonzero_value, mask}, Op::kAnd)
            .status());
    return true;
  }
  XLS_ASSIGN_OR_RETURN(Node * literal_zero,
                       node->function_base()->MakeNode<Literal>(
                           node->loc(), ZeroOfType(node->GetType())));
  XLS_RETURN_IF_ERROR(
      node->ReplaceUsesWithNew<Select>(nonzero_condition,
                                       std::vector<Node*>({literal_zero}),
                                       /*default_value=*/*only_nonzero_value)
          .status());
  return true;
}

absl::StatusOr<bool> SimplifyNode(Node* node, const QueryEngine& query_engine,
                                  int64_t opt_level) {
  // Select with a constant selector can be replaced with the respective
  // case.
  if (node->Is<Select>() &&
      query_engine.IsFullyKnown(node->As<Select>()->selector())) {
    Select* sel = node->As<Select>();
    const Bits selector = *query_engine.KnownValueAsBits(sel->selector());
    VLOG(2) << absl::StrFormat("Simplifying select with constant selector: %s",
                               node->ToString());
    if (bits_ops::UGreaterThan(
            selector, UBits(sel->cases().size() - 1, selector.bit_count()))) {
      XLS_RET_CHECK(sel->default_value().has_value());
      XLS_RETURN_IF_ERROR(sel->ReplaceUsesWith(*sel->default_value()));
    } else {
      XLS_ASSIGN_OR_RETURN(uint64_t i, selector.ToUint64());
      XLS_RETURN_IF_ERROR(sel->ReplaceUsesWith(sel->get_case(i)));
    }
    return true;
  }

  // Priority select where we know the selector ends with a one followed by
  // zeros can be replaced with the selected case.
  if (node->Is<PrioritySelect>()) {
    PrioritySelect* sel = node->As<PrioritySelect>();
    XLS_RET_CHECK(sel->selector()->GetType()->IsBits());
    const TernaryVector selector =
        query_engine.GetTernary(sel->selector()).Get({});
    auto first_nonzero_case = absl::c_find_if(
        selector, [](TernaryValue v) { return v != TernaryValue::kKnownZero; });
    if (first_nonzero_case == selector.end()) {
      // All zeros; priority select with a zero selector returns zero.
      XLS_RETURN_IF_ERROR(
          sel->ReplaceUsesWithNew<Literal>(ZeroOfType(sel->GetType()))
              .status());
      return true;
    }
    if (*first_nonzero_case == TernaryValue::kKnownOne) {
      // Ends with a one followed by zeros; returns the corresponding case.
      int64_t case_num = std::distance(selector.begin(), first_nonzero_case);
      XLS_RETURN_IF_ERROR(sel->ReplaceUsesWith(sel->get_case(case_num)));
      return true;
    }
    // Has an unknown bit before the first known one, so the result is unknown.
    // TODO(https://github.com/google/xls/issues/1446): Trim out all cases that
    // are known-zero or after the first known one.
  }

  // One-hot-select with a constant selector can be replaced with OR of the
  // activated cases.
  if (node->Is<OneHotSelect>() &&
      query_engine.IsFullyKnown(node->As<OneHotSelect>()->selector()) &&
      node->GetType()->IsBits()) {
    OneHotSelect* sel = node->As<OneHotSelect>();
    const Bits selector = *query_engine.KnownValueAsBits(sel->selector());
    Node* replacement = nullptr;
    for (int64_t i = 0; i < selector.bit_count(); ++i) {
      if (selector.Get(i)) {
        if (replacement == nullptr) {
          replacement = sel->get_case(i);
        } else {
          XLS_ASSIGN_OR_RETURN(
              replacement,
              node->function_base()->MakeNode<NaryOp>(
                  node->loc(),
                  std::vector<Node*>{replacement, sel->get_case(i)}, Op::kOr));
        }
      }
    }
    if (replacement == nullptr) {
      XLS_ASSIGN_OR_RETURN(
          replacement,
          node->function_base()->MakeNode<Literal>(
              node->loc(), Value(UBits(0, node->BitCountOrDie()))));
    }
    VLOG(2) << absl::StrFormat(
        "Simplifying one-hot-select with constant selector: %s",
        node->ToString());
    XLS_RETURN_IF_ERROR(sel->ReplaceUsesWith(replacement));
    return true;
  }

  // Select with identical cases can be replaced with the value.
  if (node->Is<Select>()) {
    Select* sel = node->As<Select>();
    if (sel->AllCases(
            [&](Node* other_case) { return other_case == sel->any_case(); })) {
      VLOG(2) << absl::StrFormat("Simplifying select with identical cases: %s",
                                 node->ToString());
      XLS_RETURN_IF_ERROR(node->ReplaceUsesWith(sel->any_case()));
      return true;
    }
  }

  // OneHotSelect & PrioritySelect with identical cases can be replaced with a
  // select between one of the identical case and the value zero where the
  // selector is: original selector == 0
  if (node->OpIn({Op::kOneHotSel, Op::kPrioritySel}) &&
      node->GetType()->IsBits()) {
    Node* selector = node->Is<OneHotSelect>()
                         ? node->As<OneHotSelect>()->selector()
                         : node->As<PrioritySelect>()->selector();
    absl::Span<Node* const> cases = node->Is<OneHotSelect>()
                                        ? node->As<OneHotSelect>()->cases()
                                        : node->As<PrioritySelect>()->cases();
    if (absl::c_all_of(cases, [&](Node* c) { return c == cases[0]; })) {
      FunctionBase* f = node->function_base();
      XLS_ASSIGN_OR_RETURN(
          Node * selector_zero,
          f->MakeNode<Literal>(node->loc(), ZeroOfType(selector->GetType())));
      XLS_ASSIGN_OR_RETURN(Node * is_zero,
                           f->MakeNode<CompareOp>(node->loc(), selector,
                                                  selector_zero, Op::kEq));
      XLS_ASSIGN_OR_RETURN(
          Node * selected_zero,
          f->MakeNode<Literal>(node->loc(), ZeroOfType(node->GetType())));
      VLOG(2) << absl::StrFormat(
          "Simplifying %s-select with identical cases: %s",
          (node->Is<OneHotSelect>() ? "one-hot" : "priority"),
          node->ToString());
      XLS_RETURN_IF_ERROR(node->ReplaceUsesWithNew<Select>(
                                  is_zero,
                                  std::vector<Node*>{cases[0], selected_zero},
                                  /*default_value=*/std::nullopt)
                              .status());
      return true;
    }
  }

  // Replace a select among tuples to a tuple of selects. Handles all of select,
  // one-hot-select, and priority-select.
  if (node->GetType()->IsTuple() &&
      node->OpIn({Op::kSel, Op::kOneHotSel, Op::kPrioritySel})) {
    // Construct a vector containing the element at 'tuple_index' for each
    // case of the select.
    auto elements_at_tuple_index =
        [&](absl::Span<Node* const> nodes,
            int64_t tuple_index) -> absl::StatusOr<std::vector<Node*>> {
      std::vector<Node*> elements;
      for (Node* n : nodes) {
        XLS_ASSIGN_OR_RETURN(Node * element,
                             node->function_base()->MakeNode<TupleIndex>(
                                 node->loc(), n, tuple_index));
        elements.push_back(element);
      }
      return elements;
    };

    if (node->Is<OneHotSelect>()) {
      OneHotSelect* sel = node->As<OneHotSelect>();
      std::vector<Node*> selected_elements;
      for (int64_t i = 0; i < node->GetType()->AsTupleOrDie()->size(); ++i) {
        XLS_ASSIGN_OR_RETURN(std::vector<Node*> case_elements,
                             elements_at_tuple_index(sel->cases(), i));
        XLS_ASSIGN_OR_RETURN(Node * selected_element,
                             node->function_base()->MakeNode<OneHotSelect>(
                                 node->loc(), sel->selector(), case_elements));
        selected_elements.push_back(selected_element);
      }
      VLOG(2) << absl::StrFormat("Decomposing tuple-typed one-hot-select: %s",
                                 node->ToString());
      XLS_RETURN_IF_ERROR(
          node->ReplaceUsesWithNew<Tuple>(selected_elements).status());
      return true;
    }

    if (node->Is<Select>()) {
      Select* sel = node->As<Select>();
      std::vector<Node*> selected_elements;
      for (int64_t i = 0; i < node->GetType()->AsTupleOrDie()->size(); ++i) {
        XLS_ASSIGN_OR_RETURN(std::vector<Node*> case_elements,
                             elements_at_tuple_index(sel->cases(), i));
        std::optional<Node*> default_element = std::nullopt;
        if (sel->default_value().has_value()) {
          XLS_ASSIGN_OR_RETURN(default_element,
                               node->function_base()->MakeNode<TupleIndex>(
                                   node->loc(), *sel->default_value(), i));
        }
        XLS_ASSIGN_OR_RETURN(
            Node * selected_element,
            node->function_base()->MakeNode<Select>(
                node->loc(), sel->selector(), case_elements, default_element));
        selected_elements.push_back(selected_element);
      }
      VLOG(2) << absl::StrFormat("Decomposing tuple-typed select: %s",
                                 node->ToString());
      XLS_RETURN_IF_ERROR(
          node->ReplaceUsesWithNew<Tuple>(selected_elements).status());
      return true;
    }

    if (node->Is<PrioritySelect>()) {
      PrioritySelect* sel = node->As<PrioritySelect>();
      std::vector<Node*> selected_elements;
      for (int64_t i = 0; i < node->GetType()->AsTupleOrDie()->size(); ++i) {
        XLS_ASSIGN_OR_RETURN(std::vector<Node*> case_elements,
                             elements_at_tuple_index(sel->cases(), i));
        XLS_ASSIGN_OR_RETURN(Node * selected_element,
                             node->function_base()->MakeNode<PrioritySelect>(
                                 node->loc(), sel->selector(), case_elements));
        selected_elements.push_back(selected_element);
      }
      VLOG(2) << absl::StrFormat("Decomposing tuple-typed priority select: %s",
                                 node->ToString());
      XLS_RETURN_IF_ERROR(
          node->ReplaceUsesWithNew<Tuple>(selected_elements).status());
      return true;
    }
  }

  // Common out equivalent cases in a one hot select.
  if (NarrowingEnabled(opt_level) && node->Is<OneHotSelect>()) {
    FunctionBase* f = node->function_base();
    OneHotSelect* sel = node->As<OneHotSelect>();
    if (!sel->cases().empty() &&
        absl::flat_hash_set<Node*>(sel->cases().begin(), sel->cases().end())
                .size() != sel->cases().size()) {
      // For any case that's equal to another case, we or together the one hot
      // selectors and common out the value to squeeze the width of the one hot
      // select.
      std::vector<Node*> new_selectors;
      std::vector<Node*> new_cases;
      for (int64_t i = 0; i < sel->cases().size(); ++i) {
        Node* old_case = sel->get_case(i);
        XLS_ASSIGN_OR_RETURN(Node * old_selector,
                             f->MakeNode<BitSlice>(node->loc(), sel->selector(),
                                                   /*start=*/i, 1));
        auto it = std::find_if(
            new_cases.begin(), new_cases.end(),
            [old_case](Node* new_case) { return old_case == new_case; });
        if (it == new_cases.end()) {
          new_selectors.push_back(old_selector);
          new_cases.push_back(old_case);
        } else {
          // Or together the selectors, no need to append the old case.
          int64_t index = std::distance(new_cases.begin(), it);
          XLS_ASSIGN_OR_RETURN(
              new_selectors[index],
              f->MakeNode<NaryOp>(
                  node->loc(),
                  std::vector<Node*>{new_selectors[index], old_selector},
                  Op::kOr));
        }
      }
      std::reverse(new_selectors.begin(), new_selectors.end());
      XLS_ASSIGN_OR_RETURN(Node * new_selector,
                           f->MakeNode<Concat>(node->loc(), new_selectors));
      VLOG(2) << absl::StrFormat("One-hot select with equivalent cases: %s",
                                 node->ToString());
      XLS_RETURN_IF_ERROR(
          node->ReplaceUsesWithNew<OneHotSelect>(new_selector, new_cases)
              .status());
      return true;
    }
  }

  // Common out equivalent cases in a priority select.
  if (SplitsEnabled(opt_level) && node->Is<PrioritySelect>() &&
      !node->As<PrioritySelect>()->cases().empty()) {
    FunctionBase* f = node->function_base();
    PrioritySelect* sel = node->As<PrioritySelect>();

    // We can merge adjacent cases with the same outputs by OR-ing together
    // the relevant bits of the selector.
    struct SelectorRange {
      int64_t start;
      int64_t width = 1;
    };
    std::vector<SelectorRange> new_selector_ranges;
    std::vector<Node*> new_cases;
    new_selector_ranges.push_back({.start = 0});
    new_cases.push_back(sel->get_case(0));
    for (int64_t i = 1; i < sel->cases().size(); ++i) {
      Node* old_case = sel->get_case(i);
      if (old_case == new_cases.back()) {
        new_selector_ranges.back().width++;
      } else {
        new_selector_ranges.push_back({.start = i});
        new_cases.push_back(old_case);
      }
    }
    if (new_cases.size() < sel->cases().size()) {
      std::vector<Node*> new_selector_slices;
      std::optional<SelectorRange> current_original_slice = std::nullopt;
      auto commit_original_slice = [&]() -> absl::Status {
        if (!current_original_slice.has_value()) {
          return absl::OkStatus();
        }
        XLS_ASSIGN_OR_RETURN(
            Node * selector_slice,
            f->MakeNode<BitSlice>(node->loc(), sel->selector(),
                                  current_original_slice->start,
                                  current_original_slice->width));
        new_selector_slices.push_back(selector_slice);
        current_original_slice.reset();
        return absl::OkStatus();
      };
      for (const SelectorRange& range : new_selector_ranges) {
        if (range.width == 1 && current_original_slice.has_value()) {
          current_original_slice->width++;
          continue;
        }

        XLS_RETURN_IF_ERROR(commit_original_slice());
        if (range.width == 1) {
          current_original_slice = SelectorRange{.start = range.start};
        } else {
          XLS_ASSIGN_OR_RETURN(
              Node * selector_slice,
              f->MakeNode<BitSlice>(node->loc(), sel->selector(), range.start,
                                    range.width));
          XLS_ASSIGN_OR_RETURN(Node * selector_bit,
                               f->MakeNode<BitwiseReductionOp>(
                                   node->loc(), selector_slice, Op::kOrReduce));
          new_selector_slices.push_back(selector_bit);
        }
      }
      XLS_RETURN_IF_ERROR(commit_original_slice());
      absl::c_reverse(new_selector_slices);
      XLS_ASSIGN_OR_RETURN(
          Node * new_selector,
          f->MakeNode<Concat>(node->loc(), new_selector_slices));
      VLOG(2) << absl::StrFormat("Priority select with equivalent cases: %s",
                                 node->ToString());
      XLS_RETURN_IF_ERROR(
          node->ReplaceUsesWithNew<PrioritySelect>(new_selector, new_cases)
              .status());
      return true;
    }
  }

  // We explode single-bit muxes into their constituent gates to expose more
  // optimization opportunities. Since this creates more ops in the general
  // case, we look for certain sub-cases:
  //
  // * At least one of the selected values is a constant.
  // * One of the selected values is also the selector.
  //
  // TODO(meheff): Handle one-hot select and priority-select here as well.
  auto is_one_bit_mux = [&] {
    return node->Is<Select>() && node->GetType()->IsBits() &&
           node->BitCountOrDie() == 1 && node->operand(0)->BitCountOrDie() == 1;
  };
  if (NarrowingEnabled(opt_level) && is_one_bit_mux() &&
      (query_engine.IsFullyKnown(node->operand(1)) ||
       query_engine.IsFullyKnown(node->operand(2)) ||
       (node->operand(0) == node->operand(1) ||
        node->operand(0) == node->operand(2)))) {
    FunctionBase* f = node->function_base();
    Select* select = node->As<Select>();
    XLS_RET_CHECK(!select->default_value().has_value()) << select->ToString();
    Node* s = select->operand(0);
    Node* on_false = select->get_case(0);
    Node* on_true = select->get_case(1);
    XLS_ASSIGN_OR_RETURN(
        Node * lhs,
        f->MakeNode<NaryOp>(select->loc(), std::vector<Node*>{s, on_true},
                            Op::kAnd));
    XLS_ASSIGN_OR_RETURN(Node * s_not,
                         f->MakeNode<UnOp>(select->loc(), s, Op::kNot));
    XLS_ASSIGN_OR_RETURN(
        Node * rhs,
        f->MakeNode<NaryOp>(select->loc(), std::vector<Node*>{s_not, on_false},
                            Op::kAnd));
    VLOG(2) << absl::StrFormat("Decomposing single-bit select: %s",
                               node->ToString());
    XLS_RETURN_IF_ERROR(
        select
            ->ReplaceUsesWithNew<NaryOp>(std::vector<Node*>{lhs, rhs}, Op::kOr)
            .status());
    return true;
  }

  // Merge consecutive one-hot-select or priority-select instructions if the
  // predecessor operation has only a single use (and is of matching type).
  if (NarrowingEnabled(opt_level) &&
      (node->Is<OneHotSelect>() || node->Is<PrioritySelect>())) {
    Node* selector;
    absl::Span<Node* const> cases;
    if (node->Is<OneHotSelect>()) {
      selector = node->As<OneHotSelect>()->selector();
      cases = node->As<OneHotSelect>()->cases();
    } else {
      XLS_RET_CHECK(node->Is<PrioritySelect>());
      selector = node->As<PrioritySelect>()->selector();
      cases = node->As<PrioritySelect>()->cases();
    }
    auto is_single_user_matching_select = [select_op = node->op()](Node* n) {
      return n->op() == select_op && HasSingleUse(n);
    };
    if (std::any_of(cases.begin(), cases.end(),
                    is_single_user_matching_select)) {
      // Cases for the replacement one-hot-select.
      std::vector<Node*> new_cases;
      // Pieces of the selector for the replacement one-hot-select. These are
      // concatted together.
      std::vector<Node*> new_selector_parts;
      // When iterating through the cases to perform this optimization, cases
      // which are to remain unmodified (ie, not a single-use one-hot-select)
      // are passed over. This lambda gathers the passed over cases and
      // updates new_cases and new_selector_parts.
      int64_t unhandled_selector_bits = 0;
      auto add_unhandled_selector_bits = [&](int64_t index) -> absl::Status {
        if (unhandled_selector_bits != 0) {
          XLS_ASSIGN_OR_RETURN(Node * selector_part,
                               node->function_base()->MakeNode<BitSlice>(
                                   node->loc(), selector,
                                   /*start=*/index - unhandled_selector_bits,
                                   /*width=*/
                                   unhandled_selector_bits));
          new_selector_parts.push_back(selector_part);
          for (int64_t i = index - unhandled_selector_bits; i < index; ++i) {
            new_cases.push_back(cases[i]);
          }
        }
        unhandled_selector_bits = 0;
        return absl::OkStatus();
      };
      // Iterate through the cases merging single-use matching-select cases.
      Node* zero = nullptr;
      for (int64_t i = 0; i < cases.size(); ++i) {
        if (is_single_user_matching_select(cases[i])) {
          Node* operand_selector;
          absl::Span<Node* const> operand_cases;
          if (cases[i]->Is<OneHotSelect>()) {
            operand_selector = cases[i]->As<OneHotSelect>()->selector();
            operand_cases = cases[i]->As<OneHotSelect>()->cases();
          } else {
            XLS_RET_CHECK(cases[i]->Is<PrioritySelect>());
            operand_selector = cases[i]->As<PrioritySelect>()->selector();
            operand_cases = cases[i]->As<PrioritySelect>()->cases();
          }
          XLS_RETURN_IF_ERROR(add_unhandled_selector_bits(i));
          // The selector bits for the predecessor bit-select need to be
          // ANDed with the original selector bit in the successor bit-select.
          // Example:
          //
          //   X = one_hot_select(selector={A, B, C},
          //                      cases=[x, y z])
          //   Y = one_hot_select(selector={..., S, ...},
          //                      cases=[..., X, ...])
          // Becomes:
          //
          //   Y = one_hot_select(
          //     selector={..., S & A, S & B, S & C, ...},
          //     cases=[..., A, B, C, ...])
          //
          XLS_ASSIGN_OR_RETURN(Node * selector_bit,
                               node->function_base()->MakeNode<BitSlice>(
                                   node->loc(), selector,
                                   /*start=*/i, /*width=*/1));
          XLS_ASSIGN_OR_RETURN(
              Node * selector_bit_mask,
              node->function_base()->MakeNode<ExtendOp>(
                  node->loc(), selector_bit,
                  /*new_bit_count=*/operand_cases.size(), Op::kSignExt));
          XLS_ASSIGN_OR_RETURN(
              Node * masked_selector,
              node->function_base()->MakeNode<NaryOp>(
                  node->loc(),
                  std::vector<Node*>{selector_bit_mask, operand_selector},
                  Op::kAnd));
          new_selector_parts.push_back(masked_selector);
          absl::c_copy(operand_cases, std::back_inserter(new_cases));
          if (node->Is<PrioritySelect>()) {
            // We also need to handle the scenario where this case is
            // selected, but the case evaluates to its default value (zero).
            Node* operand_selector_is_zero;
            if (operand_selector->BitCountOrDie() == 1) {
              XLS_ASSIGN_OR_RETURN(
                  operand_selector_is_zero,
                  node->function_base()->MakeNode<UnOp>(
                      cases[i]->loc(), operand_selector, Op::kNot));
            } else {
              XLS_ASSIGN_OR_RETURN(
                  Node * operand_selector_zero,
                  node->function_base()->MakeNode<Literal>(
                      cases[i]->loc(),
                      ZeroOfType(operand_selector->GetType())));
              XLS_ASSIGN_OR_RETURN(operand_selector_is_zero,
                                   node->function_base()->MakeNode<CompareOp>(
                                       node->loc(), operand_selector,
                                       operand_selector_zero, Op::kEq));
            }
            XLS_ASSIGN_OR_RETURN(
                Node * masked_operand_selector_is_zero,
                node->function_base()->MakeNode<NaryOp>(
                    cases[i]->loc(),
                    std::vector<Node*>{selector_bit, operand_selector_is_zero},
                    Op::kAnd));
            if (zero == nullptr) {
              XLS_ASSIGN_OR_RETURN(
                  zero, node->function_base()->MakeNode<Literal>(
                            cases[i]->loc(), ZeroOfType(cases[i]->GetType())));
            }
            new_selector_parts.push_back(masked_operand_selector_is_zero);
            new_cases.push_back(zero);
          }
        } else {
          unhandled_selector_bits++;
        }
      }
      XLS_RETURN_IF_ERROR(add_unhandled_selector_bits(cases.size()));
      // Reverse selector parts because concat operand zero is the msb.
      std::reverse(new_selector_parts.begin(), new_selector_parts.end());
      XLS_ASSIGN_OR_RETURN(Node * new_selector,
                           node->function_base()->MakeNode<Concat>(
                               node->loc(), new_selector_parts));
      if (node->Is<OneHotSelect>()) {
        VLOG(2) << absl::StrFormat("Merging consecutive one-hot-selects: %s",
                                   node->ToString());
        XLS_RETURN_IF_ERROR(
            node->ReplaceUsesWithNew<OneHotSelect>(new_selector, new_cases)
                .status());
      } else {
        XLS_RET_CHECK(node->Is<PrioritySelect>());
        VLOG(2) << absl::StrFormat("Merging consecutive priority-selects: %s",
                                   node->ToString());
        XLS_RETURN_IF_ERROR(
            node->ReplaceUsesWithNew<PrioritySelect>(new_selector, new_cases)
                .status());
      }
      return true;
    }
  }

  // Since masking with an 'and' can't be reasoned through as easily (e.g., by
  // conditional specialization), we want to avoid doing this until fairly
  // late in the pipeline.
  if (SplitsEnabled(opt_level)) {
    XLS_ASSIGN_OR_RETURN(bool converted_to_mask,
                         MaybeConvertSelectToMask(node, query_engine));
    if (converted_to_mask) {
      return true;
    }
  }

  // Literal zero cases or positions where the selector is zero can be removed
  // from OneHotSelects and priority selects.
  if (NarrowingEnabled(opt_level) &&
      (node->Is<OneHotSelect>() || node->Is<PrioritySelect>())) {
    Node* selector = node->Is<OneHotSelect>()
                         ? node->As<OneHotSelect>()->selector()
                         : node->As<PrioritySelect>()->selector();
    absl::Span<Node* const> cases = node->Is<OneHotSelect>()
                                        ? node->As<OneHotSelect>()->cases()
                                        : node->As<PrioritySelect>()->cases();
    if (query_engine.IsTracked(selector)) {
      TernaryVector selector_bits = query_engine.GetTernary(selector).Get({});
      // For one-hot-selects if either the selector bit or the case value is
      // zero, the case can be removed. For priority selects, the case can be
      // removed only if the selector bit is zero, or if *all later* cases are
      // removable.
      bool all_later_cases_removable = false;
      auto is_removable_case = [&](int64_t c) {
        if (all_later_cases_removable) {
          return true;
        }
        if (node->Is<PrioritySelect>() &&
            selector_bits[c] == TernaryValue::kKnownOne) {
          all_later_cases_removable = true;
          return false;
        }
        if (selector_bits[c] == TernaryValue::kKnownZero) {
          return true;
        }
        return node->Is<OneHotSelect>() && query_engine.IsAllZeros(cases[c]);
      };
      bool has_removable_case = false;
      std::vector<int64_t> nonremovable_indices;
      for (int64_t i = 0; i < cases.size(); ++i) {
        if (is_removable_case(i)) {
          has_removable_case = true;
        } else {
          nonremovable_indices.push_back(i);
        }
      }
      if (node->Is<PrioritySelect>()) {
        // Go back and check the trailing cases; we can remove trailing zeros.
        while (!nonremovable_indices.empty() &&
               query_engine.IsAllZeros(cases[nonremovable_indices.back()])) {
          nonremovable_indices.pop_back();
        }
      }
      if (!SplitsEnabled(opt_level) && !nonremovable_indices.empty() &&
          has_removable_case) {
        // No splitting, so we can only remove the leading and trailing cases.
        int64_t first_nonremovable_index = nonremovable_indices.front();
        int64_t last_nonremovable_index = nonremovable_indices.back();
        nonremovable_indices.clear();
        for (int64_t i = first_nonremovable_index; i <= last_nonremovable_index;
             ++i) {
          nonremovable_indices.push_back(i);
        }
        if (nonremovable_indices.size() == cases.size()) {
          // No cases are removable.
          has_removable_case = false;
        }
      }
      if (has_removable_case) {
        // Assemble the slices of the selector which correspond to non-zero
        // cases.
        if (nonremovable_indices.empty()) {
          // If all cases were zeros, just replace the op with literal zero.
          XLS_RETURN_IF_ERROR(
              node->ReplaceUsesWithNew<Literal>(ZeroOfType(node->GetType()))
                  .status());
          return true;
        }
        XLS_ASSIGN_OR_RETURN(Node * new_selector,
                             GatherBits(selector, nonremovable_indices));
        std::vector<Node*> new_cases =
            GatherFromSequence(cases, nonremovable_indices);
        VLOG(2) << absl::StrFormat(
            "Literal zero cases removed from %s-select: %s",
            node->Is<OneHotSelect>() ? "one-hot" : "priority",
            node->ToString());
        if (node->Is<OneHotSelect>()) {
          XLS_RETURN_IF_ERROR(
              node->ReplaceUsesWithNew<OneHotSelect>(new_selector, new_cases)
                  .status());
        } else {
          XLS_RETURN_IF_ERROR(
              node->ReplaceUsesWithNew<PrioritySelect>(new_selector, new_cases)
                  .status());
        }
        return true;
      }
    }
  }

  // "Squeeze" the width of the mux when bits are known to reduce the cost of
  // the operation.
  //
  // Sel(...) => Concat(Known, Sel(...), Known)
  if (SplitsEnabled(opt_level)) {
    auto is_squeezable_mux = [&](Bits* msb, Bits* lsb) {
      if (!node->Is<Select>() || !node->GetType()->IsBits()) {
        return false;
      }
      int64_t leading_known = bits_ops::CountLeadingOnes(
          ternary_ops::ToKnownBits(query_engine.GetTernary(node).Get({})));
      int64_t trailing_known = bits_ops::CountTrailingOnes(
          ternary_ops::ToKnownBits(query_engine.GetTernary(node).Get({})));
      if (leading_known == 0 && trailing_known == 0) {
        return false;
      }
      int64_t bit_count = node->BitCountOrDie();
      *msb =
          ternary_ops::ToKnownBitsValues(query_engine.GetTernary(node).Get({}))
              .Slice(/*start=*/bit_count - leading_known,
                     /*width=*/leading_known);
      if (leading_known == trailing_known && leading_known == bit_count) {
        // This is just a constant value, just say we only have high constant
        // bits, the replacement will be the same.
        return true;
      }
      *lsb =
          ternary_ops::ToKnownBitsValues(query_engine.GetTernary(node).Get({}))
              .Slice(/*start=*/0, /*width=*/trailing_known);
      return true;
    };
    Bits const_msb, const_lsb;
    if (is_squeezable_mux(&const_msb, &const_lsb)) {
      std::function<absl::StatusOr<Select*>(Select*, std::vector<Node*>)>
          make_select =
              [](Select* original,
                 std::vector<Node*> new_cases) -> absl::StatusOr<Select*> {
        std::optional<Node*> new_default;
        if (original->default_value().has_value()) {
          new_default = new_cases.back();
          new_cases.pop_back();
        }
        return original->function_base()->MakeNode<Select>(
            original->loc(), original->selector(), new_cases, new_default);
      };
      return SqueezeSelect(const_msb, const_lsb, make_select,
                           node->As<Select>());
    }
  }

  // Collapse consecutive two-ways selects which have share a common case. For
  // example:
  //
  //   s1 = select(p1, [y, x])
  //   s0 = select(p0, [s_1, x])
  //
  // In this case, 'x' is a common case between the two selects and the above
  // can be replaced with:
  //
  //   p' = or(p0, p1)
  //   s0 = select(p', [x, y])
  //
  // There are four different cases to consider depending upon whether the
  // common case is on the LHS or RHS of the selects.
  auto is_2way_select = [](Node* n) {
    return n->Is<Select>() &&
           n->As<Select>()->selector()->BitCountOrDie() == 1 &&
           n->As<Select>()->cases().size() == 2;
  };
  if (is_2way_select(node)) {
    //  The variable names correspond to the names of the nodes in the
    //  diagrams below.
    Select* sel0 = node->As<Select>();
    Node* p0 = sel0->selector();
    // The values below are by each matching cases below.
    Node* x = nullptr;
    Node* y = nullptr;
    // The predicate to select the common case 'x' in the newly constructed
    // select.
    Node* p_x = nullptr;
    if (is_2way_select(sel0->get_case(0))) {
      Select* sel1 = sel0->get_case(0)->As<Select>();
      Node* p1 = sel1->selector();
      if (sel0->get_case(1) == sel1->get_case(0)) {
        //       x   y
        //        \ /
        //  p1 -> sel1   x
        //           \   /
        //      p0 -> sel0
        //
        // p_x = p0 | !p1
        x = sel0->get_case(1);
        y = sel1->get_case(1);
        XLS_ASSIGN_OR_RETURN(
            Node * not_p1,
            sel0->function_base()->MakeNode<UnOp>(sel0->loc(), p1, Op::kNot));
        XLS_ASSIGN_OR_RETURN(
            p_x, sel0->function_base()->MakeNode<NaryOp>(
                     sel0->loc(), std::vector<Node*>{p0, not_p1}, Op::kOr));
      } else if (sel0->get_case(1) == sel1->get_case(1)) {
        //         y   x
        //          \ /
        //   p1 -> sel1   x
        //            \   /
        //       p0 -> sel0
        //
        // p_x = p0 | p1
        x = sel0->get_case(1);
        y = sel1->get_case(0);
        XLS_ASSIGN_OR_RETURN(
            p_x, sel0->function_base()->MakeNode<NaryOp>(
                     sel0->loc(), std::vector<Node*>{p0, p1}, Op::kOr));
      }
    } else if (is_2way_select(sel0->get_case(1))) {
      Select* sel1 = sel0->get_case(1)->As<Select>();
      Node* p1 = sel1->selector();
      if (sel0->get_case(0) == sel1->get_case(0)) {
        //  x    x   y
        //   \    \ /
        //    \  sel1 <- p1
        //     \  /
        //      sel0 <- p0
        //
        // p_x = nand(p0, p1)
        x = sel0->get_case(0);
        y = sel1->get_case(1);
        XLS_ASSIGN_OR_RETURN(
            p_x, sel0->function_base()->MakeNode<NaryOp>(
                     sel0->loc(), std::vector<Node*>{p0, p1}, Op::kNand));
      } else if (sel0->get_case(0) == sel1->get_case(1)) {
        //  x    y   x
        //   \    \ /
        //    \  sel1 <- p1
        //     \  /
        //      sel0 <- p0
        //
        // p_x = !p0 | p1
        x = sel0->get_case(0);
        y = sel1->get_case(0);
        XLS_ASSIGN_OR_RETURN(
            Node * not_p0,
            sel0->function_base()->MakeNode<UnOp>(sel0->loc(), p0, Op::kNot));
        XLS_ASSIGN_OR_RETURN(
            p_x, sel0->function_base()->MakeNode<NaryOp>(
                     sel0->loc(), std::vector<Node*>{not_p0, p1}, Op::kOr));
      }
    }
    if (x != nullptr) {
      VLOG(2) << absl::StrFormat(
          "Consecutive binary select with common cases: %s", node->ToString());
      XLS_ASSIGN_OR_RETURN(p_x, sel0->ReplaceUsesWithNew<Select>(
                                    p_x, std::vector<Node*>{y, x},
                                    /*default_value=*/std::nullopt));
      return true;
    }
  }

  // Consecutive selects which share a selector can be collapsed into a single
  // select. If sel0 selects sel1 on when p is false:
  //
  //  a   b
  //   \ /
  //   sel1 ----+-- p       a   c
  //    |       |       =>   \ /
  //    |  c    |            sel -- p
  //    | /     |             |
  //   sel0 ----+
  //    |
  //
  // If sel0 selects sel1 on when p is true:
  //
  //    a   b
  //     \ /
  //     sel1 -+-- p       c   b
  //      |    |       =>   \ /
  //   c  |    |            sel -- p
  //    \ |    |             |
  //     sel0 -+
  //      |
  //
  // TODO(meheff): Generalize this to multi-way selects and possibly
  // one-hot-selects.
  if (is_2way_select(node)) {
    Select* sel0 = node->As<Select>();
    if (is_2way_select(sel0->get_case(0))) {
      Select* sel1 = sel0->get_case(0)->As<Select>();
      if (sel0->selector() == sel1->selector()) {
        XLS_RETURN_IF_ERROR(sel0->ReplaceOperandNumber(1, sel1->get_case(0)));
        return true;
      }
    }
    if (is_2way_select(sel0->get_case(1))) {
      Select* sel1 = sel0->get_case(1)->As<Select>();
      if (sel0->selector() == sel1->selector()) {
        XLS_RETURN_IF_ERROR(sel0->ReplaceOperandNumber(2, sel1->get_case(1)));
        return true;
      }
    }
  }

  // Decompose single-bit, two-way OneHotSelects into ANDs and ORs.
  if (SplitsEnabled(opt_level) && node->Is<OneHotSelect>() &&
      node->GetType()->IsBits() && node->BitCountOrDie() == 1 &&
      node->As<OneHotSelect>()->cases().size() == 2) {
    OneHotSelect* ohs = node->As<OneHotSelect>();
    XLS_ASSIGN_OR_RETURN(Node * sel0,
                         node->function_base()->MakeNode<BitSlice>(
                             node->loc(), ohs->selector(), /*start=*/0,
                             /*width=*/1));
    XLS_ASSIGN_OR_RETURN(Node * sel1,
                         node->function_base()->MakeNode<BitSlice>(
                             node->loc(), ohs->selector(), /*start=*/1,
                             /*width=*/1));
    XLS_ASSIGN_OR_RETURN(
        Node * and0,
        node->function_base()->MakeNode<NaryOp>(
            node->loc(), std::vector<Node*>{sel0, ohs->get_case(0)}, Op::kAnd));
    XLS_ASSIGN_OR_RETURN(
        Node * and1,
        node->function_base()->MakeNode<NaryOp>(
            node->loc(), std::vector<Node*>{sel1, ohs->get_case(1)}, Op::kAnd));
    VLOG(2) << absl::StrFormat("Decompose single-bit one-hot-select: %s",
                               node->ToString());
    XLS_RETURN_IF_ERROR(node->ReplaceUsesWithNew<NaryOp>(
                                std::vector<Node*>{and0, and1}, Op::kOr)
                            .status());
    return true;
  }

  // Replace a single-bit input kOneHot with the concat of the input and its
  // inverse.
  if (NarrowingEnabled(opt_level) && node->Is<OneHot>() &&
      node->BitCountOrDie() == 2) {
    XLS_ASSIGN_OR_RETURN(Node * inv_operand,
                         node->function_base()->MakeNode<UnOp>(
                             node->loc(), node->operand(0), Op::kNot));
    VLOG(2) << absl::StrFormat("Replace single-bit input one-hot to concat: %s",
                               node->ToString());
    XLS_RETURN_IF_ERROR(
        node->ReplaceUsesWithNew<Concat>(
                std::vector<Node*>{inv_operand, node->operand(0)})
            .status());
    return true;
  }

  // Remove kOneHot operations with an input that is mutually exclusive.
  if (node->Is<OneHot>()) {
    if (query_engine.AtMostOneBitTrue(node->operand(0))) {
      XLS_ASSIGN_OR_RETURN(
          Node * zero,
          node->function_base()->MakeNode<Literal>(
              node->loc(),
              Value(UBits(0,
                          /*bit_count=*/node->operand(0)->BitCountOrDie()))));
      XLS_ASSIGN_OR_RETURN(Node * operand_eq_zero,
                           node->function_base()->MakeNode<CompareOp>(
                               node->loc(), node->operand(0), zero, Op::kEq));
      VLOG(2) << absl::StrFormat(
          "Replace one-hot with mutually exclusive input: %s",
          node->ToString());
      XLS_RETURN_IF_ERROR(
          node->ReplaceUsesWithNew<Concat>(
                  std::vector{operand_eq_zero, node->operand(0)})
              .status());
      return true;
    }

    if (std::optional<TreeBitLocation> unknown_bit =
            query_engine.ExactlyOneBitUnknown(node->operand(0));
        unknown_bit.has_value()) {
      Node* input = node->operand(0);
      // When only one bit is unknown there are only two possible values, so
      // we can strength reduce this to a select between the two possible
      // values based on the unknown bit, which should unblock more subsequent
      // optimizations.
      // 1. Determine the unknown bit (for use as a selector).
      XLS_ASSIGN_OR_RETURN(
          Node * selector,
          node->function_base()->MakeNode<BitSlice>(
              node->loc(), input, /*start=*/unknown_bit->bit_index(),
              /*width=*/1));

      // 2. Create the literals we select among based on whether the bit is
      //    populated or not.
      const int64_t input_bit_count =
          input->GetType()->AsBitsOrDie()->bit_count();

      // Build up inputs for the case where the unknown value is true and
      // false, respectively.
      InlineBitmap input_on_true(input_bit_count);
      InlineBitmap input_on_false(input_bit_count);
      int64_t seen_unknown = 0;
      for (int64_t bitno = 0; bitno < input_bit_count; ++bitno) {
        TreeBitLocation tree_location(input, bitno);
        std::optional<bool> known_value =
            query_engine.KnownValue(tree_location);
        if (known_value.has_value()) {
          input_on_false.Set(bitno, known_value.value());
          input_on_true.Set(bitno, known_value.value());
        } else {
          seen_unknown++;
          input_on_false.Set(bitno, false);
          input_on_true.Set(bitno, true);
        }
      }
      CHECK_EQ(seen_unknown, 1)
          << "Query engine noted exactly one bit was unknown; saw unexpected "
             "number of unknown bits";

      // Wrapper lambda that invokes the right priority for the one hot op
      // based on the node metadata.
      auto do_one_hot = [&](const Bits& input) {
        OneHot* one_hot = node->As<OneHot>();
        if (one_hot->priority() == LsbOrMsb::kLsb) {
          return bits_ops::OneHotLsbToMsb(input);
        }
        return bits_ops::OneHotMsbToLsb(input);
      };

      Bits output_on_false = do_one_hot(Bits::FromBitmap(input_on_false));
      Bits output_on_true = do_one_hot(Bits::FromBitmap(input_on_true));
      VLOG(2) << absl::StrFormat(
          "input_on_false: %s input_on_true: %s output_on_false: %s "
          "output_on_true: %s",
          Bits::FromBitmap(input_on_false).ToDebugString(),
          Bits::FromBitmap(input_on_true).ToDebugString(),
          output_on_false.ToDebugString(), output_on_true.ToDebugString());
      XLS_ASSIGN_OR_RETURN(Node * on_false,
                           node->function_base()->MakeNode<Literal>(
                               node->loc(), Value(std::move(output_on_false))));
      XLS_ASSIGN_OR_RETURN(Node * on_true,
                           node->function_base()->MakeNode<Literal>(
                               node->loc(), Value(std::move(output_on_true))));

      // 3. Create the select.
      XLS_RETURN_IF_ERROR(node->ReplaceUsesWithNew<Select>(
                                  selector,
                                  std::vector<Node*>{on_false, on_true},
                                  /*default_value=*/std::nullopt)
                              .status());
      return true;
    }
  }

  return false;
}

}  // namespace

absl::StatusOr<bool> SelectSimplificationPass::RunOnFunctionBaseInternal(
    FunctionBase* func, const OptimizationPassOptions& options,
    PassResults* results) const {
  std::vector<std::unique_ptr<QueryEngine>> query_engines;
  query_engines.push_back(std::make_unique<StatelessQueryEngine>());
  query_engines.push_back(std::make_unique<TernaryQueryEngine>());

  UnionQueryEngine query_engine(std::move(query_engines));
  XLS_RETURN_IF_ERROR(query_engine.Populate(func).status());

  bool changed = false;
  for (Node* node : TopoSort(func)) {
    XLS_ASSIGN_OR_RETURN(bool node_changed,
                         SimplifyNode(node, query_engine, opt_level_));
    changed = changed || node_changed;
  }

  // Use a worklist to split OneHotSelects based on common bits in the cases
  // because this transformation creates many more OneHotSelects exposing
  // further opportunities for optimizations.
  if (SplitsEnabled(opt_level_)) {
    std::deque<OneHotSelect*> worklist;
    for (Node* node : func->nodes()) {
      if (node->Is<OneHotSelect>()) {
        worklist.push_back(node->As<OneHotSelect>());
      }
    }
    while (!worklist.empty()) {
      OneHotSelect* ohs = worklist.front();
      worklist.pop_front();
      // Note that query_engine may be stale at this point but that is
      // ok; we'll fall back on the stateless query engine.
      XLS_ASSIGN_OR_RETURN(std::vector<OneHotSelect*> new_ohses,
                           MaybeSplitOneHotSelect(ohs, query_engine));
      if (!new_ohses.empty()) {
        changed = true;
        worklist.insert(worklist.end(), new_ohses.begin(), new_ohses.end());
      }
    }
  }
  return changed;
}

REGISTER_OPT_PASS(SelectSimplificationPass, pass_config::kOptLevel);

}  // namespace xls
