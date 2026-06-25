#include <lsp/server.hpp>

#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <optional>
#include <stdexcept>
#include <string>
#include <tuple>
#include <algorithm>
#include <cstdlib>

#include <frontend/project.hpp>
#include <lsp/format.hpp>
#include <lsp/ide.hpp>
#include <lsp/json.hpp>
#include <rye_paths.hpp>

namespace {

bool lsp_debug_enabled() {
  const char *value = std::getenv("RYE_LSP_DEBUG");
  return value != nullptr && value[0] != '\0' && value[0] != '0';
}

using Json = lsp::json::Value;
using Object = lsp::json::Object;
using Array = lsp::json::Array;

struct OpenDocument {
  std::string uri;
  std::string path;
  std::string text;
  int version = 0;
};

class LspTrafficLog {
public:
  void open() {
    namespace fs = std::filesystem;
    std::error_code ec;
    fs::create_directories(".rye", ec);
    _out.open(".rye/lsp-traffic.log", std::ios::trunc);
    if (!_out.is_open())
      return;
    _out << "rye lsp traffic log\n";
    _out << "cwd: " << fs::current_path().string() << "\n\n";
    _out.flush();
  }

  bool is_open() const { return _out.is_open(); }

  void log(std::string_view direction, std::string_view label,
           std::string_view body) {
    if (!_out.is_open())
      return;
    _out << "=== " << direction << " [" << label << "] (" << body.size()
         << " bytes) ===\n";
    _out << body << "\n";
    for (size_t i = 0; i < body.size(); i++) {
      unsigned char ch = static_cast<unsigned char>(body[i]);
      if (ch < 0x20 && ch != '\t') {
        _out << "!!! invalid JSON control char U+"
             << std::format("{:04X}", static_cast<unsigned>(ch)) << " at byte "
             << i << "\n";
      }
    }
    _out << "\n";
    _out.flush();
  }

private:
  std::ofstream _out;
};

class LspServer {
public:
  explicit LspServer(std::optional<std::string> argv0) :
      _argv0(std::move(argv0)) {}

  void run();

private:
  std::optional<std::string> read_message();
  void write_message(const Json &message);

  void handle_message(const Json &message);
  void handle_request(int id, std::string_view method, const Json &params);
  void handle_notification(std::string_view method, const Json &params);

  void rebuild_project();
  std::vector<std::string> roots() const;
  void ensure_import_paths();
  void apply_settings(const Json &settings);

  void publish_diagnostics(const std::string &uri);
  void publish_all_diagnostics();

  Json to_range(const LspRange &range) const;
  Json to_position(size_t line, size_t character) const;
  Json to_location(const LspLocation &location) const;

  std::optional<std::string>
  parse_text_document_uri(const Json &params) const;
  std::optional<std::tuple<size_t, size_t>>
  parse_position(const Json &params) const;
  std::optional<LspRange> parse_range(const Json &obj) const;

  std::string outgoing_label(const Json &message) const;
  void log_outgoing_json(std::string_view body, const Json &message) const;

  mutable LspTrafficLog _traffic_log;
  Project _project;
  std::map<std::string, OpenDocument> _documents;
  std::vector<std::string> _import_paths;
  bool _initialized = false;
  IdeInlayHintOptions _inlay_hint_options{};
  std::map<std::string, std::vector<uint32_t>> _semantic_token_cache{};
  std::map<std::string, std::string> _semantic_token_ids{};
  uint64_t _semantic_token_serial = 0;
  bool _lsp_debug = false;
  std::optional<std::string> _root_uri;
  std::optional<std::string> _workspace_root;
  std::optional<std::string> _argv0;
};

std::optional<std::string> LspServer::read_message() {
  std::string line;
  if (!std::getline(std::cin, line))
    return std::nullopt;
  if (!line.starts_with("Content-Length: "))
    return std::nullopt;

  size_t length = std::stoul(line.substr(16));
  std::string blank;
  std::getline(std::cin, blank);

  std::string body(length, '\0');
  std::cin.read(body.data(), static_cast<std::streamsize>(length));
  return body;
}

std::string LspServer::outgoing_label(const Json &message) const {
  if (!message.is_object())
    return "response";
  const auto &obj = message.as_object();
  if (auto it = obj.find("method"); it != obj.end() && it->second.is_string())
    return it->second.as_string();
  if (auto id = obj.find("id"); id != obj.end())
    return std::format("response id={}", static_cast<int>(id->second.as_number()));
  return "response";
}

void LspServer::log_outgoing_json(std::string_view body,
                                  const Json &message) const {
  if (!_lsp_debug)
    return;
  _traffic_log.log("OUT", outgoing_label(message), body);
}

void LspServer::write_message(const Json &message) {
  std::string body = message.stringify();
  log_outgoing_json(body, message);
  std::cout << "Content-Length: " << body.size() << "\r\n\r\n" << body
            << std::flush;
}

Json LspServer::to_position(size_t line, size_t character) const {
  return Json(Object{{"line", static_cast<int>(line)},
                     {"character", static_cast<int>(character)}});
}

Json LspServer::to_range(const LspRange &range) const {
  return Json(Object{{"start", to_position(range.start_line, range.start_character)},
                     {"end", to_position(range.end_line, range.end_character)}});
}

Json LspServer::to_location(const LspLocation &location) const {
  return Json(Object{{"uri", location.uri}, {"range", to_range(location.range)}});
}

std::optional<std::string>
LspServer::parse_text_document_uri(const Json &params) const {
  if (!params.is_object())
    return std::nullopt;
  const auto &obj = params.as_object();
  auto td = obj.find("textDocument");
  if (td == obj.end() || !td->second.is_object())
    return std::nullopt;
  const auto &tdoc = td->second.as_object();
  auto u = tdoc.find("uri");
  if (u == tdoc.end() || !u->second.is_string())
    return std::nullopt;
  return u->second.as_string();
}

std::optional<std::tuple<size_t, size_t>>
LspServer::parse_position(const Json &params) const {
  if (!params.is_object())
    return std::nullopt;
  const auto &obj = params.as_object();
  auto pos = obj.find("position");
  if (pos == obj.end() || !pos->second.is_object())
    return std::nullopt;
  const auto &p = pos->second.as_object();
  size_t line = 0;
  size_t character = 0;
  if (auto l = p.find("line"); l != p.end() && l->second.is_number())
    line = static_cast<size_t>(l->second.as_number());
  if (auto c = p.find("character"); c != p.end() && c->second.is_number())
    character = static_cast<size_t>(c->second.as_number());
  return std::make_tuple(line, character);
}

std::optional<LspRange> LspServer::parse_range(const Json &obj) const {
  if (!obj.is_object())
    return std::nullopt;
  const auto &range = obj.as_object();
  auto start_it = range.find("start");
  auto end_it = range.find("end");
  if (start_it == range.end() || end_it == range.end() ||
      !start_it->second.is_object() || !end_it->second.is_object())
    return std::nullopt;
  const auto &start = start_it->second.as_object();
  const auto &end = end_it->second.as_object();
  LspRange result{};
  if (auto l = start.find("line"); l != start.end() && l->second.is_number())
    result.start_line = static_cast<size_t>(l->second.as_number());
  if (auto c = start.find("character"); c != start.end() && c->second.is_number())
    result.start_character = static_cast<size_t>(c->second.as_number());
  if (auto l = end.find("line"); l != end.end() && l->second.is_number())
    result.end_line = static_cast<size_t>(l->second.as_number());
  if (auto c = end.find("character"); c != end.end() && c->second.is_number())
    result.end_character = static_cast<size_t>(c->second.as_number());
  return result;
}

std::vector<std::string> LspServer::roots() const {
  std::optional<std::string> dev_root;
  if (_workspace_root.has_value()) {
    namespace fs = std::filesystem;
    fs::path root(*_workspace_root);
    std::error_code ec;
    if (fs::exists(root / "std" / "string.rye", ec))
      dev_root = _workspace_root;
  }

  std::vector<std::string> result =
      rye_builtin_modules(_argv0, dev_root);
  for (const auto &[uri, doc]: _documents)
    result.push_back(doc.path);
  return result;
}

void LspServer::ensure_import_paths() {
  if (_import_paths.empty())
    _import_paths.push_back(std::filesystem::current_path().string());
  if (auto root = rye_install_root(_argv0)) {
    std::string root_str = root.value();
    if (std::find(_import_paths.begin(), _import_paths.end(), root_str) ==
        _import_paths.end())
      _import_paths.push_back(root_str);
  }
  if (!_workspace_root.has_value())
    return;
  if (std::find(_import_paths.begin(), _import_paths.end(),
                *_workspace_root) == _import_paths.end())
    _import_paths.push_back(*_workspace_root);
}

void LspServer::rebuild_project() {
  ensure_import_paths();
  for (const auto &[uri, doc]: _documents)
    _project.set_document_text(doc.path, doc.text);
  _project.set_import_paths(_import_paths);
  _project.rebuild(roots());
}

void LspServer::apply_settings(const Json &settings) {
  const Json *rye_settings = &settings;
  if (settings.is_object()) {
    const auto &obj = settings.as_object();
    if (auto rye = obj.find("rye"); rye != obj.end())
      rye_settings = &rye->second;
    else if (auto nested = obj.find("settings"); nested != obj.end())
      rye_settings = &nested->second;
  }

  if (!rye_settings->is_object())
    return;

  const auto &obj = rye_settings->as_object();
  if (auto paths = obj.find("importPaths");
      paths != obj.end() && paths->second.is_array()) {
    _import_paths.clear();
    for (const auto &path: paths->second.as_array()) {
      if (path.is_string())
        _import_paths.push_back(path.as_string());
    }
  }

  if (auto root = obj.find("workspaceRoot");
      root != obj.end() && root->second.is_string())
    _workspace_root = project_abs_path(root->second.as_string());

  if (auto hints = obj.find("inlayHints");
      hints != obj.end() && hints->second.is_object()) {
    const auto &hint_obj = hints->second.as_object();
    if (auto v = hint_obj.find("parameterNames");
        v != hint_obj.end() && v->second.is_bool())
      _inlay_hint_options.parameter_names = v->second.as_bool();
    if (auto v = hint_obj.find("variableTypes");
        v != hint_obj.end() && v->second.is_bool())
      _inlay_hint_options.variable_types = v->second.as_bool();
  }

  ensure_import_paths();
  _project.set_import_paths(_import_paths);
  if (!_documents.empty()) {
    rebuild_project();
    publish_all_diagnostics();
  }
}

void LspServer::publish_diagnostics(const std::string &uri) {
  if (!_documents.contains(uri))
    return;
  const OpenDocument &doc = _documents.at(uri);
  std::string rel = project_rel_path(doc.path);

  Array items{};
  for (const auto &diag: _project.diagnostics()) {
    if (diag.file_path.empty())
      continue;
    if (diag.file_path != rel && diag.file_path != doc.path)
      continue;

    const ParsedModule *module = _project.find_module(doc.path);
    std::string_view source = module != nullptr ? module->source : doc.text;
    LspRange range = lsp_range_from_offsets(source, diag.start, diag.end);
    Object diag_obj{
        {"range", to_range(range)},
        {"severity", 1},
        {"message", diag.message},
        {"source", "rye"},
    };

    Array related{};
    if (!diag.hint.empty()) {
      related.push_back(Json(Object{
          {"location", Json(Object{{"uri", uri}, {"range", to_range(range)}})},
          {"message", diag.hint},
      }));
    }
    for (const auto &help: diag.helps) {
      LspRange help_range = lsp_range_from_offsets(source, help.start, help.end);
      related.push_back(Json(Object{
          {"location",
           Json(Object{{"uri", uri}, {"range", to_range(help_range)}})},
          {"message", help.message},
      }));
    }
    if (!related.empty())
      diag_obj["relatedInformation"] = related;

    items.push_back(Json(diag_obj));
  }

  write_message(Json(Object{
      {"jsonrpc", "2.0"},
      {"method", "textDocument/publishDiagnostics"},
      {"params", Json(Object{{"uri", uri}, {"diagnostics", items}})},
  }));
}

void LspServer::publish_all_diagnostics() {
  for (const auto &[uri, doc]: _documents)
    publish_diagnostics(uri);
}

void LspServer::handle_request(int id, std::string_view method,
                               const Json &params) {
  if (method == "initialize") {
    if (params.is_object()) {
      const auto &obj = params.as_object();
      if (auto it = obj.find("rootUri"); it != obj.end() && it->second.is_string()) {
        _root_uri = it->second.as_string();
        _workspace_root = project_abs_path(lsp_uri_to_path(*_root_uri));
      }

      if (auto init = obj.find("initializationOptions");
          init != obj.end() && init->second.is_object()) {
        apply_settings(init->second);
      }
    }

    ensure_import_paths();

    Object result{
        {"capabilities",
         Object{
             {"textDocumentSync",
              Object{{"openClose", true}, {"change", static_cast<int>(2)}}},
             {"hoverProvider", true},
             {"definitionProvider", true},
             {"typeDefinitionProvider", true},
             {"documentHighlightProvider", true},
             {"documentSymbolProvider", true},
             {"workspaceSymbolProvider", true},
             {"referencesProvider", true},
             {"renameProvider", Object{{"prepareProvider", true}}},
             {"codeActionProvider",
              Object{{"codeActionKinds", Array{"quickfix"}}}},
             {"foldingRangeProvider", true},
             {"documentFormattingProvider", true},
             {"callHierarchyProvider", true},
             {"semanticTokensProvider",
              Object{{"legend",
                      Object{
                          {"tokenTypes",
                           Array{"type", "struct", "parameter", "variable",
                                 "property", "function"}},
                          {"tokenModifiers",
                           Array{"declaration", "definition", "readonly"}},
                      }},
                      {"full", Object{{"delta", Json(nullptr)}}},
                      {"range", true}}},
             {"completionProvider",
              Object{{"triggerCharacters", Array{".", ":"}}}},
             {"signatureHelpProvider",
              Object{{"triggerCharacters", Array{"(", ","}}}},
             {"inlayHintProvider", true},
         }},
    };

    write_message(Json(Object{{"jsonrpc", "2.0"},
                              {"id", id},
                              {"result", Json(std::move(result))}}));
    return;
  }

  if (method == "shutdown") {
    write_message(
        Json(Object{{"jsonrpc", "2.0"}, {"id", id}, {"result", nullptr}}));
    return;
  }

  if (method == "textDocument/documentSymbol") {
    std::string uri;
    if (params.is_object()) {
      const auto &obj = params.as_object();
      if (auto it = obj.find("textDocument"); it != obj.end() && it->second.is_object()) {
        const auto &td = it->second.as_object();
        if (auto u = td.find("uri"); u != td.end() && u->second.is_string())
          uri = u->second.as_string();
      }
    }

    Array symbols{};
    if (_documents.contains(uri)) {
      IdeService ide(_project);
      for (const auto &symbol:
           ide.document_symbols(_documents.at(uri).path)) {
        Array children{};
        for (const auto &child: symbol.children) {
          children.push_back(Json(Object{
              {"name", child.name},
              {"kind", child.kind},
              {"range", to_range(child.range)},
              {"selectionRange", to_range(child.range)},
          }));
        }
        symbols.push_back(Json(Object{
            {"name", symbol.name},
            {"kind", symbol.kind},
            {"range", to_range(symbol.range)},
            {"selectionRange", to_range(symbol.range)},
            {"children", children},
        }));
      }
    }

    write_message(Json(Object{{"jsonrpc", "2.0"},
                              {"id", id},
                              {"result", symbols}}));
    return;
  }

  if (method == "textDocument/hover") {
    std::string uri;
    size_t line = 0;
    size_t character = 0;
    if (params.is_object()) {
      const auto &obj = params.as_object();
      if (auto td = obj.find("textDocument"); td != obj.end() && td->second.is_object()) {
        const auto &tdoc = td->second.as_object();
        if (auto u = tdoc.find("uri"); u != tdoc.end() && u->second.is_string())
          uri = u->second.as_string();
      }
      if (auto pos = obj.find("position"); pos != obj.end() && pos->second.is_object()) {
        const auto &p = pos->second.as_object();
        if (auto l = p.find("line"); l != p.end() && l->second.is_number())
          line = static_cast<size_t>(l->second.as_number());
        if (auto c = p.find("character"); c != p.end() && c->second.is_number())
          character = static_cast<size_t>(c->second.as_number());
      }
    }

    Json result = nullptr;
    if (_documents.contains(uri)) {
      IdeService ide(_project);
      if (auto hover = ide.hover(_documents.at(uri).path, line, character)) {
        result = Json(Object{
            {"contents",
             Object{{"kind", "markdown"}, {"value", "```rye\n" + hover->text + "\n```"}}},
        });
      }
    }

    write_message(Json(Object{{"jsonrpc", "2.0"}, {"id", id}, {"result", result}}));
    return;
  }

  if (method == "textDocument/definition") {
    std::string uri;
    size_t line = 0;
    size_t character = 0;
    if (params.is_object()) {
      const auto &obj = params.as_object();
      if (auto td = obj.find("textDocument"); td != obj.end() && td->second.is_object()) {
        const auto &tdoc = td->second.as_object();
        if (auto u = tdoc.find("uri"); u != tdoc.end() && u->second.is_string())
          uri = u->second.as_string();
      }
      if (auto pos = obj.find("position"); pos != obj.end() && pos->second.is_object()) {
        const auto &p = pos->second.as_object();
        if (auto l = p.find("line"); l != p.end() && l->second.is_number())
          line = static_cast<size_t>(l->second.as_number());
        if (auto c = p.find("character"); c != p.end() && c->second.is_number())
          character = static_cast<size_t>(c->second.as_number());
      }
    }

    Json result = nullptr;
    if (_documents.contains(uri)) {
      IdeService ide(_project);
      if (auto location = ide.definition(_documents.at(uri).path, line, character))
        result = to_location(*location);
    }

    write_message(Json(Object{{"jsonrpc", "2.0"}, {"id", id}, {"result", result}}));
    return;
  }

  if (method == "textDocument/typeDefinition") {
    Json result = nullptr;
    auto uri = parse_text_document_uri(params);
    auto pos = parse_position(params);
    if (uri.has_value() && pos.has_value() && _documents.contains(*uri)) {
      IdeService ide(_project);
      auto [line, character] = *pos;
      if (auto location =
              ide.type_definition(_documents.at(*uri).path, line, character))
        result = to_location(*location);
    }
    write_message(Json(Object{{"jsonrpc", "2.0"}, {"id", id}, {"result", result}}));
    return;
  }

  if (method == "textDocument/documentHighlight") {
    Array highlights{};
    auto uri = parse_text_document_uri(params);
    auto pos = parse_position(params);
    if (uri.has_value() && pos.has_value() && _documents.contains(*uri)) {
      IdeService ide(_project);
      auto [line, character] = *pos;
      for (const auto &hl:
           ide.document_highlight(_documents.at(*uri).path, line, character)) {
        highlights.push_back(
            Json(Object{{"range", to_range(hl.range)}, {"kind", hl.kind}}));
      }
    }
    write_message(Json(Object{{"jsonrpc", "2.0"},
                              {"id", id},
                              {"result", highlights}}));
    return;
  }

  if (method == "workspace/symbol") {
    std::string query;
    if (params.is_object()) {
      const auto &obj = params.as_object();
      if (auto q = obj.find("query"); q != obj.end() && q->second.is_string())
        query = q->second.as_string();
    }
    Array symbols{};
    IdeService ide(_project);
    for (const auto &sym: ide.workspace_symbols(query)) {
      symbols.push_back(Json(Object{
          {"name", sym.name},
          {"containerName", sym.container_name},
          {"kind", sym.kind},
          {"location", to_location(sym.location)},
      }));
    }
    write_message(Json(Object{{"jsonrpc", "2.0"},
                              {"id", id},
                              {"result", symbols}}));
    return;
  }

  if (method == "textDocument/foldingRange") {
    Array ranges{};
    auto uri = parse_text_document_uri(params);
    if (uri.has_value() && _documents.contains(*uri)) {
      IdeService ide(_project);
      for (const auto &fold:
           ide.folding_ranges(_documents.at(*uri).path)) {
        ranges.push_back(Json(Object{
            {"startLine", static_cast<int>(fold.start_line)},
            {"endLine", static_cast<int>(fold.end_line)},
            {"kind", fold.kind == 0 ? "region" : "region"},
        }));
      }
    }
    write_message(Json(Object{{"jsonrpc", "2.0"},
                              {"id", id},
                              {"result", ranges}}));
    return;
  }

  if (method == "textDocument/formatting") {
    Array edits{};
    auto uri = parse_text_document_uri(params);
    if (uri.has_value() && _documents.contains(*uri)) {
      const OpenDocument &doc = _documents.at(*uri);
      IdeService ide(_project);
      if (auto formatted = ide.format_document(doc.path)) {
        auto [end_line, end_col] =
            offset_to_line_column(doc.text, doc.text.size());
        edits.push_back(Json(Object{
            {"range",
             to_range(LspRange{0, 0, end_line, end_col})},
            {"newText", *formatted},
        }));
        write_message(Json(Object{{"jsonrpc", "2.0"},
                                  {"id", id},
                                  {"result", edits}}));
        return;
      }
      write_message(Json(Object{{"jsonrpc", "2.0"},
                                {"id", id},
                                {"error",
                                 Object{{"code", -32000},
                                        {"message",
                                         "Cannot format: source has parse errors"}}}}));
      return;
    }
    write_message(Json(Object{{"jsonrpc", "2.0"},
                              {"id", id},
                              {"result", edits}}));
    return;
  }

  if (method == "textDocument/prepareCallHierarchy") {
    Json result = nullptr;
    auto uri = parse_text_document_uri(params);
    auto pos = parse_position(params);
    if (uri.has_value() && pos.has_value() && _documents.contains(*uri)) {
      IdeService ide(_project);
      auto [line, character] = *pos;
      if (auto item = ide.prepare_call_hierarchy(_documents.at(*uri).path, line,
                                                 character)) {
        result = Json(Object{
            {"name", item->name},
            {"kind", static_cast<int>(6)},
            {"detail", item->detail},
            {"uri", item->uri},
            {"range", to_range(item->range)},
            {"selectionRange", to_range(item->selection_range)},
            {"data", item->data},
        });
      }
    }
    write_message(Json(Object{{"jsonrpc", "2.0"}, {"id", id}, {"result", result}}));
    return;
  }

  if (method == "callHierarchy/incomingCalls") {
    Array calls{};
    if (params.is_object()) {
      const auto &obj = params.as_object();
      auto item_it = obj.find("item");
      if (item_it != obj.end() && item_it->second.is_object()) {
        const auto &item_obj = item_it->second.as_object();
        CallHierarchyItem item{};
        if (auto d = item_obj.find("data"); d != item_obj.end() && d->second.is_string())
          item.data = d->second.as_string();
        if (auto u = item_obj.find("uri"); u != item_obj.end() && u->second.is_string())
          item.uri = u->second.as_string();
        IdeService ide(_project);
        for (const auto &call:
             ide.call_hierarchy_incoming(lsp_uri_to_path(item.uri), item)) {
          calls.push_back(Json(Object{
              {"from",
               Object{
                   {"name", call.to.name},
                   {"kind", static_cast<int>(6)},
                   {"detail", call.to.detail},
                   {"uri", call.to.uri},
                   {"range", to_range(call.to.range)},
                   {"selectionRange", to_range(call.to.selection_range)},
                   {"data", call.to.data},
               }},
              {"fromRanges", Array{to_range(call.range)}},
          }));
        }
      }
    }
    write_message(Json(Object{{"jsonrpc", "2.0"},
                              {"id", id},
                              {"result", calls}}));
    return;
  }

  if (method == "callHierarchy/outgoingCalls") {
    Array calls{};
    if (params.is_object()) {
      const auto &obj = params.as_object();
      auto item_it = obj.find("item");
      if (item_it != obj.end() && item_it->second.is_object()) {
        const auto &item_obj = item_it->second.as_object();
        CallHierarchyItem item{};
        if (auto d = item_obj.find("data"); d != item_obj.end() && d->second.is_string())
          item.data = d->second.as_string();
        if (auto u = item_obj.find("uri"); u != item_obj.end() && u->second.is_string())
          item.uri = u->second.as_string();
        IdeService ide(_project);
        for (const auto &call:
             ide.call_hierarchy_outgoing(lsp_uri_to_path(item.uri), item)) {
          calls.push_back(Json(Object{
              {"to",
               Object{
                   {"name", call.to.name},
                   {"kind", static_cast<int>(6)},
                   {"detail", call.to.detail},
                   {"uri", call.to.uri},
                   {"range", to_range(call.to.range)},
                   {"selectionRange", to_range(call.to.selection_range)},
                   {"data", call.to.data},
               }},
              {"fromRanges", Array{to_range(call.range)}},
          }));
        }
      }
    }
    write_message(Json(Object{{"jsonrpc", "2.0"},
                              {"id", id},
                              {"result", calls}}));
    return;
  }

  if (method == "textDocument/completion") {
    std::string uri;
    size_t line = 0;
    size_t character = 0;
    if (params.is_object()) {
      const auto &obj = params.as_object();
      if (auto td = obj.find("textDocument"); td != obj.end() && td->second.is_object()) {
        const auto &tdoc = td->second.as_object();
        if (auto u = tdoc.find("uri"); u != tdoc.end() && u->second.is_string())
          uri = u->second.as_string();
      }
      if (auto pos = obj.find("position"); pos != obj.end() && pos->second.is_object()) {
        const auto &p = pos->second.as_object();
        if (auto l = p.find("line"); l != p.end() && l->second.is_number())
          line = static_cast<size_t>(l->second.as_number());
        if (auto c = p.find("character"); c != p.end() && c->second.is_number())
          character = static_cast<size_t>(c->second.as_number());
      }
    }

    Array items{};
    if (_documents.contains(uri)) {
      IdeService ide(_project);
      for (const auto &item:
           ide.completion(_documents.at(uri).path, line, character)) {
        items.push_back(Json(Object{
            {"label", item.label},
            {"kind", item.kind},
            {"detail", item.detail},
            {"insertText", item.insert_text},
        }));
      }
    }

    write_message(Json(Object{{"jsonrpc", "2.0"},
                              {"id", id},
                              {"result", Json(Object{{"isIncomplete", false},
                                                     {"items", items}})}}));
    return;
  }

  if (method == "textDocument/signatureHelp") {
    std::string uri;
    size_t line = 0;
    size_t character = 0;
    if (params.is_object()) {
      const auto &obj = params.as_object();
      if (auto td = obj.find("textDocument"); td != obj.end() && td->second.is_object()) {
        const auto &tdoc = td->second.as_object();
        if (auto u = tdoc.find("uri"); u != tdoc.end() && u->second.is_string())
          uri = u->second.as_string();
      }
      if (auto pos = obj.find("position"); pos != obj.end() && pos->second.is_object()) {
        const auto &p = pos->second.as_object();
        if (auto l = p.find("line"); l != p.end() && l->second.is_number())
          line = static_cast<size_t>(l->second.as_number());
        if (auto c = p.find("character"); c != p.end() && c->second.is_number())
          character = static_cast<size_t>(c->second.as_number());
      }
    }

    Json result = nullptr;
    if (_documents.contains(uri)) {
      IdeService ide(_project);
      if (auto sig = ide.signature_help(_documents.at(uri).path, line, character)) {
        result = Json(Object{
            {"signatures",
             Array{Json(Object{
                 {"label", sig->label},
                 {"documentation", sig->documentation},
             })}},
            {"activeSignature", 0},
            {"activeParameter", static_cast<int>(sig->active_parameter)},
        });
      }
    }

    write_message(Json(Object{{"jsonrpc", "2.0"}, {"id", id}, {"result", result}}));
    return;
  }

  if (method == "textDocument/references") {
    bool include_declaration = true;
    if (params.is_object()) {
      const auto &obj = params.as_object();
      if (auto ctx = obj.find("context"); ctx != obj.end() && ctx->second.is_object()) {
        const auto &context = ctx->second.as_object();
        if (auto inc = context.find("includeDeclaration");
            inc != context.end() && inc->second.is_bool())
          include_declaration = inc->second.as_bool();
      }
    }

    Array locations{};
    auto uri = parse_text_document_uri(params);
    auto pos = parse_position(params);
    if (uri.has_value() && pos.has_value() && _documents.contains(*uri)) {
      IdeService ide(_project);
      auto [line, character] = *pos;
      for (const auto &loc:
           ide.references(_documents.at(*uri).path, line, character,
                          include_declaration))
        locations.push_back(to_location(loc));
    }

    write_message(Json(Object{{"jsonrpc", "2.0"},
                              {"id", id},
                              {"result", locations}}));
    return;
  }

  if (method == "textDocument/prepareRename") {
    Json result = nullptr;
    auto uri = parse_text_document_uri(params);
    auto pos = parse_position(params);
    if (uri.has_value() && pos.has_value() && _documents.contains(*uri)) {
      IdeService ide(_project);
      auto [line, character] = *pos;
      if (auto range = ide.prepare_rename(_documents.at(*uri).path, line,
                                          character))
        result = to_range(*range);
    }

    write_message(Json(Object{{"jsonrpc", "2.0"}, {"id", id}, {"result", result}}));
    return;
  }

  if (method == "textDocument/rename") {
    std::string new_name;
    if (params.is_object()) {
      const auto &obj = params.as_object();
      if (auto name = obj.find("newName"); name != obj.end() && name->second.is_string())
        new_name = name->second.as_string();
    }

    Object changes{};
    auto uri = parse_text_document_uri(params);
    auto pos = parse_position(params);
    if (uri.has_value() && pos.has_value() && _documents.contains(*uri) &&
        !new_name.empty()) {
      if (!IdeService::is_valid_rename_name(new_name)) {
        write_message(Json(Object{{"jsonrpc", "2.0"},
                                  {"id", id},
                                  {"error",
                                   Object{{"code", -32602},
                                          {"message",
                                           "Invalid identifier for rename"}}}}));
        return;
      }
      IdeService ide(_project);
      auto [line, character] = *pos;
      auto rename_edits =
          ide.rename(_documents.at(*uri).path, line, character, new_name);
      if (rename_edits.empty()) {
        write_message(Json(Object{{"jsonrpc", "2.0"},
                                  {"id", id},
                                  {"error",
                                   Object{{"code", -32000},
                                          {"message",
                                           "No symbol at position to rename"}}}}));
        return;
      }
      for (const auto &[edit_uri, edits]: rename_edits) {
        Array text_edits{};
        for (const auto &[range, text]: edits) {
          text_edits.push_back(
              Json(Object{{"range", to_range(range)}, {"newText", text}}));
        }
        changes.emplace(edit_uri, Json(text_edits));
      }
    }

    write_message(Json(Object{{"jsonrpc", "2.0"},
                              {"id", id},
                              {"result", Json(Object{{"changes", changes}})}}));
    return;
  }

  if (method == "textDocument/semanticTokens/full" ||
      method == "textDocument/semanticTokens/full/delta" ||
      method == "textDocument/semanticTokens/range") {
    auto uri = parse_text_document_uri(params);
    std::string previous_id;
    std::optional<LspRange> token_range;
    if (params.is_object()) {
      const auto &obj = params.as_object();
      if (auto rid = obj.find("previousResultId");
          rid != obj.end() && rid->second.is_string())
        previous_id = rid->second.as_string();
      if (method == "textDocument/semanticTokens/range") {
        if (auto r = obj.find("range"); r != obj.end())
          token_range = parse_range(r->second);
      }
    }

    if (uri.has_value() && _documents.contains(*uri)) {
      const std::string &path = _documents.at(*uri).path;
      IdeService ide(_project);

      if (method == "textDocument/semanticTokens/range") {
        if (!token_range.has_value()) {
          write_message(Json(Object{{"jsonrpc", "2.0"},
                                    {"id", id},
                                    {"result", Object{{"data", Array{}}}}}));
          return;
        }
        SemanticTokens tokens =
            ide.semantic_tokens_range(path, *token_range);
        Array data{};
        for (uint32_t value: tokens.data)
          data.push_back(Json(static_cast<double>(value)));
        write_message(Json(Object{{"jsonrpc", "2.0"},
                                  {"id", id},
                                  {"result", Object{{"data", data}}}}));
        return;
      }

      std::vector<uint32_t> previous =
          _semantic_token_cache.contains(*uri)
              ? _semantic_token_cache.at(*uri)
              : std::vector<uint32_t>{};

      if (method.ends_with("/delta") && !previous_id.empty()) {
        auto delta = ide.semantic_tokens_delta(path, previous_id, previous);
        _semantic_token_cache[*uri] = delta.data;
        _semantic_token_ids[*uri] = delta.result_id;
        if (delta.data.empty() && !delta.full_rebuild) {
          write_message(Json(Object{{"jsonrpc", "2.0"},
                                    {"id", id},
                                    {"result",
                                     Object{{"resultId", delta.result_id}}}}));
          return;
        }
        Array delta_data{};
        for (uint32_t v: delta.data)
          delta_data.push_back(Json(static_cast<double>(v)));
        write_message(Json(
            Object{{"jsonrpc", "2.0"},
                   {"id", id},
                   {"result",
                    Object{{"resultId", delta.result_id},
                           {"edits",
                            Array{Object{
                                {"start", static_cast<int>(delta.start)},
                                {"deleteCount",
                                 static_cast<int>(delta.delete_count)},
                                {"data", delta_data},
                            }}}}}}));
        return;
      }

      SemanticTokens tokens = ide.semantic_tokens(path);
      _semantic_token_cache[*uri] = tokens.data;
      std::string result_id = std::to_string(++_semantic_token_serial);
      _semantic_token_ids[*uri] = result_id;
      Array data{};
      for (uint32_t value: tokens.data)
        data.push_back(Json(static_cast<double>(value)));
      write_message(Json(Object{{"jsonrpc", "2.0"},
                                {"id", id},
                                {"result",
                                 Object{{"resultId", result_id}, {"data", data}}}}));
      return;
    }

    write_message(Json(Object{{"jsonrpc", "2.0"},
                              {"id", id},
                              {"result", Json(Object{{"data", Array{}}})}}));
    return;
  }

  if (method == "textDocument/codeAction") {
    Array actions{};
    auto uri = parse_text_document_uri(params);
    std::optional<LspRange> range;
    if (params.is_object()) {
      const auto &obj = params.as_object();
      if (auto r = obj.find("range"); r != obj.end())
        range = parse_range(r->second);
    }

    if (uri.has_value() && range.has_value() && _documents.contains(*uri)) {
      IdeService ide(_project);
      for (const auto &action:
           ide.code_actions(_documents.at(*uri).path, *range)) {
        Array edits{};
        for (const auto &[edit_range, text]: action.edits) {
          edits.push_back(
              Json(Object{{"range", to_range(edit_range)}, {"newText", text}}));
        }
        Object change{};
        change.emplace(*uri, edits);

        Array diagnostics{};
        if (action.diagnostic_message.has_value()) {
          diagnostics.push_back(Json(Object{
              {"range", to_range(action.diagnostic_range)},
              {"severity", 1},
              {"message", *action.diagnostic_message},
              {"source", "rye"},
          }));
        }

        Object action_obj{
            {"title", action.title},
            {"kind", action.kind},
            {"isPreferred", action.is_preferred},
            {"edit", Json(Object{{"changes", change}})},
        };
        if (!diagnostics.empty())
          action_obj["diagnostics"] = diagnostics;
        actions.push_back(Json(action_obj));
      }
    }

    write_message(Json(Object{{"jsonrpc", "2.0"},
                              {"id", id},
                              {"result", actions}}));
    return;
  }

  if (method == "textDocument/inlayHint") {
    Array hints{};
    auto uri = parse_text_document_uri(params);
  std::optional<LspRange> range{};
  if (params.is_object()) {
    const auto &obj = params.as_object();
    if (auto r = obj.find("range"); r != obj.end())
      range = parse_range(r->second);
  }
    if (uri.has_value() && _documents.contains(*uri)) {
      IdeService ide(_project);
      for (const auto &hint:
           ide.inlay_hints(_documents.at(*uri).path, _inlay_hint_options, range)) {
        hints.push_back(Json(Object{
            {"position", to_position(hint.line, hint.character)},
            {"label", hint.label},
            {"kind", hint.kind},
            {"paddingLeft", hint.padding_left},
            {"paddingRight", hint.padding_right},
        }));
      }
    }

    write_message(Json(Object{{"jsonrpc", "2.0"},
                              {"id", id},
                              {"result", hints}}));
    return;
  }

  write_message(Json(Object{{"jsonrpc", "2.0"},
                            {"id", id},
                            {"error",
                             Object{{"code", -32601},
                                    {"message", "Method not found"}}}}));
}

void LspServer::handle_notification(std::string_view method,
                                    const Json &params) {
  if (method == "initialized") {
    _initialized = true;
    return;
  }

  if (method == "exit")
    std::exit(0);

  if (method == "workspace/didChangeConfiguration") {
    if (params.is_object()) {
      const auto &obj = params.as_object();
      if (auto settings = obj.find("settings"); settings != obj.end())
        apply_settings(settings->second);
      else
        apply_settings(params);
    }
    return;
  }

  if (method == "textDocument/didOpen") {
    if (!params.is_object())
      return;
    const auto &obj = params.as_object();
    auto td = obj.find("textDocument");
    if (td == obj.end() || !td->second.is_object())
      return;
    const auto &tdoc = td->second.as_object();

    OpenDocument doc{};
    if (auto uri = tdoc.find("uri"); uri != tdoc.end() && uri->second.is_string())
      doc.uri = uri->second.as_string();
    if (auto text = tdoc.find("text"); text != tdoc.end() && text->second.is_string())
      doc.text = text->second.as_string();
    if (auto ver = tdoc.find("version"); ver != tdoc.end() && ver->second.is_number())
      doc.version = static_cast<int>(ver->second.as_number());
    doc.path = lsp_uri_to_path(doc.uri);

    std::string saved_uri = doc.uri;
    _documents[saved_uri] = std::move(doc);
    rebuild_project();
    publish_all_diagnostics();
    return;
  }

  if (method == "textDocument/didChange") {
    if (!params.is_object())
      return;
    const auto &obj = params.as_object();
    auto td = obj.find("textDocument");
    if (td == obj.end() || !td->second.is_object())
      return;
    const auto &tdoc = td->second.as_object();
    std::string uri;
    if (auto u = tdoc.find("uri"); u != tdoc.end() && u->second.is_string())
      uri = u->second.as_string();
    if (!_documents.contains(uri))
      return;

    if (auto changes = obj.find("contentChanges");
        changes != obj.end() && changes->second.is_array()) {
      for (const auto &change: changes->second.as_array()) {
        if (!change.is_object())
          continue;
        const auto &change_obj = change.as_object();
        if (auto text = change_obj.find("text");
            text != change_obj.end() && text->second.is_string()) {
          if (auto range_it = change_obj.find("range");
              range_it != change_obj.end()) {
            if (auto range = parse_range(range_it->second)) {
              _documents[uri].text =
                  apply_lsp_range_edit(_documents[uri].text, *range,
                                       text->second.as_string());
            } else {
              _documents[uri].text = text->second.as_string();
            }
          } else {
            _documents[uri].text = text->second.as_string();
          }
        }
      }
    }

    rebuild_project();
    _semantic_token_cache.erase(uri);
    _semantic_token_ids.erase(uri);
    publish_all_diagnostics();
    return;
  }

  if (method == "textDocument/didClose") {
    if (!params.is_object())
      return;
    const auto &obj = params.as_object();
    auto td = obj.find("textDocument");
    if (td == obj.end() || !td->second.is_object())
      return;
    const auto &tdoc = td->second.as_object();
    if (auto uri = tdoc.find("uri"); uri != tdoc.end() && uri->second.is_string()) {
      std::string doc_uri = uri->second.as_string();
      if (_documents.contains(doc_uri)) {
        _project.remove_document(_documents.at(doc_uri).path);
        _documents.erase(doc_uri);
        _semantic_token_cache.erase(doc_uri);
        _semantic_token_ids.erase(doc_uri);
        rebuild_project();
        publish_diagnostics(doc_uri);
      }
    }
    return;
  }
}

void LspServer::handle_message(const Json &message) {
  try {
    if (!message.is_object())
      return;
    const auto &obj = message.as_object();

    auto method_it = obj.find("method");
    if (method_it == obj.end() || !method_it->second.is_string())
      return;
    std::string_view method = method_it->second.as_string();

    const Json *params = nullptr;
    if (auto it = obj.find("params"); it != obj.end())
      params = &it->second;
    Json empty_params = nullptr;
    if (params == nullptr)
      params = &empty_params;

    if (auto id_it = obj.find("id"); id_it != obj.end()) {
      int id = static_cast<int>(id_it->second.as_number());
      handle_request(id, method, *params);
      return;
    }

    handle_notification(method, *params);
  } catch (const std::exception &e) {
    std::cerr << "rye lsp: unhandled exception: " << e.what() << '\n';
  }
}

void LspServer::run() {
  _lsp_debug = lsp_debug_enabled();
  if (_lsp_debug)
    _traffic_log.open();

  while (true) {
    auto body = read_message();
    if (!body.has_value())
      break;
    if (_lsp_debug)
      _traffic_log.log("IN", "message", *body);
    auto parsed = lsp::json::parse(body.value());
    if (!parsed.has_value())
      continue;
    handle_message(parsed.value());
  }
}

} // namespace

int run_lsp_server(int argc, char **argv) {
  std::optional<std::string> argv0;
  if (argc > 0)
    argv0 = argv[0];
  LspServer server(std::move(argv0));
  server.run();
  return 0;
}
