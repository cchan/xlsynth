// Copyright 2024 The XLS Authors
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

#include "xls/dslx/fmt/format_disabler.h"

#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "gtest/gtest.h"
#include "absl/strings/str_cat.h"
#include "xls/common/casts.h"
#include "xls/common/status/matchers.h"
#include "xls/dslx/fmt/comments.h"
#include "xls/dslx/frontend/ast.h"
#include "xls/dslx/frontend/comment_data.h"
#include "xls/dslx/frontend/pos.h"
#include "xls/dslx/parse_and_typecheck.h"

namespace xls::dslx {
namespace {

constexpr std::string_view kFmtOn = "// dslx-fmt::on\n";

TEST(FormatDisablerTest, NotDisabled) {
  // Arrange.
  const std::string kProgram = "import bar;\n";

  std::vector<CommentData> comments_list;
  FileTable file_table;
  XLS_ASSERT_OK_AND_ASSIGN(
      std::unique_ptr<Module> m,
      ParseModule(kProgram, "fake.x", "fake", file_table, &comments_list));
  const Comments comments = Comments::Create(comments_list);

  Import* import_node = std::get<Import*>(m->top().at(0));
  ASSERT_NE(import_node, nullptr);

  // Act.
  FormatDisabler disabler(comments, kProgram);
  XLS_ASSERT_OK_AND_ASSIGN(std::optional<AstNode*> actual,
                           disabler(import_node));

  // Assert that it is nullopt, which indicates "node not modified".
  EXPECT_EQ(actual, std::nullopt);
}

TEST(FormatDisablerTest, NotDisabled_WithComments) {
  // There are comments but not enable/disable comments.
  const std::string kProgram = R"(
      // comment
      import bar;
      // another comment
  )";

  std::vector<CommentData> comments_list;
  FileTable file_table;
  XLS_ASSERT_OK_AND_ASSIGN(
      std::unique_ptr<Module> m,
      ParseModule(kProgram, "fake.x", "fake", file_table, &comments_list));
  const Comments comments = Comments::Create(comments_list);

  Import* import_node = std::get<Import*>(m->top().at(0));
  ASSERT_NE(import_node, nullptr);

  FormatDisabler disabler(comments, kProgram);
  XLS_ASSERT_OK_AND_ASSIGN(std::optional<AstNode*> actual,
                           disabler(import_node));

  EXPECT_EQ(actual, std::nullopt);
}

TEST(FormatDisablerTest, DisabledAroundImport) {
  const std::string kImport = "  import\n  bar;\n";
  const std::string kProgram =
      absl::StrCat("// dslx-fmt::off\n", kImport, kFmtOn);
  std::vector<CommentData> comments_list;
  FileTable file_table;
  XLS_ASSERT_OK_AND_ASSIGN(
      std::unique_ptr<Module> m,
      ParseModule(kProgram, "fake.x", "fake", file_table, &comments_list));
  const Comments comments = Comments::Create(comments_list);

  Import* import_node = std::get<Import*>(m->top().at(0));
  ASSERT_NE(import_node, nullptr);

  FormatDisabler disabler(comments, kProgram);
  XLS_ASSERT_OK_AND_ASSIGN(std::optional<AstNode*> actual,
                           disabler(import_node));

  ASSERT_TRUE(actual.has_value());
  VerbatimNode* actual_node = down_cast<VerbatimNode*>(*actual);
  ASSERT_NE(actual_node, nullptr);

  EXPECT_EQ(actual_node->text(), absl::StrCat(kImport, kFmtOn));
}

TEST(FormatDisablerTest, EnabledOnSameLine) {
  // Note trailing space, which we want to be part of the unformatted text.
  const std::string kImport = "  import  bar; ";
  const std::string kProgram =
      absl::StrCat("// dslx-fmt::off\n", kImport, kFmtOn);
  std::vector<CommentData> comments_list;
  FileTable file_table;
  XLS_ASSERT_OK_AND_ASSIGN(
      std::unique_ptr<Module> m,
      ParseModule(kProgram, "fake.x", "fake", file_table, &comments_list));
  const Comments comments = Comments::Create(comments_list);

  Import* import_node = std::get<Import*>(m->top().at(0));
  ASSERT_NE(import_node, nullptr);

  FormatDisabler disabler(comments, kProgram);
  XLS_ASSERT_OK_AND_ASSIGN(std::optional<AstNode*> actual,
                           disabler(import_node));

  ASSERT_TRUE(actual.has_value());
  VerbatimNode* actual_node = down_cast<VerbatimNode*>(*actual);
  ASSERT_NE(actual_node, nullptr);

  EXPECT_EQ(actual_node->text(), absl::StrCat(kImport, kFmtOn));
}

TEST(FormatDisablerTest, EnabledOnSameLineWithNewlineBetween) {
  // Note trailing space, which we want to be part of the unformatted text.
  const std::string kImport = "  import\n bar; ";
  const std::string kProgram =
      absl::StrCat("// dslx-fmt::off\n", kImport, kFmtOn);
  std::vector<CommentData> comments_list;
  FileTable file_table;
  XLS_ASSERT_OK_AND_ASSIGN(
      std::unique_ptr<Module> m,
      ParseModule(kProgram, "fake.x", "fake", file_table, &comments_list));
  const Comments comments = Comments::Create(comments_list);

  Import* import_node = std::get<Import*>(m->top().at(0));
  ASSERT_NE(import_node, nullptr);

  FormatDisabler disabler(comments, kProgram);
  XLS_ASSERT_OK_AND_ASSIGN(std::optional<AstNode*> actual,
                           disabler(import_node));

  ASSERT_TRUE(actual.has_value());
  VerbatimNode* actual_node = down_cast<VerbatimNode*>(*actual);
  ASSERT_NE(actual_node, nullptr);

  EXPECT_EQ(actual_node->text(), absl::StrCat(kImport, kFmtOn));
}

TEST(FormatDisablerTest, MultipleDisabledStatements) {
  const std::string kTwoImports = "  import\n  foo;\n  import  bar;\n";
  const std::string kProgram =
      absl::StrCat("// dslx-fmt::off\n", kTwoImports, kFmtOn);
  std::vector<CommentData> comments_list;
  FileTable file_table;
  XLS_ASSERT_OK_AND_ASSIGN(
      std::unique_ptr<Module> m,
      ParseModule(kProgram, "fake.x", "fake", file_table, &comments_list));
  const Comments comments = Comments::Create(comments_list);

  Import* first_import_node = std::get<Import*>(m->top().at(0));
  Import* second_import_node = std::get<Import*>(m->top().at(1));
  ASSERT_NE(first_import_node, nullptr);
  ASSERT_NE(second_import_node, nullptr);

  FormatDisabler disabler(comments, kProgram);
  XLS_ASSERT_OK_AND_ASSIGN(std::optional<AstNode*> first_actual,
                           disabler(first_import_node));

  ASSERT_TRUE(first_actual.has_value());
  VerbatimNode* first_verbatim_node = down_cast<VerbatimNode*>(*first_actual);
  ASSERT_NE(first_verbatim_node, nullptr);

  // Text should be the two imports concatenated.
  EXPECT_EQ(first_verbatim_node->text(), absl::StrCat(kTwoImports, kFmtOn));

  // The second node should be replaced with an empty verbatim node since it's
  // within the "disable" range.
  XLS_ASSERT_OK_AND_ASSIGN(std::optional<AstNode*> second_actual,
                           disabler(second_import_node));
  ASSERT_TRUE(second_actual.has_value());
  VerbatimNode* second_verbatim_node = down_cast<VerbatimNode*>(*second_actual);
  ASSERT_NE(second_verbatim_node, nullptr);

  EXPECT_EQ(second_verbatim_node->text(), "");
}

TEST(FormatDisablerTest, OneDisabledOneEnabledStatement) {
  const std::string kUnformattedImport = "  import\n  foo;\n";
  const std::string kProgram = absl::StrCat(
      "// dslx-fmt::off\n", kUnformattedImport, kFmtOn, "import bar;\n");
  std::vector<CommentData> comments_list;
  FileTable file_table;
  XLS_ASSERT_OK_AND_ASSIGN(
      std::unique_ptr<Module> m,
      ParseModule(kProgram, "fake.x", "fake", file_table, &comments_list));
  const Comments comments = Comments::Create(comments_list);

  Import* first_import_node = std::get<Import*>(m->top().at(0));
  Import* second_import_node = std::get<Import*>(m->top().at(1));
  ASSERT_NE(first_import_node, nullptr);
  ASSERT_NE(second_import_node, nullptr);

  FormatDisabler disabler(comments, kProgram);
  XLS_ASSERT_OK_AND_ASSIGN(std::optional<AstNode*> first_actual,
                           disabler(first_import_node));

  ASSERT_TRUE(first_actual.has_value());
  VerbatimNode* actual_node = down_cast<VerbatimNode*>(*first_actual);
  ASSERT_NE(actual_node, nullptr);

  // Text should be just the first import.
  EXPECT_EQ(actual_node->text(), absl::StrCat(kUnformattedImport, kFmtOn));

  // The second import should be left as-is since it's outside the "disable"
  // range.
  XLS_ASSERT_OK_AND_ASSIGN(std::optional<AstNode*> second_actual,
                           disabler(second_import_node));
  EXPECT_EQ(second_actual, std::nullopt);
}

TEST(FormatDisablerTest, MultipleEnabledStatements) {
  const std::string kTwoImports = "  import\n  foo;\n  import  bar;\n";
  const std::string kProgram =
      absl::StrCat("// comment 1\n", kTwoImports, "// comment 2\n");
  std::vector<CommentData> comments_list;
  FileTable file_table;
  XLS_ASSERT_OK_AND_ASSIGN(
      std::unique_ptr<Module> m,
      ParseModule(kProgram, "fake.x", "fake", file_table, &comments_list));
  const Comments comments = Comments::Create(comments_list);

  Import* first_import_node = std::get<Import*>(m->top().at(0));
  Import* second_import_node = std::get<Import*>(m->top().at(1));
  ASSERT_NE(first_import_node, nullptr);
  ASSERT_NE(second_import_node, nullptr);

  FormatDisabler disabler(comments, kProgram);
  // First node should be returned as-is, since there's no "start disable"
  // before it.
  XLS_ASSERT_OK_AND_ASSIGN(std::optional<AstNode*> first_actual,
                           disabler(first_import_node));
  EXPECT_EQ(first_actual, std::nullopt);

  // The second node should be returned as-is too.
  XLS_ASSERT_OK_AND_ASSIGN(std::optional<AstNode*> second_actual,
                           disabler(second_import_node));
  EXPECT_EQ(second_actual, std::nullopt);
}

TEST(FormatDisablerTest, EnabledOnly) {
  const std::string kProgram = R"(
  import
  bar;
// dslx-fmt::on
)";
  std::vector<CommentData> comments_list;
  FileTable file_table;
  XLS_ASSERT_OK_AND_ASSIGN(
      std::unique_ptr<Module> m,
      ParseModule(kProgram, "fake.x", "fake", file_table, &comments_list));
  const Comments comments = Comments::Create(comments_list);

  Import* import_node = std::get<Import*>(m->top().at(0));
  ASSERT_NE(import_node, nullptr);

  FormatDisabler disabler(comments, kProgram);
  XLS_ASSERT_OK_AND_ASSIGN(std::optional<AstNode*> actual,
                           disabler(import_node));

  // No change.
  EXPECT_EQ(actual, std::nullopt);
}

TEST(FormatDisablerTest, NeverEnabled) {
  const std::string kImport = "  import\n  bar;\n";
  const std::string kProgram = absl::StrCat("// dslx-fmt::off\n", kImport);
  std::vector<CommentData> comments_list;
  FileTable file_table;
  XLS_ASSERT_OK_AND_ASSIGN(
      std::unique_ptr<Module> m,
      ParseModule(kProgram, "fake.x", "fake", file_table, &comments_list));
  const Comments comments = Comments::Create(comments_list);

  Import* import_node = std::get<Import*>(m->top().at(0));
  ASSERT_NE(import_node, nullptr);

  FormatDisabler disabler(comments, kProgram);
  XLS_ASSERT_OK_AND_ASSIGN(std::optional<AstNode*> actual,
                           disabler(import_node));

  ASSERT_TRUE(actual.has_value());
  VerbatimNode* actual_node = down_cast<VerbatimNode*>(*actual);
  ASSERT_NE(actual_node, nullptr);

  EXPECT_EQ(actual_node->text(), kImport);
}

}  // namespace
}  // namespace xls::dslx
