# Prototype 2 — Architecture

## Overview

Prototype 2 is an end-to-end FHE SQL query execution system. SQL queries are
parsed and planned on the client, sensitive literals are encrypted with OpenFHE
before leaving the client, the server executes predicates homomorphically over
encrypted database columns, and encrypted results are returned to the client for
decryption and row reconstruction.

---

## Pipeline Diagram

```
CLIENT
  │
  ├── SQL Terminal          (client/client_main.cpp)
  │       │
  ├── SQL Parser            (parser/parser.cpp)
  │       │  → SelectStatement AST (with ORDER BY, LIMIT)
  ├── Semantic Analyzer     (semantic/semantic.cpp)
  │       │  validates table/column/type correctness + ORDER BY columns
  ├── Logical Planner       (planner/planner.cpp)
  │       │  → LogicalNode tree (Project→Filter→Scan)
  │       │  includes ORDER BY columns in projection for client-side sorting
  ├── Physical Planner      (planner/planner.cpp)
  │       │  → PhysicalPlan (column lists, filter flag)
  ├── Query Encryptor       (query/query_encryptor.cpp)
  │       │  encrypts integer literals → EncryptedLiteralEntry
  │       │  builds PredNode tree (literal IDs, not values)
  │       │  handles =, !=, >, <, >=, <=, AND, OR, NOT
  ├── QueryPackage          (query/query_package.cpp)
  │       │  serialized via cereal binary archive
  └── TcpClientTransport    (transport/transport.cpp)
          │
          │  [TCP: length-prefixed binary frames]
          │
=============================================
SERVER
=============================================
          │
  ┌── TcpServerTransport    (transport/transport.cpp)
  ├── QueryValidator        (query/query_validator.cpp)
  │       validates protocol version, schema, FHE params,
  │       predicate tree structure, encrypted literal presence
  ├── QueryExecutor         (execution/query_executor.cpp)
  │       rebuilds proto1 physical plan from PredNode tree:
  │         ProjectExec → FilterExec → TableScanExec
  │       deserializes encrypted literals (server never sees plaintext)
  │       implements !=, >=, <= via FHE composition:
  │         != = NOT(EQ), >= = GT OR EQ, <= = LT OR EQ
  ├── proto1 Execution Engine
  │       TableScanExec  — reads encrypted columns from FilesystemStorage
  │       FilterExec     — evaluates predicate via ExpressionExecutor
  │       ProjectExec    — selects output columns
  ├── ExpressionExecutor    (proto1: expression_executor.cpp)
  │       EQ(a,b)  = 1 - (a-b)^(p-1)          [Fermat's little theorem]
  │       GT(a,b)  = Σ_{k=1}^{(p-1)/2} EQ(a-b, k)
  │       LT(a,b)  = GT(b,a)
  │       AND      = M1 * M2
  │       OR       = M1 + M2 - M1*M2
  │       NOT      = 1 - M
  ├── EncryptedResultBatch  (result/result.cpp)
  │       serialized projected columns + selection mask
  └── TcpServerTransport    streams batches back
          │
          │  [TCP: length-prefixed binary frames]
          │
=============================================
CLIENT
=============================================
          │
  ├── ResultDecoder         (result/result_decoder.cpp)
  │       decrypts selection mask
  │       decrypts each projected column
  │       reconstructs rows where mask[i] == 1
  ├── Sort & Limit          (result/result_decoder.cpp)
  │       applies ORDER BY (client-side, post-decryption)
  │       applies LIMIT (truncates result set)
  └── SQL Result Printer    (client/client_main.cpp)
          displays only originally requested columns
```

---

## Component Responsibilities

| Component | Location | Responsibility |
|---|---|---|
| SQLParser | parser/ | Recursive-descent SQL → AST |
| ASTExpr / SelectStatement | include/proto2/ast.h | Pure data AST nodes |
| SemanticAnalyzer | semantic/ | Validate table/column/type before encryption |
| Catalog | catalog/ | Schema registry (tableId → columns + types) |
| LogicalPlanner | planner/ | AST → LogicalNode tree |
| PhysicalPlanner | planner/ | LogicalNode → PhysicalPlan descriptor |
| ClientCryptoContext | crypto/ | Wraps ClientKeyContext; owns secret key |
| ServerCryptoContext | crypto/ | Wraps FHEKeyContext; no secret key |
| QueryEncryptor | query/ | Walks AST, encrypts literals, builds PredNode tree |
| QueryPackage | query/ | Serializable query unit (cereal binary) |
| QueryValidator | query/ | Server-side package validation |
| QueryExecutor | execution/ | Builds proto1 plan from PredNode tree, executes |
| EncryptedResultBatch | result/ | Serializable encrypted result unit |
| ResultDecoder | result/ | Client-side decrypt + mask apply + row reconstruct |
| TcpClientTransport | transport/ | Sends QueryPackage, receives result batches |
| TcpServerTransport | transport/ | Accepts connections, dispatches to QueryHandler |

---

## Key Design Decisions

### Reuse of Prototype 1
The entire FHE execution engine (TableScanExec, FilterExec, ProjectExec,
ExpressionExecutor, FilesystemStorage, BFVKeyContext) is reused unchanged from
Prototype 1. Prototype 2 adds the SQL front-end, query packaging, transport,
and result decoding layers on top.

### Trust Boundary
`ClientCryptoContext` and `ServerCryptoContext` are distinct types. The server
only ever receives a `ServerCryptoContext` which wraps `FHEKeyContext` (no
decrypt method). The `ClientKeyContext` (with decrypt) never crosses the
network boundary.

### Predicate Serialization
The predicate tree is serialized as a flat array of `PredNode` structs. Integer
literals are replaced by `LiteralRef` nodes that reference an index into the
`encryptedLiterals` array. The server reconstructs the proto1 `Expression` tree
by deserializing ciphertexts from the package — it never sees plaintext values.

Supported comparison operators (=, !=, >, <, >=, <=) are all implemented via
FHE operations. Compound operators are composed from primitives:
- `!=` (NotEqual) = `NOT(Equal(a,b))`
- `>=` (GreaterEqual) = `OR(GT(a,b), Equal(a,b))`
- `<=` (LessEqual) = `OR(LT(a,b), Equal(a,b))`

### Client-Side Post-Processing
ORDER BY and LIMIT are implemented client-side after decryption. This is
necessary because:
1. FHE-based sorting is extremely expensive (O(n²) comparisons, each requiring
   deep circuits)
2. The client must decrypt results anyway, so sorting on plaintext is efficient
3. LIMIT can be applied immediately after sorting with no additional cost

ORDER BY columns are automatically included in the projection (if not already
present) so they can be decrypted for sorting. The final output displays only
the originally requested columns.

### Transport
Simple TCP with 4-byte type + 4-byte length + payload framing. No gRPC or
protobuf dependency required. Binary serialization via cereal avoids base64
overhead on large ciphertexts.

### FHE Scheme
BFV with p=257, depth=12, N=128. Values must be in [0, 128] for correct GT/LT
semantics (positive half of Z_p). The paramSetId is validated on the server
before execution.
