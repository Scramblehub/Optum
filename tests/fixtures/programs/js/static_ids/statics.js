// A static method's id is Class.static.name whether or not the class also has an instance
// method of that name (compare Lexer in ../members): adding one later must not rename it.
class Lexer {
  static lex(src) {
    return clean(src)
  }
}

class Parser {
  static parse(src) {
    return this.build(src)
  }

  static build(src) {
    return clean(src)
  }
}

function clean(src) {
  return src.trim()
}

function main(src) {
  return Parser.parse(Lexer.lex(src))
}

module.exports = { main }
