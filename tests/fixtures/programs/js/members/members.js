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

// `lex` is a static and an instance method of one class: the receiver's form picks one
// (narrowed_by receiver_form). A call result gives no guess.
class Host {
  constructor(lexer) {
    this.lexer = lexer
  }

  run(src) {
    return this.lexer.lex(src)
  }
}

function viaClass(Cmd, src) {
  return Cmd.lex(src)
}

function make() {
  return null
}

function indirect(src) {
  return make().lex(src)
}

function main(src) {
  new Engine().start(src)
  return Parser.parse(Lexer.lex(src))
}

module.exports = { main, shutdown, Host, viaClass, indirect }
