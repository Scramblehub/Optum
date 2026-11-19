# Shadowing: pkg defines cli() and also has a submodule pkg/cli.py.
import pkg

if __name__ == "__main__":
    pkg.cli()
