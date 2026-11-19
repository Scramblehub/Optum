// Commands loaded by computed path, as npm does: every file the pattern matches may run.
class Cli {
  static load (name) {
    return require(`./commands/${name}.js`)
  }

  run (name, args) {
    const Command = this.constructor.load(name)
    const command = new Command(this)
    return command.exec(args.map(normalize).sort(byName))
  }
}

const normalize = (a) => a.trim()
const byName = (a, b) => a.localeCompare(b)

module.exports = Cli
