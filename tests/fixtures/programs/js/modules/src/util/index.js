// CommonJS module, reached through its directory name.
function format(total) {
  return JSON.stringify({ total });
}

function unusedHelper() {
  return 1;
}

module.exports = { format, unusedHelper };
