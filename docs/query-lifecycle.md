# Query Lifecycle

## Example Query

```sql
SELECT id, salary
FROM employees
WHERE id = 10
  AND age > 3;
```

---

## Step-by-Step

### Step 1 — SQL entered at terminal

The user types the query at the `SQL>` prompt in `prototype2_client`.

---

### Step 2 — Parser creates AST

`SQLParser::parse()` produces:

```
SelectStatement
  projections: [id, salary]
  tableName:   employees
  whereClause:
    AND
    ├── Equal
    │   ├── ColumnRef(id)
    │   └── IntLiteral(10)       ← plaintext at this stage
    └── GreaterThan
        ├── ColumnRef(age)
        └── IntLiteral(3)        ← plaintext at this stage
```

---

### Step 3 — Semantic analyzer validates

`SemanticAnalyzer::analyze()` checks:
- `employees` exists in the Catalog ✓
- `id`, `salary` are valid projection columns ✓
- `id`, `age` are valid WHERE columns ✓
- `id = 10`: INT64 column compared to integer literal ✓
- `age > 3`: INT64 column compared to integer literal ✓

Throws `SemanticError` on any violation before any encryption occurs.

---

### Step 4 — Logical planner creates logical plan

`LogicalPlanner::plan()` produces:

```
LogicalProject(id, salary)
  └── LogicalFilter(id=10 AND age>3)
        └── LogicalScan(employees)
```

Literals remain plaintext in the logical plan.

---

### Step 5 — Physical planner creates physical plan

`PhysicalPlanner::plan()` produces:

```
PhysicalPlan {
  tableName:      employees
  scanColumns:    [id, age, salary]   ← union of predicate + projection cols
  projectColumns: [id, salary]
  hasFilter:      true
}
```

---

### Step 6 — Query encryptor encrypts literals

`QueryEncryptor::encrypt()` walks the AST WHERE clause:

- `IntLiteral(10)` → `ctx.encryptScalar(10)` → `Enc(10)` (BFV ciphertext)
- `IntLiteral(3)`  → `ctx.encryptScalar(3)`  → `Enc(3)`  (BFV ciphertext)

The predicate tree is rebuilt as `PredNode` structs using `LiteralRef` IDs:

```
PredNode[4]: AND
  left  → PredNode[1]: Equal
            left  → PredNode[0]: ColumnRef(id)
            right → PredNode[1]: LiteralRef(id=0)   ← references Enc(10)
  right → PredNode[3]: GreaterThan
            left  → PredNode[2]: ColumnRef(age)
            right → PredNode[3]: LiteralRef(id=1)   ← references Enc(3)
```

The server will see the structure but not the values 10 or 3.

---

### Step 7 — QueryPackage created and serialized

```
QueryPackage {
  queryId:          "af28448dbd50589e"
  protocolVersion:  2
  tableName:        "employees"
  scanColumns:      ["id", "age", "salary"]
  projectColumns:   ["id", "salary"]
  predicateNodes:   [ColumnRef(id), LiteralRef(0), Equal,
                     ColumnRef(age), LiteralRef(1), GreaterThan, And]
  encryptedLiterals: [
    { id:0, type:"INT64", ciphertextBytes: <9615 bytes> },
    { id:1, type:"INT64", ciphertextBytes: <9615 bytes> }
  ]
  fheParams: { paramSetId:"bfv-257-depth9", slotCount:64, ... }
}
```

Serialized to ~19 KB via cereal binary archive.

---

### Step 8 — Query transmitted over TCP

`TcpClientTransport::sendQuery()` sends:
```
[type=QueryRequest][length=19695][payload=serialized QueryPackage]
```

---

### Step 9 — Server validates

`QueryValidator::validate()` checks:
- protocolVersion == 2 ✓
- table "employees" exists in server catalog ✓
- scan/project columns exist ✓
- fheParams.paramSetId matches server context ✓
- all LiteralRef IDs have corresponding encryptedLiterals ✓
- predicate tree is structurally valid ✓

---

### Step 10 — Server scans encrypted columns

`QueryExecutor::execute()` builds:
```
ProjectExec([id, salary])
  └── FilterExec(predicate)
        └── TableScanExec(employees, [id, age, salary])
```

`TableScanExec` reads from `FilesystemStorage`:
- `employees/id/p0/chunk_0.bin`   → encrypted id column
- `employees/age/p0/chunk_0.bin`  → encrypted age column
- `employees/salary/p0/chunk_0.bin` → encrypted salary column

Each column is a single BFV ciphertext packing all 10 row values.

---

### Step 11 — FHE predicates execute

`ExpressionExecutor::evaluate()` on the predicate tree:

```
EQ(id_ct, Enc(10)):
  d = EvalSub(id_ct, Enc(10))
  mask1 = 1 - d^(p-1)          [Fermat: d^256 = 0 iff d=0 mod 257]

GT(age_ct, Enc(3)):
  d = EvalSub(age_ct, Enc(3))
  mask2 = Σ_{k=1}^{128} [1 - (d-k)^256]

AND(mask1, mask2):
  final_mask = EvalMult(mask1, mask2)
```

All operations are homomorphic — the server never decrypts.

---

### Step 12 — Encrypted result generated

`ProjectExec` selects `id` and `salary` columns.
`FilterExec` attaches the encrypted selection mask.

`EncryptedResultBatch`:
```
{
  batchId: 0
  logicalRowCount: 10
  columns: [
    { name:"id",     ciphertextBytes: <9615 bytes> },
    { name:"salary", ciphertextBytes: <9615 bytes> }
  ]
  selectionMaskBytes: <9615 bytes>
  hasMask: true
}
```

---

### Step 13 — Result streamed to client

Server sends:
```
[type=QueryResponse][length=...][success=true, batchCount=1]
[type=ResultBatch][length=28698][serialized EncryptedResultBatch]
```

---

### Step 14 — Client decrypts

`ResultDecoder::decode()`:

1. Decrypt selection mask:
   ```
   mask = [0,0,0,0,0,0,0,0,0,1]   ← row 10 (index 9) matches
   ```

2. Decrypt id column:
   ```
   ids = [1,2,3,4,5,6,7,8,9,10]
   ```

3. Decrypt salary column:
   ```
   salaries = [5,6,7,8,9,10,11,12,13,14]
   ```

---

### Step 15 — Client applies mask and reconstructs rows

For each index i where mask[i] == 1:
```
i=9: id=10, salary=14  ← selected
```

---

### Step 16 — Result printed

```
id | salary
---+-------
10 | 14
(1 row)

[Verification] ✓ PASS — FHE result matches plaintext reference
```
