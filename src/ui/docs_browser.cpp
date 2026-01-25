#include "ui/docs_browser.h"

#include <imgui.h>

#include <algorithm>
#include <cctype>
#include <cstddef>
#include <filesystem>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

#include "nebula4x/util/file_io.h"
#include "nebula4x/util/strings.h"

#include "ui/ui_state.h"

namespace nebula4x::ui {

namespace {

struct DocEntry {
  std::string title;
  std::string ref;           // normalized reference (lowercase, forward slashes)
  std::string display_path;  // human readable relative path
  std::string abs_path;
  bool from_data{false};
};

struct Heading {
  int level{1};
  std::string text;
  std::string anchor;
  int line_index{0};
};

struct SearchHit {
  int doc_index{-1};
  int line_index{-1};
  std::string snippet;
};

struct MdLink {
  std::string text;
  std::string target;
};

struct DocsBrowserState {
  bool initialized{false};

  // Discovered docs.
  std::vector<DocEntry> docs;
  std::unordered_map<std::string, int> doc_by_ref;
  int selected_doc{-1};

  // Current document.
  std::string current_ref;
  std::string current_title;
  std::string current_abs_path;
  std::vector<std::string> lines;
  std::vector<Heading> headings;
  std::unordered_map<std::string, int> anchor_to_line;

  // UI controls.
  char list_filter[128]{};
  bool wrap_text{true};
  bool show_toc{true};
  bool show_raw{false};
  bool show_line_numbers{false};

  // In-document find.
  char find_query[128]{};
  std::string last_find_query;
  std::vector<int> find_matches;
  int find_cursor{0};

  // Cross-doc search.
  char global_query[128]{};
  std::string last_global_query;
  std::vector<SearchHit> global_hits;
  std::string global_status;

  // Navigation.
  std::vector<std::string> back_stack;
  std::vector<std::string> forward_stack;
  std::optional<int> request_scroll_line;
  std::optional<std::string> request_scroll_anchor;

  // Status.
  std::string status;
  std::string error;
};

DocsBrowserState& st() {
  static DocsBrowserState s;
  return s;
}

static inline bool is_space(char c) {
  return c == ' ' || c == '\t' || c == '\n' || c == '\r';
}

std::string trim_copy(std::string_view in) {
  std::size_t a = 0;
  std::size_t b = in.size();
  while (a < b && is_space(in[a])) ++a;
  while (b > a && is_space(in[b - 1])) --b;
  return std::string(in.substr(a, b - a));
}

std::string strip_trailing_cr(std::string s) {
  if (!s.empty() && s.back() == '\r') s.pop_back();
  return s;
}

std::string normalize_ref(std::string_view path) {
  std::string p = trim_copy(path);

  // Remove leading ./
  while (p.rfind("./", 0) == 0) p = p.substr(2);

  // Normalize separators.
  for (char& c : p) {
    if (c == '\\') c = '/';
  }

  // Strip leading slash.
  while (!p.empty() && p.front() == '/') p.erase(p.begin());

  // Strip common prefixes.
  const auto lower = nebula4x::to_lower(p);
  if (lower.rfind("data/docs/", 0) == 0) p = p.substr(std::string("data/docs/").size());
  if (lower.rfind("docs/", 0) == 0) p = p.substr(std::string("docs/").size());

  // Lowercase for lookup.
  p = nebula4x::to_lower(p);
  return p;
}

std::string make_anchor(std::string_view heading_text) {
  std::string s = nebula4x::to_lower(trim_copy(heading_text));
  std::string out;
  out.reserve(s.size());

  bool prev_dash = false;
  for (char c : s) {
    const unsigned char uc = static_cast<unsigned char>(c);
    if (std::isalnum(uc)) {
      out.push_back(static_cast<char>(c));
      prev_dash = false;
    } else if (c == '-' || c == '_' || std::isspace(uc)) {
      if (!out.empty() && !prev_dash) {
        out.push_back('-');
        prev_dash = true;
      }
    } else {
      // drop punctuation
    }
  }

  while (!out.empty() && out.back() == '-') out.pop_back();
  return out;
}

bool contains_ci(std::string_view hay, std::string_view needle) {
  if (needle.empty()) return true;
  std::string h = nebula4x::to_lower(std::string(hay));
  std::string n = nebula4x::to_lower(std::string(needle));
  return h.find(n) != std::string::npos;
}

std::vector<std::string> split_lines(const std::string& text) {
  std::vector<std::string> out;
  out.reserve(256);

  std::size_t start = 0;
  while (start <= text.size()) {
    const std::size_t end = text.find('\n', start);
    if (end == std::string::npos) {
      out.push_back(strip_trailing_cr(text.substr(start)));
      break;
    }
    out.push_back(strip_trailing_cr(text.substr(start, end - start)));
    start = end + 1;
  }
  return out;
}

std::string extract_title_from_markdown(const std::string& contents, std::string_view fallback) {
  // First non-empty markdown heading.
  const auto lines = split_lines(contents);
  for (const auto& ln : lines) {
    std::string_view s = ln;
    std::size_t i = 0;
    while (i < s.size() && s[i] == '#') ++i;
    if (i == 0) continue;
    if (i < s.size() && s[i] == ' ') {
      const auto t = trim_copy(s.substr(i + 1));
      if (!t.empty()) return t;
    }
  }
  return std::string(fallback);
}

void add_doc(DocsBrowserState& s, const DocEntry& e) {
  if (e.ref.empty()) return;
  if (s.doc_by_ref.find(e.ref) != s.doc_by_ref.end()) return;

  const int idx = static_cast<int>(s.docs.size());
  s.docs.push_back(e);
  s.doc_by_ref[e.ref] = idx;
}

void scan_dir_for_docs(DocsBrowserState& s, const std::filesystem::path& base, bool from_data) {
  if (!std::filesystem::exists(base)) return;
  if (!std::filesystem::is_directory(base)) return;

  try {
    for (const auto& it : std::filesystem::recursive_directory_iterator(base)) {
      if (!it.is_regular_file()) continue;
      const auto p = it.path();
      const auto ext = nebula4x::to_lower(p.extension().string());
      if (ext != ".md" && ext != ".markdown") continue;

      const auto rel = std::filesystem::relative(p, base).generic_string();
      const auto ref = normalize_ref(rel);
      if (ref.empty()) continue;

      std::string title;
      try {
        const std::string contents = nebula4x::read_text_file(p.string());
        title = extract_title_from_markdown(contents, p.stem().string());
      } catch (...) {
        title = p.stem().string();
      }

      DocEntry e;
      e.title = title;
      e.ref = ref;
      e.display_path = rel;
      e.abs_path = p.string();
      e.from_data = from_data;
      add_doc(s, e);
    }
  } catch (...) {
    // Ignore scan failures (permissions, etc.)
  }
}

std::string current_dir_of_ref(const std::string& ref) {
  const auto pos = ref.find_last_of('/');
  if (pos == std::string::npos) return std::string();
  return ref.substr(0, pos);
}

std::optional<std::string> resolve_doc_ref(const DocsBrowserState& s,
                                           const std::string& current_ref,
                                           std::string_view raw_path) {
  std::string p = trim_copy(raw_path);
  if (p.empty()) return std::nullopt;

  // External scheme? (very small check)
  if (p.find("://") != std::string::npos) return std::nullopt;

  // Strip leading '#': anchor-only link.
  if (!p.empty() && p[0] == '#') return current_ref;

  // Trim leading ./
  while (p.rfind("./", 0) == 0) p = p.substr(2);

  // Normalize separators.
  for (char& c : p) {
    if (c == '\\') c = '/';
  }

  // Candidate paths:
  std::vector<std::string> cand;
  cand.reserve(6);

  std::string p0 = p;
  while (!p0.empty() && p0.front() == '/') p0.erase(p0.begin());

  // Relative to current doc dir.
  const std::string cur_dir = current_dir_of_ref(current_ref);
  if (!cur_dir.empty()) {
    cand.push_back(cur_dir + "/" + p0);
  }
  // As-is (root-relative).
  cand.push_back(p0);

  // If no extension, try adding .md.
  if (p0.find('.') == std::string::npos) {
    if (!cur_dir.empty()) cand.push_back(cur_dir + "/" + p0 + ".md");
    cand.push_back(p0 + ".md");
  }

  for (const auto& c : cand) {
    const std::string ref = normalize_ref(c);
    if (ref.empty()) continue;
    if (s.doc_by_ref.find(ref) != s.doc_by_ref.end()) return ref;
  }

  // Fallback: filename-only match.
  const std::filesystem::path fp(p0);
  const std::string fname = nebula4x::to_lower(fp.filename().generic_string());
  if (!fname.empty()) {
    for (const auto& [ref, idx] : s.doc_by_ref) {
      const std::filesystem::path rp(ref);
      if (nebula4x::to_lower(rp.filename().generic_string()) == fname) return ref;
    }
  }

  return std::nullopt;
}

// Extract Markdown links in the form [text](target) and returns the line with the
// markup replaced by just `text`.
std::string strip_md_links(std::string_view line, std::vector<MdLink>* links_out) {
  std::string out;
  out.reserve(line.size());

  std::size_t i = 0;
  while (i < line.size()) {
    const std::size_t lb = line.find('[', i);
    if (lb == std::string::npos) {
      out.append(line.substr(i));
      break;
    }

    out.append(line.substr(i, lb - i));

    const std::size_t rb = line.find(']', lb + 1);
    if (rb == std::string::npos) {
      out.push_back('[');
      i = lb + 1;
      continue;
    }

    if (rb + 1 >= line.size() || line[rb + 1] != '(') {
      // Not a markdown link.
      out.append(line.substr(lb, rb - lb + 1));
      i = rb + 1;
      continue;
    }

    const std::size_t rp = line.find(')', rb + 2);
    if (rp == std::string::npos) {
      out.append(line.substr(lb, rb - lb + 1));
      i = rb + 1;
      continue;
    }

    const std::string text = std::string(line.substr(lb + 1, rb - (lb + 1)));
    const std::string target = std::string(line.substr(rb + 2, rp - (rb + 2)));
    if (!text.empty() && !target.empty() && links_out) {
      links_out->push_back(MdLink{text, target});
    }
    out.append(text);
    i = rp + 1;
  }

  return out;
}

void parse_headings(DocsBrowserState& s) {
  s.headings.clear();
  s.anchor_to_line.clear();
  s.headings.reserve(64);

  for (int i = 0; i < static_cast<int>(s.lines.size()); ++i) {
    std::string_view ln = s.lines[static_cast<std::size_t>(i)];
    int level = 0;
    std::size_t pos = 0;
    while (pos < ln.size() && ln[pos] == '#') {
      ++level;
      ++pos;
    }
    if (level <= 0 || level > 6) continue;
    if (pos >= ln.size() || ln[pos] != ' ') continue;

    const std::string text = trim_copy(ln.substr(pos + 1));
    if (text.empty()) continue;

    const std::string anchor = make_anchor(text);
    Heading h;
    h.level = level;
    h.text = text;
    h.anchor = anchor;
    h.line_index = i;
    s.headings.push_back(std::move(h));

    if (!anchor.empty() && s.anchor_to_line.find(anchor) == s.anchor_to_line.end()) {
      s.anchor_to_line[anchor] = i;
    }
  }
}

void recompute_find_matches(DocsBrowserState& s) {
  const std::string q = trim_copy(s.find_query);
  if (q == s.last_find_query) return;
  s.last_find_query = q;
  s.find_matches.clear();
  s.find_cursor = 0;

  if (q.empty()) return;

  for (int i = 0; i < static_cast<int>(s.lines.size()); ++i) {
    if (contains_ci(s.lines[static_cast<std::size_t>(i)], q)) s.find_matches.push_back(i);
  }
}

void open_doc_by_ref(DocsBrowserState& s,
                     const std::string& ref_raw,
                     bool push_history,
                     const std::optional<std::string>& anchor = std::nullopt,
                     const std::optional<int>& scroll_line = std::nullopt) {
  const std::string ref = normalize_ref(ref_raw);
  const auto it = s.doc_by_ref.find(ref);
  if (it == s.doc_by_ref.end()) {
    s.status = "Doc not found: " + ref_raw;
    return;
  }

  const int idx = it->second;
  if (idx < 0 || idx >= static_cast<int>(s.docs.size())) return;

  if (push_history && !s.current_ref.empty() && s.current_ref != ref) {
    s.back_stack.push_back(s.current_ref);
    s.forward_stack.clear();
  }

  s.selected_doc = idx;
  s.current_ref = ref;
  s.current_title = s.docs[static_cast<std::size_t>(idx)].title;
  s.current_abs_path = s.docs[static_cast<std::size_t>(idx)].abs_path;
  s.error.clear();

  try {
    const std::string contents = nebula4x::read_text_file(s.current_abs_path);
    s.lines = split_lines(contents);
  } catch (const std::exception& e) {
    s.lines.clear();
    s.error = e.what();
  }

  parse_headings(s);
  s.last_find_query.clear();
  recompute_find_matches(s);

  // Default scroll to top when opening a new doc.
  s.request_scroll_line = 0;
  s.request_scroll_anchor.reset();

  if (scroll_line) s.request_scroll_line = *scroll_line;
  if (anchor && !anchor->empty()) {
    const std::string a = make_anchor(*anchor);
    if (auto it2 = s.anchor_to_line.find(a); it2 != s.anchor_to_line.end()) {
      s.request_scroll_line = it2->second;
      s.request_scroll_anchor = a;
    } else {
      s.status = "Anchor not found: #" + *anchor;
    }
  }
}

void ensure_initialized(DocsBrowserState& s) {
  if (s.initialized) return;
  s.initialized = true;

  // Prefer docs shipped with the build.
  scan_dir_for_docs(s, std::filesystem::path("data") / "docs", /*from_data=*/true);
  // When running from the repo, allow browsing the repo docs too.
  scan_dir_for_docs(s, std::filesystem::path("docs"), /*from_data=*/false);

  // Also include top-level README / patch notes when present (dev builds).
  const std::vector<std::string> extra = {"README.md", "PATCH_NOTES.md", "PATCH_PACK_NOTES.md"};
  for (const auto& p : extra) {
    try {
      const std::filesystem::path fp(p);
      if (!std::filesystem::exists(fp) || !std::filesystem::is_regular_file(fp)) continue;
      const std::string contents = nebula4x::read_text_file(fp.string());
      DocEntry e;
      e.title = extract_title_from_markdown(contents, fp.stem().string());
      e.ref = normalize_ref(fp.filename().generic_string());
      e.display_path = fp.filename().generic_string();
      e.abs_path = fp.string();
      e.from_data = false;
      add_doc(s, e);
    } catch (...) {
      // ignore
    }
  }

  // Sort: data docs first, then alpha by title.
  std::stable_sort(s.docs.begin(), s.docs.end(), [](const DocEntry& a, const DocEntry& b) {
    if (a.from_data != b.from_data) return a.from_data > b.from_data;
    if (a.title != b.title) return a.title < b.title;
    return a.display_path < b.display_path;
  });

  // Rebuild index map after sort.
  s.doc_by_ref.clear();
  for (int i = 0; i < static_cast<int>(s.docs.size()); ++i) {
    s.doc_by_ref[s.docs[static_cast<std::size_t>(i)].ref] = i;
  }

  // Default doc.
  if (!s.docs.empty()) {
    // Prefer an index page if present.
    if (auto it = s.doc_by_ref.find("index.md"); it != s.doc_by_ref.end()) {
      open_doc_by_ref(s, "index.md", /*push_history=*/false);
    } else {
      open_doc_by_ref(s, s.docs.front().ref, /*push_history=*/false);
    }
  }
}

void run_global_search(DocsBrowserState& s) {
  const std::string q = trim_copy(s.global_query);
  if (q.empty()) {
    s.global_hits.clear();
    s.global_status.clear();
    s.last_global_query.clear();
    return;
  }

  if (q == s.last_global_query) return;
  s.last_global_query = q;

  s.global_hits.clear();
  s.global_hits.reserve(64);

  int scanned_docs = 0;
  const int kHitLimit = 250;
  for (int di = 0; di < static_cast<int>(s.docs.size()); ++di) {
    const auto& d = s.docs[static_cast<std::size_t>(di)];
    scanned_docs++;
    std::string contents;
    try {
      contents = nebula4x::read_text_file(d.abs_path);
    } catch (...) {
      continue;
    }
    const auto lines = split_lines(contents);
    for (int li = 0; li < static_cast<int>(lines.size()); ++li) {
      if (!contains_ci(lines[static_cast<std::size_t>(li)], q)) continue;

      SearchHit h;
      h.doc_index = di;
      h.line_index = li;
      h.snippet = trim_copy(lines[static_cast<std::size_t>(li)]);
      if (h.snippet.size() > 160) h.snippet = h.snippet.substr(0, 157) + "...";
      s.global_hits.push_back(std::move(h));

      if (static_cast<int>(s.global_hits.size()) >= kHitLimit) break;
    }
    if (static_cast<int>(s.global_hits.size()) >= kHitLimit) break;
  }

  s.global_status = "Matches: " + std::to_string(s.global_hits.size()) +
                    "  | Docs scanned: " + std::to_string(scanned_docs);
  if (static_cast<int>(s.global_hits.size()) >= kHitLimit) s.global_status += "  (hit limit reached)";
}

// Renders a markdown-ish line and emits extracted links into `links_out`.
// The caller handles link click resolution.
void render_markdown_line(DocsBrowserState& s,
                          const std::string& raw_line,
                          int line_index,
                          bool& in_code_block,
                          std::vector<MdLink>& links_out) {
  links_out.clear();

  const std::string stripped = strip_trailing_cr(raw_line);
  const std::string line = strip_md_links(stripped, &links_out);
  std::string_view sv = line;

  // Code fences.
  if (sv.rfind("```", 0) == 0) {
    in_code_block = !in_code_block;
    ImGui::Separator();
    return;
  }

  // Optional scroll request.
  if (s.request_scroll_line && *s.request_scroll_line == line_index) {
    ImGui::SetScrollHereY(0.20f);
    s.request_scroll_line.reset();
  }

  if (s.show_raw) {
    if (s.show_line_numbers) {
      ImGui::TextDisabled("%4d", line_index + 1);
      ImGui::SameLine();
    }
    ImGui::TextUnformatted(stripped.c_str());
    return;
  }

  // Blank line.
  if (trim_copy(sv).empty()) {
    ImGui::Spacing();
    return;
  }

  if (in_code_block) {
    if (s.show_line_numbers) {
      ImGui::TextDisabled("%4d", line_index + 1);
      ImGui::SameLine();
    }
    ImGui::Indent();
    ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
    ImGui::TextUnformatted(sv.data(), sv.data() + sv.size());
    ImGui::PopStyleColor();
    ImGui::Unindent();
    return;
  }

  // Headings.
  {
    int level = 0;
    std::size_t pos = 0;
    while (pos < sv.size() && sv[pos] == '#') {
      ++level;
      ++pos;
    }
    if (level > 0 && level <= 6 && pos < sv.size() && sv[pos] == ' ') {
      const std::string text = trim_copy(sv.substr(pos + 1));
      if (!text.empty()) {
        if (level <= 2) {
          ImGui::SeparatorText(text.c_str());
        } else {
          ImGui::TextUnformatted(text.c_str());
        }
        return;
      }
    }
  }

  // Horizontal rule.
  if (sv == "---" || sv == "***") {
    ImGui::Separator();
    return;
  }

  // Block quotes.
  if (sv.rfind("> ", 0) == 0 || sv.rfind(">", 0) == 0) {
    std::string_view q = sv;
    if (q.rfind("> ", 0) == 0) q = q.substr(2);
    else if (q.rfind(">", 0) == 0) q = q.substr(1);

    if (s.show_line_numbers) {
      ImGui::TextDisabled("%4d", line_index + 1);
      ImGui::SameLine();
    }
    ImGui::Indent();
    ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
    ImGui::TextWrapped("%s", trim_copy(q).c_str());
    ImGui::PopStyleColor();
    ImGui::Unindent();
    return;
  }

  // Bullets.
  if (sv.rfind("- ", 0) == 0 || sv.rfind("* ", 0) == 0) {
    std::string_view b = sv.substr(2);
    if (s.show_line_numbers) {
      ImGui::TextDisabled("%4d", line_index + 1);
      ImGui::SameLine();
    }
    ImGui::Bullet();
    ImGui::SameLine();
    ImGui::TextWrapped("%s", trim_copy(b).c_str());
    return;
  }

  // Numbered list: "1. foo".
  {
    std::size_t j = 0;
    while (j < sv.size() && std::isdigit(static_cast<unsigned char>(sv[j]))) ++j;
    if (j > 0 && j + 1 < sv.size() && sv[j] == '.' && sv[j + 1] == ' ') {
      const std::string num = std::string(sv.substr(0, j + 1));
      const std::string rest = trim_copy(sv.substr(j + 2));
      if (s.show_line_numbers) {
        ImGui::TextDisabled("%4d", line_index + 1);
        ImGui::SameLine();
      }
      ImGui::TextDisabled("%s", num.c_str());
      ImGui::SameLine();
      ImGui::TextWrapped("%s", rest.c_str());
      return;
    }
  }

  // Normal paragraph.
  if (s.show_line_numbers) {
    ImGui::TextDisabled("%4d", line_index + 1);
    ImGui::SameLine();
  }
  ImGui::TextWrapped("%s", sv.data());
}

// Renders the doc contents and returns a clicked link target (if any).
std::optional<std::string> render_doc_view(DocsBrowserState& s) {
  std::optional<std::string> clicked_link;

  // Find bar.
  ImGui::SeparatorText("Find");
  ImGui::SetNextItemWidth(280.0f);
  ImGui::InputText("In document", s.find_query, sizeof(s.find_query));
  recompute_find_matches(s);

  if (!s.find_matches.empty()) {
    ImGui::SameLine();
    ImGui::TextDisabled("%d matches", static_cast<int>(s.find_matches.size()));

    ImGui::SameLine();
    if (ImGui::SmallButton("Prev")) {
      if (s.find_cursor <= 0) s.find_cursor = static_cast<int>(s.find_matches.size()) - 1;
      else s.find_cursor--;
      s.request_scroll_line = s.find_matches[static_cast<std::size_t>(s.find_cursor)];
    }
    ImGui::SameLine();
    if (ImGui::SmallButton("Next")) {
      s.find_cursor++;
      if (s.find_cursor >= static_cast<int>(s.find_matches.size())) s.find_cursor = 0;
      s.request_scroll_line = s.find_matches[static_cast<std::size_t>(s.find_cursor)];
    }
    ImGui::SameLine();
    ImGui::TextDisabled("%d/%d", s.find_cursor + 1, static_cast<int>(s.find_matches.size()));
  } else if (!trim_copy(s.find_query).empty()) {
    ImGui::SameLine();
    ImGui::TextDisabled("no matches");
  }

  // Doc contents.
  ImGui::SeparatorText("Content");
  ImGuiWindowFlags child_flags = ImGuiWindowFlags_HorizontalScrollbar;
  if (ImGui::BeginChild("##doc_view", ImVec2(0, 0), true, child_flags)) {
    if (!s.wrap_text) {
      // When not wrapping, a clipper is stable and keeps large docs responsive.
      ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(4, 2));
      bool in_code = false;
      std::vector<MdLink> links;
      links.reserve(4);

      ImGuiListClipper clipper;
      clipper.Begin(static_cast<int>(s.lines.size()));
      while (clipper.Step()) {
        for (int i = clipper.DisplayStart; i < clipper.DisplayEnd; ++i) {
          render_markdown_line(s, s.lines[static_cast<std::size_t>(i)], i, in_code, links);

          // Render extracted links as tiny inline controls.
          if (!links.empty()) {
            ImGui::Indent();
            for (int li = 0; li < static_cast<int>(links.size()); ++li) {
              ImGui::PushID((i << 8) ^ li);
              const auto& l = links[static_cast<std::size_t>(li)];
              std::string btn = (l.text.empty() ? "Link" : l.text);
              if (btn.size() > 36) btn = btn.substr(0, 33) + "...";
              if (ImGui::SmallButton(btn.c_str())) clicked_link = l.target;
              if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", l.target.c_str());
              ImGui::SameLine();
              ImGui::PopID();
            }
            ImGui::NewLine();
            ImGui::Unindent();
          }
        }
      }
      ImGui::PopStyleVar();
    } else {
      // Wrapped mode: render everything (docs shipped with the build are small).
      ImGui::PushTextWrapPos(0.0f);
      ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(4, 3));
      bool in_code = false;
      std::vector<MdLink> links;
      links.reserve(4);

      for (int i = 0; i < static_cast<int>(s.lines.size()); ++i) {
        render_markdown_line(s, s.lines[static_cast<std::size_t>(i)], i, in_code, links);
        if (!links.empty()) {
          ImGui::Indent();
          for (int li = 0; li < static_cast<int>(links.size()); ++li) {
            ImGui::PushID((i << 8) ^ li);
            const auto& l = links[static_cast<std::size_t>(li)];
            std::string btn = (l.text.empty() ? "Link" : l.text);
            if (btn.size() > 48) btn = btn.substr(0, 45) + "...";
            if (ImGui::SmallButton(btn.c_str())) clicked_link = l.target;
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", l.target.c_str());
            ImGui::SameLine();
            ImGui::PopID();
          }
          ImGui::NewLine();
          ImGui::Unindent();
        }
      }

      ImGui::PopStyleVar();
      ImGui::PopTextWrapPos();
    }
  }
  ImGui::EndChild();

  return clicked_link;
}

void handle_clicked_link(DocsBrowserState& s, const std::string& target) {
  const std::string t = trim_copy(target);
  if (t.empty()) return;

  // External.
  if (t.find("://") != std::string::npos) {
    ImGui::SetClipboardText(t.c_str());
    s.status = "Copied link to clipboard";
    return;
  }

  // Split path#anchor.
  const std::size_t h = t.find('#');
  const std::string path = (h == std::string::npos) ? t : t.substr(0, h);
  const std::string anchor = (h == std::string::npos) ? std::string() : t.substr(h + 1);

  std::string path_part = path;
  if (path_part.empty()) path_part = s.current_ref;

  if (auto ref = resolve_doc_ref(s, s.current_ref, path_part); ref) {
    open_doc_by_ref(s, *ref, /*push_history=*/true, anchor.empty() ? std::nullopt : std::optional<std::string>(anchor));
  } else {
    // If we can't resolve it, at least copy it.
    ImGui::SetClipboardText(t.c_str());
    s.status = "Unknown doc link; copied to clipboard";
  }
}

}  // namespace

void draw_docs_browser_panel(UIState& ui) {
  auto& s = st();
  ensure_initialized(s);

  // External open request (e.g. from guided tours or other UI surfaces).
  if (!ui.request_open_doc_ref.empty()) {
    const std::string ref = normalize_ref(ui.request_open_doc_ref);
    ui.request_open_doc_ref.clear();
    if (!ref.empty()) {
      open_doc_by_ref(s, ref, /*push_history=*/true);
    }
  }


  if (s.docs.empty()) {
    ImGui::TextDisabled("No docs found. Expected: data/docs/*.md");
    return;
  }

  // Toolbar.
  if (ImGui::Button("Back")) {
    if (!s.back_stack.empty()) {
      s.forward_stack.push_back(s.current_ref);
      const std::string ref = s.back_stack.back();
      s.back_stack.pop_back();
      open_doc_by_ref(s, ref, /*push_history=*/false);
    }
  }
  ImGui::SameLine();
  if (ImGui::Button("Forward")) {
    if (!s.forward_stack.empty()) {
      s.back_stack.push_back(s.current_ref);
      const std::string ref = s.forward_stack.back();
      s.forward_stack.pop_back();
      open_doc_by_ref(s, ref, /*push_history=*/false);
    }
  }
  ImGui::SameLine();
  if (ImGui::Button("Reload")) {
    if (!s.current_ref.empty()) open_doc_by_ref(s, s.current_ref, /*push_history=*/false);
  }

  ImGui::SameLine();
  ImGui::Checkbox("Wrap", &s.wrap_text);
  ImGui::SameLine();
  ImGui::Checkbox("TOC", &s.show_toc);
  ImGui::SameLine();
  ImGui::Checkbox("Raw", &s.show_raw);
  ImGui::SameLine();
  ImGui::Checkbox("Line #", &s.show_line_numbers);

  if (!s.current_title.empty()) {
    ImGui::SameLine();
    ImGui::TextDisabled("%s", s.current_title.c_str());
  }

  if (!s.status.empty()) {
    ImGui::SameLine();
    ImGui::TextDisabled("| %s", s.status.c_str());
  }

  ImGui::Separator();

  // Split into list/toc vs document view.
  const ImGuiTableFlags tf = ImGuiTableFlags_Resizable | ImGuiTableFlags_SizingStretchProp;
  if (ImGui::BeginTable("##docs_split", 2, tf)) {
    ImGui::TableSetupColumn("Left", ImGuiTableColumnFlags_WidthFixed, 300.0f);
    ImGui::TableSetupColumn("Right", ImGuiTableColumnFlags_WidthStretch);
    ImGui::TableNextRow();

    // --- Left column: doc list + TOC + global search.
    ImGui::TableSetColumnIndex(0);
    if (ImGui::BeginChild("##docs_left", ImVec2(0, 0), false)) {
      ImGui::SeparatorText("Documents");
      ImGui::SetNextItemWidth(-1.0f);
      ImGui::InputText("Filter", s.list_filter, sizeof(s.list_filter));
      const std::string f = trim_copy(s.list_filter);

      if (ImGui::BeginChild("##docs_list", ImVec2(0, 220), true)) {
        for (int i = 0; i < static_cast<int>(s.docs.size()); ++i) {
          const auto& d = s.docs[static_cast<std::size_t>(i)];
          if (!f.empty() && !contains_ci(d.title + " " + d.display_path, f)) continue;

          const bool sel = (i == s.selected_doc);
          std::string label = d.title;
          if (label.empty()) label = d.display_path;
          if (ImGui::Selectable(label.c_str(), sel)) {
            open_doc_by_ref(s, d.ref, /*push_history=*/true);
          }
          if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("%s", d.display_path.c_str());
          }
        }
      }
      ImGui::EndChild();

      if (s.show_toc) {
        ImGui::Spacing();
        ImGui::SeparatorText("Table of contents");
        if (s.headings.empty()) {
          ImGui::TextDisabled("(no headings)");
        } else {
          if (ImGui::BeginChild("##docs_toc", ImVec2(0, 220), true)) {
            for (int i = 0; i < static_cast<int>(s.headings.size()); ++i) {
              const auto& h = s.headings[static_cast<std::size_t>(i)];
              const float indent = static_cast<float>(std::max(0, h.level - 1)) * 14.0f;
              ImGui::Indent(indent);
              if (ImGui::Selectable(h.text.c_str(), false)) {
                s.request_scroll_line = h.line_index;
              }
              if (ImGui::IsItemHovered() && !h.anchor.empty()) {
                ImGui::SetTooltip("#%s", h.anchor.c_str());
              }
              ImGui::Unindent(indent);
            }
          }
          ImGui::EndChild();
        }
      }

      ImGui::Spacing();
      ImGui::SeparatorText("Search all docs");
      ImGui::SetNextItemWidth(-1.0f);
      ImGui::InputText("Query", s.global_query, sizeof(s.global_query));
      if (ImGui::SmallButton("Search")) {
        s.last_global_query.clear();
        run_global_search(s);
      }
      ImGui::SameLine();
      if (ImGui::SmallButton("Clear")) {
        s.global_query[0] = '\0';
        s.last_global_query.clear();
        s.global_hits.clear();
        s.global_status.clear();
      }
      if (!s.global_status.empty()) {
        ImGui::TextDisabled("%s", s.global_status.c_str());
      }

      if (!trim_copy(s.global_query).empty()) {
        // Keep results fresh if the user edits the query.
        run_global_search(s);
      }

      if (!s.global_hits.empty()) {
        if (ImGui::BeginChild("##docs_hits", ImVec2(0, 0), true)) {
          for (int i = 0; i < static_cast<int>(s.global_hits.size()); ++i) {
            const auto& hit = s.global_hits[static_cast<std::size_t>(i)];
            if (hit.doc_index < 0 || hit.doc_index >= static_cast<int>(s.docs.size())) continue;
            const auto& d = s.docs[static_cast<std::size_t>(hit.doc_index)];

            std::string label = d.title + "  (L" + std::to_string(hit.line_index + 1) + ")";
            if (ImGui::Selectable(label.c_str(), false)) {
              open_doc_by_ref(s, d.ref, /*push_history=*/true, std::nullopt, hit.line_index);
            }
            if (ImGui::IsItemHovered()) {
              ImGui::BeginTooltip();
              ImGui::TextUnformatted(hit.snippet.c_str());
              ImGui::EndTooltip();
            }
          }
        }
        ImGui::EndChild();
      }
    }
    ImGui::EndChild();

    // --- Right column: document view.
    ImGui::TableSetColumnIndex(1);
    if (ImGui::BeginChild("##docs_right", ImVec2(0, 0), false)) {
      ImGui::SeparatorText("Document");
      if (!s.current_abs_path.empty()) {
        ImGui::TextDisabled("%s", s.current_abs_path.c_str());
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Click to copy path");
        if (ImGui::IsItemClicked()) {
          ImGui::SetClipboardText(s.current_abs_path.c_str());
          s.status = "Copied path";
        }
      }
      if (!s.error.empty()) {
        ImGui::TextDisabled("Error reading doc: %s", s.error.c_str());
      }

      // Render, capture a clicked link.
      if (auto clicked = render_doc_view(s); clicked) {
        handle_clicked_link(s, *clicked);
      }
    }
    ImGui::EndChild();

    ImGui::EndTable();
  }
}

}  // namespace nebula4x::ui
