const fs = require('fs')
const path = require('path')

const [inputPath, outputPath] = process.argv.slice(2)
if (!inputPath || !outputPath) {
  throw new Error('Expected input and output paths')
}

const eol = '\n'
const source = fs.readFileSync(inputPath, 'utf8').replace(/\r\n/g, eol)
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

fs.mkdirSync(path.dirname(outputPath), { recursive: true })
fs.writeFileSync(outputPath, patched)
