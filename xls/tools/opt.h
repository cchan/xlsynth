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

// Library that backs the `opt_main` tool's primary functionality.

#ifndef XLS_TOOLS_OPT_H_
#define XLS_TOOLS_OPT_H_

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "absl/status/status.h"
#include "absl/status/statusor.h"

// TODO(meheff): 2021-10-04 Remove this header.
#include "absl/types/span.h"
#include "xls/ir/package.h"
#include "xls/passes/optimization_pass.h"

namespace xls::tools {

// Various optimizer options (that generally funnel from the `opt_main` tool to
// this consolidated library).
struct OptOptions {
  int64_t opt_level = xls::kMaxOptLevel;
  std::string_view top;
  std::string ir_dump_path = "";
  std::optional<std::string> ir_path = std::nullopt;
  std::vector<std::string> skip_passes;
  std::optional<int64_t> convert_array_index_to_select = std::nullopt;
  std::optional<int64_t> split_next_value_selects = std::nullopt;
  bool inline_procs;
  std::vector<RamRewrite> ram_rewrites = {};
  bool use_context_narrowing_analysis;
  std::optional<std::string> pass_list;
  std::optional<int64_t> bisect_limit;
};

// Helper used in the opt_main tool, optimizes the given IR for a particular
// top-level entity (e.g., function, proc, etc) at the given opt level and
// modifies the package in place.
absl::Status OptimizeIrForTop(Package* package, const OptOptions& options);

// Helper used in the opt_main tool, optimizes the given IR for a particular
// top-level entity (e.g., function, proc, etc) at the given opt level and
// returns the resulting optimized IR.
absl::StatusOr<std::string> OptimizeIrForTop(std::string_view ir,
                                             const OptOptions& options);

// Convenience wrapper around the above that builds an OptOptions appropriately.
// Analogous to calling opt_main with each argument being a flag.
absl::StatusOr<std::string> OptimizeIrForTop(
    std::string_view input_path, int64_t opt_level, std::string_view top,
    std::string_view ir_dump_path, absl::Span<const std::string> skip_passes,
    int64_t convert_array_index_to_select, int64_t split_next_value_selects,
    bool inline_procs, std::string_view ram_rewrites_pb,
    bool use_context_narrowing_analysis, std::optional<std::string> pass_list,
    std::optional<int64_t> bisect_limit);

}  // namespace xls::tools

#endif  // XLS_TOOLS_OPT_H_
