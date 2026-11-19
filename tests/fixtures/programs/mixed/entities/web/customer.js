const axios = require('axios')

class Customer { // customer, place 3 of 4
  constructor (email, firstName, lastName, phone) {
    this.email = email
    this.firstName = firstName
    this.lastName = lastName
    this.phone = phone
  }
}

function pushToCrm (c) { // customer, place 4 of 4: the CRM's contact payload
  return axios.post('https://crm.example.com/api/contacts', {
    email: c.email, firstName: c.firstName, lastName: c.lastName, phone: c.phone, source: 'web'
  })
}

module.exports = { Customer, pushToCrm }
