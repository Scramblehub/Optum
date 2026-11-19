// Planted unused code in JS, and decoys.
function unusedFormatter (x) { // PLANTED: not exported, never used
  return String(x)
}

const byDate = (a, b) => a.date - b.date // decoy: a callback

function sortOrders (orders) { // decoy: exported
  return orders.sort(byDate)
}

class Cart {
  #recount () { // PLANTED: a #private method nothing calls
    return 0
  }

  #total () { // decoy: a #private method used inside the class
    return 1
  }

  sum () {
    return this.#total()
  }
}

module.exports = { sortOrders, Cart }
