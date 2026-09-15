#pragma once
// Minimal recursive-descent SQL parser.
// Supports: SELECT col_list FROM table [WHERE expr]
// Expressions: =, !=, >, <, >=, <=, AND, OR, NOT, integer literals, column refs.

#include "proto2/ast.h"
#include <stdexcept>
#include <string>

namespace proto2 {

struct ParseError : std::runtime_error {
    explicit ParseError(const std::string& msg) : std::runtime_error(msg) {}
};

class SQLParser {
public:
    // Parse a single SELECT statement. Throws ParseError on failure.
    SelectStatement parse(const std::string& sql);

private:
    std::string src_;
    size_t      pos_{0};

    // Lexer helpers
    void        skipWS();
    bool        atEnd() const;
    char        peek() const;
    char        consume();
    std::string consumeIdent();
    int64_t     consumeInt();
    bool        tryKeyword(const std::string& kw);
    void        expectKeyword(const std::string& kw);
    void        expectChar(char c);

    // Parser
    SelectStatement parseSelect();
    std::vector<std::string> parseProjectionList();
    std::shared_ptr<ASTExpr> parseExpr();
    std::shared_ptr<ASTExpr> parseOr();
    std::shared_ptr<ASTExpr> parseAnd();
    std::shared_ptr<ASTExpr> parseNot();
    std::shared_ptr<ASTExpr> parseComparison();
    std::shared_ptr<ASTExpr> parsePrimary();
};

} // namespace proto2
