#!/usr/bin/env node
'use strict'

const childProcess = require('child_process')
const fs = require('fs')
const os = require('os')
const path = require('path')

const root = path.resolve(__dirname, '..')
const addonPath = path.join(root, 'build', 'Release', 'git.node')
const hiddenAddonPath = `${addonPath}.package-smoke`
const temporaryRoot = fs.mkdtempSync(path.join(os.tmpdir(), 'git-utils-package-'))
let addonHidden = false

function run (command, args, options = {}) {
  const result = childProcess.spawnSync(command, args, {
    cwd: options.cwd || root,
    encoding: options.encoding || 'utf8',
    env: process.env,
    shell: options.shell || false,
    stdio: options.stdio || ['ignore', 'pipe', 'inherit']
  })
  if (result.error) throw result.error
  if (result.status !== 0) {
    throw new Error(`${command} ${args.join(' ')} failed with exit code ${result.status}`)
  }
  return result.stdout || ''
}

function assertExists (target) {
  if (!fs.existsSync(target)) throw new Error(`Packed build input is missing: ${target}`)
}

try {
  if (fs.existsSync(addonPath)) {
    if (fs.existsSync(hiddenAddonPath)) {
      throw new Error(`Refusing to overwrite package-smoke backup: ${hiddenAddonPath}`)
    }
    fs.renameSync(addonPath, hiddenAddonPath)
    addonHidden = true
  }

  const packOutput = run('npm', [
    'pack', '--json', '--ignore-scripts', '--pack-destination', temporaryRoot
  ], { shell: process.platform === 'win32' })
  const packed = JSON.parse(packOutput)[0]
  const tarball = path.join(temporaryRoot, packed.filename)

  if (addonHidden) {
    fs.renameSync(hiddenAddonPath, addonPath)
    addonHidden = false
  }

  const consumer = path.join(temporaryRoot, 'consumer')
  fs.mkdirSync(consumer)
  fs.writeFileSync(path.join(consumer, 'package.json'), JSON.stringify({
    name: 'git-utils-package-smoke',
    private: true,
    version: '1.0.0'
  }))
  run('npm', ['install', '--ignore-scripts', tarball], {
    cwd: consumer,
    shell: process.platform === 'win32'
  })

  const installed = path.join(consumer, 'node_modules', '@lumine-code', 'git-utils')
  assertExists(path.join(installed, 'binding.gyp'))
  assertExists(path.join(installed, 'src', 'native.cc'))
  assertExists(path.join(installed, 'scripts', 'patch-libgit2-ignore.js'))
  assertExists(path.join(installed, 'deps', 'libgit2', 'include', 'git2.h'))
  assertExists(path.join(installed, 'deps', 'libgit2', 'src', 'libgit2', 'repository.c'))
  assertExists(path.join(installed, 'deps', 'libgit2', 'deps', 'xdiff', 'xdiffi.c'))
  if (fs.existsSync(path.join(installed, 'build', 'Release', 'git.node'))) {
    throw new Error('The no-prebuilt package smoke unexpectedly installed git.node')
  }

  const nodeGyp = require.resolve('node-gyp/bin/node-gyp.js')
  run(process.execPath, [nodeGyp, 'rebuild'], {
    cwd: installed,
    stdio: 'inherit'
  })

  const versions = JSON.parse(run(process.execPath, [
    '-e',
    'process.stdout.write(JSON.stringify(require(process.argv[1]).versions()))',
    installed
  ]))
  if (versions.gitUtils !== '10.0.0' || versions.libgit2 !== '1.9.6') {
    throw new Error(`Unexpected rebuilt addon versions: ${JSON.stringify(versions)}`)
  }
  process.stdout.write(JSON.stringify({
    packageSize: packed.size,
    unpackedSize: packed.unpackedSize,
    entryCount: packed.entryCount,
    versions
  }, null, 2) + '\n')
} finally {
  if (addonHidden) fs.renameSync(hiddenAddonPath, addonPath)
  fs.rmSync(temporaryRoot, { recursive: true, force: true, maxRetries: 5, retryDelay: 100 })
}
