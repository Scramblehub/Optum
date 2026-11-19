import requests


def push_order(order):
    payload = {"customer_email": order.customer_email, "total_cents": order.total_cents,
               "currency": order.currency, "shipping_address": order.shipping_address}
    return requests.post("https://shop.example.com/api/orders", json=payload)  # order, place 3 of 3
