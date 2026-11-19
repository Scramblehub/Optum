const BaseCommand = require('./base.js')

class Bye extends BaseCommand {
  exec (args) {
    return 'bye ' + args.join(' ')
  }
}

module.exports = Bye
