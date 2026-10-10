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
SELECT * FROM employees WHERE age >= 5 ORDER BY salary DESC LIMIT 3;
SELECT id, age FROM employees WHERE id != 5 ORDER BY age ASC;
SELECT id, name FROM employees WHERE name = 'Alice';
SELECT e.id, e.name, d.department_name
FROM employees AS e
JOIN departments AS d ON e.department_id = d.id;
SELECT e.id, e.age, d.department_name
FROM employees e
JOIN departments d ON e.department_id = d.id
WHERE e.age > 5;
```

Expected output (first query):
```
id | salary
---+-------
10 | 14
(1 row)
```

## Run Tests

```bash
cd build && ctest --output-on-failure
# 69/69 tests pass
```

## Run Benchmark

```bash
./bin/prototype2_benchmark
```

## Supported SQL

```sql
SELECT col1, col2, ...   -- or SELECT *
FROM table [AS alias]
[INNER] JOIN table [AS alias] ON qualified_column = qualified_column
[WHERE expr]
[ORDER BY column [ASC|DESC], column [ASC|DESC], ...]
[LIMIT n]

-- Expressions:
column = literal
column != literal
column > integer
column < integer
column >= integer
column <= integer
expr AND expr
expr OR expr
NOT expr
```

### Supported query examples

Single-table filtering:

```sql
SELECT id, salary
FROM employees
WHERE id = 10 AND age > 3;
```

String equality and inequality:

```sql
SELECT id, name
FROM employees
WHERE name = 'Alice';

SELECT id
FROM employees
WHERE name != 'Alice';
```

Inner equality JOIN:

```sql
SELECT e.id, e.name, d.department_name
FROM employees AS e
JOIN departments AS d
  ON e.department_id = d.id;
```

JOIN with a left-table WHERE predicate:

```sql
SELECT e.id, e.age, d.department_name
FROM employees e
JOIN departments d
  ON e.department_id = d.id
WHERE e.age > 5;
```

`WHERE` predicates and JOIN equality masks are evaluated over encrypted
ciphertexts on the server. `ORDER BY` and `LIMIT` are applied by the client
after result decryption.

## Supported Tables

```
employees: id INT64, age INT64, salary INT64, department_id INT64,
           name STRING(16)
departments: id INT64, department_name STRING(16)
```

Values must be in [0, 128] due to BFV plaintext modulus p=257.
String columns use fixed-width ASCII encoding. String predicates currently
support `=` and `!=`; the configured sample columns support up to 16 ASCII
characters. Unicode, `LIKE`, and string ordering are not supported.

## Unsupported (Future Work)

- LEFT/RIGHT/FULL OUTER JOIN, CROSS JOIN, multiple JOIN clauses
- Non-equality JOIN conditions
- GROUP BY, aggregates, HAVING
- NULL, BETWEEN, IN, LIKE
- String ordering comparisons and Unicode strings
- Transactions, MVCC, WAL
- Distributed sharding
- Encrypted indexes / ORAM
- Full query structure confidentiality

The current JOIN executor uses encrypted nested-loop evaluation and the setup
path ingests JOIN test data with one logical row per encrypted chunk. This is
correct but substantially slower than the single-table execution path for
larger datasets.

## Architecture

See `docs/architecture.md`, `docs/query-lifecycle.md`, `docs/security-model.md`.

## Security Notice

This prototype uses N=128 ring dimension for demonstration speed. This does NOT
provide cryptographic security. Production use requires N ≥ 4096 with
`HEStd_128_classic` or higher. See `docs/security-model.md`.
