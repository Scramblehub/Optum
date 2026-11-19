"""Planted: a customer and an order, each defined in several places. Decoys: Config, AuditEvent."""
from dataclasses import dataclass


@dataclass
class Customer:  # customer, place 1 of 4
    email: str
    first_name: str
    last_name: str
    phone: str
    plan: str = "free"


class Order:  # order, place 1 of 3
    def __init__(self, customer_email, total_cents, currency, shipping_address):
        self.customer_email = customer_email
        self.total_cents = total_cents
        self.currency = currency
        self.shipping_address = shipping_address


@dataclass
class Config:  # decoy: settings data, not an entity found elsewhere
    host: str
    port: int
    timeout_seconds: float
    retries: int


@dataclass
class AuditEvent:  # decoy: only generic fields shared with the products table
    id: int
    name: str
    created_at: str
    actor: str
