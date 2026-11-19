const BaseCommand = require('./base.js')

class Hello extends BaseCommand {
  exec (args) {
    return 'hello ' + args.join(' ')
  }
}

module.exports = Hello
