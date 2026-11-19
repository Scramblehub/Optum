// CommonJS function export, called straight from require(): require("./audit")(...).
module.exports = function audit(message) {
  return `audit: ${message}`;
};
