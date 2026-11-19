"""Imports: package, submodules, aliases, a relative import, a re-export, a stdlib import (no node)."""
import shop
from shop.orders import place_order


def run():
    return place_order([{"price": 10.0}])


if __name__ == "__main__":
    print(run())
    print(shop.place_order([{"price": 5.0}]))
