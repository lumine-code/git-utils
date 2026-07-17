#include "binding.hh"
#include "js_native_api_types.h"
#include "napi.h"
#include <cstring>
#ifndef S_IFMT
#include <sys/stat.h>
#endif
#include <utility>

static unsigned GetCommitCount(
  git_repository *repository,
  git_oid *left_oid,
  git_oid *right_oid
) {
  git_revwalk* revwalk;
  if (git_revwalk_new(&revwalk, repository) != GIT_OK) return 0;
  git_revwalk_push(revwalk, left_oid);
  git_revwalk_hide(revwalk, right_oid);

  unsigned result = 0;
  git_oid current_commit;
  while (git_revwalk_next(&current_commit, revwalk) == GIT_OK) result++;
  git_revwalk_free(revwalk);

  return result;
}


git_repository* Repository::GetRepository(const Napi::CallbackInfo& info) {
  return repository;
}

git_repository* Repository::GetAsyncRepository(const Napi::CallbackInfo& info) {
  return async_repository;
}

git_diff_options Repository::CreateDefaultGitDiffOptions() {
  git_diff_options options = { 0 };
  options.version = GIT_DIFF_OPTIONS_VERSION;
  options.context_lines = 3;
  return options;
}

Napi::Value Repository::Exists(const Napi::CallbackInfo& info) {
  auto result = Napi::Boolean::New(info.Env(), GetRepository(info) != NULL);
  return result;
}

// Returns `{ klass, message }` describing why the last open failed, or null
// when the repository opened successfully. Lets git.js turn a dubious-ownership
// rejection into a typed error instead of an indistinguishable null.
Napi::Value Repository::GetOpenError(const Napi::CallbackInfo& info) {
  auto env = info.Env();
  Napi::HandleScope scope(env);
  if (repository != NULL) {
    return env.Null();
  }
  auto result = Napi::Object::New(env);
  result.Set("klass", Napi::Number::New(env, open_error_klass));
  result.Set("message", Napi::String::New(env, open_error_message));
  return result;
}

// Toggle libgit2's repository ownership validation (its equivalent of git's
// safe.directory / "dubious ownership" guard). Disabling it lets Lumine open a
// repository whose directory is owned by another account (common on Windows
// when a clone was created from an elevated terminal). The flag is process-wide
// and resets when the process exits, which matches a "silence this session"
// bypass.
static Napi::Value SetOwnerValidation(const Napi::CallbackInfo& info) {
  auto env = info.Env();
  CHECK(info.Length() >= 1 && info[0].IsBoolean(),
        "Expected boolean as first argument", env);
  int enabled = info[0].As<Napi::Boolean>().Value() ? 1 : 0;
  git_libgit2_opts(GIT_OPT_SET_OWNER_VALIDATION, enabled);
  return env.Undefined();
}

static Napi::Value GetOwnerValidation(const Napi::CallbackInfo& info) {
  auto env = info.Env();
  int enabled = 1;
  git_libgit2_opts(GIT_OPT_GET_OWNER_VALIDATION, &enabled);
  return Napi::Boolean::New(env, enabled != 0);
}

Napi::Value Repository::GetPath(const Napi::CallbackInfo& info) {
  auto env = info.Env();
  Napi::HandleScope scope(env);
  git_repository* repository = GetRepository(info);
  const char* path = git_repository_path(repository);

  return Napi::String::New(env, path);
}

Napi::Value Repository::GetWorkingDirectory(const Napi::CallbackInfo& info) {
  auto env = info.Env();
  Napi::HandleScope scope(env);
  git_repository* repository = GetRepository(info);
  const char* path = git_repository_workdir(repository);

  return Napi::String::New(env, path);
}

int Repository::SubmoduleCallback(
  git_submodule* submodule,
  const char* name,
  void* payload
) {
  std::vector<std::string>* submodules = static_cast<std::vector<std::string>*>(payload);
  const char* submodulePath = git_submodule_path(submodule);
  if (submodulePath != NULL) {
    submodules->push_back(submodulePath);
  }
  return GIT_OK;
}

Napi::Value Repository::GetSubmodulePaths(const Napi::CallbackInfo& info) {
  auto env = info.Env();
  Napi::HandleScope scope(env);
  git_repository* repository = GetRepository(info);

  std::vector<std::string> paths;
  git_submodule_foreach(repository, SubmoduleCallback, &paths);

  auto result = Napi::Array::New(env);
  for (size_t i = 0; i < paths.size(); i++) {
    result.Set(i, Napi::String::New(env, paths[i].data()));
  }
  return result;
}

HeadWorker::HeadWorker(Napi::Env env, git_repository* repository): env(env), repository(repository) {}

void HeadWorker::Execute() {
  git_reference* head;
  if (git_repository_head(&head, repository) != GIT_OK) return;

  if (git_repository_head_detached(repository) == 1) {
    const git_oid *oid = git_reference_target(head);
    if (oid) {
      result.resize(GIT_OID_HEXSZ);
      git_oid_tostr(&result[0], GIT_OID_HEXSZ + 1, oid);
    }
  } else {
    result = git_reference_name(head);
  }

  git_reference_free(head);
}

std::pair<Napi::Value, Napi::Value> HeadWorker::Finish() {
  if (result.empty()) {
    return {env.Null(), env.Null()};
  } else {
    return {env.Null(), Napi::String::New(env, result)};
  }
}

Napi::Value Repository::GetHead(const Napi::CallbackInfo& info) {
  HeadWorker worker(info.Env(), GetRepository(info));
  worker.Execute();
  return worker.Finish().second;
}

HeadAsyncWorker::HeadAsyncWorker(
  Napi::Function& callback,
  git_repository* repository
) : AsyncWorker(callback), worker(Env(), repository) {}

void HeadAsyncWorker::Execute() {
  worker.Execute();
}

void HeadAsyncWorker::OnOK() {
  auto result = worker.Finish();
  Callback().Call({
    result.first,
    result.second
  });
}

Napi::Value Repository::GetHeadAsync(const Napi::CallbackInfo& info) {
  auto env = info.Env();
  CHECK(
    info[0].IsFunction(),
    "Expected first argument to be a function",
    env
  );

  auto fn = info[0].As<Napi::Function>();
  HeadAsyncWorker* asyncWorker = new HeadAsyncWorker(fn, GetAsyncRepository(info));
  asyncWorker->Queue();
  return env.Undefined();
}

void Repository::RefreshIndex(const Napi::CallbackInfo& info) {
  auto env = info.Env();
  Napi::HandleScope scope(env);
  git_repository* repository = GetRepository(info);
  git_index* index;
  if (git_repository_index(&index, repository) == GIT_OK) {
    git_index_read(index, 0);
    git_index_free(index);
  }
}

Napi::Value Repository::IsIgnored(const Napi::CallbackInfo& info) {
  auto env = info.Env();
  Napi::HandleScope scope(env);
  if (info.Length() < 1) {
    return Napi::Boolean::New(env, false);
  }

  CHECK(info[0].IsString(), "Expected string as argument", env);

  git_repository* repository = GetRepository(info);
  std::string path(info[0].As<Napi::String>());
  int ignored;
  bool result;
  if (
    git_ignore_path_is_ignored(
      &ignored,
      repository,
      path.c_str()
    ) == GIT_OK
  ) {
    result = ignored == 1;
  } else {
    result = false;
  }
  return Napi::Boolean::New(env, result);
}

Napi::Value Repository::IsSubmodule(const Napi::CallbackInfo& info) {
  auto env = info.Env();
  Napi::HandleScope scope(env);
  if (info.Length() < 1) {
    return Napi::Boolean::New(env, false);
  }

  CHECK(info[0].IsString(), "Expected string as argument", env);

  git_index* index;
  git_repository* repository = GetRepository(info);
  if (git_repository_index(&index, repository) == GIT_OK) {
    std::string path(info[0].As<Napi::String>());
    const git_index_entry* entry = git_index_get_bypath(index, path.c_str(), 0);
    auto isSubmodule = Napi::Boolean::New(
      env,
      entry != NULL && (entry->mode & S_IFMT) == GIT_FILEMODE_COMMIT
    );
    git_index_free(index);
    return isSubmodule;
  } else {
    return Napi::Boolean::New(env, false);
  }
}

Napi::Value Repository::GetConfigValue(const Napi::CallbackInfo& info) {
  auto env = info.Env();
  Napi::HandleScope scope(env);
  if (info.Length() < 1) return env.Null();

  git_config* config;
  git_repository* repository = GetRepository(info);
  if (git_repository_config_snapshot(&config, repository) != GIT_OK) {
    return env.Null();
  }

  CHECK(info[0].IsString(), "Expected string as first argument", env);

  std::string configKey(info[0].As<Napi::String>());
  const char* configValue;
  if (
    git_config_get_string(
      &configValue,
      config,
      configKey.c_str()
    ) == GIT_OK
  ) {
    auto result = Napi::String::New(env, configValue);
    git_config_free(config);
    return result;
  } else {
    git_config_free(config);
    return env.Null();
  }
}

Napi::Value Repository::SetConfigValue(const Napi::CallbackInfo& info) {
  auto env = info.Env();
  Napi::HandleScope scope(env);
  if (info.Length() != 2) {
    return Napi::Boolean::New(env, false);
  }

  git_config* config;
  git_repository* repository = GetRepository(info);
  if (git_repository_config(&config, repository) != GIT_OK) {
    return Napi::Boolean::New(env, false);
  }

  CHECK(info[0].IsString(), "Expected string as first argument", env);
  CHECK(info[1].IsString(), "Expected string as second argument", env);

  std::string configKey(info[0].As<Napi::String>());
  std::string configValue(info[1].As<Napi::String>());

  int errorCode = git_config_set_string(
    config,
    configKey.c_str(),
    configValue.c_str()
  );

  git_config_free(config);

  return Napi::Boolean::New(env, errorCode == GIT_OK);
}

int StatusCallback(
  const char* path,
  unsigned int status,
  void* payload
) {
  auto statuses = static_cast<std::map<std::string, unsigned int> *>(payload);
  statuses->insert(std::make_pair(std::string(path), status));
  return GIT_OK;
}

StatusWorker::StatusWorker(
  Napi::Env env,
  git_repository* repository,
  Napi::Value path_filter
): env(env), repository{repository} {
  if (path_filter.IsArray()) {
    auto js_paths = path_filter.As<Napi::Array>();
    path_count = js_paths.Length();
    paths = reinterpret_cast<char **>(malloc(path_count * sizeof(char *)));
    for (unsigned i = 0; i < path_count; i++) {
      std::string path = js_paths.Get(i).As<Napi::String>();
      paths[i] = reinterpret_cast<char *>(malloc(path.length() + 1));
      strcpy(paths[i], path.c_str());
    }
  } else {
    paths = NULL;
    path_count = 0;
  }
}

void StatusWorker::Execute() {
  git_status_options options = GIT_STATUS_OPTIONS_INIT;
  options.flags = GIT_STATUS_OPT_INCLUDE_UNTRACKED | GIT_STATUS_OPT_RECURSE_UNTRACKED_DIRS;
  if (paths) {
    options.pathspec.count = path_count;
    options.pathspec.strings = paths;
  }
  code = git_status_foreach_ext(
    repository,
    &options,
    StatusCallback,
    &statuses
  );

  if (paths) {
    git_strarray_free(&options.pathspec);
  }
}

std::pair<Napi::Value, Napi::Value> StatusWorker::Finish() {
  if (code != GIT_OK) {
    auto err = Napi::Error::New(env, "Git status failed");
    return {err.Value(), env.Null()};
  }
  auto result = Napi::Object::New(env);
  for (
    auto iter = statuses.begin(), end = statuses.end();
    iter != end;
    ++iter
  ) {
    result.Set(iter->first, Napi::Number::New(env, iter->second));
  }
  return {env.Null(), result};
}

void StatusAsyncWorker::Execute() {
  worker.Execute();
}

void StatusAsyncWorker::OnOK() {
  auto result = worker.Finish();
  Callback().Call({
    result.first,
    result.second
  });
}

Napi::Value Repository::GetStatusAsync(const Napi::CallbackInfo& info) {
  auto env = info.Env();
  Napi::HandleScope scope(env);

  CHECK(info[0].IsFunction(), "Expected function as first argument", env);
  auto callback = info[0].As<Napi::Function>();

  Napi::Value path_filter;
  if (info.Length() > 1) {
    path_filter = info[1];
  } else {
    path_filter = env.Null();
  }

  StatusAsyncWorker* worker = new StatusAsyncWorker(callback, GetAsyncRepository(info), path_filter);
  worker->Queue();

  return env.Undefined();
}

Napi::Value Repository::GetStatus(const Napi::CallbackInfo& info) {
  auto env = info.Env();
  Napi::HandleScope scope(env);

  Napi::Value path_filter;
  if (info.Length() > 0) {
    path_filter = info[0];
  } else {
    path_filter = env.Null();
  }

  StatusWorker worker(env, GetRepository(info), path_filter);
  worker.Execute();
  auto result = worker.Finish();
  if (result.first.IsNull()) {
    return result.second;
  } else {
    return result.first;
  }
}

Napi::Value Repository::GetStatusForPath(const Napi::CallbackInfo& info) {
  auto env = info.Env();
  Napi::HandleScope scope(env);
  CHECK(info[0].IsString(), "Expected string as first argument", env);
  std::string path = info[0].As<Napi::String>();
  git_repository* repository = GetRepository(info);
  unsigned int status = 0;
  if (git_status_file(&status, repository, path.c_str()) == GIT_OK) {
    return Napi::Number::New(env, status);
  } else {
    return Napi::Number::New(env, 0);
  }
}

Napi::Value Repository::GetSymbolicRefTarget(const Napi::CallbackInfo& info) {
  auto env = info.Env();
  Napi::HandleScope scope(env);

  if (info.Length() < 1) return env.Null();
  CHECK(info[0].IsString(), "Expected string as first argument", env);

  std::string refName(info[0].As<Napi::String>());
  git_reference* ref = NULL;

  if (git_reference_lookup(&ref, GetRepository(info), refName.c_str()) != GIT_OK) {
    return env.Null();
  }

  Napi::Value result = env.Null();
  if (git_reference_type(ref) == GIT_REFERENCE_SYMBOLIC) {
    const char* target = git_reference_symbolic_target(ref);
    if (target) result = Napi::String::New(env, target);
  }

  git_reference_free(ref);
  return result;
}

Napi::Value Repository::CheckoutHead(const Napi::CallbackInfo& info) {
  auto env = info.Env();
  Napi::HandleScope scope(env);

  if (info.Length() < 1) {
    return Napi::Boolean::New(env, false);
  }

  std::string pathStr = info[0].As<Napi::String>();
  char* path = pathStr.data();

  git_checkout_options options = GIT_CHECKOUT_OPTIONS_INIT;
  options.checkout_strategy = GIT_CHECKOUT_FORCE | GIT_CHECKOUT_DISABLE_PATHSPEC_MATCH;

  git_strarray paths;
  paths.count = 1;
  paths.strings = &path;
  options.paths = paths;

  int result = git_checkout_head(GetRepository(info), &options);
  return Napi::Boolean::New(env, result == GIT_OK);
}

Napi::Value Repository::GetReferenceTarget(const Napi::CallbackInfo& info) {
  auto env = info.Env();
  Napi::HandleScope scope(env);

  if (info.Length() < 1) return env.Null();
  CHECK(info[0].IsString(), "Expected string as first argument", env);

  std::string refName(info[0].As<Napi::String>());
  git_oid sha;

  if (
    git_reference_name_to_id(
      &sha,
      GetRepository(info),
      refName.c_str()
    ) == GIT_OK
  ) {
    char oid[GIT_OID_HEXSZ + 1];
    git_oid_tostr(oid, GIT_OID_HEXSZ + 1, &sha);
    return Napi::String::New(env, oid);
  } else {
    return env.Null();
  }
}

Napi::Value Repository::GetDiffStats(const Napi::CallbackInfo& info) {
  auto env = info.Env();
  Napi::HandleScope scope(env);

  int added = 0;
  int deleted = 0;

  auto result = Napi::Object::New(env);
  result.Set("added", added);
  result.Set("deleted", deleted);

  if (info.Length() < 1) return result;
  CHECK(info[0].IsString(), "Expected string as first argument", env);

  git_repository* repository = GetRepository(info);
  git_reference* head;

  if (git_repository_head(&head, repository) != GIT_OK) {
    return result;
  }

  const git_oid* sha = git_reference_target(head);
  git_commit* commit;
  int commitStatus = git_commit_lookup(&commit, repository, sha);
  git_reference_free(head);
  if (commitStatus != GIT_OK) return result;

  git_tree* tree;
  int treeStatus = git_commit_tree(&tree, commit);
  git_commit_free(commit);
  if (treeStatus != GIT_OK) return result;

  std::string pathStr = info[0].As<Napi::String>();
  char* path = pathStr.data();

  git_diff_options options = CreateDefaultGitDiffOptions();
  git_strarray paths;
  paths.count = 1;
  paths.strings = &path;
  options.pathspec = paths;
  options.context_lines = 0;
  options.flags = GIT_DIFF_DISABLE_PATHSPEC_MATCH;

  git_diff* diffs;
  int diffStatus = git_diff_tree_to_workdir(&diffs, repository, tree, &options);
  git_tree_free(tree);

  if (diffStatus != GIT_OK) return result;

  int deltas = git_diff_num_deltas(diffs);
  if (deltas != 1) {
    git_diff_free(diffs);
    return result;
  }

  git_patch* patch;
  int patchStatus = git_patch_from_diff(&patch, diffs, 0);
  git_diff_free(diffs);
  if (patchStatus != GIT_OK) return result;

  int hunks = git_patch_num_hunks(patch);
  for (int i = 0; i < hunks; i++) {
    int lines = git_patch_num_lines_in_hunk(patch, i);
    for (int j = 0; j < lines; j++) {
      const git_diff_line* line;
      if (git_patch_get_line_in_hunk(&line, patch, i, j) == GIT_OK) {
        switch (line->origin) {
          case GIT_DIFF_LINE_ADDITION:
            added++;
            break;
          case GIT_DIFF_LINE_DELETION:
            deleted++;
            break;
        }
      }
    }
  }
  git_patch_free(patch);

  result.Set("added", Napi::Number::New(env, added));
  result.Set("deleted", Napi::Number::New(env, deleted));

  return result;
}

Napi::Value Repository::GetHeadBlob(const Napi::CallbackInfo& info) {
  auto env = info.Env();
  Napi::HandleScope scope(env);

  if (info.Length() < 1) return env.Null();
  CHECK(info[0].IsString(), "Expected string as first argument", env);

  std::string path(info[0].As<Napi::String>());

  git_repository* repo = GetRepository(info);
  git_reference* head;
  if (git_repository_head(&head, repo) != GIT_OK) {
    return env.Null();
  }

  const git_oid* sha = git_reference_target(head);
  git_commit* commit;
  int commitStatus = git_commit_lookup(&commit, repo, sha);
  git_reference_free(head);
  if (commitStatus != GIT_OK) return env.Null();

  git_tree* tree;
  int treeStatus = git_commit_tree(&tree, commit);
  git_commit_free(commit);
  if (treeStatus != GIT_OK) return env.Null();

  git_tree_entry* treeEntry;
  if (git_tree_entry_bypath(&treeEntry, tree, path.c_str()) != GIT_OK) {
    git_tree_free(tree);
    return env.Null();
  }

  git_blob* blob = NULL;
  const git_oid* blobSha = git_tree_entry_id(treeEntry);
  if (blobSha != NULL && git_blob_lookup(&blob, repo, blobSha) != GIT_OK)
    blob = NULL;
  git_tree_entry_free(treeEntry);
  git_tree_free(tree);
  if (blob == NULL) return env.Null();

  const char* content = static_cast<const char*>(git_blob_rawcontent(blob));
  auto value = Napi::String::New(env, content);
  git_blob_free(blob);
  return value;
}

Napi::Value Repository::GetIndexBlob(const Napi::CallbackInfo& info) {
  auto env = info.Env();
  Napi::HandleScope scope(env);

  if (info.Length() < 1) return env.Null();
  CHECK(info[0].IsString(), "Expected string as first argument", env);

  std::string path(info[0].As<Napi::String>());

  git_repository* repo = GetRepository(info);
  git_index* index;
  if (git_repository_index(&index, repo) != GIT_OK) return env.Null();

  git_index_read(index, 0);
  const git_index_entry* entry = git_index_get_bypath(index, path.data(), 0);
  if (entry == NULL) {
    git_index_free(index);
    return env.Null();
  }

  git_blob* blob = NULL;
  const git_oid* blobSha = &entry->id;
  if (blobSha != NULL && git_blob_lookup(&blob, repo, blobSha) != GIT_OK)
    blob = NULL;
  git_index_free(index);
  if (blob == NULL) return env.Null();

  const char* content = static_cast<const char*>(git_blob_rawcontent(blob));
  auto value = Napi::String::New(env, content);
  git_blob_free(blob);
  return value;
}

int SubmoduleCallback(
  git_submodule* submodule,
  const char* name,
  void* payload
) {
  std::vector<std::string>* submodules = static_cast<std::vector<std::string>*>(payload);

  const char* submodulePath = git_submodule_path(submodule);
  if (submodulePath != NULL) {
    submodules->push_back(submodulePath);
  }

  return GIT_OK;
}

void Repository::Release(const Napi::CallbackInfo& info) {
  auto env = info.Env();
  Napi::HandleScope scope(env);

  if (this->repository != NULL) {
    git_repository_free(this->repository);
    this->repository = NULL;
  }
}

CompareCommitsWorker::CompareCommitsWorker(
  Napi::Env env,
  git_repository* repository,
  Napi::Value js_left_id,
  Napi::Value js_right_id
): env(env), repository(repository) {
  left_id = js_left_id.As<Napi::String>();
  right_id = js_right_id.As<Napi::String>();
}

void CompareCommitsWorker::Execute() {
  git_oid left_oid;
  if (git_oid_fromstr(&left_oid, left_id.c_str()) != GIT_OK) return;

  git_oid right_oid;
  if (git_oid_fromstr(&right_oid, right_id.c_str()) != GIT_OK) return;

  git_oid merge_base;
  if (git_merge_base(&merge_base, repository, &left_oid, &right_oid) != GIT_OK) return;

  ahead_count = GetCommitCount(repository, &left_oid, &merge_base);
  behind_count = GetCommitCount(repository, &right_oid, &merge_base);
}


std::pair<Napi::Value, Napi::Value> CompareCommitsWorker::Finish() {
  auto result = Napi::Object::New(env);
  result.Set("ahead", ahead_count);
  result.Set("behind", behind_count);
  return {env.Null(), result};
}

Napi::Value Repository::CompareCommits(const Napi::CallbackInfo& info) {
  auto env = info.Env();
  Napi::HandleScope scope(env);

  if (info.Length() < 2) return env.Null();

  CompareCommitsWorker worker(env, GetRepository(info), info[0], info[1]);
  worker.Execute();
  return worker.Finish().second;
}

CompareCommitsAsyncWorker::CompareCommitsAsyncWorker(
  Napi::Function& callback,
  git_repository* repository,
  Napi::Value js_left_id,
  Napi::Value js_right_id
) : AsyncWorker(callback), worker(Env(), repository, js_left_id, js_right_id) {}

void CompareCommitsAsyncWorker::Execute() {
  worker.Execute();
}

void CompareCommitsAsyncWorker::OnOK() {
  auto result = worker.Finish();
  Callback().Call({result.first, result.second});
}

Napi::Value Repository::CompareCommitsAsync(const Napi::CallbackInfo& info) {
  auto env = info.Env();
  Napi::HandleScope scope(env);
  if (info.Length() < 2) return env.Null();

  CHECK(info[0].IsFunction(), "Expected function as first argument", env);
  CHECK(info[1].IsString(), "Expected string as second argument", env);
  CHECK(info[2].IsString(), "Expected string as third argument", env);

  auto fn = info[0].As<Napi::Function>();
  CompareCommitsAsyncWorker* worker = new CompareCommitsAsyncWorker(
    fn,
    GetAsyncRepository(info),
    info[1],
    info[2]
  );
  worker->Queue();
  return env.Undefined();
}


static bool IsBooleanOptionEnabled(Napi::Object object, std::string key) {
  auto value = object.Get(key);
  if (!value.IsBoolean()) return false;
  auto boolValue = value.As<Napi::Boolean>();
  return boolValue.Value();
}

int Repository::DiffHunkCallback(
  const git_diff_delta* delta,
  const git_diff_hunk* range,
  void* payload
) {
  std::vector<git_diff_hunk>* ranges = static_cast<std::vector<git_diff_hunk>*>(payload);
  ranges->push_back(*range);
  return GIT_OK;
}

int Repository::GetBlob(
  const Napi::CallbackInfo& info,
  git_repository* repo,
  git_blob*& blob
) {
  std::string path(info[0].As<Napi::String>());
  int useIndex = false;
  if (info.Length() >= 3) {
    auto optionsArg = info[2].As<Napi::Object>();
    if (IsBooleanOptionEnabled(optionsArg, "useIndex")) {
      useIndex = true;
    }
  }

  if (useIndex) {
    git_index* index;
    if (git_repository_index(&index, repo) != GIT_OK)
      return -1;

    git_index_read(index, 0);
    const git_index_entry* entry = git_index_get_bypath(index, path.data(), 0);
    if (entry == NULL) {
      git_index_free(index);
      return -1;
    }

    const git_oid* blobSha = &entry->id;
    if (blobSha != NULL && git_blob_lookup(&blob, repo, blobSha) != GIT_OK)
      blob = NULL;
  } else {
    git_reference* head;
    if (git_repository_head(&head, repo) != GIT_OK)
      return -1;

    const git_oid* sha = git_reference_target(head);
    git_commit* commit;
    int commitStatus = git_commit_lookup(&commit, repo, sha);
    git_reference_free(head);
    if (commitStatus != GIT_OK)
      return -1;

    git_tree* tree;
    int treeStatus = git_commit_tree(&tree, commit);
    git_commit_free(commit);
    if (treeStatus != GIT_OK)
      return -1;

    git_tree_entry* treeEntry;
    if (git_tree_entry_bypath(&treeEntry, tree, path.c_str()) != GIT_OK) {
      git_tree_free(tree);
      return -1;
    }

    const git_oid* blobSha = git_tree_entry_id(treeEntry);
    if (blobSha != NULL && git_blob_lookup(&blob, repo, blobSha) != GIT_OK)
      blob = NULL;
    git_tree_entry_free(treeEntry);
    git_tree_free(tree);
  }

  if (blob == NULL) return -1;

  return 0;
}

Napi::Value Repository::GetLineDiffs(const Napi::CallbackInfo& info) {
  auto env = info.Env();
  Napi::HandleScope scope(env);
  if (info.Length() < 2) return env.Null();

  CHECK(info[1].IsString(), "Expected string as second argument", env);

  std::string text(info[1].As<Napi::String>());
  git_repository* repo = GetRepository(info);
  git_blob* blob = NULL;

  int getBlobResult = GetBlob(info, repo, blob);
  if (getBlobResult != 0) return env.Null();

  std::vector<git_diff_hunk> ranges;
  git_diff_options options = CreateDefaultGitDiffOptions();

  if (info.Length() >= 3) {
    CHECK(info[2].IsObject(), "Expected object as third argument", env);
    auto optionsArg = info[2].As<Napi::Object>();
    if (IsBooleanOptionEnabled(optionsArg, "ignoreAllSpace")) {
      options.flags = GIT_DIFF_IGNORE_WHITESPACE;
    } else if (IsBooleanOptionEnabled(optionsArg, "ignoreSpaceChange")) {
      options.flags = GIT_DIFF_IGNORE_WHITESPACE_CHANGE;
    } else if (
      IsBooleanOptionEnabled(optionsArg, "ignoreSpaceAtEOL") ||
      IsBooleanOptionEnabled(optionsArg, "ignoreEolWhitespace")
    ) {
      options.flags = GIT_DIFF_IGNORE_WHITESPACE_EOL;
    } else {
      options.flags = GIT_DIFF_NORMAL;
    }
  }

  options.context_lines = 0;

  if (
    git_diff_blob_to_buffer(
      blob,
      NULL,
      text.data(),
      text.length(),
      NULL,
      &options,
      NULL,
      NULL,
      DiffHunkCallback,
      NULL,
      &ranges
    ) == GIT_OK
  ) {
    auto v8Ranges = Napi::Array::New(env, ranges.size());
    for (size_t i = 0; i < ranges.size(); i++) {
      auto v8Range = Napi::Object::New(env);
      v8Range.Set("oldStart", Napi::Number::New(env, ranges[i].old_start));
      v8Range.Set("oldLines", Napi::Number::New(env, ranges[i].old_lines));
      v8Range.Set("newStart", Napi::Number::New(env, ranges[i].new_start));
      v8Range.Set("newLines", Napi::Number::New(env, ranges[i].new_lines));

      v8Ranges.Set(i, v8Range);
    }
    git_blob_free(blob);
    return v8Ranges;
  } else {
    git_blob_free(blob);
    return env.Null();
  }
}

int Repository::DiffLineCallback(
  const git_diff_delta* delta,
  const git_diff_hunk* range,
  const git_diff_line* line,
  void* payload
) {
  LineDiff lineDiff;
  lineDiff.hunk = *range;
  lineDiff.line = *line;
  std::vector<LineDiff>* lineDiffs = static_cast<std::vector<LineDiff>*>(payload);
  lineDiffs->push_back(lineDiff);
  return GIT_OK;
}

Napi::Value Repository::GetLineDiffDetails(const Napi::CallbackInfo& info) {
  auto env = info.Env();
  Napi::HandleScope scope(env);
  if (info.Length() < 2) return env.Null();

  CHECK(info[1].IsString(), "Expected string as second argument", env);

  std::string text(info[1].As<Napi::String>());

  git_repository* repo = GetRepository(info);

  git_blob* blob = NULL;

  int getBlobResult = GetBlob(info, repo, blob);

  if (getBlobResult != 0) return env.Null();

  std::vector<LineDiff> lineDiffs;
  git_diff_options options = CreateDefaultGitDiffOptions();

  if (info.Length() >= 3) {
    CHECK(info[2].IsObject(), "Expected object as third argument", env);
    auto optionsArg = info[2].As<Napi::Object>();
    if (IsBooleanOptionEnabled(optionsArg, "ignoreAllSpace")) {
      options.flags = GIT_DIFF_IGNORE_WHITESPACE;
    } else if (IsBooleanOptionEnabled(optionsArg, "ignoreSpaceChange")) {
      options.flags = GIT_DIFF_IGNORE_WHITESPACE_CHANGE;
    } else if (
      IsBooleanOptionEnabled(optionsArg, "ignoreSpaceAtEOL") ||
      IsBooleanOptionEnabled(optionsArg, "ignoreEolWhitespace")
    ) {
      options.flags = GIT_DIFF_IGNORE_WHITESPACE_EOL;
    } else {
      options.flags = GIT_DIFF_NORMAL;
    }
  }

  options.context_lines = 0;

  if (
    git_diff_blob_to_buffer(
      blob,
      NULL,
      text.data(),
      text.length(),
      NULL,
      &options,
      NULL,
      NULL,
      NULL,
      DiffLineCallback,
      &lineDiffs
    ) == GIT_OK
  ) {
    auto v8Ranges = Napi::Array::New(env, lineDiffs.size());
    for (size_t i = 0; i < lineDiffs.size(); i++) {
      auto v8Range = Napi::Object::New(env);
      v8Range.Set("oldLineNumber", Napi::Number::New(env, lineDiffs[i].line.old_lineno));
      v8Range.Set("newLineNumber", Napi::Number::New(env, lineDiffs[i].line.new_lineno));
      v8Range.Set("oldStart", Napi::Number::New(env, lineDiffs[i].hunk.old_start));
      v8Range.Set("oldLines", Napi::Number::New(env, lineDiffs[i].hunk.old_lines));
      v8Range.Set("newStart", Napi::Number::New(env, lineDiffs[i].hunk.new_start));
      v8Range.Set("newLines", Napi::Number::New(env, lineDiffs[i].hunk.new_lines));
      v8Range.Set(
        "line",
        Napi::String::New(
          env,
          lineDiffs[i].line.content,
          lineDiffs[i].line.content_len
        )
      );

      v8Ranges.Set(i, v8Range);
    }
    git_blob_free(blob);
    return v8Ranges;
  } else {
    git_blob_free(blob);
    return env.Null();
  }
}

Napi::Array Repository::ConvertStringVectorToV8Array(
  Napi::Env env,
  const std::vector<std::string>& vector
) {
  size_t i = 0, size = vector.size();
  auto array = Napi::Array::New(env, size);
  for (i = 0; i < size; i++) {
    array.Set(i, Napi::String::New(env, vector[i].c_str()));
  }
  return array;
}

Napi::Value Repository::GetReferences(const Napi::CallbackInfo& info) {
  auto env = info.Env();
  Napi::HandleScope scope(env);

  auto references = Napi::Object::New(env);
  std::vector<std::string> heads, remotes, tags;

  git_strarray strarray;
  git_reference_list(&strarray, GetRepository(info));

  for (unsigned int i = 0; i < strarray.count; i++) {
    if (strncmp(strarray.strings[i], "refs/heads/", 11) == 0)
      heads.push_back(strarray.strings[i]);
    else if (strncmp(strarray.strings[i], "refs/remotes/", 13) == 0)
      remotes.push_back(strarray.strings[i]);
    else if (strncmp(strarray.strings[i], "refs/tags/", 10) == 0)
      tags.push_back(strarray.strings[i]);
  }

  git_strarray_free(&strarray);

  references.Set("heads", ConvertStringVectorToV8Array(env, heads));
  references.Set("remotes", ConvertStringVectorToV8Array(env, remotes));
  references.Set("tags", ConvertStringVectorToV8Array(env, tags));

  return references;
}

int branch_checkout(git_repository* repo, const char* refName) {
  git_reference* ref = NULL;
  git_object* git_obj = NULL;
  git_checkout_options opts = GIT_CHECKOUT_OPTIONS_INIT;
  opts.checkout_strategy = GIT_CHECKOUT_SAFE;
  int success = -1;

  if (!(success = git_reference_lookup(&ref, repo, refName)) &&
      !(success = git_reference_peel(&git_obj, ref, GIT_OBJ_TREE)) &&
      !(success = git_checkout_tree(repo, git_obj, &opts)))
    success = git_repository_set_head(repo, refName);

  git_object_free(git_obj);
  git_obj = NULL;
  git_reference_free(ref);
  ref = NULL;

  return success;
}

Napi::Value Repository::CheckoutReference(const Napi::CallbackInfo& info) {
  auto env = info.Env();
  Napi::HandleScope scope(env);

  if (info.Length() < 1) return Napi::Boolean::New(env, false);

  bool shouldCreateNewRef;
  if (info.Length() > 1 && info[1].IsBoolean() && info[1].As<Napi::Boolean>().Value()) {
    shouldCreateNewRef = true;
  } else {
    shouldCreateNewRef = false;
  }

  std::string strRefName(info[0].As<Napi::String>());
  const char* refName = strRefName.c_str();

  git_repository* repo = GetRepository(info);

  if (branch_checkout(repo, refName) == GIT_OK) {
    return Napi::Boolean::New(env, true);
  } else if (shouldCreateNewRef) {
    git_reference* head;
    if (git_repository_head(&head, repo) != GIT_OK) {
      return Napi::Boolean::New(env, false);
    }
    const git_oid* sha = git_reference_target(head);
    git_commit* commit;
    int commitStatus = git_commit_lookup(&commit, repo, sha);
    git_reference_free(head);

    if (commitStatus != GIT_OK) {
      return Napi::Boolean::New(env, false);
    }
    git_reference* branch;
    // N.B.: git_branch_create needs a name like 'xxx', not 'refs/heads/xxx'
    const int kShortNameLength = strRefName.length() - 11;
    std::string shortRefName(strRefName.c_str() + 11, kShortNameLength);

    int branchCreateStatus = git_branch_create(
        &branch, repo, shortRefName.c_str(), commit, 0);
    git_commit_free(commit);

    if (branchCreateStatus != GIT_OK) {
      return Napi::Boolean::New(env, false);
    }

    git_reference_free(branch);

    if (branch_checkout(repo, refName) == GIT_OK) {
      return Napi::Boolean::New(env, true);
    }
  }

  return Napi::Boolean::New(env, false);
}

Napi::Value Repository::Add(const Napi::CallbackInfo& info) {
  auto env = info.Env();
  Napi::HandleScope scope(env);

  git_repository* repository = GetRepository(info);
  CHECK(info[0].IsString(), "Expected string as first argument", env);
  std::string path(info[0].As<Napi::String>());

  git_index* index;
  if (git_repository_index(&index, repository) != GIT_OK) {
    const git_error* e = giterr_last();
    if (e != NULL) {
      THROW_AND_RETURN(env, e->message);
    } else {
      THROW_AND_RETURN(env, "Unknown error opening index");
    }
  }
  // Modify the in-memory index.
  if (git_index_add_bypath(index, path.c_str()) != GIT_OK) {
    git_index_free(index);
    const git_error* e = giterr_last();
    if (e != NULL) {
      THROW_AND_RETURN(env, e->message);
    } else {
      THROW_AND_RETURN(env, "Unknown error adding path to index");
    }
  }
  // Write this change in the index back to disk, so it is persistent
  if (git_index_write(index) != GIT_OK) {
    git_index_free(index);
    const git_error* e = giterr_last();
    if (e != NULL) {
      THROW_AND_RETURN(env, e->message);
    } else {
      THROW_AND_RETURN(env, "Unknown error adding path to index");
    }
  }
  git_index_free(index);
  return Napi::Boolean::New(env, true);
}

Repository::Repository(
  const Napi::CallbackInfo& info
): Napi::ObjectWrap<Repository>(info) {
  auto env = info.Env();
  Napi::HandleScope scope(env);

  int flags = 0;
  CHECK_VOID(info[0].IsString(), "Expected string as first argument", env);
  if (info.Length() > 1) {
    CHECK_VOID(info[1].IsBoolean(), "Expected boolean as second argument", env);
    auto search = info[1].As<Napi::Boolean>();
    if (!search.Value()) {

      flags |= GIT_REPOSITORY_OPEN_NO_SEARCH;
    }
  }

  std::string repository_path(info[0].As<Napi::String>());
  int result = git_repository_open_ext(
    &repository,
    repository_path.c_str(),
    flags,
    NULL
  );

  if (result != GIT_OK) {
    const git_error* error = git_error_last();
    open_error_klass = error != NULL ? error->klass : 0;
    open_error_message = error != NULL && error->message != NULL
      ? error->message
      : "";
    repository = NULL;
    async_repository = NULL;
    return;
  }

  result = git_repository_open_ext(
    &async_repository,
    git_repository_path(repository),
    GIT_REPOSITORY_OPEN_NO_SEARCH,
    NULL
  );

  if (result != GIT_OK) {
    repository = NULL;
    async_repository = NULL;
    return;
  }
}

Repository::~Repository() {
  if (repository != NULL) {
    git_repository_free(repository);
    repository = NULL;
  }
  if (async_repository != NULL) {
    git_repository_free(async_repository);
    async_repository = NULL;
  }
}

void Repository::Init(Napi::Env env, Napi::Object exports) {
  Napi::HandleScope scope(env);

  Napi::Function func = DefineClass(
    env,
    "Repository",
    {
      InstanceMethod<&Repository::GetPath>("getPath", napi_default_method),
      InstanceMethod<&Repository::GetWorkingDirectory>("_getWorkingDirectory", napi_default_method),
      InstanceMethod<&Repository::Exists>("exists", napi_default_method),
      InstanceMethod<&Repository::GetSubmodulePaths>("getSubmodulePaths", napi_default_method),
      InstanceMethod<&Repository::GetHead>("getHead", napi_default_method),
      InstanceMethod<&Repository::GetHeadAsync>("getHeadAsync", napi_default_method),
      InstanceMethod<&Repository::RefreshIndex>("refreshIndex", napi_default_method),
      InstanceMethod<&Repository::IsIgnored>("isIgnored", napi_default_method),
      InstanceMethod<&Repository::IsSubmodule>("isSubmodule", napi_default_method),
      InstanceMethod<&Repository::GetConfigValue>("getConfigValue", napi_default_method),
      InstanceMethod<&Repository::SetConfigValue>("setConfigValue", napi_default_method),
      InstanceMethod<&Repository::GetStatus>("getStatus", napi_default_method),
      InstanceMethod<&Repository::GetStatusForPath>("getStatusForPath", napi_default_method),
      InstanceMethod<&Repository::GetStatusAsync>("getStatusAsync", napi_default_method),
      InstanceMethod<&Repository::GetSymbolicRefTarget>("getSymbolicRefTarget", napi_default_method),
      InstanceMethod<&Repository::CheckoutHead>("checkoutHead", napi_default_method),
      InstanceMethod<&Repository::GetReferenceTarget>("getReferenceTarget", napi_default_method),
      InstanceMethod<&Repository::GetDiffStats>("getDiffStats", napi_default_method),
      InstanceMethod<&Repository::GetIndexBlob>("getIndexBlob", napi_default_method),
      InstanceMethod<&Repository::GetHeadBlob>("getHeadBlob", napi_default_method),
      InstanceMethod<&Repository::CompareCommits>("compareCommits", napi_default_method),
      InstanceMethod<&Repository::CompareCommitsAsync>("compareCommitsAsync", napi_default_method),
      InstanceMethod<&Repository::Release>("_release", napi_default_method),
      InstanceMethod<&Repository::GetLineDiffs>("getLineDiffs", napi_default_method),
      InstanceMethod<&Repository::GetLineDiffDetails>("getLineDiffDetails", napi_default_method),
      InstanceMethod<&Repository::GetReferences>("getReferences", napi_default_method),
      InstanceMethod<&Repository::CheckoutReference>("checkoutRef", napi_default_method),
      InstanceMethod<&Repository::Add>("add", napi_default_method),
      InstanceMethod<&Repository::GetOpenError>("getOpenError", napi_default_method),
    }
  );

  exports.Set("Repository", func);

}

// ========

Napi::Object Init(Napi::Env env, Napi::Object exports) {
  git_libgit2_init();
  Repository::Init(env, exports);
  exports.Set(
    "setOwnerValidation",
    Napi::Function::New(env, SetOwnerValidation)
  );
  exports.Set(
    "getOwnerValidation",
    Napi::Function::New(env, GetOwnerValidation)
  );
  return exports;
}

NODE_API_MODULE(NODE_GYP_MODULE_NAME, Init)
