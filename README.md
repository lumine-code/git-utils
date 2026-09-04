# @lumine-code/git-utils

Provides native Git repository utilities built on libgit2.

This project is retired and archived. Lumine now uses the system Git executable for repository operations, and this native backend is no longer maintained.

The final release provided a stateless, asynchronous native backend based on libgit2 1.9.6 and exposed no repository handles to callers.

Lumine briefly evaluated this backend against system Git. Native metadata reads were faster, but status and worktree diffs were substantially slower, while build, packaging, and maintenance costs remained high. Lumine therefore returned all repository operations to system Git.

The remaining documentation describes the archived 10.0.0 API for historical reference.

## Installation

```sh
npm install @lumine-code/git-utils
```

Lumine pins this repository by commit rather than installing a registry release. The package contains the Node-API addon at `build/Release/git.node` and the exact binding/libgit2 inputs needed by `node-gyp` or `electron-rebuild`. A compatible packaged addon skips redundant compilation; source checkouts always rebuild. Script-suppressed application installs run `node scripts/install.js --prepare-build` to hydrate the exact libgit2 pin without compiling before `electron-rebuild`. Intermediate `.lib`, `.obj`, and `.pdb` products are never shipped.

## API

```js
const git = require('@lumine-code/git-utils')

const descriptor = {
  gitDirectory: '/workspace/project/.git',
  workingDirectory: '/workspace/project'
}
```

A descriptor supplies the exact Git directory and working directory. `git-utils` never searches parent directories. `workingDirectory` may be `null` for a bare repository. Every repository operation returns a Promise, opens the repository for that operation, and releases all native handles before settling; `versions()` and the process-wide `configure()` are synchronous.

### Runtime information

- `versions()` returns `{gitUtils: '10.0.0', napi: number, libgit2: '1.9.6', libgit2Features: number}`.
- `configure({validateOwnership})` sets libgit2's process-wide repository ownership validation before the Git host begins serving work and returns the applied option.

### Read operations

- `snapshot(descriptor, options)` reads status and refs together. `options` accepts `status`, `refs`, `includeIgnored`, `generations`, `knownFingerprints`, and `signal`; status and refs default to enabled.
- Each requested snapshot section is `{fingerprint, unchanged, value?}`. The fingerprint is a deterministic SHA-256 digest of the section value; `value` is omitted when its fingerprint matches `knownFingerprints.status` or `knownFingerprints.refs`.
- `diff(descriptor, request)` supports commit, index, worktree and empty-tree pairs plus file/empty buffer pairs. `request.format` is `structured` by default and may be `patch` or `both`; `diffFilter` follows Git's uppercase-inclusion and lowercase-exclusion syntax; `maxBytes` stops native structured or patch construction when either requested representation exceeds the limit. The result is `{schemaVersion: 1, files, rawPatch?}`.
- `history(descriptor, request)` reads pathless history; `allRefs: true` walks every local, remote, and tag ref with shared commits deduplicated. `commit(descriptor, request)`, `blame(descriptor, request)`, `describe(descriptor, request)`, and `branchesContaining(descriptor, request)` read their corresponding repository metadata.
- `readObjects(descriptor, requests, options)` batches `{oid}`, `{revision, path}` and `{source: 'index', path}` lookups and returns objects whose `content` is a Buffer.
- `readConfig(descriptor, {keys, signal})` batches configuration lookups; `fileMode(descriptor, path, options)` reads an index mode; `submodulePaths(descriptor, options)` lists repository-relative submodule paths.
- `lineDiff(oldBuffer, newBuffer, options)` computes line hunks without opening a repository. It accepts buffers or strings and the whitespace options `ignoreEolWhitespace`, `ignoreSpaceAtEOL`, `ignoreSpaceChange`, and `ignoreAllSpace`.

### Mutations

`mutate(descriptor, request)` accepts only the native operation allowlist: `stageFileModeChange`, `stageFileSymlinkChange`, `setConfig`, `unsetConfig`, `addRemote`, `removeRemote`, `setRemoteUrl`, `deleteRef`, `createBlob`, `expandBlobToFile`, `mergeFile`, and `writeMergeConflictToIndex`. Mutations that require hooks, filters, signing, credential helpers, transports, or porcelain semantics belong to the system Git backend in Lumine.

### Errors and cancellation

Invalid JavaScript arguments reject with `ERR_GIT_NATIVE_ARGUMENT`. Native failures reject with an operation-specific `ERR_GIT_NATIVE_*` code and include `operation`, `libgit2Code`, `libgit2Class`, and `libgit2Message`. An aborted request rejects with `AbortError` and `ERR_GIT_NATIVE_ABORTED`; a result completed after cancellation is discarded.

An oversized diff rejects with `ERR_GIT_NATIVE_DIFF_TOO_LARGE` and the fields `maxBytes`, `structuredBytes`, and `patchBytes`; it never switches to system Git.

Paths and messages cross Node-API as UTF-8 strings. On POSIX, invalid UTF-8 bytes follow Node's normal decoding policy and become U+FFFD, matching Lumine's system-Git process decoding; callers that require byte-exact non-UTF-8 path identity are unsupported.

## Development

- Clone the repository with its submodules, or run `npm run prepare` to hydrate libgit2.
- Run `npm install` to build the addon for the current Node-API runtime.
- Run `npm test` and `npm run lint` to verify the implementation.
- Run `npm run test:package` to verify a script-suppressed install can rebuild from packed sources without a prebuilt addon.
- Run `npm run benchmark -- <working-directory> [git-directory]` to emit native-only benchmark results as JSON.

## Changes in v10

Version 10 removes the stateful `Repository`, `open()`, synchronous repository calls, and renderer-owned native handles. It adds a stateless Promise API, combined fingerprinted snapshots, structured diffs, batched reads, explicit native mutations, stable errors, cancellation, SHA-256 repository support, and libgit2 1.9.6.

## Contributing

Issues and pull requests are welcome at the GitHub repository.
