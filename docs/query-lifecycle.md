# Query Lifecycle

## Example Query

```sql
SELECT id, salary
FROM employees
WHERE age >= 5
ORDER BY salary DESC
LIMIT 3;
```

This query demonstrates:
- Projection: `id, salary`
- Filter with new operator: `age >= 5`
- Client-side sorting: `ORDER BY salary DESC`
- Result truncation: `LIMIT 3`

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
    GreaterEqual
    ├── ColumnRef(age)
    └── IntLiteral(5)        ← plaintext at this stage
  orderByColumns: [
    { columnName: "salary", direction: Descending }
  ]
  limitCount: 3
```

---

### Step 3 — Semantic analyzer validates

`SemanticAnalyzer::analyze()` checks:
- `employees` exists in the Catalog ✓
- `id`, `salary` are valid projection columns ✓
- `age` is a valid WHERE column ✓
- `age >= 5`: INT64 column compared to integer literal ✓
- `salary` is a valid ORDER BY column ✓
- `limitCount = 3` is non-negative ✓

Throws `SemanticError` on any violation before any encryption occurs.

---

### Step 4 — Logical planner creates logical plan

`LogicalPlanner::plan()` produces:

```
LogicalProject(id, salary, age)  ← age added for ORDER BY sorting
  └── LogicalFilter(age>=5)
        └── LogicalScan(employees)
```

Note: `age` is added to the projection even though not requested by the user,
because it's needed for client-side ORDER BY sorting.

---

### Step 5 — Physical planner creates physical plan

`PhysicalPlanner::plan()` produces:

```
PhysicalPlan {
  tableName:      employees
  scanColumns:    [id, salary, age]   ← union of predicate + projection cols
  projectColumns: [id, salary, age]   ← includes ORDER BY column
  hasFilter:      true
}
```

---

### Step 6 — Query encryptor encrypts literals

`QueryEncryptor::encrypt()` walks the AST WHERE clause:

- `IntLiteral(5)` → `ctx.encryptScalar(5)` → `Enc(5)` (BFV ciphertext)

The predicate tree is rebuilt as `PredNode` structs using `LiteralRef` IDs:

```
PredNode[2]: GreaterEqual
  left  → PredNode[0]: ColumnRef(age)
  right → PredNode[1]: LiteralRef(id=0)   ← references Enc(5)
```

The server will see the structure but not the value 5. The `>=` operator is
implemented server-side as `GT(age, 5) OR EQ(age, 5)` using FHE operations.

---

### Step 7 — QueryPackage created and serialized

```
QueryPackage {
  queryId:          "af28448dbd50589e"
  protocolVersion:  2
  tableName:        "employees"
  scanColumns:      ["id", "age", "salary"]
  projectColumns:   ["id", "salary", "age"]
  predicateNodes:   [ColumnRef(age), LiteralRef(0), GreaterEqual]
  encryptedLiterals: [
    { id:0, type:"INT64", ciphertextBytes: <9615 bytes> }
  ]
  fheParams: { paramSetId:"bfv-257-depth12", slotCount:64, ... }
}
```

Note: ORDER BY and LIMIT are NOT sent to the server — they are applied
client-side after decryption.

Serialized to ~10 KB via cereal binary archive.

---

### Step 8 — Query transmitted over TCP

`TcpClientTransport::sendQuery()` sends:
```
[type=QueryRequest][length=10245][payload=serialized QueryPackage]
```

---

### Step 9 — Server validates

`QueryValidator::validate()` checks:
- protocolVersion == 2 ✓
- table "employees" exists in server catalog ✓
- scan/project columns exist ✓
- fheParams.paramSetId matches server context ✓
- all LiteralRef IDs have corresponding encryptedLiterals ✓
- predicate tree is structurally valid (GreaterEqual recognized) ✓

---

### Step 10 — Server scans encrypted columns

`QueryExecutor::execute()` builds:
```
ProjectExec([id, salary, age])
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
GE(age_ct, Enc(5)):
  # Implemented as GT(age_ct, Enc(5)) OR EQ(age_ct, Enc(5))
  
  # First compute GT:
  d = EvalSub(age_ct, Enc(5))
  mask_gt = Σ_{k=1}^{128} [1 - (d-k)^256]
  
  # Then compute EQ:
  mask_eq = 1 - d^256
  
  # Combine with OR:
  mask_ge = mask_gt + mask_eq - (mask_gt * mask_eq)
```

All operations are homomorphic — the server never decrypts.

---

### Step 12 — Encrypted result generated

`ProjectExec` selects `id`, `salary`, and `age` columns.
`FilterExec` attaches the encrypted selection mask.

`EncryptedResultBatch`:
```
{
  batchId: 0
  logicalRowCount: 10
  columns: [
    { name:"id",     ciphertextBytes: <9615 bytes> },
    { name:"salary", ciphertextBytes: <9615 bytes> },
    { name:"age",    ciphertextBytes: <9615 bytes> }
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
[type=ResultBatch][length=38313][serialized EncryptedResultBatch]
```

---

### Step 14 — Client decrypts

`ResultDecoder::decode()`:

1. Decrypt selection mask:
   ```
   mask = [0,0,0,0,1,1,1,1,1,1]   ← rows 5-10 match (age >= 5)
   ```

2. Decrypt id column:
   ```
   ids = [1,2,3,4,5,6,7,8,9,10]
   ```

3. Decrypt salary column:
   ```
   salaries = [5,6,7,8,9,10,11,12,13,14]
   ```

4. Decrypt age column:
   ```
   ages = [2,3,4,5,6,7,8,9,10,11]
   ```

---

### Step 15 — Client applies mask and reconstructs rows

For each index i where mask[i] == 1:
```
i=4: id=5,  salary=9,  age=6
i=5: id=6,  salary=10, age=7
i=6: id=7,  salary=11, age=8
i=7: id=8,  salary=12, age=9
i=8: id=9,  salary=13, age=10
i=9: id=10, salary=14, age=11
```

---

### Step 16 — Client applies ORDER BY and LIMIT

`ResultDecoder::applySortAndLimit()`:

1. Sort by salary DESC:
   ```
   id=10, salary=14, age=11
   id=9,  salary=13, age=10
   id=8,  salary=12, age=9
   id=7,  salary=11, age=8
   id=6,  salary=10, age=7
   id=5,  salary=9,  age=6
   ```

2. Apply LIMIT 3:
   ```
   id=10, salary=14, age=11
   id=9,  salary=13, age=10
   id=8,  salary=12, age=9
   ```

---

### Step 17 — Result printed

Display only originally requested columns (id, salary):
```
id | salary
---+-------
10 | 14
 9 | 13
 8 | 12
(3 rows)

[Verification] ✓ PASS — FHE result matches plaintext reference
```
