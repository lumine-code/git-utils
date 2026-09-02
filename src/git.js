'use strict'

const crypto = require('crypto')
const fs = require('fs')
const path = require('path')
const binding = require('../build/Release/git.node')

const DESCRIPTOR_KEYS = ['gitDirectory', 'workingDirectory']
let nextCancelId = 1

function nativeError (operation, message, code = 'ERR_GIT_NATIVE_ARGUMENT') {
  const error = new TypeError(message)
  error.code = code
  error.operation = operation
  return error
}

function validateDescriptor (operation, descriptor) {
  if (!descriptor || typeof descriptor !== 'object') {
    throw nativeError(operation, 'A repository descriptor is required')
  }
  if (typeof descriptor.gitDirectory !== 'string' || descriptor.gitDirectory.length === 0) {
    throw nativeError(operation, 'descriptor.gitDirectory must be a non-empty string')
  }
  if (descriptor.workingDirectory != null && typeof descriptor.workingDirectory !== 'string') {
    throw nativeError(operation, 'descriptor.workingDirectory must be a string or null')
  }
  return Object.fromEntries(DESCRIPTOR_KEYS.map(key => [key, descriptor[key] ?? null]))
}

function abortError (operation) {
  const error = new Error(`Native Git ${operation} was aborted`)
  error.name = 'AbortError'
  error.code = 'ERR_GIT_NATIVE_ABORTED'
  error.operation = operation
  return error
}

async function invoke (operation, descriptor, request = {}, signal = request?.signal) {
  if (signal?.aborted) throw abortError(operation)
  const cleanRequest = request && typeof request === 'object'
    ? Object.fromEntries(Object.entries(request).filter(([key]) => key !== 'signal'))
    : request
  const cancelId = nextCancelId++
  if (nextCancelId >= Number.MAX_SAFE_INTEGER) nextCancelId = 1
  const promise = binding.run(operation, descriptor, cleanRequest, cancelId)
  if (!signal || typeof signal.addEventListener !== 'function') {
    return JSON.parse(await promise)
  }

  return await new Promise((resolve, reject) => {
    let settled = false
    const onAbort = () => {
      if (settled) return
      settled = true
      binding.cancel(cancelId)
      reject(abortError(operation))
    }
    signal.addEventListener('abort', onAbort, { once: true })
    promise.then(
      raw => {
        if (settled) return
        settled = true
        signal.removeEventListener('abort', onAbort)
        resolve(JSON.parse(raw))
      },
      error => {
        if (settled) return
        settled = true
        signal.removeEventListener('abort', onAbort)
        reject(error)
      }
    )
  })
}

function fingerprint (value) {
  const stableValue = value && typeof value === 'object' && 'generation' in value
    ? Object.fromEntries(Object.entries(value).filter(([key]) => key !== 'generation'))
    : value
  return crypto.createHash('sha256').update(JSON.stringify(stableValue)).digest('hex')
}

function section (value, knownFingerprint) {
  const currentFingerprint = fingerprint(value)
  return currentFingerprint === knownFingerprint
    ? { fingerprint: currentFingerprint, unchanged: true }
    : { fingerprint: currentFingerprint, unchanged: false, value }
}

function reviveCommit (value) {
  if (!value) return value
  if (value.author?.date != null) value.author.date = new Date(value.author.date)
  if (value.committer?.date != null) value.committer.date = new Date(value.committer.date)
  if (value.committerDate != null) value.committerDate = new Date(value.committerDate)
  return value
}

function reviveSnapshot (value) {
  for (const collection of [value.branches, value.remoteBranches, value.tags]) {
    for (const entry of collection || []) reviveCommit(entry.lastCommit)
  }
  const worktrees = new Map()
  for (const entry of value.worktrees || []) {
    const normalizedPath = realpathRecursive(entry.path)
    entry.path = normalizedPath
    const key = process.platform === 'win32' ? normalizedPath.toLowerCase() : normalizedPath
    const existing = worktrees.get(key)
    if (!existing) {
      worktrees.set(key, entry)
    } else {
      existing.headOid ??= entry.headOid
      existing.branch ??= entry.branch
      existing.detached ||= entry.detached
      existing.bare ||= entry.bare
      existing.locked ||= entry.locked
      existing.lockedReason ??= entry.lockedReason
      existing.prunable ||= entry.prunable
    }
  }
  if (value.worktrees) {
    value.worktrees = Array.from(worktrees.values()).sort((left, right) =>
      left.path.localeCompare(right.path))
  }
  return value
}

function realpathRecursive (target) {
  let current = path.resolve(target)
  const remainder = []
  while (!fs.existsSync(current)) {
    const parent = path.dirname(current)
    if (parent === current) return path.resolve(target)
    remainder.unshift(path.basename(current))
    current = parent
  }
  try {
    const resolved = typeof fs.realpathSync.native === 'function'
      ? fs.realpathSync.native(current)
      : fs.realpathSync(current)
    return path.join(resolved, ...remainder)
  } catch {
    return path.resolve(target)
  }
}

exports.versions = function versions () {
  return binding.versions()
}

exports.configure = function configure (options = {}) {
  if (typeof options.validateOwnership !== 'boolean') {
    throw nativeError('configure', 'options.validateOwnership must be a boolean')
  }
  return binding.configure(options.validateOwnership)
}

exports.snapshot = async function snapshot (descriptor, options = {}) {
  descriptor = validateDescriptor('snapshot', descriptor)
  const statusRequested = options.status !== false
  const refsRequested = options.refs !== false
  if (!statusRequested && !refsRequested) return {}

  const generations = options.generations || {}
  const value = await invoke('snapshot', descriptor, {
    status: statusRequested,
    refs: refsRequested,
    includeIgnored: options.includeIgnored === true,
    statusGeneration: generations.status ?? 1,
    refsGeneration: generations.refs ?? 1
  }, options.signal)
  const known = options.knownFingerprints || {}
  const result = {}
  if (statusRequested) result.status = section(value.status, known.status)
  if (refsRequested) result.refs = section(reviveSnapshot(value.refs), known.refs)
  return result
}

exports.diff = async function diff (descriptor, request = {}) {
  descriptor = validateDescriptor('diff', descriptor)
  const format = request.format || 'structured'
  if (!['structured', 'patch', 'both'].includes(format)) {
    throw nativeError('diff', `Unsupported diff format: ${format}`)
  }
  const native = await invoke('diff', descriptor, { ...request, format }, request.signal)
  const result = { schemaVersion: 1, files: native.files }
  if (format === 'patch' || format === 'both') result.rawPatch = native.rawPatch
  return result
}

exports.history = async function history (descriptor, request = {}) {
  descriptor = validateDescriptor('history', descriptor)
  const result = await invoke('history', descriptor, request, request.signal)
  return result.map(reviveCommit)
}

exports.commit = async function commit (descriptor, request = {}) {
  descriptor = validateDescriptor('commit', descriptor)
  return reviveCommit(await invoke('commit', descriptor, request, request.signal))
}

exports.blame = async function blame (descriptor, request = {}) {
  descriptor = validateDescriptor('blame', descriptor)
  const result = await invoke('blame', descriptor, request, request.signal)
  for (const row of result) {
    if (row.author?.date != null) row.author.date = new Date(row.author.date)
  }
  return result
}

exports.describe = async function describe (descriptor, request = {}) {
  descriptor = validateDescriptor('describe', descriptor)
  return invoke('describe', descriptor, request, request.signal)
}

exports.branchesContaining = async function branchesContaining (descriptor, request = {}) {
  descriptor = validateDescriptor('branchesContaining', descriptor)
  return invoke('branchesContaining', descriptor, request, request.signal)
}

exports.readObjects = async function readObjects (descriptor, requests, options = {}) {
  descriptor = validateDescriptor('readObjects', descriptor)
  if (!Array.isArray(requests)) throw nativeError('readObjects', 'requests must be an array')
  const result = await invoke('readObjects', descriptor, { requests }, options.signal)
  return result.map(object => object == null
    ? null
    : { ...object, content: Buffer.from(object.content, 'base64') })
}

exports.readConfig = async function readConfig (descriptor, request = {}) {
  descriptor = validateDescriptor('readConfig', descriptor)
  return invoke('readConfig', descriptor, request, request.signal)
}

exports.fileMode = async function fileMode (descriptor, path, options = {}) {
  descriptor = validateDescriptor('fileMode', descriptor)
  return invoke('fileMode', descriptor, { path }, options.signal)
}

exports.submodulePaths = async function submodulePaths (descriptor, options = {}) {
  descriptor = validateDescriptor('submodulePaths', descriptor)
  return invoke('submodulePaths', descriptor, {}, options.signal)
}

exports.lineDiff = async function lineDiff (oldText, newText, options = {}) {
  const asString = value => Buffer.isBuffer(value) ? value.toString() : String(value ?? '')
  return invoke('lineDiff', {}, {
    oldText: asString(oldText),
    newText: asString(newText),
    ignoreEolWhitespace: options.ignoreEolWhitespace === true || options.ignoreSpaceAtEOL === true,
    ignoreSpaceChange: options.ignoreSpaceChange === true,
    ignoreAllSpace: options.ignoreAllSpace === true
  }, options.signal)
}

exports.mutate = async function mutate (descriptor, request = {}) {
  descriptor = validateDescriptor('mutate', descriptor)
  if (typeof request.operation !== 'string' || request.operation.length === 0) {
    throw nativeError('mutate', 'request.operation must be a non-empty string')
  }
  const normalized = { ...request }
  if (Buffer.isBuffer(normalized.content)) normalized.content = normalized.content.toString('base64')
  normalized.contentEncoding = Buffer.isBuffer(request.content) ? 'base64' : 'utf8'
  return invoke('mutate', descriptor, normalized, request.signal)
}
