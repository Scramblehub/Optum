"""Decoy: a medical record (regulated) with the customer's fields: never proposed for merging."""
from dataclasses import dataclass


@dataclass
class PatientRecord:
    email: str
    first_name: str
    last_name: str
    phone: str
    mrn: str
