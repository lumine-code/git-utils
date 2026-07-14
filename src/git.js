const path = require('path')
const fs = require('@lumine-code/fs-plus')
const { Repository } = require('../build/Release/git.node')

const statusIndexNew = 1 << 0
const statusIndexModified = 1 << 1
const statusIndexDeleted = 1 << 2
const statusIndexRenamed = 1 << 3
const statusIndexTypeChange = 1 << 4
const statusWorkingDirNew = 1 << 7
const statusWorkingDirModified = 1 << 8
const statusWorkingDirDelete = 1 << 9
const statusWorkingDirTypeChange = 1 << 10
const statusIgnored = 1 << 14

const modifiedStatusFlags =
  statusWorkingDirModified |
  statusIndexModified |
  statusWorkingDirDelete |
  statusIndexDeleted |
  statusWorkingDirTypeChange |
  statusIndexTypeChange

const newStatusFlags = statusWorkingDirNew | statusIndexNew

const deletedStatusFlags = statusWorkingDirDelete | statusIndexDeleted

const indexStatusFlags =
  statusIndexNew |
  statusIndexModified |
  statusIndexDeleted |
  statusIndexRenamed |
  statusIndexTypeChange

const IS_WINDOWS = process.platform === 'win32'

// Given a path on disk (real or hypothetical), attempt to normalize it by
// (optionally) resolving `realpath` and (if on Windows) converting all path
// separators to forward slashes.
function normalizePath (filePath, useRealpath = true) {
  if (typeof filePath !== 'string') return filePath

  // On Windows we always resolve `realpath` so that 8.3 short names (e.g.
  // `RUNNER~1`) are normalized to their long form — this is what lets
  // `pathStartsWith` (which has no short-name fallback of its own) match
  // reliably even when the caller passes `useRealpath = false`. Off Windows we
  // only resolve when asked. Either way we resolve at most once.
  if (useRealpath || IS_WINDOWS) {
    filePath = realpath(filePath)
  }
  if (!IS_WINDOWS) return filePath
  return filePath.replace(/\\/g, '/')
}

// Compare two paths to determine whether they resolve to the same file or
// directory on disk.
//
// This is more complicated than it sounds — not just because of symlinks but
// also because of files/directories on Windows possibly having both a short
// name and a long name.
function pathsAreEqual (pathA, pathB, caseInsensitive = false, useRealpath = true) {
  if (typeof pathA !== 'string' || typeof pathB !== 'string') {
    return false
  }

  pathA = normalizePath(pathA, useRealpath)
  pathB = normalizePath(pathB, useRealpath)

  if (IS_WINDOWS || caseInsensitive) {
    pathA = pathA.toLowerCase()
    pathB = pathB.toLowerCase()
  }

  const result = pathA === pathB
  if (result || !IS_WINDOWS) return result
  if (!pathA.includes('~') && !pathB.includes('~')) {
    return result
  }

  // If we get this far, we're on Windows and comparing two paths, at least one
  // of which contains an 8.3 short name. The only obvious and reliable way to
  // address this is to `statSync` both paths and verify their IDs are the
  // same.
  if (!fs.existsSync(pathA) || !fs.existsSync(pathB)) {
    return result
  }
  const statA = fs.statSync(pathA)
  const statB = fs.statSync(pathB)

  return statA.ino === statB.ino && statA.dev === statB.dev
}

// Returns whether `pathA` is a strict descendant of `pathB` — i.e., `pathB` is
// a proper ancestor directory of `pathA`. This is exclusive: equal paths return
// `false`, and callers (e.g. `relativize`) handle the equality case separately
// via `pathsAreEqual`.
function pathStartsWith (pathA, pathB, caseInsensitive = false, useRealpath = true) {
  if (IS_WINDOWS) {
    pathA = normalizePath(pathA, useRealpath)
    pathB = normalizePath(pathB, useRealpath)
  }
  if (caseInsensitive) {
    pathA = pathA.toLowerCase()
    pathB = pathB.toLowerCase()
  }
  if (!pathB.endsWith('/')) {
    pathB = `${pathB}/`
  }
  return pathA.startsWith(pathB)
}

Repository.prototype.release = function () {
  for (const submodulePath in this.submodules) {
    const submoduleRepo = this.submodules[submodulePath]
    if (submoduleRepo) submoduleRepo.release()
  }
  return this._release()
}

Repository.prototype.getWorkingDirectory = function () {
  if (!this.workingDirectory) {
    this.workingDirectory = this._getWorkingDirectory()
    if (this.workingDirectory) this.workingDirectory = this.workingDirectory.replace(/\/$/, '')
  }
  return this.workingDirectory
}

Repository.prototype.getShortHead = function () {
  const head = this.getHead()
  if (head == null) return head
  if (head.startsWith('refs/heads/')) return head.substring(11)
  if (head.startsWith('refs/tags/')) return head.substring(10)
  if (head.startsWith('refs/remotes/')) return head.substring(13)
  if (head.match(/[a-fA-F0-9]{40}/)) return head.substring(0, 7)
  return head
}

Repository.prototype.isStatusModified = function (status = 0) {
  return (status & modifiedStatusFlags) > 0
}

Repository.prototype.isPathModified = function (path) {
  return this.isStatusModified(this.getStatus(path))
}

Repository.prototype.isStatusNew = function (status = 0) {
  return (status & newStatusFlags) > 0
}

Repository.prototype.isPathNew = function (path) {
  return this.isStatusNew(this.getStatus(path))
}

Repository.prototype.isStatusDeleted = function (status = 0) {
  return (status & deletedStatusFlags) > 0
}

Repository.prototype.isPathDeleted = function (path) {
  return this.isStatusDeleted(this.getStatus(path))
}

Repository.prototype.isPathStaged = function (path) {
  return this.isStatusStaged(this.getStatus(path))
}

Repository.prototype.isStatusIgnored = function (status = 0) {
  return (status & statusIgnored) > 0
}

Repository.prototype.isStatusStaged = function (status = 0) {
  return (status & indexStatusFlags) > 0
}

Repository.prototype.getUpstreamBranch = function (branch) {
  if (branch == null) branch = this.getHead()
  if (!branch || !branch.startsWith('refs/heads/')) return null
  const shortBranch = branch.substring(11)

  const branchMerge = this.getConfigValue(`branch.${shortBranch}.merge`)
  if (!branchMerge || !branchMerge.startsWith('refs/heads/')) return null
  const shortBranchMerge = branchMerge.substring(11)

  const branchRemote = this.getConfigValue(`branch.${shortBranch}.remote`)
  if (!branch || branch.length === 0) return null

  return `refs/remotes/${branchRemote}/${shortBranchMerge}`
}

Repository.prototype.getRemoteHead = function (remoteName = 'origin') {
  return this.getSymbolicRefTarget(`refs/remotes/${remoteName}/HEAD`)
}

Repository.prototype.getAheadBehindCount = function (branch = 'HEAD') {
  if (branch !== 'HEAD' && !branch.startsWith('refs/heads/')) {
    branch = `refs/heads/${branch}`
  }

  const headCommit = this.getReferenceTarget(branch)
  if (!headCommit || headCommit.length === 0) return { ahead: 0, behind: 0 }

  const upstream = this.getUpstreamBranch()
  if (!upstream || upstream.length === 0) return { ahead: 0, behind: 0 }

  const upstreamCommit = this.getReferenceTarget(upstream)
  if (!upstreamCommit || upstreamCommit.length === 0) return { ahead: 0, behind: 0 }

  return this.compareCommits(headCommit, upstreamCommit)
}

Repository.prototype.getAheadBehindCountAsync = async function (branch = 'HEAD') {
  if (branch !== 'HEAD' && !branch.startsWith('refs/heads/')) {
    branch = `refs/heads/${branch}`
  }

  const headCommit = this.getReferenceTarget(branch)
  if (!headCommit || headCommit.length === 0) return { ahead: 0, behind: 0 }

  const upstream = this.getUpstreamBranch()
  if (!upstream || upstream.length === 0) return { ahead: 0, behind: 0 }

  const upstreamCommit = this.getReferenceTarget(upstream)
  if (!upstreamCommit || upstreamCommit.length === 0) return { ahead: 0, behind: 0 }

  return performAsyncWork(this, done => this.compareCommitsAsync(
    done,
    headCommit,
    upstreamCommit
  ))
}

Repository.prototype.checkoutReference = function (branch, create) {
  if (branch.indexOf('refs/heads/') !== 0) branch = `refs/heads/${branch}`
  return this.checkoutRef(branch, create)
}

Repository.prototype.relativize = function (filePath) {
  let workingDirectory
  if (!filePath) return filePath
  filePath = realpathRecursive(filePath)

  if (!IS_WINDOWS && filePath[0] !== '/') {
    return filePath
  }

  workingDirectory = this.getWorkingDirectory()
  if (workingDirectory) {
    if (pathStartsWith(filePath, workingDirectory, this.caseInsensitiveFs, false)) {
      return filePath.substring(workingDirectory.length + 1)
    } else if (pathsAreEqual(filePath, workingDirectory, this.caseInsensitiveFs, false)) {
      return ''
    }
  }

  if (this.openedWorkingDirectory) {
    workingDirectory = this.openedWorkingDirectory
    if (pathStartsWith(filePath, workingDirectory, this.caseInsensitiveFs, false)) {
      return filePath.substring(workingDirectory.length + 1)
    } else if (pathsAreEqual(filePath, workingDirectory, this.caseInsensitiveFs, false)) {
      return ''
    }
  }

  return filePath
}

Repository.prototype.submoduleForPath = function (filePath) {
  filePath = this.relativize(filePath)
  if (!filePath) return null

  for (const submodulePath in this.submodules) {
    const submoduleRepo = this.submodules[submodulePath]
    if (filePath === submodulePath) {
      return submoduleRepo
    } else if (filePath.startsWith(`${submodulePath}/`)) {
      filePath = filePath.substring(submodulePath.length + 1)
      return submoduleRepo.submoduleForPath(filePath) || submoduleRepo
    }
  }

  return null
}

Repository.prototype.isWorkingDirectory = function (dirPath) {
  if (!dirPath) return false
  dirPath = normalizePath(dirPath)

  if (!IS_WINDOWS && dirPath[0] !== '/') {
    return false
  }

  const workingDirectory = this.getWorkingDirectory()
  if (workingDirectory && pathsAreEqual(workingDirectory, dirPath, this.caseInsensitiveFs)) {
    return true
  }

  const openedWorkingDirectory = this.openedWorkingDirectory
  if (openedWorkingDirectory && pathsAreEqual(openedWorkingDirectory, dirPath, this.caseInsensitiveFs)) {
    return true
  }

  return false
}

const { getHeadAsync, getStatus, getStatusAsync, getStatusForPath } = Repository.prototype
delete Repository.prototype.getStatusForPath

Repository.prototype.getStatusForPaths = function (paths) {
  if (paths && paths.length > 0) {
    return getStatus.call(this, paths)
  } else {
    return {}
  }
}

Repository.prototype.getStatus = function (filePath) {
  if (typeof filePath === 'string') {
    return getStatusForPath.call(this, filePath)
  } else {
    return getStatus.call(this)
  }
}

Repository.prototype.getHeadAsync = function () {
  return performAsyncWork(this, done => getHeadAsync.call(this, done))
}

Repository.prototype.getStatusAsync = function () {
  return performAsyncWork(this, done => getStatusAsync.call(this, done))
}

Repository.prototype.getStatusForPathsAsync = function (paths) {
  return performAsyncWork(this, done => getStatusAsync.call(this, done, paths))
}

function performAsyncWork (repo, fn) {
  fn = promisify(fn)

  if (repo._lastAsyncPromise) {
    repo._lastAsyncPromise = repo._lastAsyncPromise.then(fn, fn)
  } else {
    repo._lastAsyncPromise = fn()
  }
  return repo._lastAsyncPromise
}

function promisify (fn) {
  return () => new Promise((resolve, reject) =>
    fn((error, result) => error ? reject(error) : resolve(result))
  )
}

// Given `unrealPath` — which may or may not exist on disk in its current form
// — resolve to a real path on disk, if possible.
//
// This is done by traversing upward to the first directory that _does_ exist,
// then getting its `realpath` and appending the rest back on.
function realpathRecursive (unrealPath) {
  let currentPath = unrealPath
  let result = unrealPath
  let remainder = ''
  if (!path.isAbsolute(unrealPath)) {
    return realpath(unrealPath)
  }
  while (!isRootPath(currentPath)) {
    try {
      result = typeof fs.realpathSync.native === 'function'
        ? fs.realpathSync.native(currentPath)
        : fs.realpathSync(currentPath)
      break
    } catch (e) {
      if (e.code === 'ENOENT') {
        currentPath = path.resolve(currentPath, '..')
        remainder = path.relative(currentPath, unrealPath)
      } else {
        return unrealPath
      }
    }
  }
  if (isRootPath(currentPath)) {
    return unrealPath
  }
  const finalResult = trimPath(`${result}/${remainder}`)
  return normalizePath(finalResult)
}

function trimPath (filePath) {
  if (!filePath.endsWith('/')) return filePath
  return filePath.replace(/\/$/, '')
}

// Attempts to resolve a path to its real path on disk; if it fails, returns
// the original path.
function realpath (unrealPath) {
  try {
    // `fs.realpathSync.native` somehow is the only thing that can consistently
    // normalize 8.3 "short names" in Windows to their long equivalents.
    if (typeof fs.realpathSync.native === 'function') {
      return fs.realpathSync.native(unrealPath)
    }
    return fs.realpathSync(unrealPath)
  } catch (e) {
    return unrealPath
  }
}

// Returns whether the path has no parent directory.
function isRootPath (repositoryPath) {
  if (IS_WINDOWS) {
    return /^[a-zA-Z]+:[\\/]$/.test(repositoryPath)
  } else {
    return repositoryPath === path.sep
  }
}

function openRepository (repositoryPath, search) {
  if (!fs.existsSync(repositoryPath)) return null
  const symlink = realpath(repositoryPath) !== repositoryPath
  repositoryPath = normalizePath(repositoryPath, false)

  const repository = new Repository(repositoryPath, search)
  if (repository.exists()) {
    repository.caseInsensitiveFs = fs.isCaseInsensitive()
    if (symlink) {
      const workingDirectory = repository.getWorkingDirectory()
      while (!isRootPath(repositoryPath)) {
        if (pathsAreEqual(repositoryPath, workingDirectory, fs.isCaseInsensitive())) {
          repository.openedWorkingDirectory = repositoryPath
          break
        }
        repositoryPath = path.resolve(repositoryPath, '..')
      }
    }
    return repository
  } else {
    return null
  }
}

function openSubmodules (repository) {
  repository.submodules = {}

  for (const relativePath of repository.getSubmodulePaths()) {
    if (relativePath) {
      const submodulePath = path.join(repository.getWorkingDirectory(), relativePath)
      const submoduleRepo = openRepository(submodulePath, false)
      if (submoduleRepo) {
        if (submoduleRepo.getPath() === repository.getPath()) {
          submoduleRepo.release()
        } else {
          openSubmodules(submoduleRepo)
          repository.submodules[relativePath] = submoduleRepo
        }
      }
    }
  }
}

exports.open = function (repositoryPath, search = true) {
  const repository = openRepository(repositoryPath, search)
  if (repository) openSubmodules(repository)
  return repository
}
