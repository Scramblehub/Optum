// Class members that used to be lost or merged: symbol-keyed methods, a static method and an
// instance method with one name, a class expression's inner name, and `exit` on a non-process.
const kRun = Symbol('run')
const kLog = Symbol('log')

class Engine {
  [kRun](job) {
    return this[kLog](job)
  }

  [kLog](job) {
    return format(job)
  }

  start(job) {
    return this[kRun](job)
  }

  exit(code) {
    return code
  }
}

class Lexer {
  static lex(src) {
    return new Lexer().lex(src)
  }

  lex(src) {
    return this.scan(src)
  }

  scan(src) {
    return src
  }
}

const Parser = class _Parser {
  static parse(tokens) {
    return new _Parser().run(tokens)
  }

  run(tokens) {
    return tokens
  }
}

function format(job) {
  return String(job)
}

function shutdown(engine) {
  process.exit(1)
  return engine.exit(2)
}

// `lex` is a static and an instance method: not unique, so x.lex() is not guessed.
function probe(x) {
  return x.lex(1)
}

function main(src) {
  new Engine().start(src)
  return Parser.parse(Lexer.lex(src))
}

module.exports = { main, shutdown, probe }
