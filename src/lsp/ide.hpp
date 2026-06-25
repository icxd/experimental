#pragma once

#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <frontend/project.hpp>

struct LspRange {
  size_t start_line = 0;
  size_t start_character = 0;
  size_t end_line = 0;
  size_t end_character = 0;
};

struct LspLocation {
  std::string uri;
  LspRange range;
};

struct DocumentSymbol {
  std::string name;
  int kind = 0;
  LspRange range;
  std::vector<DocumentSymbol> children;
};

struct WorkspaceSymbol {
  std::string name;
  std::string container_name;
  int kind = 0;
  LspLocation location;
};

struct HoverInfo {
  std::string text;
};

struct CompletionItem {
  std::string label;
  std::string detail;
  std::string insert_text;
  int kind = 3;
};

struct SignatureInfo {
  std::string label;
  std::string documentation;
  size_t active_parameter = 0;
};

struct CodeAction {
  std::string title;
  std::string kind;
  LspRange range;
  std::vector<std::pair<LspRange, std::string>> edits;
  bool is_preferred = false;
  std::optional<std::string> diagnostic_message;
  LspRange diagnostic_range;
};

struct SemanticTokens {
  std::vector<uint32_t> data;
};

struct SemanticTokensDelta {
  std::string result_id;
  std::vector<uint32_t> data{};
  size_t start = 0;
  size_t delete_count = 0;
  bool full_rebuild = true;
};

struct IdeSymbol {
  enum class Kind { Proc, Struct, Const, LocalVar, Param, StructField } kind;
  std::string module_name;
  std::string name;
  std::string struct_name;
  std::string proc_name;
  std::string file_path;
  size_t def_start = 0;
  size_t def_end = 0;
  bool renamable = true;
};

struct InlayHint {
  size_t line = 0;
  size_t character = 0;
  std::string label;
  int kind = 2;
  bool padding_left = false;
  bool padding_right = true;
};

struct IdeInlayHintOptions {
  bool parameter_names = true;
  bool variable_types = true;
};

struct DocumentHighlight {
  LspRange range;
  int kind = 1;
};

struct FoldingRange {
  size_t start_line = 0;
  size_t end_line = 0;
  int kind = 0;
};

struct CallHierarchyItem {
  std::string name;
  std::string detail;
  std::string uri;
  LspRange range;
  LspRange selection_range;
  std::string data;
};

struct CallHierarchyCall {
  LspRange range;
  CallHierarchyItem to;
};

class IdeService {
public:
  explicit IdeService(Project &project);

  std::vector<DocumentSymbol>
  document_symbols(std::string_view abs_path) const;
  std::vector<WorkspaceSymbol>
  workspace_symbols(std::string_view query) const;
  std::optional<HoverInfo> hover(std::string_view abs_path, size_t line,
                                 size_t character) const;
  std::optional<LspLocation> definition(std::string_view abs_path, size_t line,
                                        size_t character) const;
  std::optional<LspLocation> type_definition(std::string_view abs_path,
                                             size_t line, size_t character) const;
  std::vector<DocumentHighlight>
  document_highlight(std::string_view abs_path, size_t line,
                     size_t character) const;
  std::vector<CompletionItem>
  completion(std::string_view abs_path, size_t line, size_t character) const;
  std::optional<SignatureInfo>
  signature_help(std::string_view abs_path, size_t line, size_t character) const;

  std::vector<LspLocation>
  references(std::string_view abs_path, size_t line, size_t character,
             bool include_declaration) const;
  std::optional<LspRange>
  prepare_rename(std::string_view abs_path, size_t line, size_t character) const;
  std::map<std::string, std::vector<std::pair<LspRange, std::string>>>
  rename(std::string_view abs_path, size_t line, size_t character,
         std::string_view new_name) const;
  SemanticTokens semantic_tokens(std::string_view abs_path) const;
  SemanticTokens semantic_tokens_range(std::string_view abs_path,
                                       const LspRange &range) const;
  SemanticTokensDelta
  semantic_tokens_delta(std::string_view abs_path, std::string_view previous_id,
                        const std::vector<uint32_t> &previous_data) const;
  std::vector<CodeAction>
  code_actions(std::string_view abs_path, const LspRange &range) const;
  std::vector<InlayHint>
  inlay_hints(std::string_view abs_path,
              const IdeInlayHintOptions &options = {},
              const std::optional<LspRange> &range = std::nullopt) const;
  std::vector<FoldingRange> folding_ranges(std::string_view abs_path) const;
  std::optional<std::string> format_document(std::string_view abs_path) const;

  std::optional<CallHierarchyItem>
  prepare_call_hierarchy(std::string_view abs_path, size_t line,
                         size_t character) const;
  std::vector<CallHierarchyCall>
  call_hierarchy_incoming(std::string_view abs_path,
                          const CallHierarchyItem &item) const;
  std::vector<CallHierarchyCall>
  call_hierarchy_outgoing(std::string_view abs_path,
                          const CallHierarchyItem &item) const;

  static bool is_valid_rename_name(std::string_view name);

private:
  Project &_project;
};

std::string lsp_path_to_uri(std::string_view path);
std::string lsp_uri_to_path(std::string_view uri);
LspRange lsp_range_from_offsets(std::string_view source, size_t start,
                                size_t end);
std::string apply_lsp_range_edit(std::string_view source, const LspRange &range,
                                 std::string_view new_text);
