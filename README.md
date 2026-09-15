# Prototype 2 — End-to-End FHE SQL Query Execution

A terminal-driven SQL client/server system where database contents remain
encrypted at rest and during server-side query execution. The server never
holds the client's private key and never decrypts data.

## Quick Start

```bash
cd "Prototype 2"
mkdir build && cd build
cmake .. -DCMAKE_BUILD_TYPE=Release
cmake --build . -j$(nproc)

# Step 1: generate FHE keys and encrypt the database (client-side)
./bin/prototype2_client --setup \
    --keys-dir /tmp/p2_keys \
    --data-dir /tmp/p2_data

# Step 2: start the server (Terminal 1)
./bin/prototype2_server \
    --keys-dir /tmp/p2_keys \
    --data-dir /tmp/p2_data \
    --port 7777

# Step 3: start the client (Terminal 2)
./bin/prototype2_client \
    --keys-dir /tmp/p2_keys \
    --port 7777
```

Then at the `SQL>` prompt:

```sql
SELECT id, salary FROM employees WHERE id = 10 AND age > 3;
```

Expected output:
```
id | salary
---+-------
10 | 14
(1 row)

[Verification] ✓ PASS — FHE result matches plaintext reference
```

## Run Tests

```bash
cd build && ctest --output-on-failure
# 35/35 tests pass
```

## Run Benchmark

```bash
./bin/prototype2_benchmark
```

## Supported SQL

```sql
SELECT col1, col2, ...   -- or SELECT *
FROM table
[WHERE expr]

-- Expressions:
column = literal
column > literal
column < literal
expr AND expr
expr OR expr
NOT expr
```

## Supported Tables

```
employees: id INT64, age INT64, salary INT64, department_id INT64
```

Values must be in [0, 128] due to BFV plaintext modulus p=257.

## Unsupported (Future Work)

- JOIN, GROUP BY, ORDER BY, LIMIT, aggregates
- String literals, NULL, BETWEEN, IN
- Multiple tables
- Transactions, MVCC, WAL
- Distributed sharding
- Encrypted indexes / ORAM
- Full query structure confidentiality

## Architecture

See `docs/architecture.md`, `docs/query-lifecycle.md`, `docs/security-model.md`.

## Security Notice

This prototype uses N=128 ring dimension for demonstration speed. This does NOT
provide cryptographic security. Production use requires N ≥ 4096 with
`HEStd_128_classic` or higher. See `docs/security-model.md`.
