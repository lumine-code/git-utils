#!/usr/bin/env node
'use strict'

const path = require('path')
const { performance } = require('perf_hooks')
const git = require('../src/git')

const workingDirectory = path.resolve(process.argv[2] || process.cwd())
const gitDirectory = path.resolve(process.argv[3] || path.join(workingDirectory, '.git'))
const descriptor = { gitDirectory, workingDirectory }
const samples = positiveInteger(process.env.GIT_UTILS_BENCHMARK_SAMPLES, 50)
const warmups = positiveInteger(process.env.GIT_UTILS_BENCHMARK_WARMUPS, 5)

function positiveInteger (value, fallback) {
  const parsed = Number.parseInt(value, 10)
  return Number.isInteger(parsed) && parsed > 0 ? parsed : fallback
}

function round (value) {
  return Number(value.toFixed(3))
}

function summarize (durations) {
  const sorted = durations.slice().sort((left, right) => left - right)
  const percentile = value => sorted[Math.min(sorted.length - 1, Math.ceil(sorted.length * value) - 1)]
  return {
    median: round(percentile(0.5)),
    p95: round(percentile(0.95)),
    min: round(sorted[0]),
    max: round(sorted[sorted.length - 1])
  }
}

async function measure (operation) {
  for (let index = 0; index < warmups; index++) await operation()
  const durations = []
  for (let index = 0; index < samples; index++) {
    const startedAt = performance.now()
    await operation()
    durations.push(performance.now() - startedAt)
  }
  return summarize(durations)
}

async function main () {
  const operations = {
    snapshot: () => git.snapshot(descriptor, { status: true, refs: true }),
    readConfig: () => git.readConfig(descriptor, {
      keys: ['core.repositoryformatversion', 'core.bare', 'remote.origin.url']
    }),
    diff: () => git.diff(descriptor, {
      from: { type: 'commit', revision: 'HEAD' },
      to: { type: 'worktree' },
      format: 'structured'
    })
  }
  const results = {}
  for (const [name, operation] of Object.entries(operations)) {
    results[name] = await measure(operation)
  }
  process.stdout.write(JSON.stringify({
    schemaVersion: 1,
    unit: 'milliseconds',
    descriptor,
    runtime: {
      node: process.version,
      platform: process.platform,
      arch: process.arch,
      versions: git.versions()
    },
    routingNote: 'Lumine statically routes status and worktree diffs to system Git for repositories that declare submodules.',
    warmups,
    samples,
    operations: results
  }, null, 2) + '\n')
}

main().catch(error => {
  process.stderr.write(`${error.stack || error}\n`)
  process.exitCode = 1
})
