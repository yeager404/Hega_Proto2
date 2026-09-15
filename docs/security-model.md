# Security Model — Prototype 2

## What This Prototype Is

Prototype 2 — End-to-End FHE SQL Query Execution.

This is NOT a production-ready encrypted database. It is a research prototype
establishing the correct client/server/query execution foundation for a future
fully homomorphically encrypted SQL database.

---

## Trust Boundary

| Component | Trusted? | Holds secret key? | Sees plaintext data? |
|---|---|---|---|
| Client process | Yes | Yes | Yes (after decryption) |
| ClientCryptoContext | Yes | Yes | Yes |
| ServerCryptoContext | No | **No** | **No** |
| Server process | No | **No** | **No** |
| EncryptedStorage (disk) | No | No | No |
| FHE Runtime (server) | No | No | No |
| Network transport | No | No | No |

The client owns the `SecretKey`. It is saved to `<keys-dir>/client/key-private.bin`
and is never transmitted to the server.

---

## What Is Protected

- **Database contents at rest**: all column values are BFV-encrypted on disk.
  The server reads only ciphertexts.

- **Query literal values**: integer literals in WHERE clauses (e.g. `id = 10`,
  `age > 30`) are encrypted client-side before the query package is sent. The
  server receives `Enc(10)` and `Enc(30)`, not `10` and `30`.

- **Intermediate results**: all predicate evaluation (EQ, GT, LT, AND, OR, NOT)
  operates on ciphertexts. The server never decrypts intermediate masks.

- **Final results**: result batches are transmitted as ciphertexts. Only the
  client decrypts them.

- **Private key**: never leaves the client process.

---

## What Is Currently Visible to the Server

- **Query structure**: table name, column names, predicate operators, projection
  list. The server sees `EQ(id, Enc(?))` — it knows the operator and column but
  not the literal value.

- **Ciphertext sizes**: the server can observe the size of encrypted literals
  and result ciphertexts.

- **Number of result batches**: the server knows how many chunks were scanned.

- **Access patterns**: the server observes which storage chunks are read.

- **Schema metadata**: table and column names are in the query package.

---

## What Is NOT Solved in This Prototype

| Gap | Notes |
|---|---|
| Full query structure confidentiality | Server sees table/column names and operators |
| Oblivious RAM (ORAM) | Server observes which storage chunks are accessed |
| Leakage-resistant indexing | No encrypted indexes; full scan always performed |
| Malicious-server verification | No proof that server executed correctly |
| Differential privacy | No noise added to results |
| Side-channel resistance | Timing of FHE operations may leak information |
| Result count leakage | Server knows how many ciphertexts are in each batch |
| Ciphertext size leakage | Ciphertext sizes are observable |

---

## Architecture Extensibility Toward Stronger Security

The current architecture is designed so future phases can add:

1. **Encrypted query structure**: replace table/column names with encrypted
   tokens; use oblivious query routing.

2. **ORAM**: replace FilesystemStorage with an ORAM-backed store so the server
   cannot observe access patterns.

3. **Verifiable computation**: add ZK proofs that the server executed the
   correct plan.

4. **Distributed execution**: the PhysicalPlan and transport abstractions are
   shard-aware (shardId, partitionId fields exist in QueryPackage and ChunkMeta)
   to support future coordinator/shard topology.

---

## FHE Scheme Constraints

- Scheme: BFV (Brakerski-Fan-Vercauteren)
- Plaintext modulus: p = 257 (prime)
- Values must be in [0, (p-1)/2] = [0, 128] for correct GT/LT semantics
- Multiplicative depth: 9 (supports EQ depth 8 + AND depth 1)
- Ring dimension: N = 128 (small, for demonstration; production needs N ≥ 4096)
- Security level: HEStd_NotSet (research only — not cryptographically secure
  at this ring dimension)

**This prototype does NOT provide cryptographic security at the chosen
parameters.** The ring dimension N=128 is chosen for speed in demonstration.
A production deployment requires N ≥ 4096 with HEStd_128_classic or higher.
