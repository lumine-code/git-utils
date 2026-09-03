const childProcess = require('child_process')
const fs = require('fs')
const path = require('path')

const root = path.resolve(__dirname, '..')
const libgit2Revision = '26055f5af74ab1cf636d272e8a34315496d3f06f'
const libgit2Tag = 'v1.9.6'
const libgit2Path = path.join(root, 'deps', 'libgit2')
const libgit2Sentinel = path.join(libgit2Path, 'src', 'libgit2', 'annotated_commit.c')
const addonPath = path.join(root, 'build', 'Release', 'git.node')
const bindingPath = path.join(root, 'binding.gyp')
const nativeSourcePath = path.join(root, 'src', 'native.cc')

function run (command, args, options = {}) {
  const result = childProcess.spawnSync(command, args, {
    cwd: options.cwd || root,
    shell: options.shell || false,
    stdio: 'inherit'
  })

  if (result.error) throw result.error
  if (result.status !== 0) {
    throw new Error(`${command} ${args.join(' ')} failed with exit code ${result.status}`)
  }
}

function hasGitMetadata () {
  return fs.existsSync(path.join(root, '.git'))
}

function hasLibgit2Sources () {
  return fs.existsSync(libgit2Sentinel)
}

function hasBindingInputs () {
  return fs.existsSync(bindingPath) && fs.existsSync(nativeSourcePath)
}

function hasValidAddon () {
  if (!fs.existsSync(addonPath)) return false
  const validation = childProcess.spawnSync(process.execPath, [
    '-e',
    "const version=require(process.argv[1]).versions();process.exit(version.gitUtils==='10.0.0'&&version.libgit2==='1.9.6'?0:1)",
    addonPath
  ], {
    cwd: root,
    stdio: 'ignore'
  })
  return !validation.error && validation.status === 0
}

function currentLibgit2Revision () {
  if (!hasLibgit2Sources()) return null
  try {
    return childProcess.execFileSync('git', ['-C', libgit2Path, 'rev-parse', 'HEAD'], {
      encoding: 'utf8',
      stdio: ['ignore', 'pipe', 'ignore']
    }).trim()
  } catch {
    return null
  }
}

function checkoutPinnedLibgit2 () {
  if (currentLibgit2Revision() === libgit2Revision) return
  run('git', ['-C', libgit2Path, 'fetch', '--depth', '1', 'origin', libgit2Revision])
  run('git', ['-C', libgit2Path, 'checkout', '--detach', libgit2Revision])
}

function ensureLibgit2Sources () {
  if (hasLibgit2Sources() && currentLibgit2Revision() === libgit2Revision) return

  if (hasGitMetadata()) {
    run('git', ['submodule', 'update', '--init', '--recursive'])
    if (hasLibgit2Sources()) {
      checkoutPinnedLibgit2()
      if (currentLibgit2Revision() === libgit2Revision) return
    }
  }

  fs.rmSync(libgit2Path, { force: true, recursive: true })
  fs.mkdirSync(path.dirname(libgit2Path), { recursive: true })
  run('git', [
    'clone',
    '--depth',
    '1',
    '--branch',
    libgit2Tag,
    '--no-checkout',
    'https://github.com/libgit2/libgit2.git',
    libgit2Path
  ])
  run('git', ['-C', libgit2Path, 'sparse-checkout', 'init', '--cone'])
  run('git', ['-C', libgit2Path, 'sparse-checkout', 'set', 'deps', 'include', 'src'])
  run('git', ['-C', libgit2Path, 'checkout', libgit2Tag])

  checkoutPinnedLibgit2()
}

// Script-suppressed application installs invoke this mode explicitly before
// electron-rebuild. It hydrates the exact libgit2 sources without compiling
// for npm's host runtime first.
if (process.argv.includes('--prepare-build')) {
  if (!hasBindingInputs()) {
    throw new Error('The git-utils package does not contain its native build inputs')
  }
  ensureLibgit2Sources()
// A source checkout always rebuilds so edits and a freshly hydrated submodule
// are reflected. Packed installs prefer their compatible Node-API binary, but
// can hydrate and compile from their complete build inputs when needed.
} else if (hasGitMetadata()) {
  ensureLibgit2Sources()
  run('node-gyp', ['rebuild'], { shell: process.platform === 'win32' })
} else if (!hasValidAddon()) {
  if (hasBindingInputs()) {
    ensureLibgit2Sources()
    run('node-gyp', ['rebuild'], { shell: process.platform === 'win32' })
  } else {
    throw new Error('The git-utils package contains neither a compatible addon nor its build inputs')
  }
}
