I added a patch to a free software library that supports key exchange using
elliptic curve `Curve25519'.

Curve25519 is a relatively new elliptic curve for ecdhe* in process of
standardization by ietf, praised for its speed and security.

* = elliptic curve diffie hellmen ephemeral key exchange

The work I explicitly did:
  branch tlsCurve25519
  commits
    20a24ec8454b0f4f1a501f06a13a33ca3eb717b4
    96b330dd8b122642f16ab3de3ad17f33b7fdda32
