'use strict'

const path = require('path')
const git = require('../src/git')

const workingDirectory = path.resolve(__dirname, '..')
const descriptor = Object.freeze({
  gitDirectory: path.join(workingDirectory, '.git'),
  workingDirectory
})

async function rejectionOf (promise) {
  try {
    await promise
  } catch (error) {
    return error
  }
  throw new Error('Expected the Promise to reject')
}

describe('git-utils v10', () => {
  it('exports only the stateless v10 API', () => {
    expect(Object.keys(git).sort()).toEqual([
      'blame',
      'branchesContaining',
      'commit',
      'configure',
      'describe',
      'diff',
      'fileMode',
      'history',
      'lineDiff',
      'mutate',
      'readConfig',
      'readObjects',
      'snapshot',
      'submodulePaths',
      'versions'
    ])
    expect(git.open).toBeUndefined()
    expect(git.Repository).toBeUndefined()
  })

  it('reports the addon and libgit2 versions', () => {
    const versions = git.versions()
    expect(versions).toEqual({
      gitUtils: '10.0.0',
      napi: jasmine.any(Number),
      libgit2: '1.9.6',
      libgit2Features: jasmine.any(Number)
    })
    expect(Number.isInteger(versions.napi)).toBeTrue()
    expect(Number.isInteger(versions.libgit2Features)).toBeTrue()
  })

  it('configures process-wide ownership validation synchronously', () => {
    expect(git.configure({ validateOwnership: false })).toEqual({ validateOwnership: false })
    expect(git.configure({ validateOwnership: true })).toEqual({ validateOwnership: true })
    expect(() => git.configure({})).toThrowError(TypeError)
  })

  describe('Promise contract', () => {
    const calls = [
      ['snapshot', () => git.snapshot(null)],
      ['diff', () => git.diff(null)],
      ['history', () => git.history(null)],
      ['commit', () => git.commit(null)],
      ['blame', () => git.blame(null)],
      ['describe', () => git.describe(null)],
      ['branchesContaining', () => git.branchesContaining(null)],
      ['readObjects', () => git.readObjects(null, [])],
      ['readConfig', () => git.readConfig(null)],
      ['fileMode', () => git.fileMode(null, 'package.json')],
      ['submodulePaths', () => git.submodulePaths(null)],
      ['lineDiff', () => git.lineDiff('before\n', 'after\n')],
      ['mutate', () => git.mutate(null, {})]
    ]

    for (const [name, call] of calls) {
      it(`${name} returns a Promise`, async () => {
        const promise = call()
        expect(promise instanceof Promise).toBeTrue()
        await promise.catch(() => {})
      })
    }
  })

  describe('validation and errors', () => {
    it('rejects an invalid descriptor with a stable argument error', async () => {
      const error = await rejectionOf(git.snapshot({}))
      expect(error).toEqual(jasmine.objectContaining({
        code: 'ERR_GIT_NATIVE_ARGUMENT',
        operation: 'snapshot'
      }))
      expect(error).toBeInstanceOf(TypeError)
    })

    it('rejects invalid operation requests before native work begins', async () => {
      const diffError = await rejectionOf(git.diff(descriptor, { format: 'html' }))
      expect(diffError.code).toBe('ERR_GIT_NATIVE_ARGUMENT')
      expect(diffError.operation).toBe('diff')

      for (const maxBytes of [-1, 1.5, Infinity, '10']) {
        const limitError = await rejectionOf(git.diff(descriptor, { maxBytes }))
        expect(limitError.code).toBe('ERR_GIT_NATIVE_ARGUMENT')
        expect(limitError.operation).toBe('diff')
      }

      const objectsError = await rejectionOf(git.readObjects(descriptor, {}))
      expect(objectsError.code).toBe('ERR_GIT_NATIVE_ARGUMENT')
      expect(objectsError.operation).toBe('readObjects')

      const mutationError = await rejectionOf(git.mutate(descriptor, {}))
      expect(mutationError.code).toBe('ERR_GIT_NATIVE_ARGUMENT')
      expect(mutationError.operation).toBe('mutate')
    })

    it('preserves structured libgit2 failure details', async () => {
      const error = await rejectionOf(git.snapshot({
        gitDirectory: path.join(workingDirectory, '.missing-git-directory'),
        workingDirectory
      }))
      expect(error.code).toBe('ERR_GIT_NATIVE_SNAPSHOT')
      expect(error.operation).toBe('snapshot')
      expect(error.libgit2Code).toEqual(jasmine.any(Number))
      expect(error.libgit2Class).toEqual(jasmine.any(Number))
      expect(error.libgit2Message).toEqual(jasmine.any(String))
    })

    it('rejects an already aborted operation without opening a repository', async () => {
      const controller = new AbortController()
      controller.abort()
      const error = await rejectionOf(git.snapshot(descriptor, { signal: controller.signal }))
      expect(error.name).toBe('AbortError')
      expect(error.code).toBe('ERR_GIT_NATIVE_ABORTED')
      expect(error.operation).toBe('snapshot')
    })
  })

  describe('native reads', () => {
    it('suppresses unchanged snapshot values by fingerprint', async () => {
      const first = await git.snapshot(descriptor, { status: true, refs: true })
      expect(first.status.fingerprint).toMatch(/^[0-9a-f]{64}$/)
      expect(first.refs.fingerprint).toMatch(/^[0-9a-f]{64}$/)
      expect(first.status.unchanged).toBeFalse()
      expect(first.refs.unchanged).toBeFalse()
      expect(first.status.value).toBeDefined()
      expect(first.refs.value).toBeDefined()

      const second = await git.snapshot(descriptor, {
        status: true,
        refs: true,
        knownFingerprints: {
          status: first.status.fingerprint,
          refs: first.refs.fingerprint
        }
      })
      expect(second.status).toEqual({
        fingerprint: first.status.fingerprint,
        unchanged: true
      })
      expect(second.refs).toEqual({
        fingerprint: first.refs.fingerprint,
        unchanged: true
      })
    })

    it('performs representative stateless config, diff, and line reads', async () => {
      const config = await git.readConfig(descriptor, {
        keys: ['core.repositoryformatversion', 'core.bare']
      })
      expect(config).toEqual(jasmine.any(Object))

      const diff = await git.diff(descriptor, {
        from: { type: 'commit', revision: 'HEAD' },
        to: { type: 'commit', revision: 'HEAD' }
      })
      expect(diff).toEqual({ schemaVersion: 1, files: [] })

      const lines = await git.lineDiff(Buffer.from('before\n'), Buffer.from('after\n'))
      expect(lines).toEqual(jasmine.any(Array))
      expect(Object.isFrozen(descriptor)).toBeTrue()
    })
  })
})
