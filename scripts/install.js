const childProcess = require('child_process')
const fs = require('fs')
const path = require('path')

const root = path.resolve(__dirname, '..')
const libgit2Revision = 'f7164261c9bc0a7e0ebf767c584e5192810a8b24'
const libgit2Tag = 'v1.9.4'
const libgit2Path = path.join(root, 'deps', 'libgit2')
const libgit2Sentinel = path.join(libgit2Path, 'src', 'libgit2', 'annotated_commit.c')
const libgit2IgnorePath = path.join(libgit2Path, 'src', 'libgit2', 'ignore.c')

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

function patchLibgit2IgnoreRules () {
  const original = fs.readFileSync(libgit2IgnorePath, 'utf8')
  const eol = '\n'
  const source = original.replace(/\r\n/g, eol)
  const parserStart = source.indexOf('static int parse_ignore_file(')
  const helpersStart = source.indexOf('/**' + eol + ' * A negative ignore pattern')

  if (helpersStart < 0 || parserStart < 0 || helpersStart > parserStart) {
    throw new Error('Unable to locate libgit2 ignore-rule optimization')
  }

  let patched = source.slice(0, helpersStart) + source.slice(parserStart)
  patched = patched.replace(
    '\twhile (!error && *scan) {' + eol + '\t\tint valid_rule = 1;' + eol + eol,
    '\twhile (!error && *scan) {' + eol
  )

  const optimizationStart = patched.indexOf(
    '\t\t\t/*' + eol + '\t\t\t * If a negative match doesn\'t actually do anything,'
  )
  const optimizationEndText =
    '\t\t\tif (!error && valid_rule)' + eol +
    '\t\t\t\terror = git_vector_insert(&attrs->rules, match);'
  const optimizationEnd = patched.indexOf(optimizationEndText, optimizationStart)

  if (optimizationStart < 0 || optimizationEnd < 0) {
    throw new Error('Unable to patch libgit2 ignore-rule optimization')
  }

  patched =
    patched.slice(0, optimizationStart) +
    '\t\t\t/* Negative rules can override patterns from lower-precedence' + eol +
    '\t\t\t * ignore sources, so they must be retained even when they do not' + eol +
    '\t\t\t * negate an earlier rule in this file. */' + eol +
    '\t\t\tif (!error)' + eol +
    '\t\t\t\terror = git_vector_insert(&attrs->rules, match);' +
    patched.slice(optimizationEnd + optimizationEndText.length)

  patched = patched.replace(
    '\t\tif (error != 0 || !valid_rule) {',
    '\t\tif (error != 0) {'
  )

  fs.writeFileSync(libgit2IgnorePath, patched)
  return () => fs.writeFileSync(libgit2IgnorePath, original)
}

ensureLibgit2Sources()
const restoreLibgit2IgnoreRules = patchLibgit2IgnoreRules()
try {
  run('node-gyp', ['rebuild'], { shell: process.platform === 'win32' })
} finally {
  restoreLibgit2IgnoreRules()
}
