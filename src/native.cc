#include <napi.h>
#include <git2.h>
#include <git2/sys/errors.h>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iomanip>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <sstream>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#if LIBGIT2_VER_MAJOR < 1 || (LIBGIT2_VER_MAJOR == 1 && LIBGIT2_VER_MINOR < 9) || \
    (LIBGIT2_VER_MAJOR == 1 && LIBGIT2_VER_MINOR == 9 && LIBGIT2_VER_REVISION < 6)
#error "git-utils v10 requires libgit2 1.9.6 or newer"
#endif

namespace {

struct Endpoint {
  std::string type;
  std::string revision;
  std::string path;
};

struct ObjectRequest {
  std::string source;
  std::string oid;
  std::string revision;
  std::string path;
};

struct Request {
  std::string entrypoint;
  std::string git_directory;
  std::string working_directory;
  std::string operation;
  std::string revision = "HEAD";
  std::string path;
  std::string oid;
  std::string commit;
  std::string pattern;
  std::string format = "structured";
  std::string diff_filter;
  std::string key;
  std::string value;
  std::string scope = "local";
  std::string name;
  std::string url;
  std::string reference;
  std::string mode;
  std::string content;
  std::string content_encoding = "utf8";
  std::string file_path;
  std::string ours_path;
  std::string base_path;
  std::string theirs_path;
  std::string result_path;
  std::string base_oid;
  std::string ours_oid;
  std::string theirs_oid;
  std::string old_text;
  std::string new_text;
  Endpoint from;
  Endpoint to;
  std::vector<std::string> paths;
  std::vector<std::string> keys;
  std::vector<std::string> labels;
  std::vector<ObjectRequest> objects;
  bool status = true;
  bool refs = true;
  bool include_ignored = false;
  bool detect_renames = true;
  bool ignore_whitespace = false;
  bool ignore_eol_whitespace = false;
  bool ignore_space_change = false;
  bool ignore_all_space = false;
  bool show_local = false;
  bool show_remote = false;
  bool add = false;
  bool replace_all = false;
  bool all = false;
  int context = 3;
  int limit = -1;
  int skip = 0;
  int status_generation = 1;
  int refs_generation = 1;
  std::shared_ptr<std::atomic_bool> cancelled;
};

struct Failure {
  int code = 0;
  int klass = 0;
  std::string message;
};

static std::mutex cancellation_mutex;
static std::unordered_map<uint64_t, std::weak_ptr<std::atomic_bool>> cancellations;

static bool IsCancelled(const Request &request) {
  return request.cancelled && request.cancelled->load(std::memory_order_relaxed);
}

template<typename T, void (*Free)(T*)>
using git_ptr = std::unique_ptr<T, decltype(Free)>;

static std::string JsonString(const std::string &value) {
  std::ostringstream out;
  out << '"';
  for (unsigned char c : value) {
    switch (c) {
      case '"': out << "\\\""; break;
      case '\\': out << "\\\\"; break;
      case '\b': out << "\\b"; break;
      case '\f': out << "\\f"; break;
      case '\n': out << "\\n"; break;
      case '\r': out << "\\r"; break;
      case '\t': out << "\\t"; break;
      default:
        if (c < 0x20) {
          out << "\\u" << std::hex << std::setw(4) << std::setfill('0') << static_cast<int>(c)
              << std::dec;
        } else {
          out << static_cast<char>(c);
        }
    }
  }
  out << '"';
  return out.str();
}

static std::string JsonNullable(const char *value) {
  return value ? JsonString(value) : "null";
}

static std::string Join(const std::vector<std::string> &values, const char *separator = ",") {
  std::ostringstream out;
  for (size_t i = 0; i < values.size(); ++i) {
    if (i) out << separator;
    out << values[i];
  }
  return out.str();
}

static std::string OidString(const git_oid *oid) {
  if (!oid) return "";
  char buffer[GIT_OID_MAX_HEXSIZE + 1] = {0};
  git_oid_tostr(buffer, sizeof(buffer), oid);
  return buffer;
}

static std::string ShortOid(const git_oid *oid) {
  std::string full = OidString(oid);
  return full.size() > 7 ? full.substr(0, 7) : full;
}

static std::string ModeString(uint32_t mode) {
  std::ostringstream out;
  out << std::oct << std::setw(6) << std::setfill('0') << mode;
  return out.str();
}

static std::string Base64Encode(const void *data, size_t size) {
  static constexpr char alphabet[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
  const auto *bytes = static_cast<const unsigned char *>(data);
  std::string result;
  result.reserve(((size + 2) / 3) * 4);
  for (size_t i = 0; i < size; i += 3) {
    uint32_t value = static_cast<uint32_t>(bytes[i]) << 16;
    if (i + 1 < size) value |= static_cast<uint32_t>(bytes[i + 1]) << 8;
    if (i + 2 < size) value |= bytes[i + 2];
    result.push_back(alphabet[(value >> 18) & 63]);
    result.push_back(alphabet[(value >> 12) & 63]);
    result.push_back(i + 1 < size ? alphabet[(value >> 6) & 63] : '=');
    result.push_back(i + 2 < size ? alphabet[value & 63] : '=');
  }
  return result;
}

static std::string Base64Decode(const std::string &input) {
  static const std::string alphabet =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
  std::string output;
  uint32_t value = 0;
  int bits = -8;
  for (unsigned char c : input) {
    if (c == '=') break;
    const auto position = alphabet.find(c);
    if (position == std::string::npos) continue;
    value = (value << 6) | static_cast<uint32_t>(position);
    bits += 6;
    if (bits >= 0) {
      output.push_back(static_cast<char>((value >> bits) & 0xff));
      bits -= 8;
    }
  }
  return output;
}

static std::string NativeCode(const std::string &operation) {
  std::string result = "ERR_GIT_NATIVE_";
  for (size_t i = 0; i < operation.size(); ++i) {
    const unsigned char c = operation[i];
    if (std::isupper(c) && i > 0) result.push_back('_');
    result.push_back(static_cast<char>(std::toupper(c)));
  }
  return result;
}

static bool IsAbsolutePath(const std::string &path) {
  return (!path.empty() && (path[0] == '/' || path[0] == '\\')) ||
    (path.size() > 2 && std::isalpha(static_cast<unsigned char>(path[0])) && path[1] == ':');
}

static std::string ResolvePath(const Request &request, const std::string &path) {
  if (IsAbsolutePath(path) || request.working_directory.empty()) return path;
#ifdef _WIN32
  constexpr char separator = '\\';
#else
  constexpr char separator = '/';
#endif
  if (request.working_directory.back() == '/' || request.working_directory.back() == '\\') {
    return request.working_directory + path;
  }
  return request.working_directory + separator + path;
}

static bool ReadFile(const std::string &path, std::string *content) {
  std::ifstream stream(path, std::ios::binary);
  if (!stream) return false;
  std::ostringstream out;
  out << stream.rdbuf();
  *content = out.str();
  return stream.good() || stream.eof();
}

static bool WriteFile(const std::string &path, const void *content, size_t size) {
  std::ofstream stream(path, std::ios::binary | std::ios::trunc);
  if (!stream) return false;
  stream.write(static_cast<const char *>(content), static_cast<std::streamsize>(size));
  return stream.good();
}

static std::string StringProperty(const Napi::Object &object, const char *key,
                                  const std::string &fallback = "") {
  Napi::Value value = object.Get(key);
  return value.IsString() ? value.As<Napi::String>().Utf8Value() : fallback;
}

static bool BoolProperty(const Napi::Object &object, const char *key, bool fallback = false) {
  Napi::Value value = object.Get(key);
  return value.IsBoolean() ? value.As<Napi::Boolean>().Value() : fallback;
}

static int IntProperty(const Napi::Object &object, const char *key, int fallback) {
  Napi::Value value = object.Get(key);
  return value.IsNumber() ? value.As<Napi::Number>().Int32Value() : fallback;
}

static std::vector<std::string> StringArrayProperty(const Napi::Object &object, const char *key) {
  std::vector<std::string> result;
  Napi::Value value = object.Get(key);
  if (!value.IsArray()) return result;
  Napi::Array array = value.As<Napi::Array>();
  result.reserve(array.Length());
  for (uint32_t i = 0; i < array.Length(); ++i) {
    Napi::Value entry = array.Get(i);
    if (entry.IsString()) result.push_back(entry.As<Napi::String>().Utf8Value());
  }
  return result;
}

static Endpoint ParseEndpoint(const Napi::Object &request, const char *key) {
  Endpoint endpoint;
  Napi::Value value = request.Get(key);
  if (!value.IsObject()) return endpoint;
  Napi::Object object = value.As<Napi::Object>();
  endpoint.type = StringProperty(object, "type");
  endpoint.revision = StringProperty(object, "revision");
  endpoint.path = StringProperty(object, "path");
  return endpoint;
}

static Request ParseRequest(const Napi::CallbackInfo &info) {
  Request result;
  result.entrypoint = info[0].As<Napi::String>().Utf8Value();
  result.operation = result.entrypoint;
  if (info[1].IsObject()) {
    Napi::Object descriptor = info[1].As<Napi::Object>();
    result.git_directory = StringProperty(descriptor, "gitDirectory");
    result.working_directory = StringProperty(descriptor, "workingDirectory");
  }
  Napi::Object request = info[2].IsObject() ? info[2].As<Napi::Object>() : Napi::Object::New(info.Env());
  result.revision = StringProperty(request, "revision", "HEAD");
  result.path = StringProperty(request, "path");
  result.oid = StringProperty(request, "oid");
  result.commit = StringProperty(request, "commit");
  result.pattern = StringProperty(request, "pattern");
  result.format = StringProperty(request, "format", "structured");
  result.diff_filter = StringProperty(request, "diffFilter");
  result.key = StringProperty(request, "key");
  result.value = StringProperty(request, "value");
  result.scope = StringProperty(request, "scope", "local");
  result.name = StringProperty(request, "name");
  result.url = StringProperty(request, "url");
  result.reference = StringProperty(request, "reference");
  result.mode = StringProperty(request, "mode");
  result.content = StringProperty(request, "content");
  result.content_encoding = StringProperty(request, "contentEncoding", "utf8");
  result.file_path = StringProperty(request, "filePath");
  result.ours_path = StringProperty(request, "oursPath");
  result.base_path = StringProperty(request, "basePath");
  result.theirs_path = StringProperty(request, "theirsPath");
  result.result_path = StringProperty(request, "resultPath");
  result.base_oid = StringProperty(request, "baseOid");
  result.ours_oid = StringProperty(request, "oursOid");
  result.theirs_oid = StringProperty(request, "theirsOid");
  result.old_text = StringProperty(request, "oldText");
  result.new_text = StringProperty(request, "newText");
  if (result.entrypoint == "mutate") result.operation = StringProperty(request, "operation");
  result.from = ParseEndpoint(request, "from");
  result.to = ParseEndpoint(request, "to");
  result.paths = StringArrayProperty(request, "paths");
  result.keys = StringArrayProperty(request, "keys");
  result.labels = StringArrayProperty(request, "labels");
  result.status = BoolProperty(request, "status", true);
  result.refs = BoolProperty(request, "refs", true);
  result.include_ignored = BoolProperty(request, "includeIgnored");
  result.detect_renames = BoolProperty(request, "detectRenames", true);
  result.ignore_whitespace = BoolProperty(request, "ignoreWhitespace");
  result.ignore_eol_whitespace = BoolProperty(request, "ignoreEolWhitespace");
  result.ignore_space_change = BoolProperty(request, "ignoreSpaceChange");
  result.ignore_all_space = BoolProperty(request, "ignoreAllSpace");
  result.show_local = BoolProperty(request, "showLocal");
  result.show_remote = BoolProperty(request, "showRemote");
  result.add = BoolProperty(request, "add");
  result.replace_all = BoolProperty(request, "replaceAll");
  result.all = BoolProperty(request, "all");
  result.context = IntProperty(request, "context", 3);
  result.limit = IntProperty(request, "limit", -1);
  result.skip = IntProperty(request, "skip", 0);
  result.status_generation = IntProperty(request, "statusGeneration", 1);
  result.refs_generation = IntProperty(request, "refsGeneration", 1);

  Napi::Value requests_value = request.Get("requests");
  if (requests_value.IsArray()) {
    Napi::Array requests = requests_value.As<Napi::Array>();
    result.objects.reserve(requests.Length());
    for (uint32_t i = 0; i < requests.Length(); ++i) {
      Napi::Value value = requests.Get(i);
      if (!value.IsObject()) continue;
      Napi::Object object = value.As<Napi::Object>();
      result.objects.push_back({
        StringProperty(object, "source"),
        StringProperty(object, "oid"),
        StringProperty(object, "revision", "HEAD"),
        StringProperty(object, "path")
      });
    }
  }
  return result;
}

static bool CaptureFailure(int code, Failure *failure, const std::string &fallback) {
  if (code >= 0) return false;
  failure->code = code;
  const git_error *error = git_error_last();
  failure->klass = error ? error->klass : 0;
  failure->message = error && error->message ? error->message : fallback;
  return true;
}

static int OpenRepository(const Request &request, git_repository **repository) {
  if (request.git_directory.empty()) {
    git_error_set_str(GIT_ERROR_INVALID, "descriptor.gitDirectory is required");
    return GIT_EINVALID;
  }
  return git_repository_open_ext(
    repository,
    request.git_directory.c_str(),
    GIT_REPOSITORY_OPEN_NO_SEARCH,
    nullptr
  );
}

static int ResolveObject(git_object **object, git_repository *repository, const std::string &revision) {
  return git_revparse_single(object, repository, revision.empty() ? "HEAD" : revision.c_str());
}

static int ResolveCommit(git_commit **commit, git_repository *repository, const std::string &revision) {
  git_object *object = nullptr;
  int error = ResolveObject(&object, repository, revision);
  if (error < 0) return error;
  error = git_object_peel(reinterpret_cast<git_object **>(commit), object, GIT_OBJECT_COMMIT);
  git_object_free(object);
  return error;
}

static std::string SignatureJson(const git_signature *signature) {
  if (!signature) return "{\"name\":null,\"email\":null,\"date\":null}";
  std::ostringstream out;
  out << "{\"name\":" << JsonNullable(signature->name)
      << ",\"email\":" << JsonNullable(signature->email)
      << ",\"date\":" << static_cast<int64_t>(signature->when.time) * 1000 << '}';
  return out.str();
}

static std::string CommitJson(git_commit *commit, bool include_body = true) {
  std::vector<std::string> parents;
  for (unsigned int i = 0; i < git_commit_parentcount(commit); ++i) {
    parents.push_back(JsonString(OidString(git_commit_parent_id(commit, i))));
  }
  std::string body = git_commit_body(commit) ? git_commit_body(commit) : "";
  while (!body.empty() && (body.back() == '\n' || body.back() == '\r')) body.pop_back();
  std::ostringstream out;
  out << "{\"sha\":" << JsonString(OidString(git_commit_id(commit)))
      << ",\"parents\":[" << Join(parents) << ']'
      << ",\"author\":" << SignatureJson(git_commit_author(commit))
      << ",\"committer\":" << SignatureJson(git_commit_committer(commit))
      << ",\"subject\":" << JsonNullable(git_commit_summary(commit))
      << ",\"body\":" << JsonString(include_body ? body : "") << '}';
  return out.str();
}

static std::string LastCommitJson(git_repository *repository, const git_oid *oid) {
  git_commit *raw = nullptr;
  if (!oid || git_commit_lookup(&raw, repository, oid) < 0) {
    git_error_clear();
    return "null";
  }
  git_ptr<git_commit, git_commit_free> commit(raw, git_commit_free);
  std::vector<std::string> parents;
  for (unsigned int i = 0; i < git_commit_parentcount(commit.get()); ++i) {
    parents.push_back(JsonString(OidString(git_commit_parent_id(commit.get(), i))));
  }
  const git_signature *author = git_commit_author(commit.get());
  const git_signature *committer = git_commit_committer(commit.get());
  std::ostringstream out;
  out << "{\"oid\":" << JsonString(OidString(git_commit_id(commit.get())))
      << ",\"parents\":[" << Join(parents) << ']'
      << ",\"authorName\":" << JsonNullable(author ? author->name : nullptr)
      << ",\"committerDate\":" << (committer ? static_cast<int64_t>(committer->when.time) * 1000 : 0)
      << ",\"subject\":" << JsonNullable(git_commit_summary(commit.get())) << '}';
  return out.str();
}

static std::string ObjectTypeName(git_object_t type) {
  switch (type) {
    case GIT_OBJECT_BLOB: return "blob";
    case GIT_OBJECT_TREE: return "tree";
    case GIT_OBJECT_COMMIT: return "commit";
    case GIT_OBJECT_TAG: return "tag";
    default: return "unknown";
  }
}

static char IndexStatusCharacter(unsigned int status) {
  if (status & GIT_STATUS_INDEX_NEW) return 'A';
  if (status & GIT_STATUS_INDEX_MODIFIED) return 'M';
  if (status & GIT_STATUS_INDEX_DELETED) return 'D';
  if (status & GIT_STATUS_INDEX_RENAMED) return 'R';
  if (status & GIT_STATUS_INDEX_TYPECHANGE) return 'T';
  return 0;
}

static char WorktreeStatusCharacter(unsigned int status) {
  if (status & GIT_STATUS_WT_NEW) return 'A';
  if (status & GIT_STATUS_WT_MODIFIED) return 'M';
  if (status & GIT_STATUS_WT_DELETED) return 'D';
  if (status & GIT_STATUS_WT_RENAMED) return 'R';
  if (status & GIT_STATUS_WT_TYPECHANGE) return 'T';
  if (status & GIT_STATUS_WT_UNREADABLE) return 'X';
  return 0;
}

static std::string DeltaPath(const git_diff_delta *delta, bool old_path = false) {
  if (!delta) return "";
  const char *path = old_path ? delta->old_file.path : delta->new_file.path;
  if (!path) path = old_path ? delta->new_file.path : delta->old_file.path;
  return path ? path : "";
}

static void ConflictStatusCharacters(git_repository *repository, const std::string &path,
                                     char *index_status, char *worktree_status) {
  git_index *raw_index = nullptr;
  if (git_repository_index(&raw_index, repository) < 0) {
    git_error_clear();
    return;
  }
  git_ptr<git_index, git_index_free> index(raw_index, git_index_free);
  const git_index_entry *ancestor = nullptr, *ours = nullptr, *theirs = nullptr;
  if (git_index_conflict_get(&ancestor, &ours, &theirs, index.get(), path.c_str()) < 0) {
    git_error_clear();
    return;
  }
  if (ancestor && !ours && !theirs) { *index_status = 'D'; *worktree_status = 'D'; }
  else if (!ancestor && ours && !theirs) { *index_status = 'A'; *worktree_status = 'U'; }
  else if (ancestor && ours && !theirs) { *index_status = 'U'; *worktree_status = 'D'; }
  else if (!ancestor && !ours && theirs) { *index_status = 'U'; *worktree_status = 'A'; }
  else if (ancestor && !ours && theirs) { *index_status = 'D'; *worktree_status = 'U'; }
  else if (!ancestor && ours && theirs) { *index_status = 'A'; *worktree_status = 'A'; }
  else { *index_status = 'U'; *worktree_status = 'U'; }
}

static std::string StatusEntryJson(git_repository *repository, const git_status_entry *entry) {
  const unsigned int status = entry->status;
  const git_diff_delta *delta = (status & GIT_STATUS_INDEX_RENAMED) ? entry->head_to_index :
    ((status & GIT_STATUS_WT_RENAMED) ? entry->index_to_workdir :
      (entry->index_to_workdir ? entry->index_to_workdir : entry->head_to_index));
  const bool conflicted = (status & GIT_STATUS_CONFLICTED) != 0;
  const bool untracked = (status & GIT_STATUS_WT_NEW) != 0;
  const bool ignored = (status & GIT_STATUS_IGNORED) != 0;
  const bool renamed = (status & (GIT_STATUS_INDEX_RENAMED | GIT_STATUS_WT_RENAMED)) != 0;
  const bool copied = delta && delta->status == GIT_DELTA_COPIED;
  std::string kind = conflicted ? "unmerged" : ignored ? "ignored" : untracked ? "untracked" :
    copied ? "copied" : renamed ? "renamed" : "ordinary";
  std::string path = DeltaPath(delta);
  std::string original_path = (renamed || copied) ? DeltaPath(delta, true) : "";
  char index_status = conflicted ? 'U' : IndexStatusCharacter(status);
  char worktree_status = conflicted ? 'U' : WorktreeStatusCharacter(status);
  if (conflicted) ConflictStatusCharacters(repository, path, &index_status, &worktree_status);

  bool is_submodule = false;
  bool commit_changed = false;
  bool modified = false;
  bool has_untracked = false;
  const bool submodule_candidate = delta &&
    (delta->old_file.mode == GIT_FILEMODE_COMMIT || delta->new_file.mode == GIT_FILEMODE_COMMIT);
  if (!path.empty() && submodule_candidate) {
    git_submodule *raw_submodule = nullptr;
    if (git_submodule_lookup(&raw_submodule, repository, path.c_str()) == 0) {
      git_ptr<git_submodule, git_submodule_free> submodule(raw_submodule, git_submodule_free);
      unsigned int submodule_status = 0;
      if (git_submodule_status(&submodule_status, repository, path.c_str(), GIT_SUBMODULE_IGNORE_UNSPECIFIED) == 0) {
        is_submodule = true;
        commit_changed = (submodule_status & (
          GIT_SUBMODULE_STATUS_INDEX_ADDED | GIT_SUBMODULE_STATUS_INDEX_DELETED |
          GIT_SUBMODULE_STATUS_INDEX_MODIFIED | GIT_SUBMODULE_STATUS_WD_ADDED |
          GIT_SUBMODULE_STATUS_WD_DELETED | GIT_SUBMODULE_STATUS_WD_MODIFIED)) != 0;
        modified = (submodule_status & (
          GIT_SUBMODULE_STATUS_WD_INDEX_MODIFIED | GIT_SUBMODULE_STATUS_WD_WD_MODIFIED)) != 0;
        has_untracked = (submodule_status & GIT_SUBMODULE_STATUS_WD_UNTRACKED) != 0;
      }
      git_error_clear();
    } else {
      git_error_clear();
    }
  }

  int similarity = delta ? static_cast<int>(delta->similarity) : 0;
  std::ostringstream out;
  out << "{\"path\":" << JsonString(path)
      << ",\"originalPath\":" << (original_path.empty() ? "null" : JsonString(original_path))
      << ",\"kind\":" << JsonString(kind)
      << ",\"indexStatus\":" << (index_status ? JsonString(std::string(1, index_status)) : "null")
      << ",\"worktreeStatus\":" << (worktree_status && !untracked ? JsonString(std::string(1, worktree_status)) : "null")
      << ",\"staged\":" << (index_status ? "true" : "false")
      << ",\"unstaged\":" << ((worktree_status || untracked) ? "true" : "false")
      << ",\"conflicted\":" << (conflicted ? "true" : "false")
      << ",\"untracked\":" << (untracked ? "true" : "false")
      << ",\"ignored\":" << (ignored ? "true" : "false")
      << ",\"similarity\":" << ((renamed || copied) && similarity > 0 ? std::to_string(similarity) : "null")
      << ",\"submodule\":{\"isSubmodule\":" << (is_submodule ? "true" : "false")
      << ",\"commitChanged\":" << (commit_changed ? "true" : "false")
      << ",\"modified\":" << (modified ? "true" : "false")
      << ",\"hasUntrackedChanges\":" << (has_untracked ? "true" : "false") << "}}";
  return out.str();
}

static std::string StatusHeadJson(git_repository *repository) {
  const bool unborn = git_repository_head_unborn(repository) == 1;
  const bool detached = git_repository_head_detached(repository) == 1;
  std::string symbolic_name;
  git_reference *raw_symbolic_head = nullptr;
  if (git_reference_lookup(&raw_symbolic_head, repository, "HEAD") == 0 && raw_symbolic_head) {
    const char *target = git_reference_symbolic_target(raw_symbolic_head);
    if (target) {
      symbolic_name = target;
      if (symbolic_name.rfind("refs/heads/", 0) == 0) {
        symbolic_name = symbolic_name.substr(std::strlen("refs/heads/"));
      }
    }
  } else {
    git_error_clear();
  }
  git_reference_free(raw_symbolic_head);
  git_reference *raw_head = nullptr;
  int error = git_repository_head(&raw_head, repository);
  if (error < 0 && error != GIT_EUNBORNBRANCH && error != GIT_ENOTFOUND) return "null";
  git_ptr<git_reference, git_reference_free> head(raw_head, git_reference_free);
  const git_oid *oid = head ? git_reference_target(head.get()) : nullptr;
  const char *name = !symbolic_name.empty() ? symbolic_name.c_str() :
    (head && !detached ? git_reference_shorthand(head.get()) : nullptr);
  std::ostringstream out;
  out << "{\"oid\":" << (oid ? JsonString(OidString(oid)) : "null")
      << ",\"name\":" << JsonNullable(name)
      << ",\"detached\":" << (detached ? "true" : "false")
      << ",\"unborn\":" << (unborn ? "true" : "false") << '}';
  git_error_clear();
  return out.str();
}

static std::string StatusUpstreamJson(git_repository *repository) {
  git_reference *raw_head = nullptr;
  if (git_repository_head(&raw_head, repository) < 0 || !raw_head ||
      !git_reference_is_branch(raw_head)) {
    git_reference_free(raw_head);
    git_error_clear();
    return "null";
  }
  git_ptr<git_reference, git_reference_free> head(raw_head, git_reference_free);
  git_reference *raw_upstream = nullptr;
  if (git_branch_upstream(&raw_upstream, head.get()) < 0) {
    git_error_clear();
    return "null";
  }
  git_ptr<git_reference, git_reference_free> upstream(raw_upstream, git_reference_free);
  size_t ahead = 0, behind = 0;
  const git_oid *head_oid = git_reference_target(head.get());
  const git_oid *upstream_oid = git_reference_target(upstream.get());
  if (head_oid && upstream_oid) {
    if (git_graph_ahead_behind(&ahead, &behind, repository, head_oid, upstream_oid) < 0) {
      ahead = behind = 0;
      git_error_clear();
    }
  }
  std::ostringstream out;
  out << "{\"name\":" << JsonNullable(git_reference_shorthand(upstream.get()))
      << ",\"ahead\":" << ahead << ",\"behind\":" << behind << '}';
  return out.str();
}

static int BuildStatusJson(std::string *json, git_repository *repository, const Request &request) {
  git_status_options options = GIT_STATUS_OPTIONS_INIT;
  options.show = GIT_STATUS_SHOW_INDEX_AND_WORKDIR;
  options.flags = GIT_STATUS_OPT_INCLUDE_UNTRACKED |
    GIT_STATUS_OPT_RECURSE_UNTRACKED_DIRS |
    GIT_STATUS_OPT_RENAMES_HEAD_TO_INDEX |
    GIT_STATUS_OPT_RENAMES_INDEX_TO_WORKDIR |
    GIT_STATUS_OPT_RENAMES_FROM_REWRITES;
  if (request.include_ignored) {
    // Match `git status --ignored=matching`: report a wholly ignored
    // directory once instead of enumerating every ignored descendant.
    options.flags |= GIT_STATUS_OPT_INCLUDE_IGNORED;
  }
  git_status_list *raw_list = nullptr;
  int error = git_status_list_new(&raw_list, repository, &options);
  if (error < 0) return error;
  git_ptr<git_status_list, git_status_list_free> list(raw_list, git_status_list_free);
  if (IsCancelled(request)) return GIT_EUSER;
  std::vector<std::pair<std::string, std::string>> entries;
  int staged = 0, unstaged = 0, conflicted = 0, untracked = 0, ignored = 0;
  for (size_t i = 0; i < git_status_list_entrycount(list.get()); ++i) {
    if (IsCancelled(request)) return GIT_EUSER;
    const git_status_entry *entry = git_status_byindex(list.get(), i);
    if (!entry || entry->status == GIT_STATUS_CURRENT) continue;
    const git_diff_delta *delta = entry->index_to_workdir ? entry->index_to_workdir : entry->head_to_index;
    std::string path = DeltaPath(delta);
    entries.emplace_back(path, StatusEntryJson(repository, entry));
    if (entry->status & (GIT_STATUS_INDEX_NEW | GIT_STATUS_INDEX_MODIFIED |
        GIT_STATUS_INDEX_DELETED | GIT_STATUS_INDEX_RENAMED | GIT_STATUS_INDEX_TYPECHANGE)) ++staged;
    if (entry->status & (GIT_STATUS_WT_NEW | GIT_STATUS_WT_MODIFIED | GIT_STATUS_WT_DELETED |
        GIT_STATUS_WT_RENAMED | GIT_STATUS_WT_TYPECHANGE | GIT_STATUS_WT_UNREADABLE)) ++unstaged;
    if (entry->status & GIT_STATUS_CONFLICTED) { ++conflicted; ++staged; ++unstaged; }
    if (entry->status & GIT_STATUS_WT_NEW) ++untracked;
    if (entry->status & GIT_STATUS_IGNORED) ++ignored;
  }
  std::sort(entries.begin(), entries.end(), [](const auto &left, const auto &right) {
    return left.first < right.first;
  });
  std::vector<std::string> values;
  values.reserve(entries.size());
  for (auto &entry : entries) values.push_back(std::move(entry.second));
  std::ostringstream out;
  out << "{\"schemaVersion\":1,\"generation\":" << request.status_generation
      << ",\"initialized\":true,\"includesIgnored\":" << (request.include_ignored ? "true" : "false")
      << ",\"head\":" << StatusHeadJson(repository)
      << ",\"upstream\":" << StatusUpstreamJson(repository)
      << ",\"files\":[" << Join(values) << ']'
      << ",\"counts\":{\"total\":" << values.size()
      << ",\"staged\":" << staged << ",\"unstaged\":" << unstaged
      << ",\"conflicted\":" << conflicted << ",\"untracked\":" << untracked
      << ",\"ignored\":" << ignored << "}}";
  *json = out.str();
  return 0;
}

static std::string TrackJson(git_repository *repository, const git_oid *local,
                             const git_oid *remote, bool gone) {
  size_t ahead = 0, behind = 0;
  if (!gone && local && remote &&
      git_graph_ahead_behind(&ahead, &behind, repository, local, remote) < 0) {
    ahead = behind = 0;
    git_error_clear();
  }
  std::ostringstream out;
  out << "\"ahead\":" << ahead << ",\"behind\":" << behind
      << ",\"gone\":" << (gone ? "true" : "false");
  return out.str();
}

static std::string ConfigString(git_repository *repository, const std::string &key) {
  git_config *raw_config = nullptr;
  if (git_repository_config(&raw_config, repository) < 0) return "";
  git_ptr<git_config, git_config_free> config(raw_config, git_config_free);
  git_buf value = GIT_BUF_INIT;
  int error = git_config_get_string_buf(&value, config.get(), key.c_str());
  if (error < 0) {
    git_buf_dispose(&value);
    git_error_clear();
    return "";
  }
  std::string result(value.ptr ? value.ptr : "", value.size);
  git_buf_dispose(&value);
  return result;
}

static std::string ConfiguredUpstreamRef(git_repository *repository, const std::string &branch) {
  std::string remote = ConfigString(repository, "branch." + branch + ".remote");
  std::string merge = ConfigString(repository, "branch." + branch + ".merge");
  if (remote.empty() || merge.rfind("refs/heads/", 0) != 0) return "";
  if (remote == ".") return merge;
  return "refs/remotes/" + remote + "/" + merge.substr(std::strlen("refs/heads/"));
}

static std::vector<std::string> ConfigValues(git_repository *repository, const std::string &key) {
  std::vector<std::string> values;
  git_config *raw_config = nullptr;
  if (git_repository_config(&raw_config, repository) < 0) return values;
  git_ptr<git_config, git_config_free> config(raw_config, git_config_free);
  git_config_iterator *raw_iterator = nullptr;
  int error = git_config_multivar_iterator_new(&raw_iterator, config.get(), key.c_str(), nullptr);
  if (error == GIT_ENOTFOUND) {
    git_error_clear();
    return values;
  }
  if (error < 0) return values;
  git_ptr<git_config_iterator, git_config_iterator_free> iterator(raw_iterator, git_config_iterator_free);
  git_config_entry *entry = nullptr;
  while ((error = git_config_next(&entry, iterator.get())) == 0) {
    if (entry && entry->value) values.emplace_back(entry->value);
  }
  if (error != GIT_ITEROVER) values.clear();
  git_error_clear();
  return values;
}

static std::string RemoteTrackingRef(git_repository *repository, const std::string &remote_name,
                                     const std::string &remote_destination) {
  if (remote_name == ".") return remote_destination;
  git_remote *raw_remote = nullptr;
  if (git_remote_lookup(&raw_remote, repository, remote_name.c_str()) < 0) {
    git_error_clear();
    return "";
  }
  git_ptr<git_remote, git_remote_free> remote(raw_remote, git_remote_free);
  git_strarray fetch_specs = {nullptr, 0};
  if (git_remote_get_fetch_refspecs(&fetch_specs, remote.get()) < 0) {
    git_error_clear();
    return "";
  }
  std::string result;
  for (size_t i = 0; i < fetch_specs.count && result.empty(); ++i) {
    git_refspec *raw_spec = nullptr;
    if (git_refspec_parse(&raw_spec, fetch_specs.strings[i], 1) < 0) {
      git_error_clear();
      continue;
    }
    git_ptr<git_refspec, git_refspec_free> spec(raw_spec, git_refspec_free);
    if (!git_refspec_src_matches(spec.get(), remote_destination.c_str())) continue;
    git_buf transformed = GIT_BUF_INIT;
    if (git_refspec_transform(&transformed, spec.get(), remote_destination.c_str()) == 0 && transformed.ptr) {
      result.assign(transformed.ptr, transformed.size);
    } else {
      git_error_clear();
    }
    git_buf_dispose(&transformed);
  }
  git_strarray_dispose(&fetch_specs);
  return result;
}

static std::string ExplicitPushRef(git_repository *repository, const std::string &remote_name,
                                   const std::string &local_ref, bool *configured) {
  std::vector<std::string> values = ConfigValues(repository, "remote." + remote_name + ".push");
  *configured = !values.empty();
  for (const std::string &value : values) {
    git_refspec *raw_spec = nullptr;
    if (git_refspec_parse(&raw_spec, value.c_str(), 0) < 0) {
      git_error_clear();
      continue;
    }
    git_ptr<git_refspec, git_refspec_free> spec(raw_spec, git_refspec_free);
    if (git_refspec_src_matches_negative(spec.get(), local_ref.c_str())) return "";
    if (!git_refspec_src_matches(spec.get(), local_ref.c_str())) continue;
    git_buf destination = GIT_BUF_INIT;
    std::string result;
    if (git_refspec_transform(&destination, spec.get(), local_ref.c_str()) == 0 && destination.ptr) {
      result = RemoteTrackingRef(repository, remote_name,
                                 std::string(destination.ptr, destination.size));
    } else {
      git_error_clear();
    }
    git_buf_dispose(&destination);
    return result;
  }
  return "";
}

static std::string PushRef(git_repository *repository, const std::string &branch,
                           const std::string &upstream_ref) {
  std::string push_default = ConfigString(repository, "push.default");
  if (push_default.empty()) push_default = "simple";
  if (push_default == "nothing") return "";
  std::string remote = ConfigString(repository, "branch." + branch + ".pushRemote");
  if (remote.empty()) remote = ConfigString(repository, "remote.pushDefault");
  if (remote.empty()) remote = ConfigString(repository, "branch." + branch + ".remote");
  if (remote.empty()) remote = "origin";
  const std::string local_ref = "refs/heads/" + branch;
  bool explicitly_configured = false;
  std::string explicit_ref = ExplicitPushRef(repository, remote, local_ref, &explicitly_configured);
  if (explicitly_configured) return explicit_ref;
  if (push_default == "upstream") {
    if (remote == ".") return upstream_ref.rfind("refs/heads/", 0) == 0 ? upstream_ref : "";
    return upstream_ref.rfind("refs/remotes/" + remote + '/', 0) == 0 ? upstream_ref : "";
  }
  if (push_default == "simple") {
    std::string expected = RemoteTrackingRef(repository, remote, local_ref);
    return upstream_ref == expected ? upstream_ref : "";
  }
  return RemoteTrackingRef(repository, remote, local_ref);
}

static std::string BranchEntryJson(git_repository *repository, git_reference *reference,
                                   bool is_head) {
  const char *full_name = git_reference_name(reference);
  const char *short_name = git_reference_shorthand(reference);
  const git_oid *oid = git_reference_target(reference);
  std::string branch = short_name ? short_name : "";
  std::string upstream_ref;
  std::string upstream_name;
  const git_oid *upstream_oid = nullptr;
  git_reference *raw_upstream = nullptr;
  bool upstream_gone = false;
  if (git_branch_upstream(&raw_upstream, reference) == 0) {
    upstream_ref = git_reference_name(raw_upstream) ? git_reference_name(raw_upstream) : "";
    upstream_name = git_reference_shorthand(raw_upstream) ? git_reference_shorthand(raw_upstream) : "";
    upstream_oid = git_reference_target(raw_upstream);
  } else {
    git_error_clear();
    upstream_ref = ConfiguredUpstreamRef(repository, branch);
    upstream_name = upstream_ref.rfind("refs/remotes/", 0) == 0
      ? upstream_ref.substr(std::strlen("refs/remotes/")) :
      (upstream_ref.rfind("refs/heads/", 0) == 0
        ? upstream_ref.substr(std::strlen("refs/heads/")) : upstream_ref);
    upstream_gone = !upstream_ref.empty();
  }
  std::string push_ref = PushRef(repository, branch, upstream_ref);
  git_reference *raw_push = nullptr;
  const git_oid *push_oid = nullptr;
  bool push_gone = false;
  if (!push_ref.empty()) {
    if (git_reference_lookup(&raw_push, repository, push_ref.c_str()) == 0) {
      push_oid = git_reference_target(raw_push);
    } else {
      push_gone = true;
      git_error_clear();
    }
  }
  std::string push_name = push_ref.rfind("refs/remotes/", 0) == 0
    ? push_ref.substr(std::strlen("refs/remotes/")) :
    (push_ref.rfind("refs/heads/", 0) == 0
      ? push_ref.substr(std::strlen("refs/heads/")) : push_ref);

  std::ostringstream out;
  out << "{\"name\":" << JsonString(branch)
      << ",\"ref\":" << JsonNullable(full_name)
      << ",\"oid\":" << JsonString(OidString(oid))
      << ",\"isHead\":" << (is_head ? "true" : "false")
      << ",\"upstream\":";
  if (upstream_ref.empty()) out << "null";
  else out << "{\"ref\":" << JsonString(upstream_ref)
           << ",\"name\":" << JsonString(upstream_name) << ','
           << TrackJson(repository, oid, upstream_oid, upstream_gone) << '}';
  out << ",\"push\":";
  if (push_ref.empty()) out << "null";
  else out << "{\"ref\":" << JsonString(push_ref)
           << ",\"name\":" << JsonString(push_name) << ','
           << TrackJson(repository, oid, push_oid, push_gone) << '}';
  out << ",\"lastCommit\":" << LastCommitJson(repository, oid) << '}';
  git_reference_free(raw_upstream);
  git_reference_free(raw_push);
  return out.str();
}

static std::string RemoteNameForRef(const std::string &full_name,
                                    const std::vector<std::string> &remote_names) {
  std::string result;
  for (const std::string &name : remote_names) {
    const std::string prefix = "refs/remotes/" + name + '/';
    if (full_name.rfind(prefix, 0) == 0 && name.size() > result.size()) result = name;
  }
  return result;
}

static std::string RemoteBranchEntryJson(git_repository *repository, git_reference *reference,
                                         const std::vector<std::string> &remote_names) {
  const char *full_name = git_reference_name(reference);
  const char *short_name = git_reference_shorthand(reference);
  const git_oid *oid = git_reference_target(reference);
  std::string remote_name = full_name ? RemoteNameForRef(full_name, remote_names) : "";
  const char *symbolic_target = git_reference_type(reference) == GIT_REFERENCE_SYMBOLIC
    ? git_reference_symbolic_target(reference) : nullptr;
  std::ostringstream out;
  out << "{\"name\":" << JsonNullable(short_name)
      << ",\"ref\":" << JsonNullable(full_name)
      << ",\"oid\":" << JsonString(OidString(oid))
      << ",\"remoteName\":" << JsonString(remote_name)
      << ",\"symrefTarget\":" << JsonNullable(symbolic_target)
      << ",\"lastCommit\":" << LastCommitJson(repository, oid) << '}';
  return out.str();
}

static std::string TagEntryJson(git_repository *repository, git_reference *reference) {
  const char *full_name = git_reference_name(reference);
  const char *short_name = git_reference_shorthand(reference);
  const git_oid *oid = git_reference_target(reference);
  git_object *raw_object = nullptr;
  git_object *raw_commit = nullptr;
  bool annotated = false;
  std::string target_oid = OidString(oid);
  const git_oid *commit_oid = oid;
  if (oid && git_object_lookup(&raw_object, repository, oid, GIT_OBJECT_ANY) == 0) {
    annotated = git_object_type(raw_object) == GIT_OBJECT_TAG;
    if (git_object_peel(&raw_commit, raw_object, GIT_OBJECT_COMMIT) == 0) {
      target_oid = OidString(git_object_id(raw_commit));
      commit_oid = git_object_id(raw_commit);
    } else {
      git_error_clear();
    }
  } else {
    git_error_clear();
  }
  std::ostringstream out;
  out << "{\"name\":" << JsonNullable(short_name)
      << ",\"ref\":" << JsonNullable(full_name)
      << ",\"oid\":" << JsonString(OidString(oid))
      << ",\"targetOid\":" << JsonString(target_oid)
      << ",\"annotated\":" << (annotated ? "true" : "false")
      << ",\"lastCommit\":" << LastCommitJson(repository, commit_oid) << '}';
  git_object_free(raw_commit);
  git_object_free(raw_object);
  return out.str();
}

static std::string RefsHeadJson(git_repository *repository) {
  bool unborn = git_repository_head_unborn(repository) == 1;
  bool detached = git_repository_head_detached(repository) == 1;
  std::string symbolic_ref;
  git_reference *raw_symbolic_head = nullptr;
  if (git_reference_lookup(&raw_symbolic_head, repository, "HEAD") == 0 && raw_symbolic_head) {
    const char *target = git_reference_symbolic_target(raw_symbolic_head);
    if (target) symbolic_ref = target;
  } else {
    git_error_clear();
  }
  git_reference_free(raw_symbolic_head);
  git_reference *raw_head = nullptr;
  int error = git_repository_head(&raw_head, repository);
  git_ptr<git_reference, git_reference_free> head(raw_head, git_reference_free);
  const git_oid *oid = head ? git_reference_target(head.get()) : nullptr;
  const char *ref = !symbolic_ref.empty() ? symbolic_ref.c_str() :
    (head && !detached ? git_reference_name(head.get()) : nullptr);
  std::string symbolic_name = symbolic_ref.rfind("refs/heads/", 0) == 0
    ? symbolic_ref.substr(std::strlen("refs/heads/")) : "";
  const char *name = !symbolic_name.empty() ? symbolic_name.c_str() :
    (head && !detached ? git_reference_shorthand(head.get()) : nullptr);
  if (error < 0) git_error_clear();
  std::ostringstream out;
  out << "{\"oid\":" << (oid ? JsonString(OidString(oid)) : "null")
      << ",\"ref\":" << JsonNullable(ref)
      << ",\"name\":" << JsonNullable(name)
      << ",\"detached\":" << (detached ? "true" : "false")
      << ",\"unborn\":" << (unborn ? "true" : "false") << '}';
  return out.str();
}

static std::string WorktreeJson(const std::string &path, git_repository *repository,
                                bool locked, const std::string &locked_reason,
                                bool prunable) {
  bool bare = git_repository_is_bare(repository) == 1;
  bool detached = git_repository_head_detached(repository) == 1;
  git_reference *raw_head = nullptr;
  int error = git_repository_head(&raw_head, repository);
  git_ptr<git_reference, git_reference_free> head(raw_head, git_reference_free);
  const git_oid *oid = head ? git_reference_target(head.get()) : nullptr;
  const char *branch = head && !detached ? git_reference_name(head.get()) : nullptr;
  if (error < 0) git_error_clear();
  std::ostringstream out;
  out << "{\"path\":" << JsonString(path)
      << ",\"headOid\":" << (oid ? JsonString(OidString(oid)) : "null")
      << ",\"branch\":" << JsonNullable(branch)
      << ",\"detached\":" << (detached ? "true" : "false")
      << ",\"bare\":" << (bare ? "true" : "false")
      << ",\"locked\":" << (locked ? "true" : "false")
      << ",\"lockedReason\":" << (locked_reason.empty() ? "null" : JsonString(locked_reason))
      << ",\"prunable\":" << (prunable ? "true" : "false") << '}';
  return out.str();
}

static std::string MissingWorktreeJson(const std::string &path, bool locked,
                                       const std::string &locked_reason, bool prunable) {
  std::ostringstream out;
  out << "{\"path\":" << JsonString(path)
      << ",\"headOid\":null,\"branch\":null,\"detached\":false,\"bare\":false"
      << ",\"locked\":" << (locked ? "true" : "false")
      << ",\"lockedReason\":" << (locked_reason.empty() ? "null" : JsonString(locked_reason))
      << ",\"prunable\":" << (prunable ? "true" : "false") << '}';
  return out.str();
}

static std::string TrimTrailingSeparators(std::string path) {
  while (path.size() > 1 && (path.back() == '/' || path.back() == '\\')) path.pop_back();
  return path;
}

static std::string ParentPath(std::string path) {
  path = TrimTrailingSeparators(std::move(path));
  size_t separator = path.find_last_of("/\\");
  return separator == std::string::npos ? std::string() : path.substr(0, separator);
}

static std::string PathKey(std::string path) {
  path = TrimTrailingSeparators(std::move(path));
  std::replace(path.begin(), path.end(), '\\', '/');
#ifdef _WIN32
  std::transform(path.begin(), path.end(), path.begin(), [](unsigned char c) {
    return static_cast<char>(std::tolower(c));
  });
#endif
  return path;
}

static int BuildRefsJson(std::string *json, git_repository *repository, const Request &request) {
  std::vector<std::string> branches, remote_branches, tags, remotes, worktrees;
  std::vector<std::string> remote_name_values;
  git_strarray remote_names = {nullptr, 0};
  int error = git_remote_list(&remote_names, repository);
  if (error < 0) return error;
  for (size_t i = 0; i < remote_names.count; ++i) {
    remote_name_values.emplace_back(remote_names.strings[i]);
    git_remote *raw_remote = nullptr;
    if (git_remote_lookup(&raw_remote, repository, remote_names.strings[i]) < 0) {
      git_strarray_dispose(&remote_names);
      return -1;
    }
    git_ptr<git_remote, git_remote_free> remote(raw_remote, git_remote_free);
    std::ostringstream out;
    out << "{\"name\":" << JsonString(remote_names.strings[i])
        << ",\"fetchUrl\":" << JsonNullable(git_remote_url(remote.get()))
        << ",\"pushUrl\":" << JsonNullable(git_remote_pushurl(remote.get()) ? git_remote_pushurl(remote.get()) : git_remote_url(remote.get())) << '}';
    remotes.push_back(out.str());
  }
  git_strarray_dispose(&remote_names);
  git_reference_iterator *raw_iterator = nullptr;
  error = git_reference_iterator_new(&raw_iterator, repository);
  if (error < 0) return error;
  git_ptr<git_reference_iterator, git_reference_iterator_free> iterator(raw_iterator, git_reference_iterator_free);
  git_reference *raw_reference = nullptr;
  while ((error = git_reference_next(&raw_reference, iterator.get())) == 0) {
    if (IsCancelled(request)) return GIT_EUSER;
    git_ptr<git_reference, git_reference_free> reference(raw_reference, git_reference_free);
    raw_reference = nullptr;
    const char *name = git_reference_name(reference.get());
    if (!name) continue;
    std::string ref_name(name);
    if (git_reference_type(reference.get()) == GIT_REFERENCE_SYMBOLIC) {
      git_reference *resolved = nullptr;
      if (git_reference_resolve(&resolved, reference.get()) == 0 && resolved) {
        // Preserve the symbolic name while using the resolved target for oid and metadata.
        const char *symbolic_target = git_reference_symbolic_target(reference.get());
        if (ref_name.rfind("refs/remotes/", 0) == 0) {
          const git_oid *oid = git_reference_target(resolved);
          std::string suffix = ref_name.substr(std::strlen("refs/remotes/"));
          std::string remote_name = RemoteNameForRef(ref_name, remote_name_values);
          std::ostringstream out;
          out << "{\"name\":" << JsonString(suffix)
              << ",\"ref\":" << JsonString(ref_name)
              << ",\"oid\":" << JsonString(OidString(oid))
              << ",\"remoteName\":" << JsonString(remote_name)
              << ",\"symrefTarget\":" << JsonNullable(symbolic_target)
              << ",\"lastCommit\":" << LastCommitJson(repository, oid) << '}';
          remote_branches.push_back(out.str());
        }
        git_reference_free(resolved);
        continue;
      }
      git_error_clear();
    }
    bool is_head = git_branch_is_head(reference.get()) == 1;
    if (ref_name.rfind("refs/heads/", 0) == 0) branches.push_back(BranchEntryJson(repository, reference.get(), is_head));
    else if (ref_name.rfind("refs/remotes/", 0) == 0) remote_branches.push_back(RemoteBranchEntryJson(repository, reference.get(), remote_name_values));
    else if (ref_name.rfind("refs/tags/", 0) == 0) tags.push_back(TagEntryJson(repository, reference.get()));
  }
  if (error != GIT_ITEROVER) return error;
  git_error_clear();

  std::string current_path = request.working_directory.empty()
    ? (git_repository_workdir(repository) ? git_repository_workdir(repository) : request.git_directory)
    : request.working_directory;
  std::set<std::string> seen_worktrees;
  seen_worktrees.insert(PathKey(current_path));
  worktrees.push_back(WorktreeJson(current_path, repository, false, "", false));

  const char *common_directory = git_repository_commondir(repository);
  if (common_directory && !git_repository_is_bare(repository)) {
    std::string common = TrimTrailingSeparators(common_directory);
    std::string primary_path = ParentPath(common);
    size_t separator = common.find_last_of("/\\");
    std::string basename = separator == std::string::npos ? common : common.substr(separator + 1);
    if (basename == ".git" && !primary_path.empty() &&
        seen_worktrees.insert(PathKey(primary_path)).second) {
      git_repository *raw_primary = nullptr;
      if (git_repository_open(&raw_primary, primary_path.c_str()) == 0) {
        git_ptr<git_repository, git_repository_free> primary(raw_primary, git_repository_free);
        worktrees.push_back(WorktreeJson(primary_path, primary.get(), false, "", false));
      } else {
        git_error_clear();
        worktrees.push_back(MissingWorktreeJson(primary_path, false, "", true));
      }
    }
  }
  git_strarray worktree_names = {nullptr, 0};
  if (git_worktree_list(&worktree_names, repository) == 0) {
    for (size_t i = 0; i < worktree_names.count; ++i) {
      git_worktree *raw_worktree = nullptr;
      if (git_worktree_lookup(&raw_worktree, repository, worktree_names.strings[i]) < 0) {
        git_error_clear();
        continue;
      }
      git_ptr<git_worktree, git_worktree_free> worktree(raw_worktree, git_worktree_free);
      const char *path = git_worktree_path(worktree.get());
      if (!path || !seen_worktrees.insert(PathKey(path)).second) continue;
      git_buf reason = GIT_BUF_INIT;
      int locked_result = git_worktree_is_locked(&reason, worktree.get());
      if (locked_result < 0) {
        git_buf_dispose(&reason);
        return locked_result;
      }
      std::string locked_reason = reason.ptr ? std::string(reason.ptr, reason.size) : "";
      git_buf_dispose(&reason);
      int prunable_result = git_worktree_is_prunable(worktree.get(), nullptr);
      if (prunable_result < 0) return prunable_result;
      git_repository *raw_worktree_repository = nullptr;
      if (git_repository_open(&raw_worktree_repository, path) == 0) {
        git_ptr<git_repository, git_repository_free> worktree_repository(raw_worktree_repository, git_repository_free);
        worktrees.push_back(WorktreeJson(path, worktree_repository.get(), locked_result > 0,
                                         locked_reason, prunable_result > 0));
      } else {
        git_error_clear();
        worktrees.push_back(MissingWorktreeJson(path, locked_result > 0,
                                                locked_reason, prunable_result > 0));
      }
    }
    git_strarray_dispose(&worktree_names);
  } else {
    git_error_clear();
  }

  std::sort(branches.begin(), branches.end());
  std::sort(remote_branches.begin(), remote_branches.end());
  std::sort(tags.begin(), tags.end());
  std::sort(remotes.begin(), remotes.end());
  std::sort(worktrees.begin(), worktrees.end());

  std::ostringstream out;
  out << "{\"schemaVersion\":1,\"generation\":" << request.refs_generation
      << ",\"initialized\":true,\"head\":" << RefsHeadJson(repository)
      << ",\"branches\":[" << Join(branches) << ']'
      << ",\"remoteBranches\":[" << Join(remote_branches) << ']'
      << ",\"tags\":[" << Join(tags) << ']'
      << ",\"remotes\":[" << Join(remotes) << ']'
      << ",\"worktrees\":[" << Join(worktrees) << "]}";
  *json = out.str();
  return 0;
}

static int ResolveTree(git_tree **tree, git_repository *repository, const std::string &revision) {
  git_object *object = nullptr;
  int error = ResolveObject(&object, repository, revision);
  if (error < 0) return error;
  error = git_object_peel(reinterpret_cast<git_object **>(tree), object, GIT_OBJECT_TREE);
  git_object_free(object);
  return error;
}

static int CancelDiffNotify(const git_diff *, const git_diff_delta *, const char *, void *payload) {
  auto *cancelled = static_cast<std::atomic_bool *>(payload);
  return cancelled && cancelled->load(std::memory_order_relaxed) ? GIT_EUSER : 0;
}

static void ConfigureDiffOptions(git_diff_options *options, const Request &request,
                                 std::vector<char *> *pathspec) {
  git_diff_options_init(options, GIT_DIFF_OPTIONS_VERSION);
  options->context_lines = request.context < 0 ? 0 : static_cast<uint32_t>(request.context);
  options->old_prefix = "a";
  options->new_prefix = "b";
  // Submodule dirtiness is represented in the status snapshot; diff callers
  // need the gitlink OID/mode change without recursively scanning each child.
  options->ignore_submodules = GIT_SUBMODULE_IGNORE_DIRTY;
  options->flags |= GIT_DIFF_INCLUDE_TYPECHANGE | GIT_DIFF_INCLUDE_TYPECHANGE_TREES;
  if (request.ignore_whitespace) options->flags |= GIT_DIFF_IGNORE_WHITESPACE;
  for (const std::string &path : request.paths) pathspec->push_back(const_cast<char *>(path.c_str()));
  options->pathspec.strings = pathspec->empty() ? nullptr : pathspec->data();
  options->pathspec.count = pathspec->size();
  if (request.cancelled) {
    options->notify_cb = CancelDiffNotify;
    options->payload = request.cancelled.get();
  }
}

static int BuildRepositoryDiff(git_diff **diff, git_repository *repository, const Request &request,
                               git_diff_options *options) {
  git_tree *raw_from_tree = nullptr;
  git_tree *raw_to_tree = nullptr;
  git_ptr<git_tree, git_tree_free> from_tree(nullptr, git_tree_free);
  git_ptr<git_tree, git_tree_free> to_tree(nullptr, git_tree_free);
  int error = 0;
  if (request.from.type == "commit") {
    error = ResolveTree(&raw_from_tree, repository, request.from.revision);
    if (error < 0) return error;
    from_tree.reset(raw_from_tree);
  }
  if (request.to.type == "commit") {
    error = ResolveTree(&raw_to_tree, repository, request.to.revision);
    if (error < 0) return error;
    to_tree.reset(raw_to_tree);
  }

  if (request.from.type == "index" && request.to.type == "worktree") {
    git_index *raw_index = nullptr;
    error = git_repository_index(&raw_index, repository);
    if (error < 0) return error;
    git_ptr<git_index, git_index_free> index(raw_index, git_index_free);
    error = git_diff_index_to_workdir(diff, repository, index.get(), options);
  } else if (request.from.type == "commit" && request.to.type == "index") {
    git_index *raw_index = nullptr;
    error = git_repository_index(&raw_index, repository);
    if (error < 0) return error;
    git_ptr<git_index, git_index_free> index(raw_index, git_index_free);
    error = git_diff_tree_to_index(diff, repository, from_tree.get(), index.get(), options);
  } else if (request.from.type == "commit" && request.to.type == "worktree") {
    error = git_diff_tree_to_workdir_with_index(diff, repository, from_tree.get(), options);
  } else if (request.from.type == "commit" && request.to.type == "commit") {
    error = git_diff_tree_to_tree(diff, repository, from_tree.get(), to_tree.get(), options);
  } else if (request.from.type == "empty" && request.to.type == "commit") {
    error = git_diff_tree_to_tree(diff, repository, nullptr, to_tree.get(), options);
  } else if (request.from.type == "commit" && request.to.type == "empty") {
    error = git_diff_tree_to_tree(diff, repository, from_tree.get(), nullptr, options);
  } else if (request.from.type == "empty" && request.to.type == "index") {
    git_index *raw_index = nullptr;
    error = git_repository_index(&raw_index, repository);
    if (error < 0) return error;
    git_ptr<git_index, git_index_free> index(raw_index, git_index_free);
    error = git_diff_tree_to_index(diff, repository, nullptr, index.get(), options);
  } else if (request.from.type == "index" && request.to.type == "empty") {
    git_index *raw_index = nullptr;
    error = git_repository_index(&raw_index, repository);
    if (error < 0) return error;
    git_ptr<git_index, git_index_free> index(raw_index, git_index_free);
    options->flags |= GIT_DIFF_REVERSE;
    error = git_diff_tree_to_index(diff, repository, nullptr, index.get(), options);
  } else if (request.from.type == "empty" && request.to.type == "worktree") {
    error = git_diff_tree_to_workdir_with_index(diff, repository, nullptr, options);
  } else if (request.from.type == "worktree" && request.to.type == "empty") {
    options->flags |= GIT_DIFF_REVERSE;
    error = git_diff_tree_to_workdir_with_index(diff, repository, nullptr, options);
  } else {
    git_error_set_str(GIT_ERROR_INVALID, "unsupported repository diff endpoint pair");
    return GIT_EINVALID;
  }
  if (error < 0) return error;
  if (request.detect_renames) {
    git_diff_find_options find_options = GIT_DIFF_FIND_OPTIONS_INIT;
    find_options.flags = GIT_DIFF_FIND_RENAMES | GIT_DIFF_FIND_COPIES |
      GIT_DIFF_FIND_REWRITES | GIT_DIFF_FIND_FOR_UNTRACKED;
    error = git_diff_find_similar(*diff, &find_options);
  }
  return error;
}

static std::string DeltaStatusName(git_delta_t status);

static char DeltaFilterLetter(git_delta_t status) {
  switch (status) {
    case GIT_DELTA_ADDED:
    case GIT_DELTA_UNTRACKED: return 'A';
    case GIT_DELTA_COPIED: return 'C';
    case GIT_DELTA_DELETED: return 'D';
    case GIT_DELTA_MODIFIED: return 'M';
    case GIT_DELTA_RENAMED: return 'R';
    case GIT_DELTA_TYPECHANGE: return 'T';
    case GIT_DELTA_CONFLICTED: return 'U';
    case GIT_DELTA_UNREADABLE:
    case GIT_DELTA_IGNORED: return 'X';
    default: return 'M';
  }
}

static bool DeltaMatchesFilter(const git_diff_delta *delta, const std::string &filter) {
  if (!delta || filter.empty()) return delta != nullptr;
  const char letter = DeltaFilterLetter(delta->status);
  std::set<char> includes;
  std::set<char> excludes;
  for (unsigned char value : filter) {
    if (value == '*') continue;
    if (std::isupper(value)) includes.insert(static_cast<char>(value));
    else if (std::islower(value)) excludes.insert(static_cast<char>(std::toupper(value)));
  }
  if (!includes.empty() && includes.count(letter) == 0) return false;
  return excludes.count(letter) == 0;
}

static std::string TrimDiffLine(const git_diff_line *line) {
  std::string content(line->content ? line->content : "", line->content_len);
  if (!content.empty() && content.back() == '\n') content.pop_back();
  return content;
}

static int StructuredPatchJson(std::string *json, git_patch *patch) {
  const git_diff_delta *delta = git_patch_get_delta(patch);
  if (!delta) {
    git_error_set_str(GIT_ERROR_INVALID, "diff patch has no delta");
    return GIT_EINVALID;
  }
  std::string old_path = delta->old_file.path ? delta->old_file.path : "";
  std::string new_path = delta->new_file.path ? delta->new_file.path : "";
  std::string status = DeltaStatusName(delta->status);
  std::vector<std::string> hunks;
  for (size_t hunk_index = 0; hunk_index < git_patch_num_hunks(patch); ++hunk_index) {
    const git_diff_hunk *hunk = nullptr;
    size_t line_count = 0;
    int error = git_patch_get_hunk(&hunk, &line_count, patch, hunk_index);
    if (error < 0) return error;
    if (!hunk) {
      git_error_set_str(GIT_ERROR_INVALID, "diff patch returned a null hunk");
      return GIT_EINVALID;
    }
    std::vector<std::string> lines;
    for (size_t line_index = 0; line_index < line_count; ++line_index) {
      const git_diff_line *line = nullptr;
      error = git_patch_get_line_in_hunk(&line, patch, hunk_index, line_index);
      if (error < 0) return error;
      if (!line) {
        git_error_set_str(GIT_ERROR_INVALID, "diff patch returned a null line");
        return GIT_EINVALID;
      }
      std::string kind;
      switch (line->origin) {
        case GIT_DIFF_LINE_ADDITION: kind = "added"; break;
        case GIT_DIFF_LINE_DELETION: kind = "deleted"; break;
        case GIT_DIFF_LINE_CONTEXT: kind = "context"; break;
        case GIT_DIFF_LINE_ADD_EOFNL:
        case GIT_DIFF_LINE_DEL_EOFNL:
        case GIT_DIFF_LINE_CONTEXT_EOFNL: kind = "nonewline"; break;
        default: continue;
      }
      lines.push_back("{\"kind\":" + JsonString(kind) + ",\"text\":" +
                      (kind == "nonewline" ? "\"\"" : JsonString(TrimDiffLine(line))) + '}');
    }
    std::string header(hunk->header, hunk->header_len);
    size_t marker = header.find("@@", 2);
    std::string heading;
    if (marker != std::string::npos) {
      heading = header.substr(marker + 2);
      while (!heading.empty() && std::isspace(static_cast<unsigned char>(heading.front()))) heading.erase(heading.begin());
      while (!heading.empty() && std::isspace(static_cast<unsigned char>(heading.back()))) heading.pop_back();
    }
    std::ostringstream out;
    out << "{\"oldStart\":" << hunk->old_start << ",\"oldLines\":" << hunk->old_lines
        << ",\"newStart\":" << hunk->new_start << ",\"newLines\":" << hunk->new_lines
        << ",\"heading\":" << (heading.empty() ? "null" : JsonString(heading))
        << ",\"lines\":[" << Join(lines) << "]}";
    hunks.push_back(out.str());
  }
  const bool rename = delta->status == GIT_DELTA_RENAMED || delta->status == GIT_DELTA_COPIED;
  const bool binary = (delta->flags & GIT_DIFF_FLAG_BINARY) != 0;
  std::ostringstream out;
  out << "{\"oldPath\":" << (delta->status == GIT_DELTA_ADDED ? "null" : JsonString(old_path))
      << ",\"newPath\":" << (delta->status == GIT_DELTA_DELETED ? "null" : JsonString(new_path))
      << ",\"status\":" << JsonString(status)
      << ",\"similarity\":" << (rename ? std::to_string(delta->similarity) : "null")
      << ",\"binary\":" << (binary ? "true" : "false")
      << ",\"oldMode\":" << (delta->old_file.mode ? JsonString(ModeString(delta->old_file.mode)) : "null")
      << ",\"newMode\":" << (delta->new_file.mode ? JsonString(ModeString(delta->new_file.mode)) : "null")
      << ",\"hunks\":[" << Join(hunks) << "]}";
  *json = out.str();
  return 0;
}

static int BuildSelectedDiff(std::string *files_json, std::string *raw_patch,
                             git_diff *diff, const Request &request) {
  std::vector<std::string> files;
  std::string patch_text;
  const bool all_or_none = request.diff_filter.find('*') != std::string::npos;
  bool select_all = false;
  if (all_or_none) {
    for (size_t index = 0; index < git_diff_num_deltas(diff); ++index) {
      if (DeltaMatchesFilter(git_diff_get_delta(diff, index), request.diff_filter)) {
        select_all = true;
        break;
      }
    }
  }
  for (size_t index = 0; index < git_diff_num_deltas(diff); ++index) {
    if (IsCancelled(request)) return GIT_EUSER;
    const git_diff_delta *delta = git_diff_get_delta(diff, index);
    if ((all_or_none && !select_all) ||
        (!all_or_none && !DeltaMatchesFilter(delta, request.diff_filter))) continue;
    git_patch *raw_patch_object = nullptr;
    int error = git_patch_from_diff(&raw_patch_object, diff, index);
    if (error < 0) return error;
    git_ptr<git_patch, git_patch_free> patch(raw_patch_object, git_patch_free);
    if (request.format != "patch") {
      std::string file;
      error = StructuredPatchJson(&file, patch.get());
      if (error < 0) return error;
      files.push_back(std::move(file));
    }
    if (request.format != "structured") {
      git_buf buffer = GIT_BUF_INIT;
      error = git_patch_to_buf(&buffer, patch.get());
      if (error < 0) {
        git_buf_dispose(&buffer);
        return error;
      }
      if (buffer.ptr) patch_text.append(buffer.ptr, buffer.size);
      git_buf_dispose(&buffer);
    }
  }
  *files_json = '[' + Join(files) + ']';
  *raw_patch = std::move(patch_text);
  return 0;
}

static int BuildDiffJson(std::string *json, git_repository *repository, const Request &request) {
  git_diff_options options;
  std::vector<char *> pathspec;
  ConfigureDiffOptions(&options, request, &pathspec);
  git_buf patch_buffer = GIT_BUF_INIT;
  std::string files = "[]";
  std::string raw_patch;
  int error = 0;

  const bool buffer_pair =
    (request.from.type == "file" || request.from.type == "empty") &&
    (request.to.type == "file" || request.to.type == "empty") &&
    (request.from.type == "file" || request.to.type == "file");
  if (buffer_pair) {
    std::string old_content, new_content;
    if (request.from.type == "file" && !ReadFile(ResolvePath(request, request.from.path), &old_content)) {
      git_error_set_str(GIT_ERROR_OS, "unable to read old diff file");
      return GIT_ERROR;
    }
    if (request.to.type == "file" && !ReadFile(ResolvePath(request, request.to.path), &new_content)) {
      git_error_set_str(GIT_ERROR_OS, "unable to read new diff file");
      return GIT_ERROR;
    }
    git_patch *raw_patch = nullptr;
    error = git_patch_from_buffers(
      &raw_patch,
      request.from.type == "empty" ? nullptr : old_content.data(), old_content.size(),
      request.from.type == "empty" ? nullptr : request.from.path.c_str(),
      request.to.type == "empty" ? nullptr : new_content.data(), new_content.size(),
      request.to.type == "empty" ? nullptr : request.to.path.c_str(),
      &options
    );
    if (error < 0) return error;
    git_ptr<git_patch, git_patch_free> patch(raw_patch, git_patch_free);
    const git_diff_delta *delta = patch ? git_patch_get_delta(patch.get()) : nullptr;
    if (patch && DeltaMatchesFilter(delta, request.diff_filter)) {
      if (request.format != "patch") {
        std::string file;
        error = StructuredPatchJson(&file, patch.get());
        if (error < 0) return error;
        files = '[' + file + ']';
      }
      if (request.format != "structured") error = git_patch_to_buf(&patch_buffer, patch.get());
    }
  } else {
    git_diff *raw_diff = nullptr;
    error = BuildRepositoryDiff(&raw_diff, repository, request, &options);
    if (error < 0) return error;
    git_ptr<git_diff, git_diff_free> diff(raw_diff, git_diff_free);
    error = BuildSelectedDiff(&files, &raw_patch, diff.get(), request);
  }
  if (error < 0) {
    git_buf_dispose(&patch_buffer);
    return error;
  }
  if (patch_buffer.ptr) raw_patch.assign(patch_buffer.ptr, patch_buffer.size);
  git_buf_dispose(&patch_buffer);
  std::ostringstream out;
  out << "{\"files\":" << files;
  if (request.format != "structured") out << ",\"rawPatch\":" << JsonString(raw_patch);
  out << '}';
  *json = out.str();
  return 0;
}

static std::string DeltaStatusName(git_delta_t status) {
  switch (status) {
    case GIT_DELTA_ADDED: return "added";
    case GIT_DELTA_DELETED: return "deleted";
    case GIT_DELTA_RENAMED: return "renamed";
    case GIT_DELTA_COPIED: return "copied";
    case GIT_DELTA_TYPECHANGE: return "typechange";
    case GIT_DELTA_CONFLICTED: return "unmerged";
    case GIT_DELTA_MODIFIED: return "modified";
    default: return "unknown";
  }
}

static int CommitFilesJson(std::string *json, git_repository *repository, git_commit *commit) {
  git_tree *raw_tree = nullptr;
  int error = git_commit_tree(&raw_tree, commit);
  if (error < 0) return error;
  git_ptr<git_tree, git_tree_free> tree(raw_tree, git_tree_free);
  git_tree *raw_parent_tree = nullptr;
  git_ptr<git_tree, git_tree_free> parent_tree(nullptr, git_tree_free);
  if (git_commit_parentcount(commit) > 0) {
    git_commit *raw_parent = nullptr;
    error = git_commit_parent(&raw_parent, commit, 0);
    if (error < 0) return error;
    git_ptr<git_commit, git_commit_free> parent(raw_parent, git_commit_free);
    error = git_commit_tree(&raw_parent_tree, parent.get());
    if (error < 0) return error;
    parent_tree.reset(raw_parent_tree);
  }
  git_diff_options options = GIT_DIFF_OPTIONS_INIT;
  options.flags |= GIT_DIFF_INCLUDE_TYPECHANGE | GIT_DIFF_INCLUDE_TYPECHANGE_TREES;
  git_diff *raw_diff = nullptr;
  error = git_diff_tree_to_tree(&raw_diff, repository, parent_tree.get(), tree.get(), &options);
  if (error < 0) return error;
  git_ptr<git_diff, git_diff_free> diff(raw_diff, git_diff_free);
  git_diff_find_options find_options = GIT_DIFF_FIND_OPTIONS_INIT;
  find_options.flags = GIT_DIFF_FIND_RENAMES | GIT_DIFF_FIND_COPIES;
  if ((error = git_diff_find_similar(diff.get(), &find_options)) < 0) return error;

  std::vector<std::string> files;
  for (size_t i = 0; i < git_diff_num_deltas(diff.get()); ++i) {
    const git_diff_delta *delta = git_diff_get_delta(diff.get(), i);
    if (!delta || delta->status == GIT_DELTA_UNMODIFIED) continue;
    std::string old_path = delta->old_file.path ? delta->old_file.path : "";
    std::string new_path = delta->new_file.path ? delta->new_file.path : old_path;
    bool rename = delta->status == GIT_DELTA_RENAMED || delta->status == GIT_DELTA_COPIED;
    std::ostringstream out;
    out << "{\"path\":" << JsonString(new_path)
        << ",\"originalPath\":" << (rename ? JsonString(old_path) : "null")
        << ",\"status\":" << JsonString(DeltaStatusName(delta->status))
        << ",\"similarity\":" << (rename ? std::to_string(delta->similarity) : "null") << '}';
    files.push_back(out.str());
  }
  *json = '[' + Join(files) + ']';
  return 0;
}

static int BuildHistoryJson(std::string *json, git_repository *repository, const Request &request) {
  git_commit *raw_start = nullptr;
  int error = ResolveCommit(&raw_start, repository, request.revision);
  if (error == GIT_EUNBORNBRANCH || error == GIT_ENOTFOUND) {
    git_error_clear();
    *json = "[]";
    return 0;
  }
  if (error < 0) return error;
  git_ptr<git_commit, git_commit_free> start(raw_start, git_commit_free);
  git_revwalk *raw_walk = nullptr;
  if ((error = git_revwalk_new(&raw_walk, repository)) < 0) return error;
  git_ptr<git_revwalk, git_revwalk_free> walk(raw_walk, git_revwalk_free);
  git_revwalk_sorting(walk.get(), GIT_SORT_TIME | GIT_SORT_TOPOLOGICAL);
  if ((error = git_revwalk_push(walk.get(), git_commit_id(start.get()))) < 0) return error;

  std::vector<std::string> commits;
  git_oid oid;
  int seen = 0;
  while ((error = git_revwalk_next(&oid, walk.get())) == 0) {
    if (IsCancelled(request)) return GIT_EUSER;
    if (seen++ < request.skip) continue;
    if (request.limit >= 0 && static_cast<int>(commits.size()) >= request.limit) break;
    git_commit *raw_commit = nullptr;
    if ((error = git_commit_lookup(&raw_commit, repository, &oid)) < 0) return error;
    git_ptr<git_commit, git_commit_free> commit(raw_commit, git_commit_free);
    commits.push_back(CommitJson(commit.get()));
  }
  if (error != GIT_ITEROVER && error < 0) return error;
  git_error_clear();
  *json = '[' + Join(commits) + ']';
  return 0;
}

static int BuildCommitJson(std::string *json, git_repository *repository, const Request &request) {
  git_commit *raw_commit = nullptr;
  int error = ResolveCommit(&raw_commit, repository, request.revision);
  if (error < 0) return error;
  git_ptr<git_commit, git_commit_free> commit(raw_commit, git_commit_free);
  std::string files;
  if ((error = CommitFilesJson(&files, repository, commit.get())) < 0) return error;
  std::string value = CommitJson(commit.get());
  if (!value.empty() && value.back() == '}') value.pop_back();
  *json = value + ",\"files\":" + files + '}';
  return 0;
}

static int ParseOid(git_oid *oid, git_repository *repository, const std::string &value) {
#ifdef GIT_EXPERIMENTAL_SHA256
  return git_oid_fromstrp(oid, value.c_str(), git_repository_oid_type(repository));
#else
  return git_oid_fromstrp(oid, value.c_str());
#endif
}

static int ResolveRequestedOid(git_oid *oid, git_repository *repository,
                               const ObjectRequest &request) {
  if (request.source == "index") {
    git_index *raw_index = nullptr;
    int error = git_repository_index(&raw_index, repository);
    if (error < 0) return error;
    git_ptr<git_index, git_index_free> index(raw_index, git_index_free);
    const git_index_entry *entry = git_index_get_bypath(index.get(), request.path.c_str(), 0);
    if (!entry) return GIT_ENOTFOUND;
    git_oid_cpy(oid, &entry->id);
    return 0;
  }
  if (!request.oid.empty()) return ParseOid(oid, repository, request.oid);
  git_object *raw_object = nullptr;
  int error = ResolveObject(&raw_object, repository, request.revision);
  if (error < 0) return error;
  git_ptr<git_object, git_object_free> object(raw_object, git_object_free);
  git_tree *raw_tree = nullptr;
  error = git_object_peel(reinterpret_cast<git_object **>(&raw_tree), object.get(), GIT_OBJECT_TREE);
  if (error < 0) return error;
  git_ptr<git_tree, git_tree_free> tree(raw_tree, git_tree_free);
  git_tree_entry *raw_entry = nullptr;
  error = git_tree_entry_bypath(&raw_entry, tree.get(), request.path.c_str());
  if (error < 0) return error;
  git_ptr<git_tree_entry, git_tree_entry_free> entry(raw_entry, git_tree_entry_free);
  git_oid_cpy(oid, git_tree_entry_id(entry.get()));
  return 0;
}

static int BuildReadObjectsJson(std::string *json, git_repository *repository,
                                const Request &request) {
  git_odb *raw_odb = nullptr;
  int error = git_repository_odb(&raw_odb, repository);
  if (error < 0) return error;
  git_ptr<git_odb, git_odb_free> odb(raw_odb, git_odb_free);
  std::vector<std::string> objects;
  objects.reserve(request.objects.size());
  for (const ObjectRequest &object_request : request.objects) {
    if (IsCancelled(request)) return GIT_EUSER;
    git_oid oid;
    error = ResolveRequestedOid(&oid, repository, object_request);
    if (error == GIT_ENOTFOUND || error == GIT_EINVALIDSPEC) {
      objects.push_back("null");
      git_error_clear();
      continue;
    }
    if (error < 0) return error;
    git_odb_object *raw_object = nullptr;
    error = git_odb_read(&raw_object, odb.get(), &oid);
    if (error == GIT_ENOTFOUND) {
      objects.push_back("null");
      git_error_clear();
      continue;
    }
    if (error < 0) return error;
    git_ptr<git_odb_object, git_odb_object_free> object(raw_object, git_odb_object_free);
    const size_t size = git_odb_object_size(object.get());
    std::ostringstream out;
    out << "{\"oid\":" << JsonString(OidString(git_odb_object_id(object.get())))
        << ",\"type\":" << JsonString(ObjectTypeName(git_odb_object_type(object.get())))
        << ",\"size\":" << size
        << ",\"content\":" << JsonString(Base64Encode(git_odb_object_data(object.get()), size)) << '}';
    objects.push_back(out.str());
  }
  *json = '[' + Join(objects) + ']';
  return 0;
}

static int BuildReadConfigJson(std::string *json, git_repository *repository,
                               const Request &request) {
  git_config *raw_config = nullptr;
  int error = git_repository_config(&raw_config, repository);
  if (error < 0) return error;
  git_ptr<git_config, git_config_free> config(raw_config, git_config_free);
  std::vector<std::string> entries;
  entries.reserve(request.keys.size());
  for (const std::string &key : request.keys) {
    git_buf value = GIT_BUF_INIT;
    error = git_config_get_string_buf(&value, config.get(), key.c_str());
    if (error == GIT_ENOTFOUND) {
      entries.push_back(JsonString(key) + ":null");
      git_buf_dispose(&value);
      git_error_clear();
      continue;
    }
    if (error < 0) {
      git_buf_dispose(&value);
      return error;
    }
    entries.push_back(JsonString(key) + ':' + JsonString(std::string(value.ptr, value.size)));
    git_buf_dispose(&value);
  }
  *json = '{' + Join(entries) + '}';
  return 0;
}

static int BuildFileModeJson(std::string *json, git_repository *repository,
                             const Request &request) {
  git_index *raw_index = nullptr;
  int error = git_repository_index(&raw_index, repository);
  if (error < 0) return error;
  git_ptr<git_index, git_index_free> index(raw_index, git_index_free);
  const git_index_entry *entry = git_index_get_bypath(index.get(), request.path.c_str(), 0);
  *json = entry ? JsonString(ModeString(entry->mode)) : "null";
  return 0;
}

static int CollectSubmodule(git_submodule *submodule, const char *, void *payload) {
  auto *paths = static_cast<std::vector<std::string> *>(payload);
  const char *path = git_submodule_path(submodule);
  if (path) paths->push_back(JsonString(path));
  return 0;
}

static int BuildSubmodulePathsJson(std::string *json, git_repository *repository) {
  std::vector<std::string> paths;
  int error = git_submodule_foreach(repository, CollectSubmodule, &paths);
  if (error < 0) return error;
  std::sort(paths.begin(), paths.end());
  *json = '[' + Join(paths) + ']';
  return 0;
}

static int BuildBlameJson(std::string *json, git_repository *repository, const Request &request) {
  if (request.path.empty()) {
    git_error_set_str(GIT_ERROR_INVALID, "blame path is required");
    return GIT_EINVALID;
  }
  git_blame_options options = GIT_BLAME_OPTIONS_INIT;
  if (request.ignore_whitespace) options.flags |= GIT_BLAME_IGNORE_WHITESPACE;
  if (!request.revision.empty() && request.revision != "HEAD") {
    git_commit *raw_commit = nullptr;
    int error = ResolveCommit(&raw_commit, repository, request.revision);
    if (error < 0) return error;
    git_ptr<git_commit, git_commit_free> commit(raw_commit, git_commit_free);
    git_oid_cpy(&options.newest_commit, git_commit_id(commit.get()));
  }
  git_blame *raw_blame = nullptr;
  int error = git_blame_file(&raw_blame, repository, request.path.c_str(), &options);
  if (error < 0) return error;
  if (IsCancelled(request)) {
    git_blame_free(raw_blame);
    return GIT_EUSER;
  }
  git_ptr<git_blame, git_blame_free> blame(raw_blame, git_blame_free);
  std::vector<std::string> rows;
  for (uint32_t index = 0; index < git_blame_get_hunk_count(blame.get()); ++index) {
    if (IsCancelled(request)) return GIT_EUSER;
    const git_blame_hunk *hunk = git_blame_get_hunk_byindex(blame.get(), index);
    if (!hunk) continue;
    std::string summary;
    git_commit *raw_commit = nullptr;
    if (git_commit_lookup(&raw_commit, repository, &hunk->final_commit_id) == 0) {
      git_ptr<git_commit, git_commit_free> commit(raw_commit, git_commit_free);
      summary = git_commit_summary(commit.get()) ? git_commit_summary(commit.get()) : "";
    } else {
      git_error_clear();
    }
    for (size_t offset = 0; offset < hunk->lines_in_hunk; ++offset) {
      const git_signature *signature = hunk->final_signature;
      std::ostringstream out;
      out << "{\"line\":" << (hunk->final_start_line_number + offset)
          << ",\"originalLine\":" << (hunk->orig_start_line_number + offset)
          << ",\"sha\":" << JsonString(OidString(&hunk->final_commit_id))
          << ",\"author\":{\"name\":" << JsonNullable(signature ? signature->name : nullptr)
          << ",\"email\":" << JsonNullable(signature ? signature->email : nullptr)
          << ",\"date\":" << (signature ? static_cast<int64_t>(signature->when.time) * 1000 : 0) << '}'
          << ",\"summary\":" << (summary.empty() ? "null" : JsonString(summary)) << '}';
      rows.push_back(out.str());
    }
  }
  *json = '[' + Join(rows) + ']';
  return 0;
}

static int CommitDistance(size_t *distance, git_repository *repository,
                          const git_oid *tip, const git_oid *target,
                          const Request &request) {
  if (git_oid_equal(tip, target)) {
    *distance = 0;
    return 0;
  }
  std::vector<std::pair<git_oid, size_t>> queue;
  std::set<std::string> visited;
  git_oid tip_copy;
  git_oid_cpy(&tip_copy, tip);
  queue.emplace_back(tip_copy, 0);
  visited.insert(OidString(tip));
  for (size_t cursor = 0; cursor < queue.size(); ++cursor) {
    if (IsCancelled(request)) return GIT_EUSER;
    git_commit *raw_commit = nullptr;
    int error = git_commit_lookup(&raw_commit, repository, &queue[cursor].first);
    if (error < 0) return error;
    git_ptr<git_commit, git_commit_free> commit(raw_commit, git_commit_free);
    for (unsigned int parent_index = 0; parent_index < git_commit_parentcount(commit.get()); ++parent_index) {
      const git_oid *parent = git_commit_parent_id(commit.get(), parent_index);
      if (git_oid_equal(parent, target)) {
        *distance = queue[cursor].second + 1;
        return 0;
      }
      std::string key = OidString(parent);
      if (!visited.insert(key).second) continue;
      git_oid copy;
      git_oid_cpy(&copy, parent);
      queue.emplace_back(copy, queue[cursor].second + 1);
    }
  }
  return GIT_ENOTFOUND;
}

static int BuildDescribeJson(std::string *json, git_repository *repository,
                             const Request &request) {
  git_commit *raw_head = nullptr;
  int error = ResolveCommit(&raw_head, repository, "HEAD");
  if (error == GIT_EUNBORNBRANCH || error == GIT_ENOTFOUND) {
    git_error_clear();
    *json = "\"\"";
    return 0;
  }
  if (error < 0) return error;
  git_ptr<git_commit, git_commit_free> head(raw_head, git_commit_free);
  const git_oid *head_oid = git_commit_id(head.get());

  struct Candidate { size_t distance; int rank; std::string name; };
  std::vector<Candidate> candidates;
  git_reference_iterator *raw_iterator = nullptr;
  if ((error = git_reference_iterator_new(&raw_iterator, repository)) < 0) return error;
  git_ptr<git_reference_iterator, git_reference_iterator_free> iterator(raw_iterator, git_reference_iterator_free);
  git_reference *raw_reference = nullptr;
  while ((error = git_reference_next(&raw_reference, iterator.get())) == 0) {
    if (IsCancelled(request)) return GIT_EUSER;
    git_ptr<git_reference, git_reference_free> reference(raw_reference, git_reference_free);
    raw_reference = nullptr;
    const char *full_name = git_reference_name(reference.get());
    if (!full_name || std::string(full_name).rfind("refs/", 0) != 0) continue;
    git_object *raw_tip = nullptr;
    int peel_error = git_reference_peel(&raw_tip, reference.get(), GIT_OBJECT_COMMIT);
    if (peel_error == GIT_ENOTFOUND || peel_error == GIT_EINVALIDSPEC) {
      git_error_clear();
      continue;
    }
    if (peel_error < 0) return peel_error;
    git_ptr<git_object, git_object_free> tip(raw_tip, git_object_free);
    const git_oid *tip_oid = git_object_id(tip.get());
    int contains = git_oid_equal(tip_oid, head_oid) ? 1 :
      git_graph_descendant_of(repository, tip_oid, head_oid);
    if (contains < 0) return contains;
    if (!contains) continue;
    size_t distance = 0;
    if ((peel_error = CommitDistance(&distance, repository, tip_oid, head_oid, request)) < 0) return peel_error;
    std::string full(full_name);
    int rank = full.rfind("refs/tags/", 0) == 0 ? 0 : full.rfind("refs/heads/", 0) == 0 ? 1 : 2;
    std::string name = git_reference_shorthand(reference.get())
      ? git_reference_shorthand(reference.get()) : full.substr(std::strlen("refs/"));
    candidates.push_back({distance, rank, name});
  }
  if (error != GIT_ITEROVER) return error;
  git_error_clear();
  if (candidates.empty()) {
    *json = JsonString(ShortOid(head_oid));
    return 0;
  }
  std::sort(candidates.begin(), candidates.end(), [](const Candidate &left, const Candidate &right) {
    if (left.distance != right.distance) return left.distance < right.distance;
    if (left.rank != right.rank) return left.rank < right.rank;
    return left.name < right.name;
  });
  std::string result = candidates[0].name;
  if (candidates[0].distance > 0) result += '~' + std::to_string(candidates[0].distance);
  *json = JsonString(result);
  return 0;
}

static bool GlobMatch(const char *pattern, const char *value) {
  if (!pattern || !*pattern) return !value || !*value;
  if (*pattern == '*') {
    while (*(pattern + 1) == '*') ++pattern;
    return GlobMatch(pattern + 1, value) || (value && *value && GlobMatch(pattern, value + 1));
  }
  if (*pattern == '?') return value && *value && GlobMatch(pattern + 1, value + 1);
  return value && *pattern == *value && GlobMatch(pattern + 1, value + 1);
}

static int BuildBranchesContainingJson(std::string *json, git_repository *repository,
                                       const Request &request) {
  git_commit *raw_commit = nullptr;
  int error = ResolveCommit(&raw_commit, repository, request.commit.empty() ? "HEAD" : request.commit);
  if (error < 0) return error;
  git_ptr<git_commit, git_commit_free> commit(raw_commit, git_commit_free);
  const git_oid *target_oid = git_commit_id(commit.get());
  std::vector<std::string> results;
  git_reference_iterator *raw_iterator = nullptr;
  if ((error = git_reference_iterator_new(&raw_iterator, repository)) < 0) return error;
  git_ptr<git_reference_iterator, git_reference_iterator_free> iterator(raw_iterator, git_reference_iterator_free);
  git_reference *raw_reference = nullptr;
  while ((error = git_reference_next(&raw_reference, iterator.get())) == 0) {
    git_ptr<git_reference, git_reference_free> reference(raw_reference, git_reference_free);
    raw_reference = nullptr;
    const char *name = git_reference_name(reference.get());
    if (!name) continue;
    std::string ref(name);
    bool local = ref.rfind("refs/heads/", 0) == 0;
    bool remote = ref.rfind("refs/remotes/", 0) == 0;
    bool include = request.show_remote
      ? (request.show_local ? local || remote : remote)
      : local;
    if (!include) continue;
    if (!request.pattern.empty() && !GlobMatch(request.pattern.c_str(), name) &&
        !GlobMatch(request.pattern.c_str(), git_reference_shorthand(reference.get()))) continue;
    git_reference *raw_resolved = nullptr;
    git_reference *candidate = reference.get();
    if (git_reference_type(candidate) == GIT_REFERENCE_SYMBOLIC) {
      if (git_reference_resolve(&raw_resolved, candidate) < 0) {
        git_error_clear();
        continue;
      }
      candidate = raw_resolved;
    }
    const git_oid *tip_oid = git_reference_target(candidate);
    int contains = tip_oid && git_oid_equal(tip_oid, target_oid)
      ? 1 : (tip_oid ? git_graph_descendant_of(repository, tip_oid, target_oid) : 0);
    git_reference_free(raw_resolved);
    if (contains < 0) return contains;
    if (contains == 1) results.push_back(JsonString(ref));
  }
  if (error != GIT_ITEROVER) return error;
  git_error_clear();
  std::sort(results.begin(), results.end());
  *json = '[' + Join(results) + ']';
  return 0;
}

struct LineDiffPayload {
  std::vector<std::string> hunks;
  std::atomic_bool *cancelled = nullptr;
};

static int LineDiffHunkCallback(const git_diff_delta *, const git_diff_hunk *hunk, void *payload) {
  auto *result = static_cast<LineDiffPayload *>(payload);
  if (result->cancelled && result->cancelled->load(std::memory_order_relaxed)) return GIT_EUSER;
  std::ostringstream out;
  out << "{\"oldStart\":" << hunk->old_start
      << ",\"oldLines\":" << hunk->old_lines
      << ",\"newStart\":" << (hunk->new_lines == 0 && hunk->new_start > 0 ? hunk->new_start - 1 : hunk->new_start)
      << ",\"newLines\":" << hunk->new_lines << '}';
  result->hunks.push_back(out.str());
  return 0;
}

static int BuildLineDiffJson(std::string *json, const Request &request) {
  git_diff_options options = GIT_DIFF_OPTIONS_INIT;
  options.context_lines = 0;
  options.interhunk_lines = 0;
  if (request.ignore_all_space) options.flags |= GIT_DIFF_IGNORE_WHITESPACE;
  else if (request.ignore_space_change) options.flags |= GIT_DIFF_IGNORE_WHITESPACE_CHANGE;
  else if (request.ignore_eol_whitespace) options.flags |= GIT_DIFF_IGNORE_WHITESPACE_EOL;
  LineDiffPayload payload;
  payload.cancelled = request.cancelled.get();
  int error = git_diff_buffers(
    request.old_text.data(), request.old_text.size(), "old",
    request.new_text.data(), request.new_text.size(), "new",
    &options, nullptr, nullptr, LineDiffHunkCallback, nullptr, &payload
  );
  if (error < 0) return error;
  *json = '[' + Join(payload.hunks) + ']';
  return 0;
}

static int OpenWritableConfig(git_config **config, git_repository *repository,
                              const std::string &scope) {
  git_config *raw_config = nullptr;
  int error = git_repository_config(&raw_config, repository);
  if (error < 0) return error;
  if (scope != "global") {
    *config = raw_config;
    return 0;
  }
  git_config *raw_global = nullptr;
  error = git_config_open_level(&raw_global, raw_config, GIT_CONFIG_LEVEL_GLOBAL);
  git_config_free(raw_config);
  if (error < 0) return error;
  *config = raw_global;
  return 0;
}

static int ParseOptionalIndexEntry(git_index_entry *entry, git_repository *repository,
                                   const std::string &path, const std::string &oid_value,
                                   uint32_t fallback_mode) {
  if (oid_value.empty()) return GIT_ENOTFOUND;
  std::memset(entry, 0, sizeof(*entry));
  entry->path = path.c_str();
  entry->mode = fallback_mode;
  return ParseOid(&entry->id, repository, oid_value);
}

static int ParseFileMode(uint32_t *mode, const std::string &value) {
  if (value.size() != 6 ||
      !std::all_of(value.begin(), value.end(), [](unsigned char c) { return c >= '0' && c <= '7'; })) {
    git_error_set_str(GIT_ERROR_INVALID, "file mode must be a six-digit octal string");
    return GIT_EINVALID;
  }
  const uint32_t parsed = static_cast<uint32_t>(std::strtoul(value.c_str(), nullptr, 8));
  if (parsed != GIT_FILEMODE_BLOB && parsed != GIT_FILEMODE_BLOB_EXECUTABLE &&
      parsed != GIT_FILEMODE_LINK && parsed != GIT_FILEMODE_COMMIT) {
    git_error_set_str(GIT_ERROR_INVALID, "unsupported Git file mode");
    return GIT_EINVALID;
  }
  *mode = parsed;
  return 0;
}

static int RunMutation(std::string *json, git_repository *repository, const Request &request) {
  const std::string &operation = request.operation;
  int error = 0;
  if (operation == "setConfig" || operation == "unsetConfig") {
    git_config *raw_config = nullptr;
    if ((error = OpenWritableConfig(&raw_config, repository, request.scope)) < 0) return error;
    git_ptr<git_config, git_config_free> config(raw_config, git_config_free);
    if (operation == "setConfig") {
      if (request.add) error = git_config_set_multivar(config.get(), request.key.c_str(), "$^", request.value.c_str());
      else if (request.replace_all) error = git_config_set_multivar(config.get(), request.key.c_str(), ".*", request.value.c_str());
      else error = git_config_set_string(config.get(), request.key.c_str(), request.value.c_str());
    } else if (request.all) {
      error = git_config_delete_multivar(config.get(), request.key.c_str(), ".*");
    } else {
      error = git_config_delete_entry(config.get(), request.key.c_str());
    }
    if (error == GIT_ENOTFOUND && operation == "unsetConfig") {
      git_error_clear();
      error = 0;
    }
    if (error < 0) return error;
    *json = "true";
    return 0;
  }

  if (operation == "addRemote") {
    git_remote *remote = nullptr;
    error = git_remote_create(&remote, repository, request.name.c_str(), request.url.c_str());
    git_remote_free(remote);
  } else if (operation == "removeRemote") {
    error = git_remote_delete(repository, request.name.c_str());
  } else if (operation == "setRemoteUrl") {
    error = git_remote_set_url(repository, request.name.c_str(), request.url.c_str());
  } else if (operation == "deleteRef") {
    git_reference *raw_reference = nullptr;
    error = git_reference_lookup(&raw_reference, repository, request.reference.c_str());
    if (error == GIT_ENOTFOUND) {
      git_error_clear();
      *json = "true";
      return 0;
    }
    if (error >= 0) {
      git_ptr<git_reference, git_reference_free> reference(raw_reference, git_reference_free);
      error = git_reference_delete(reference.get());
    }
  } else if (operation == "createBlob") {
    std::string content = request.content_encoding == "base64"
      ? Base64Decode(request.content) : request.content;
    if (!request.file_path.empty() && !ReadFile(ResolvePath(request, request.file_path), &content)) {
      git_error_set_str(GIT_ERROR_OS, "unable to read blob source file");
      return GIT_ERROR;
    }
    git_odb *raw_odb = nullptr;
    if ((error = git_repository_odb(&raw_odb, repository)) < 0) return error;
    git_ptr<git_odb, git_odb_free> odb(raw_odb, git_odb_free);
    git_oid oid;
    error = git_odb_write(&oid, odb.get(), content.data(), content.size(), GIT_OBJECT_BLOB);
    if (error >= 0) *json = JsonString(OidString(&oid));
    return error;
  } else if (operation == "expandBlobToFile") {
    git_oid oid;
    if ((error = ParseOid(&oid, repository, request.oid)) < 0) return error;
    git_blob *raw_blob = nullptr;
    if ((error = git_blob_lookup(&raw_blob, repository, &oid)) < 0) return error;
    git_ptr<git_blob, git_blob_free> blob(raw_blob, git_blob_free);
    std::string path = ResolvePath(request, request.path);
    if (!WriteFile(path, git_blob_rawcontent(blob.get()), git_blob_rawsize(blob.get()))) {
      git_error_set_str(GIT_ERROR_OS, "unable to write blob target file");
      return GIT_ERROR;
    }
    *json = JsonString(path);
    return 0;
  } else if (operation == "mergeFile") {
    std::string ours, base, theirs;
    if (!ReadFile(ResolvePath(request, request.ours_path), &ours) ||
        !ReadFile(ResolvePath(request, request.base_path), &base) ||
        !ReadFile(ResolvePath(request, request.theirs_path), &theirs)) {
      git_error_set_str(GIT_ERROR_OS, "unable to read merge input file");
      return GIT_ERROR;
    }
    git_merge_file_input ancestor_input = GIT_MERGE_FILE_INPUT_INIT;
    git_merge_file_input ours_input = GIT_MERGE_FILE_INPUT_INIT;
    git_merge_file_input theirs_input = GIT_MERGE_FILE_INPUT_INIT;
    ancestor_input.ptr = base.data(); ancestor_input.size = base.size(); ancestor_input.path = request.base_path.c_str();
    ours_input.ptr = ours.data(); ours_input.size = ours.size(); ours_input.path = request.ours_path.c_str();
    theirs_input.ptr = theirs.data(); theirs_input.size = theirs.size(); theirs_input.path = request.theirs_path.c_str();
    git_merge_file_options options = GIT_MERGE_FILE_OPTIONS_INIT;
    if (request.labels.size() > 0) options.our_label = request.labels[0].c_str();
    if (request.labels.size() > 1) options.ancestor_label = request.labels[1].c_str();
    if (request.labels.size() > 2) options.their_label = request.labels[2].c_str();
    git_merge_file_result result{};
    error = git_merge_file(&result, &ancestor_input, &ours_input, &theirs_input, &options);
    if (error < 0) return error;
    std::string result_path = ResolvePath(request, request.result_path);
    bool written = WriteFile(result_path, result.ptr, result.len);
    unsigned int automergeable = result.automergeable;
    git_merge_file_result_free(&result);
    if (!written) {
      git_error_set_str(GIT_ERROR_OS, "unable to write merge result file");
      return GIT_ERROR;
    }
    *json = automergeable ? "0" : "1";
    return 0;
  } else if (operation == "stageFileModeChange" || operation == "stageFileSymlinkChange" ||
             operation == "writeMergeConflictToIndex") {
    git_index *raw_index = nullptr;
    if ((error = git_repository_index(&raw_index, repository)) < 0) return error;
    git_ptr<git_index, git_index_free> index(raw_index, git_index_free);
    if (operation == "stageFileModeChange") {
      const git_index_entry *existing = git_index_get_bypath(index.get(), request.path.c_str(), 0);
      if (!existing) {
        git_error_set_str(GIT_ERROR_INDEX, "no index entry exists for path");
        return GIT_ENOTFOUND;
      }
      git_index_entry replacement = *existing;
      if ((error = ParseFileMode(&replacement.mode, request.mode)) < 0) return error;
      error = git_index_add(index.get(), &replacement);
    } else if (operation == "stageFileSymlinkChange") {
      error = git_index_remove_bypath(index.get(), request.path.c_str());
      if (error == GIT_ENOTFOUND) { error = 0; git_error_clear(); }
    } else {
      const git_index_entry *ancestor = nullptr, *ours = nullptr, *theirs = nullptr;
      int conflict_error = git_index_conflict_get(&ancestor, &ours, &theirs, index.get(), request.path.c_str());
      if (conflict_error < 0 && conflict_error != GIT_ENOTFOUND) return conflict_error;
      if (conflict_error == GIT_ENOTFOUND) git_error_clear();
      uint32_t fallback_mode = ours ? ours->mode : theirs ? theirs->mode :
        ancestor ? ancestor->mode : GIT_FILEMODE_BLOB;
      if (!request.mode.empty() && (error = ParseFileMode(&fallback_mode, request.mode)) < 0) return error;
      git_index_entry ancestor_entry{}, ours_entry{}, theirs_entry{};
      git_index_entry *ancestor_ptr = nullptr, *ours_ptr = nullptr, *theirs_ptr = nullptr;
      if (!request.base_oid.empty()) {
        if ((error = ParseOptionalIndexEntry(&ancestor_entry, repository, request.path, request.base_oid,
                                             ancestor ? ancestor->mode : fallback_mode)) < 0) return error;
        ancestor_ptr = &ancestor_entry;
      }
      if (!request.ours_oid.empty()) {
        if ((error = ParseOptionalIndexEntry(&ours_entry, repository, request.path, request.ours_oid,
                                             ours ? ours->mode : fallback_mode)) < 0) return error;
        ours_ptr = &ours_entry;
      }
      if (!request.theirs_oid.empty()) {
        if ((error = ParseOptionalIndexEntry(&theirs_entry, repository, request.path, request.theirs_oid,
                                             theirs ? theirs->mode : fallback_mode)) < 0) return error;
        theirs_ptr = &theirs_entry;
      }
      error = git_index_conflict_add(index.get(), ancestor_ptr, ours_ptr, theirs_ptr);
    }
    if (error >= 0) error = git_index_write(index.get());
  } else {
    git_error_set_str(GIT_ERROR_INVALID, "unsupported native mutation");
    return GIT_EINVALID;
  }
  if (error < 0) return error;
  *json = "true";
  return 0;
}

class NativeWorker final : public Napi::AsyncWorker {
public:
  NativeWorker(Napi::Env env, Request request, uint64_t cancel_id)
    : Napi::AsyncWorker(env), deferred_(Napi::Promise::Deferred::New(env)),
      request_(std::move(request)), cancel_id_(cancel_id) {}

  Napi::Promise Promise() const { return deferred_.Promise(); }

  void Execute() override {
    int error = 0;
    if (IsCancelled(request_)) {
      git_error_set_str(GIT_ERROR_CALLBACK, "native Git operation aborted");
      error = GIT_EUSER;
    } else if (request_.entrypoint == "lineDiff") {
      error = BuildLineDiffJson(&result_, request_);
    } else {
      git_repository *raw_repository = nullptr;
      error = OpenRepository(request_, &raw_repository);
      if (error >= 0) {
        git_ptr<git_repository, git_repository_free> repository(raw_repository, git_repository_free);
        if (request_.entrypoint == "snapshot") {
          std::string status, refs;
          if (request_.status) error = BuildStatusJson(&status, repository.get(), request_);
          if (error >= 0 && request_.refs) error = BuildRefsJson(&refs, repository.get(), request_);
          if (error >= 0) {
            std::vector<std::string> sections;
            if (request_.status) sections.push_back("\"status\":" + status);
            if (request_.refs) sections.push_back("\"refs\":" + refs);
            result_ = '{' + Join(sections) + '}';
          }
        } else if (request_.entrypoint == "diff") {
          error = BuildDiffJson(&result_, repository.get(), request_);
        } else if (request_.entrypoint == "history") {
          error = BuildHistoryJson(&result_, repository.get(), request_);
        } else if (request_.entrypoint == "commit") {
          error = BuildCommitJson(&result_, repository.get(), request_);
        } else if (request_.entrypoint == "blame") {
          error = BuildBlameJson(&result_, repository.get(), request_);
        } else if (request_.entrypoint == "describe") {
          error = BuildDescribeJson(&result_, repository.get(), request_);
        } else if (request_.entrypoint == "branchesContaining") {
          error = BuildBranchesContainingJson(&result_, repository.get(), request_);
        } else if (request_.entrypoint == "readObjects") {
          error = BuildReadObjectsJson(&result_, repository.get(), request_);
        } else if (request_.entrypoint == "readConfig") {
          error = BuildReadConfigJson(&result_, repository.get(), request_);
        } else if (request_.entrypoint == "fileMode") {
          error = BuildFileModeJson(&result_, repository.get(), request_);
        } else if (request_.entrypoint == "submodulePaths") {
          error = BuildSubmodulePathsJson(&result_, repository.get());
        } else if (request_.entrypoint == "mutate") {
          error = RunMutation(&result_, repository.get(), request_);
        } else {
          git_error_set_str(GIT_ERROR_INVALID, "unsupported native operation");
          error = GIT_EINVALID;
        }
      }
    }
    if (error >= 0 && IsCancelled(request_)) {
      git_error_set_str(GIT_ERROR_CALLBACK, "native Git operation aborted");
      error = GIT_EUSER;
    }
    if (error < 0) {
      CaptureFailure(error, &failure_, "native Git operation failed");
      SetError(failure_.message);
    }
  }

  void OnOK() override {
    ForgetCancellation();
    deferred_.Resolve(Napi::String::New(Env(), result_));
  }

  void OnError(const Napi::Error &error) override {
    ForgetCancellation();
    Napi::Object value = error.Value();
    const std::string operation = request_.entrypoint == "mutate" && !request_.operation.empty()
      ? request_.operation : request_.entrypoint;
    value.Set("code", NativeCode(operation));
    value.Set("operation", operation);
    value.Set("libgit2Code", failure_.code);
    value.Set("libgit2Class", failure_.klass);
    value.Set("libgit2Message", failure_.message);
    deferred_.Reject(value);
  }

private:
  void ForgetCancellation() {
    if (cancel_id_ == 0) return;
    std::lock_guard<std::mutex> lock(cancellation_mutex);
    cancellations.erase(cancel_id_);
  }

  Napi::Promise::Deferred deferred_;
  Request request_;
  uint64_t cancel_id_ = 0;
  std::string result_;
  Failure failure_;
};

static Napi::Value Run(const Napi::CallbackInfo &info) {
  Napi::Env env = info.Env();
  if (info.Length() < 3 || !info[0].IsString() || !info[1].IsObject() || !info[2].IsObject()) {
    Napi::TypeError error = Napi::TypeError::New(env, "run(operation, descriptor, request) is required");
    error.Set("code", "ERR_GIT_NATIVE_ARGUMENT");
    error.ThrowAsJavaScriptException();
    return env.Undefined();
  }
  Request request = ParseRequest(info);
  uint64_t cancel_id = info.Length() > 3 && info[3].IsNumber()
    ? static_cast<uint64_t>(info[3].As<Napi::Number>().Int64Value()) : 0;
  request.cancelled = std::make_shared<std::atomic_bool>(false);
  if (cancel_id != 0) {
    std::lock_guard<std::mutex> lock(cancellation_mutex);
    cancellations[cancel_id] = request.cancelled;
  }
  auto *worker = new NativeWorker(env, std::move(request), cancel_id);
  Napi::Promise promise = worker->Promise();
  worker->Queue();
  return promise;
}

static Napi::Value Cancel(const Napi::CallbackInfo &info) {
  if (info.Length() < 1 || !info[0].IsNumber()) return Napi::Boolean::New(info.Env(), false);
  uint64_t cancel_id = static_cast<uint64_t>(info[0].As<Napi::Number>().Int64Value());
  std::shared_ptr<std::atomic_bool> cancelled;
  {
    std::lock_guard<std::mutex> lock(cancellation_mutex);
    auto entry = cancellations.find(cancel_id);
    if (entry != cancellations.end()) cancelled = entry->second.lock();
  }
  if (!cancelled) return Napi::Boolean::New(info.Env(), false);
  cancelled->store(true, std::memory_order_relaxed);
  return Napi::Boolean::New(info.Env(), true);
}

static Napi::Value Versions(const Napi::CallbackInfo &info) {
  int major = 0, minor = 0, revision = 0;
  git_libgit2_version(&major, &minor, &revision);
  std::ostringstream version;
  version << major << '.' << minor << '.' << revision;
  Napi::Object result = Napi::Object::New(info.Env());
  result.Set("gitUtils", "10.0.0");
  result.Set("napi", NAPI_VERSION);
  result.Set("libgit2", version.str());
  result.Set("libgit2Features", git_libgit2_features());
  return result;
}

static Napi::Value Configure(const Napi::CallbackInfo &info) {
  Napi::Env env = info.Env();
  if (info.Length() < 1 || !info[0].IsBoolean()) {
    Napi::TypeError error = Napi::TypeError::New(env, "validateOwnership boolean is required");
    error.Set("code", "ERR_GIT_NATIVE_ARGUMENT");
    error.Set("operation", "configure");
    error.ThrowAsJavaScriptException();
    return env.Undefined();
  }
  bool enabled = info[0].As<Napi::Boolean>().Value();
  int result = git_libgit2_opts(GIT_OPT_SET_OWNER_VALIDATION, enabled ? 1 : 0);
  if (result < 0) {
    const git_error *last = git_error_last();
    Napi::Error error = Napi::Error::New(env, last && last->message ? last->message : "unable to configure ownership validation");
    error.Set("code", "ERR_GIT_NATIVE_CONFIGURE");
    error.Set("operation", "configure");
    error.Set("libgit2Code", Napi::Number::New(env, result));
    error.Set("libgit2Class", Napi::Number::New(env, last ? last->klass : 0));
    error.Set("libgit2Message", last && last->message ? last->message : "unable to configure ownership validation");
    error.ThrowAsJavaScriptException();
    return env.Undefined();
  }
  Napi::Object value = Napi::Object::New(env);
  value.Set("validateOwnership", enabled);
  return value;
}

static void ShutdownLibgit2() {
  git_libgit2_shutdown();
}

static Napi::Object Initialize(Napi::Env env, Napi::Object exports) {
  int major = 0, minor = 0, revision = 0;
  git_libgit2_version(&major, &minor, &revision);
  if (major < 1 || (major == 1 && minor < 9) || (major == 1 && minor == 9 && revision < 6)) {
    Napi::Error::New(env, "git-utils v10 requires libgit2 1.9.6 or newer").ThrowAsJavaScriptException();
    return exports;
  }
  git_libgit2_init();
  env.AddCleanupHook(ShutdownLibgit2);
  exports.Set("run", Napi::Function::New(env, Run));
  exports.Set("versions", Napi::Function::New(env, Versions));
  exports.Set("configure", Napi::Function::New(env, Configure));
  exports.Set("cancel", Napi::Function::New(env, Cancel));
  return exports;
}

} // namespace

NODE_API_MODULE(git, Initialize)
