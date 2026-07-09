const childProcess = require('child_process')
const fs = require('fs')
const path = require('path')

const root = path.resolve(__dirname, '..')
const libgit2Revision = 'f7164261c9bc0a7e0ebf767c584e5192810a8b24'
const libgit2Tag = 'v1.9.4'
const libgit2Path = path.join(root, 'deps', 'libgit2')
const libgit2Sentinel = path.join(libgit2Path, 'src', 'libgit2', 'annotated_commit.c')

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

function ensureLibgit2Sources () {
  if (hasLibgit2Sources()) return

  if (hasGitMetadata()) {
    run('git', ['submodule', 'update', '--init', '--recursive'])
    if (hasLibgit2Sources()) return
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

  const revision = childProcess.execFileSync('git', ['-C', libgit2Path, 'rev-parse', 'HEAD'], {
    encoding: 'utf8'
  }).trim()

  if (revision !== libgit2Revision) {
    run('git', ['-C', libgit2Path, 'fetch', '--depth', '1', 'origin', libgit2Revision])
    run('git', ['-C', libgit2Path, 'checkout', libgit2Revision])
  }
}

ensureLibgit2Sources()
run('node-gyp', ['rebuild'], { shell: process.platform === 'win32' })
