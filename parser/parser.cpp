// Recursive-descent SQL parser.
// Grammar (simplified):
//   select_stmt  := SELECT proj_list FROM ident [WHERE expr] [;]
//   proj_list    := * | ident (',' ident)*
//   expr         := or_expr
//   or_expr      := and_expr (OR and_expr)*
//   and_expr     := not_expr (AND not_expr)*
//   not_expr     := NOT not_expr | comparison
//   comparison   := primary (op primary)?
//   op           := = | != | > | < | >= | <=
//   primary      := ident | integer | '(' expr ')'

#include "proto2/parser.h"
#include <cctype>
#include <algorithm>

namespace proto2 {

static std::string toUpper(std::string s) {
    for (auto& c : s) c = (char)std::toupper((unsigned char)c);
    return s;
}

SelectStatement SQLParser::parse(const std::string& sql) {
    src_ = sql;
    pos_ = 0;
    auto stmt = parseSelect();
    skipWS();
    // Allow trailing semicolon
    if (!atEnd() && peek() == ';') consume();
    skipWS();
    if (!atEnd())
        throw ParseError("Unexpected token at position " + std::to_string(pos_) +
                         ": '" + std::string(1, peek()) + "'");
    return stmt;
}

void SQLParser::skipWS() {
    while (pos_ < src_.size() && std::isspace((unsigned char)src_[pos_]))
        ++pos_;
}

bool SQLParser::atEnd() const { return pos_ >= src_.size(); }

char SQLParser::peek() const {
    if (atEnd()) throw ParseError("Unexpected end of input");
    return src_[pos_];
}

char SQLParser::consume() {
    if (atEnd()) throw ParseError("Unexpected end of input");
    return src_[pos_++];
}

std::string SQLParser::consumeIdent() {
    skipWS();
    if (atEnd() || (!std::isalpha((unsigned char)peek()) && peek() != '_'))
        throw ParseError("Expected identifier at position " + std::to_string(pos_));
    std::string id;
    while (!atEnd() && (std::isalnum((unsigned char)src_[pos_]) || src_[pos_] == '_'))
        id += src_[pos_++];
    return id;
}

int64_t SQLParser::consumeInt() {
    skipWS();
    bool neg = false;
    if (!atEnd() && peek() == '-') { neg = true; consume(); }
    if (atEnd() || !std::isdigit((unsigned char)peek()))
        throw ParseError("Expected integer at position " + std::to_string(pos_));
    int64_t v = 0;
    while (!atEnd() && std::isdigit((unsigned char)src_[pos_]))
        v = v * 10 + (src_[pos_++] - '0');
    return neg ? -v : v;
}

bool SQLParser::tryKeyword(const std::string& kw) {
    skipWS();
    size_t saved = pos_;
    std::string id;
    while (pos_ < src_.size() && (std::isalnum((unsigned char)src_[pos_]) || src_[pos_] == '_'))
        id += src_[pos_++];
    if (toUpper(id) == kw) return true;
    pos_ = saved;
    return false;
}

void SQLParser::expectKeyword(const std::string& kw) {
    if (!tryKeyword(kw))
        throw ParseError("Expected keyword '" + kw + "' at position " + std::to_string(pos_));
}

void SQLParser::expectChar(char c) {
    skipWS();
    if (atEnd() || peek() != c)
        throw ParseError(std::string("Expected '") + c + "' at position " + std::to_string(pos_));
    consume();
}

SelectStatement SQLParser::parseSelect() {
    expectKeyword("SELECT");
    SelectStatement stmt;
    stmt.projections = parseProjectionList();
    expectKeyword("FROM");
    stmt.tableName = consumeIdent();
    skipWS();
    if (tryKeyword("WHERE"))
        stmt.whereClause = parseExpr();
    return stmt;
}

std::vector<std::string> SQLParser::parseProjectionList() {
    skipWS();
    if (!atEnd() && peek() == '*') {
        consume();
        return {}; // empty = SELECT *
    }
    std::vector<std::string> cols;
    cols.push_back(consumeIdent());
    skipWS();
    while (!atEnd() && peek() == ',') {
        consume();
        cols.push_back(consumeIdent());
        skipWS();
    }
    return cols;
}

std::shared_ptr<ASTExpr> SQLParser::parseExpr() { return parseOr(); }

std::shared_ptr<ASTExpr> SQLParser::parseOr() {
    auto left = parseAnd();
    while (tryKeyword("OR")) {
        auto right = parseAnd();
        left = ASTExpr::binary(ASTExprKind::Or, std::move(left), std::move(right));
    }
    return left;
}

std::shared_ptr<ASTExpr> SQLParser::parseAnd() {
    auto left = parseNot();
    while (tryKeyword("AND")) {
        auto right = parseNot();
        left = ASTExpr::binary(ASTExprKind::And, std::move(left), std::move(right));
    }
    return left;
}

std::shared_ptr<ASTExpr> SQLParser::parseNot() {
    if (tryKeyword("NOT"))
        return ASTExpr::notExpr(parseNot());
    return parseComparison();
}

std::shared_ptr<ASTExpr> SQLParser::parseComparison() {
    auto left = parsePrimary();
    skipWS();
    if (atEnd()) return left;

    ASTExprKind op;
    if (pos_ + 1 < src_.size() && src_[pos_] == '!' && src_[pos_+1] == '=') {
        pos_ += 2; op = ASTExprKind::NotEqual;
    } else if (pos_ + 1 < src_.size() && src_[pos_] == '>' && src_[pos_+1] == '=') {
        pos_ += 2; op = ASTExprKind::GreaterEqual;
    } else if (pos_ + 1 < src_.size() && src_[pos_] == '<' && src_[pos_+1] == '=') {
        pos_ += 2; op = ASTExprKind::LessEqual;
    } else if (src_[pos_] == '=') {
        ++pos_; op = ASTExprKind::Equal;
    } else if (src_[pos_] == '>') {
        ++pos_; op = ASTExprKind::GreaterThan;
    } else if (src_[pos_] == '<') {
        ++pos_; op = ASTExprKind::LessThan;
    } else {
        return left; // no operator — just a primary
    }

    auto right = parsePrimary();
    return ASTExpr::binary(op, std::move(left), std::move(right));
}

std::shared_ptr<ASTExpr> SQLParser::parsePrimary() {
    skipWS();
    if (atEnd()) throw ParseError("Expected expression");

    if (peek() == '(') {
        consume();
        auto e = parseExpr();
        expectChar(')');
        return e;
    }

    // Integer literal (possibly negative)
    if (std::isdigit((unsigned char)peek()) || peek() == '-') {
        return ASTExpr::intLiteral(consumeInt());
    }

    // Identifier (column name or keyword — keywords handled by tryKeyword above)
    auto id = consumeIdent();
    return ASTExpr::columnRef(id);
}

} // namespace proto2
