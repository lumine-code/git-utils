const childProcess = require('child_process')
const fs = require('fs')
const path = require('path')

const root = path.resolve(__dirname, '..')

if (fs.existsSync(path.join(root, '.git'))) {
  const result = childProcess.spawnSync('git', ['submodule', 'update', '--init', '--recursive'], {
    cwd: root,
    stdio: 'inherit'
  })

  if (result.error) throw result.error
  process.exit(result.status)
}
