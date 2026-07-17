#pragma once
#include "git2.h"
#include <napi.h>
#include <string>
#include <map>
#include <vector>

#define CHECK(cond, msg, env)                                  \
if (!(cond)) {                                                 \
  Napi::TypeError::New(env, msg).ThrowAsJavaScriptException(); \
  return env.Null();                                           \
}

#define CHECK_VOID(cond, msg, env)                             \
if (!(cond)) {                                                 \
  Napi::TypeError::New(env, msg).ThrowAsJavaScriptException(); \
  return;                                                      \
}

#define THROW(env, msg) {                                      \
  Napi::TypeError::New(env, msg).ThrowAsJavaScriptException(); \
}

#define THROW_AND_RETURN(env, msg) {                           \
  Napi::TypeError::New(env, msg).ThrowAsJavaScriptException(); \
  return env.Null();                                           \
}


class Repository : public Napi::ObjectWrap<Repository> {
public:
  static void Init(Napi::Env env, Napi::Object exports);
  Repository(const Napi::CallbackInfo& info);
  ~Repository();

private:
  Napi::Value GetPath(const Napi::CallbackInfo& info);
  Napi::Value GetWorkingDirectory(const Napi::CallbackInfo& info);
  Napi::Value GetSubmodulePaths(const Napi::CallbackInfo& info);
  Napi::Value Exists(const Napi::CallbackInfo& info);
  Napi::Value GetHead(const Napi::CallbackInfo& info);
  Napi::Value GetHeadAsync(const Napi::CallbackInfo& info);
  void RefreshIndex(const Napi::CallbackInfo& info);
  Napi::Value IsIgnored(const Napi::CallbackInfo& info);
  Napi::Value IsSubmodule(const Napi::CallbackInfo& info);
  Napi::Value GetConfigValue(const Napi::CallbackInfo& info);
  Napi::Value SetConfigValue(const Napi::CallbackInfo& info);
  Napi::Value GetStatus(const Napi::CallbackInfo& info);
  Napi::Value GetStatusAsync(const Napi::CallbackInfo& info);
  Napi::Value GetStatusForPath(const Napi::CallbackInfo& info);
  Napi::Value GetSymbolicRefTarget(const Napi::CallbackInfo& info);
  Napi::Value CheckoutHead(const Napi::CallbackInfo& info);
  Napi::Value GetReferenceTarget(const Napi::CallbackInfo& info);
  Napi::Value GetDiffStats(const Napi::CallbackInfo& info);
  Napi::Value GetIndexBlob(const Napi::CallbackInfo& info);
  Napi::Value GetHeadBlob(const Napi::CallbackInfo& info);
  Napi::Value CompareCommits(const Napi::CallbackInfo& info);
  Napi::Value CompareCommitsAsync(const Napi::CallbackInfo& info);
  void Release(const Napi::CallbackInfo& info);
  Napi::Value GetLineDiffs(const Napi::CallbackInfo& info);
  Napi::Value GetLineDiffDetails(const Napi::CallbackInfo& info);
  Napi::Value GetReferences(const Napi::CallbackInfo& info);
  Napi::Value CheckoutReference(const Napi::CallbackInfo& info);
  Napi::Value Add(const Napi::CallbackInfo& info);
  Napi::Value GetOpenError(const Napi::CallbackInfo& info);

  static int SubmoduleCallback(
    git_submodule* submodule,
    const char* name,
    void* payload
  );

  static int StatusCallback(
    const char* path,
    unsigned int status,
    void* payload
  );

  static int DiffHunkCallback(
    const git_diff_delta* delta,
    const git_diff_hunk* range,
    void* payload
  );

  static int DiffLineCallback(
    const git_diff_delta* delta,
    const git_diff_hunk* range,
    const git_diff_line* line,
    void* payload
  );

  git_repository* GetRepository(const Napi::CallbackInfo& info);
  git_repository* GetAsyncRepository(const Napi::CallbackInfo& info);
  int GetBlob(
    const Napi::CallbackInfo& info,
    git_repository* repo,
    git_blob*& blob
  );

  static git_diff_options CreateDefaultGitDiffOptions();

  Napi::Array ConvertStringVectorToV8Array(
    Napi::Env env,
    const std::vector<std::string>& vector
  );

  git_repository* repository;
  git_repository* async_repository;

  // Captured from the last failed open so callers can distinguish a genuine
  // "not a repository" from a libgit2 rejection such as dubious ownership.
  int open_error_klass = 0;
  std::string open_error_message;
};


class HeadWorker {
  Napi::Env env;
  git_repository *repository;
  std::string result;

public:
  void Execute();
  std::pair<Napi::Value, Napi::Value> Finish();
  HeadWorker(Napi::Env env, git_repository* repository);
};

class StatusWorker {
  Napi::Env env;
  git_repository *repository;
  std::map<std::string, unsigned int> statuses;
  char **paths;
  unsigned path_count;
  int code;

public:
  void Execute();
  std::pair<Napi::Value, Napi::Value> Finish();
  StatusWorker(Napi::Env env, git_repository* repository, Napi::Value path_filter);
};

class HeadAsyncWorker : public Napi::AsyncWorker {
public:
  HeadAsyncWorker(
    Napi::Function& callback,
    git_repository* repository
  );
  void Execute() override;
  void OnOK() override;

private:
  HeadWorker worker;
};

class StatusAsyncWorker: public Napi::AsyncWorker {
public:
  StatusAsyncWorker(
    Napi::Function& callback,
    git_repository* repository,
    Napi::Value path_filter
  ) : AsyncWorker(callback), worker(Env(), repository, path_filter) {}
  void Execute() override;
  void OnOK() override;

private:
  StatusWorker worker;
};


class CompareCommitsWorker {
public:
  CompareCommitsWorker(
    Napi::Env env,
    git_repository* repository,
    Napi::Value js_left_id,
    Napi::Value js_right_id
  );

  void Execute();
  std::pair<Napi::Value, Napi::Value> Finish();

private:
  Napi::Env env;
  git_repository *repository;
  std::string left_id;
  std::string right_id;
  unsigned ahead_count;
  unsigned behind_count;
};

class CompareCommitsAsyncWorker: public Napi::AsyncWorker {
public:
  CompareCommitsAsyncWorker(
    Napi::Function& callback,
    git_repository* repository,
    Napi::Value js_left_id,
    Napi::Value js_right_id
  );

  void Execute() override;
  void OnOK() override;

private:
  CompareCommitsWorker worker;
};

struct LineDiff {
  git_diff_hunk hunk;
  git_diff_line line;
};
