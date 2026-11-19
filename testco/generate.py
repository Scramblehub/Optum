#!/usr/bin/env python3
"""The test company's data, with its ground truth (docs/plans/phase3.md).

    python3 testco/generate.py [--seed 1] [--people 500] [--orders 2000] [-o testco/data]

Writes one JSON file per tool (each with its own record ids and its own way of spelling people)
and ground_truth.json: which records in which tools are the same person, and which are the same
order. Deterministic for a given seed. No third-party packages.

Planted, so record matching has traps (the plan's "never by coincidence"):
  - two different people named John Smith (different emails, phones, cities): never one person;
  - one person with a second email, used in some tools only: still one person;
  - name typos and nicknames in some tools (Jon, Katherine/Catherine, a dropped letter);
  - missing phones and addresses where a tool doesn't ask for them.
"""
import argparse
import json
import os
import random

FIRST = ["James", "Mary", "Robert", "Patricia", "Michael", "Jennifer", "William", "Linda", "David", "Elizabeth",
         "Richard", "Barbara", "Joseph", "Susan", "Thomas", "Jessica", "Charles", "Sarah", "Daniel", "Karen",
         "Matthew", "Lisa", "Anthony", "Nancy", "Mark", "Betty", "Steven", "Sandra", "Paul", "Ashley", "Andrew",
         "Emily", "Joshua", "Donna", "Kevin", "Michelle", "Brian", "Carol", "George", "Amanda", "Aisha", "Wei",
         "Priya", "Mateo", "Sofia", "Yusuf", "Chloe", "Liam", "Noor", "Hiro", "Catherine", "Jon"]
LAST = ["Smith", "Johnson", "Williams", "Brown", "Jones", "Garcia", "Miller", "Davis", "Rodriguez", "Martinez",
        "Hernandez", "Lopez", "Gonzalez", "Wilson", "Anderson", "Thomas", "Taylor", "Moore", "Jackson", "Martin",
        "Lee", "Perez", "Thompson", "White", "Harris", "Sanchez", "Clark", "Ramirez", "Lewis", "Robinson", "Walker",
        "Young", "Allen", "King", "Wright", "Scott", "Torres", "Nguyen", "Hill", "Flores", "Green", "Adams", "Khan",
        "Patel", "Chen", "Okafor", "Novak", "Silva", "Dubois", "Rossi"]
CITIES = ["Leeds", "Bristol", "Manchester", "Glasgow", "Cardiff", "Belfast", "York", "Norwich", "Brighton", "Oxford"]
STREETS = ["High Street", "Station Road", "Church Lane", "Victoria Road", "Mill Lane", "Park Avenue", "Green Lane"]
DOMAINS = ["example.com", "mail.example.net", "post.example.org", "inbox.example.co.uk"]
NICK = {"Jonathan": "Jon", "William": "Bill", "Robert": "Bob", "Elizabeth": "Liz", "Catherine": "Katherine",
        "Michael": "Mike", "Daniel": "Dan", "Joseph": "Joe", "Thomas": "Tom", "Jennifer": "Jen"}
PRODUCTS = [("SKU-TEE-01", "Organic cotton tee", 1800), ("SKU-MUG-02", "Stoneware mug", 1200),
            ("SKU-BAG-03", "Canvas tote", 1500), ("SKU-CAP-04", "Wool cap", 2200), ("SKU-NB-05", "Recycled notebook", 900),
            ("SKU-SOX-06", "Merino socks", 1400), ("SKU-SCF-07", "Linen scarf", 2600), ("SKU-BTL-08", "Steel bottle", 2000)]


def person(rng, pid, first=None, last=None):
    first = first or rng.choice(FIRST)
    last = last or rng.choice(LAST)
    n = rng.randint(1, 999)
    return {
        "person_id": pid,
        "first": first,
        "last": last,
        "email": f"{first.lower()}.{last.lower()}{n}@{rng.choice(DOMAINS)}",
        "phone": f"+44 7{rng.randint(100, 999)} {rng.randint(100000, 999999)}",
        "street": f"{rng.randint(1, 220)} {rng.choice(STREETS)}",
        "city": rng.choice(CITIES),
        "postcode": f"{rng.choice('BLMGCYNO')}{rng.choice('SDNREX')}{rng.randint(1, 20)} {rng.randint(1, 9)}"
                    f"{rng.choice('ABDEFGH')}{rng.choice('JLNPQRSTUWXYZ')}",
    }


def spelled(rng, p, typo_rate):
    """How a tool might spell someone: a nickname, or a dropped letter, now and then."""
    first, last = p["first"], p["last"]
    r = rng.random()  # drawn even when unused, so the random sequence doesn't depend on who's a trap
    if p.get("exact_name"):
        return first, last
    if r < typo_rate / 2 and first in NICK:
        first = NICK[first]
    elif r < typo_rate and len(last) > 4:
        i = rng.randrange(1, len(last) - 1)
        last = last[:i] + last[i + 1:]
    return first, last


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--seed", type=int, default=1)
    ap.add_argument("--people", type=int, default=500)
    ap.add_argument("--orders", type=int, default=2000)
    ap.add_argument("-o", "--out", default=os.path.join(os.path.dirname(os.path.abspath(__file__)), "data"))
    a = ap.parse_args()
    rng = random.Random(a.seed)

    people = [person(rng, f"P{i:04d}") for i in range(a.people)]
    # The traps: two different John Smiths; one person with a second email.
    people[0] = person(rng, "P0000", "John", "Smith")
    people[1] = person(rng, "P0001", "John", "Smith")
    people[0]["exact_name"] = people[1]["exact_name"] = True  # always spelled alike: the name can't tell them apart
    while people[1]["city"] == people[0]["city"]:
        people[1]["city"] = rng.choice(CITIES)
    people[2] = person(rng, "P0002", "Catherine", "Novak")
    people[2]["email_alt"] = f"cathy.novak@{DOMAINS[3]}"

    tools = {t: [] for t in ["shop", "crm", "helpdesk", "marketing", "forms", "scheduling", "sheets",
                            "payments", "accounting"]}
    truth = {"people": {}, "orders": {}}

    def add(tool, p, rec_id, rec, typo_rate=0.0, alt_email=False):
        first, last = spelled(rng, p, typo_rate)
        rec = {"id": rec_id, **rec}
        rec.setdefault("first_name", first)
        rec.setdefault("last_name", last)
        rec.setdefault("email", p.get("email_alt") if alt_email and p.get("email_alt") else p["email"])
        tools[tool].append(rec)
        truth["people"].setdefault(p["person_id"], []).append({"tool": tool, "id": rec_id})

    # Who is in which tool. Buyers: 60% of people; everyone in the CRM (leads and buyers).
    buyers = [p for p in people if rng.random() < 0.6] or people[:1]
    for trap in people[:3]:
        if trap not in buyers:
            buyers.append(trap)
    for i, p in enumerate(people):
        add("crm", p, f"crm-{1000 + i}", {"phone": p["phone"], "company": None,
                                          "stage": "customer" if p in buyers else "lead"}, typo_rate=0.15)
    for i, p in enumerate(buyers):
        add("shop", p, 101 + i, {"billing": {"address_1": p["street"], "city": p["city"], "postcode": p["postcode"],
                                             "phone": p["phone"]}}, alt_email=(p["person_id"] == "P0002"))
        add("payments", p, f"cus_{a.seed:02d}{i:06d}", {"name": f"{p['first']} {p['last']}"})
        add("accounting", p, 7 + i, {"name": f"{p['first']} {p['last']}", "street": p["street"], "city": p["city"],
                                     "zip": p["postcode"]}, typo_rate=0.05)
    for i, p in enumerate(rng.sample(people, k=a.people // 3)):
        add("helpdesk", p, 500 + i, {"phone": p["phone"] if rng.random() < 0.5 else None}, typo_rate=0.1,
            alt_email=(p["person_id"] == "P0002"))
    for i, p in enumerate(rng.sample(people, k=int(a.people * 0.7))):
        add("marketing", p, f"sub-{i}", {"subscribed": rng.random() < 0.9})
    for i, p in enumerate(rng.sample(people, k=a.people // 4)):
        add("forms", p, f"resp_{i:05d}", {"answers": {"how_did_you_hear": rng.choice(["friend", "search", "ad"])}})
    for i, p in enumerate(rng.sample(people, k=a.people // 10)):
        add("scheduling", p, f"bk-{i}", {"event": "fitting appointment"})
    for i, p in enumerate(rng.sample(buyers, k=len(buyers) // 5)):  # a VIP list kept by hand
        add("sheets", p, f"row{i + 2}", {"notes": "VIP"}, typo_rate=0.2)

    # Orders: each in the shop, paid in payments, invoiced in accounting (a tax document).
    shop_id = {r["email"]: r["id"] for r in tools["shop"]}
    for k in range(a.orders):
        p = rng.choice(buyers)
        items = [(sku, name, price, rng.randint(1, 3)) for sku, name, price in rng.sample(PRODUCTS, k=rng.randint(1, 3))]
        total = sum(price * q for _, _, price, q in items)
        oid = f"O{k:05d}"
        email = p.get("email_alt") if p["person_id"] == "P0002" else p["email"]
        order = {"id": 5000 + k, "customer_id": shop_id.get(email), "total_cents": total, "currency": "GBP",
                 "line_items": [{"sku": s, "name": n, "unit_cents": pr, "quantity": q} for s, n, pr, q in items]}
        tools["shop"].append({**order, "type": "order"})
        charge = {"id": f"ch_{a.seed:02d}{k:07d}", "type": "charge", "amount": total, "currency": "gbp",
                  "receipt_email": p["email"]}
        tools["payments"].append(charge)
        invoice = {"id": 90000 + k, "type": "invoice", "regulated": "tax", "partner_email": p["email"],
                   "amount_total_cents": total, "lines": [{"name": n, "quantity": q, "price_unit_cents": pr}
                                                         for _, n, pr, q in items]}
        tools["accounting"].append(invoice)
        truth["orders"][oid] = {"person_id": p["person_id"], "shop": order["id"], "payments": charge["id"],
                                "accounting": invoice["id"], "total_cents": total}

    os.makedirs(a.out, exist_ok=True)
    for tool, recs in tools.items():
        with open(os.path.join(a.out, f"{tool}.json"), "w") as f:
            json.dump({"tool": tool, "records": recs}, f, indent=1)
    truth["traps"] = {"two_people_one_name": ["P0000", "P0001"], "one_person_two_emails": "P0002"}
    truth["seed"] = a.seed
    with open(os.path.join(a.out, "ground_truth.json"), "w") as f:
        json.dump(truth, f, indent=1)
    counts = ", ".join(f"{t} {len(r)}" for t, r in tools.items())
    print(f"{len(people)} people, {a.orders} orders -> {a.out}: {counts}")


if __name__ == "__main__":
    main()
