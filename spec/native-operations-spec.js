'use strict'

const childProcess = require('child_process')
const fs = require('fs')
const os = require('os')
const path = require('path')
const git = require('../src/git')

function runGit (workingDirectory, args, { allowFailure = false } = {}) {
  const result = childProcess.spawnSync('git', args, {
    cwd: workingDirectory,
    encoding: 'utf8'
  })
  if (!allowFailure && (result.error || result.status !== 0)) {
    throw result.error || new Error(`git ${args.join(' ')} failed: ${result.stderr}`)
  }
  return result
}

function makeRepository ({ sha256 = false } = {}) {
  const workingDirectory = fs.mkdtempSync(path.join(os.tmpdir(), 'git-utils-v10-'))
  const init = runGit(workingDirectory, ['init', ...(sha256 ? ['--object-format=sha256'] : [])], {
    allowFailure: sha256
  })
  if (init.status !== 0) {
    fs.rmSync(workingDirectory, { recursive: true, force: true })
    return null
  }
  runGit(workingDirectory, ['config', 'user.name', 'Git Utils'])
  runGit(workingDirectory, ['config', 'user.email', 'git-utils@example.test'])
  runGit(workingDirectory, ['config', 'core.autocrlf', 'false'])
  fs.writeFileSync(path.join(workingDirectory, 'tracked.txt'), 'one\ntwo\n')
  runGit(workingDirectory, ['add', 'tracked.txt'])
  runGit(workingDirectory, ['commit', '-m', 'initial'])
  return {
    workingDirectory,
    descriptor: {
      gitDirectory: path.join(workingDirectory, '.git'),
      workingDirectory
    }
  }
}

function canonicalPath (target) {
  if (fs.existsSync(target)) return fs.realpathSync.native(target)
  return path.join(fs.realpathSync.native(path.dirname(target)), path.basename(target))
}

describe('native v10 operations', () => {
  let fixture

  beforeEach(() => {
    fixture = makeRepository()
  })

  afterEach(() => {
    fs.rmSync(fixture.workingDirectory, { recursive: true, force: true })
  })

  it('returns Lumine status and refs snapshots from one native call', async () => {
    fs.writeFileSync(path.join(fixture.workingDirectory, 'tracked.txt'), 'one\nchanged\n')
    fs.writeFileSync(path.join(fixture.workingDirectory, 'untracked.txt'), 'new\n')
    const result = await git.snapshot(fixture.descriptor)
    const status = result.status.value
    const refs = result.refs.value

    expect(status).toEqual(jasmine.objectContaining({
      schemaVersion: 1,
      initialized: true,
      includesIgnored: false
    }))
    expect(status.files).toEqual(jasmine.arrayContaining([
      jasmine.objectContaining({ path: 'tracked.txt', worktreeStatus: 'M', unstaged: true }),
      jasmine.objectContaining({ path: 'untracked.txt', kind: 'untracked', untracked: true })
    ]))
    expect(status.counts.untracked).toBe(1)
    expect(refs).toEqual(jasmine.objectContaining({
      schemaVersion: 1,
      initialized: true,
      branches: jasmine.any(Array),
      remoteBranches: jasmine.any(Array),
      tags: jasmine.any(Array),
      remotes: jasmine.any(Array),
      worktrees: jasmine.any(Array)
    }))
    expect(refs.head.oid).toMatch(/^[0-9a-f]{40}$/)
  })

  it('reads structured diffs, history, commit details, blame, and objects', async () => {
    fs.writeFileSync(path.join(fixture.workingDirectory, 'tracked.txt'), 'one\nchanged\n')
    const diff = await git.diff(fixture.descriptor, {
      from: { type: 'commit', revision: 'HEAD' },
      to: { type: 'worktree' },
      format: 'both'
    })
    expect(diff.rawPatch).toEqual(jasmine.any(String))
    expect(diff.files[0]).toEqual(jasmine.objectContaining({
      newPath: 'tracked.txt',
      status: 'modified',
      hunks: jasmine.any(Array)
    }))
    const excluded = await git.diff(fixture.descriptor, {
      from: { type: 'commit', revision: 'HEAD' },
      to: { type: 'worktree' },
      diffFilter: 'm',
      format: 'both'
    })
    expect(excluded.files).toEqual([])
    expect(excluded.rawPatch).toBe('')
    const fromEmpty = await git.diff(fixture.descriptor, {
      from: { type: 'empty' },
      to: { type: 'commit', revision: 'HEAD' }
    })
    expect(fromEmpty.files).toEqual([
      jasmine.objectContaining({ newPath: 'tracked.txt', status: 'added' })
    ])

    const commits = await git.history(fixture.descriptor, { limit: 1 })
    expect(commits.length).toBe(1)
    expect(commits[0].author.date).toBeInstanceOf(Date)
    const detail = await git.commit(fixture.descriptor, { revision: commits[0].sha })
    expect(detail.files).toEqual([
      jasmine.objectContaining({ path: 'tracked.txt', status: 'added' })
    ])
    const blame = await git.blame(fixture.descriptor, { path: 'tracked.txt' })
    expect(blame.length).toBe(2)
    expect(blame[0].author.date).toBeInstanceOf(Date)

    runGit(fixture.workingDirectory, ['add', 'tracked.txt'])
    const objects = await git.readObjects(fixture.descriptor, [
      { revision: 'HEAD', path: 'tracked.txt' },
      { source: 'index', path: 'tracked.txt' },
      { oid: '0'.repeat(40) }
    ])
    expect(objects[0].type).toBe('blob')
    expect(objects[0].content.toString()).toBe('one\ntwo\n')
    expect(objects[1].content.toString()).toBe('one\nchanged\n')
    expect(objects[2]).toBeNull()
  })

  it('performs the safe native config, remote, blob, file, and merge mutations', async () => {
    await git.mutate(fixture.descriptor, {
      operation: 'setConfig',
      key: 'git-utils.test',
      value: 'native'
    })
    expect(await git.readConfig(fixture.descriptor, { keys: ['git-utils.test'] }))
      .toEqual({ 'git-utils.test': 'native' })
    await git.mutate(fixture.descriptor, { operation: 'unsetConfig', key: 'git-utils.test' })

    await git.mutate(fixture.descriptor, {
      operation: 'addRemote',
      name: 'test',
      url: 'https://example.test/one.git'
    })
    await git.mutate(fixture.descriptor, {
      operation: 'setRemoteUrl',
      name: 'test',
      url: 'https://example.test/two.git'
    })
    expect((await git.snapshot(fixture.descriptor, { status: false })).refs.value.remotes)
      .toEqual(jasmine.arrayContaining([
        jasmine.objectContaining({ name: 'test', fetchUrl: 'https://example.test/two.git' })
      ]))
    await git.mutate(fixture.descriptor, { operation: 'removeRemote', name: 'test' })

    const oid = await git.mutate(fixture.descriptor, {
      operation: 'createBlob',
      content: Buffer.from('native blob\n')
    })
    expect(oid).toMatch(/^[0-9a-f]{40}$/)
    const target = path.join(fixture.workingDirectory, 'expanded.txt')
    expect(await git.mutate(fixture.descriptor, {
      operation: 'expandBlobToFile',
      path: target,
      oid
    })).toBe(target)
    expect(fs.readFileSync(target, 'utf8')).toBe('native blob\n')

    const ours = path.join(fixture.workingDirectory, 'ours.txt')
    const base = path.join(fixture.workingDirectory, 'base.txt')
    const theirs = path.join(fixture.workingDirectory, 'theirs.txt')
    const result = path.join(fixture.workingDirectory, 'result.txt')
    fs.writeFileSync(ours, 'ours\n')
    fs.writeFileSync(base, 'base\n')
    fs.writeFileSync(theirs, 'theirs\n')
    expect(await git.mutate(fixture.descriptor, {
      operation: 'mergeFile',
      oursPath: ours,
      basePath: base,
      theirsPath: theirs,
      resultPath: result,
      labels: ['ours', 'base', 'theirs']
    })).toBe(1)
    expect(fs.readFileSync(result, 'utf8')).toContain('<<<<<<< ours')
  })

  it('preserves unborn symbolic HEAD names and reports ignored directories once', async () => {
    const empty = fs.mkdtempSync(path.join(os.tmpdir(), 'git-utils-unborn-'))
    try {
      runGit(empty, ['init', '--initial-branch=native-unborn'])
      fs.writeFileSync(path.join(empty, '.gitignore'), 'ignored/\n')
      fs.mkdirSync(path.join(empty, 'ignored', 'nested'), { recursive: true })
      fs.writeFileSync(path.join(empty, 'ignored', 'nested', 'file.txt'), 'ignored\n')
      const result = await git.snapshot({
        gitDirectory: path.join(empty, '.git'),
        workingDirectory: empty
      }, { includeIgnored: true })
      expect(result.status.value.head).toEqual(jasmine.objectContaining({
        oid: null,
        name: 'native-unborn',
        unborn: true
      }))
      expect(result.refs.value.head).toEqual(jasmine.objectContaining({
        oid: null,
        ref: 'refs/heads/native-unborn',
        name: 'native-unborn',
        unborn: true
      }))
      const ignored = result.status.value.files.filter(entry => entry.ignored)
      expect(ignored.map(entry => entry.path)).toEqual(['ignored/'])
    } finally {
      fs.rmSync(empty, { recursive: true, force: true })
    }
  })

  it('preserves CRLF hunk bytes and implements diff-filter all-or-none semantics', async () => {
    fs.writeFileSync(path.join(fixture.workingDirectory, 'tracked.txt'), 'one\r\nchanged\r\n')
    const diff = await git.diff(fixture.descriptor, {
      from: { type: 'commit', revision: 'HEAD' },
      to: { type: 'worktree' },
      format: 'both'
    })
    expect(diff.files[0].hunks[0].lines.some(line => line.text.endsWith('\r'))).toBeTrue()
    expect(diff.rawPatch).toContain('changed\r\n')
    const none = await git.diff(fixture.descriptor, {
      from: { type: 'commit', revision: 'HEAD' },
      to: { type: 'worktree' },
      diffFilter: 'A*'
    })
    expect(none.files).toEqual([])
    const all = await git.diff(fixture.descriptor, {
      from: { type: 'commit', revision: 'HEAD' },
      to: { type: 'worktree' },
      diffFilter: 'M*'
    })
    expect(all.files.length).toBe(1)
  })

  it('classifies binary patches and no-newline markers', async () => {
    fs.writeFileSync(path.join(fixture.workingDirectory, 'binary.dat'), Buffer.from([0, 1, 2, 3]))
    runGit(fixture.workingDirectory, ['add', 'binary.dat'])
    runGit(fixture.workingDirectory, ['commit', '-m', 'binary'])
    fs.writeFileSync(path.join(fixture.workingDirectory, 'binary.dat'), Buffer.from([0, 1, 9, 3]))
    fs.writeFileSync(path.join(fixture.workingDirectory, 'tracked.txt'), 'without newline')
    const diff = await git.diff(fixture.descriptor, {
      from: { type: 'commit', revision: 'HEAD' },
      to: { type: 'worktree' }
    })
    expect(diff.files.find(entry => entry.newPath === 'binary.dat').binary).toBeTrue()
    const text = diff.files.find(entry => entry.newPath === 'tracked.txt')
    expect(text.hunks.some(hunk => hunk.lines.some(line => line.kind === 'nonewline'))).toBeTrue()
  })

  it('reports exact unmerged index/worktree status characters', async () => {
    runGit(fixture.workingDirectory, ['checkout', '-b', 'conflict-side'])
    fs.writeFileSync(path.join(fixture.workingDirectory, 'tracked.txt'), 'side\n')
    runGit(fixture.workingDirectory, ['commit', '-am', 'side'])
    runGit(fixture.workingDirectory, ['checkout', 'master'])
    fs.writeFileSync(path.join(fixture.workingDirectory, 'tracked.txt'), 'main\n')
    runGit(fixture.workingDirectory, ['commit', '-am', 'main'])
    expect(runGit(fixture.workingDirectory, ['merge', 'conflict-side'], { allowFailure: true }).status)
      .not.toBe(0)
    const entry = (await git.snapshot(fixture.descriptor, { refs: false }))
      .status.value.files.find(file => file.path === 'tracked.txt')
    expect(entry).toEqual(jasmine.objectContaining({
      kind: 'unmerged',
      indexStatus: 'U',
      worktreeStatus: 'U',
      conflicted: true,
      staged: true,
      unstaged: true
    }))
  })

  it('matches contains/all describe for a detached ancestor', async () => {
    fs.writeFileSync(path.join(fixture.workingDirectory, 'tracked.txt'), 'second\n')
    runGit(fixture.workingDirectory, ['commit', '-am', 'second'])
    runGit(fixture.workingDirectory, ['checkout', '--detach', 'HEAD~1'])
    const expected = runGit(fixture.workingDirectory, [
      'describe', '--contains', '--all', '--always', 'HEAD'
    ]).stdout.trim()
    expect(await git.describe(fixture.descriptor)).toBe(expected)
  })

  it('resolves push refspecs and remote names containing slashes', async () => {
    runGit(fixture.workingDirectory, ['remote', 'add', 'team/origin', 'https://example.test/repo.git'])
    runGit(fixture.workingDirectory, ['config', 'branch.master.pushRemote', 'team/origin'])
    runGit(fixture.workingDirectory, ['config', 'push.default', 'current'])
    runGit(fixture.workingDirectory, ['update-ref', 'refs/remotes/team/origin/master', 'HEAD'])
    let refs = (await git.snapshot(fixture.descriptor, { status: false })).refs.value
    let branch = refs.branches.find(entry => entry.isHead)
    expect(branch.push.ref).toBe('refs/remotes/team/origin/master')
    expect(refs.remoteBranches.find(entry => entry.ref === branch.push.ref).remoteName)
      .toBe('team/origin')

    runGit(fixture.workingDirectory, [
      'config', 'remote.team/origin.push', 'refs/heads/master:refs/heads/deploy'
    ])
    runGit(fixture.workingDirectory, ['update-ref', 'refs/remotes/team/origin/deploy', 'HEAD'])
    refs = (await git.snapshot(fixture.descriptor, { status: false })).refs.value
    branch = refs.branches.find(entry => entry.isHead)
    expect(branch.push.ref).toBe('refs/remotes/team/origin/deploy')
  })

  it('includes primary, linked, and missing prunable worktrees', async () => {
    const suffix = `${process.pid}-${Date.now()}`
    const linked = path.join(os.tmpdir(), `git-utils-linked-${suffix}`)
    const missing = path.join(os.tmpdir(), `git-utils-missing-${suffix}`)
    try {
      runGit(fixture.workingDirectory, ['worktree', 'add', '-b', 'linked-test', linked])
      runGit(fixture.workingDirectory, ['worktree', 'add', '-b', 'missing-test', missing])
      fs.rmSync(missing, { recursive: true, force: true })
      const linkedGitDirectory = runGit(linked, ['rev-parse', '--absolute-git-dir']).stdout.trim()
      const refs = (await git.snapshot({
        gitDirectory: linkedGitDirectory,
        workingDirectory: linked
      }, { status: false })).refs.value
      expect(refs.worktrees.map(entry => canonicalPath(entry.path))).toEqual(jasmine.arrayContaining([
        canonicalPath(fixture.workingDirectory),
        canonicalPath(linked),
        canonicalPath(missing)
      ]))
      expect(refs.worktrees.find(entry => canonicalPath(entry.path) === canonicalPath(missing)).prunable)
        .toBeTrue()
    } finally {
      fs.rmSync(linked, { recursive: true, force: true })
      fs.rmSync(missing, { recursive: true, force: true })
      runGit(fixture.workingDirectory, ['worktree', 'prune'], { allowFailure: true })
    }
  })

  it('rejects invalid file modes without changing the index', async () => {
    const before = await git.fileMode(fixture.descriptor, 'tracked.txt')
    let error
    try {
      await git.mutate(fixture.descriptor, {
        operation: 'stageFileModeChange',
        path: 'tracked.txt',
        mode: 'garbage'
      })
    } catch (caught) {
      error = caught
    }
    expect(error.code).toBe('ERR_GIT_NATIVE_STAGE_FILE_MODE_CHANGE')
    expect(await git.fileMode(fixture.descriptor, 'tracked.txt')).toBe(before)
  })

  it('cancels queued native work with an AbortSignal', async () => {
    const controller = new AbortController()
    const text = 'unchanged line\n'.repeat(500000)
    const promise = git.lineDiff(text, `${text}changed\n`, { signal: controller.signal })
    setImmediate(() => controller.abort())
    let error
    try {
      await promise
    } catch (caught) {
      error = caught
    }
    expect(error.name).toBe('AbortError')
    expect(error.code).toBe('ERR_GIT_NATIVE_ABORTED')
  })

  it('uses Node UTF-8 replacement semantics for invalid POSIX path bytes', async () => {
    if (process.platform === 'win32') return pending('Windows paths are Unicode')
    const prefix = Buffer.from(`${fixture.workingDirectory}${path.sep}`)
    const invalidPath = Buffer.concat([prefix, Buffer.from([0x62, 0x61, 0x64, 0x80])])
    fs.writeFileSync(invalidPath, 'invalid path\n')
    const files = (await git.snapshot(fixture.descriptor, { refs: false })).status.value.files
    expect(files.some(entry => entry.path === 'bad�')).toBeTrue()
  })

  it('opens experimental SHA-256 repositories and preserves full OID lengths', async () => {
    const sha256 = makeRepository({ sha256: true })
    if (!sha256) return pending('The installed system Git cannot create SHA-256 repositories')
    try {
      const snapshot = await git.snapshot(sha256.descriptor)
      expect(snapshot.status.value.head.oid).toMatch(/^[0-9a-f]{64}$/)
      expect(snapshot.refs.value.head.oid).toMatch(/^[0-9a-f]{64}$/)
      const object = (await git.readObjects(sha256.descriptor, [
        { revision: 'HEAD', path: 'tracked.txt' }
      ]))[0]
      expect(object.oid).toMatch(/^[0-9a-f]{64}$/)
      const emptyDiff = await git.diff(sha256.descriptor, {
        from: { type: 'empty' },
        to: { type: 'commit', revision: 'HEAD' }
      })
      expect(emptyDiff.files[0].newPath).toBe('tracked.txt')
    } finally {
      fs.rmSync(sha256.workingDirectory, { recursive: true, force: true })
    }
  })
})
