// Recursive-descent SQL parser.
// Grammar (simplified):
//   select_stmt  := SELECT proj_list FROM ident [WHERE expr] [ORDER BY order_list] [LIMIT integer] [;]
//   proj_list    := * | ident (',' ident)*
//   order_list   := ident [ASC|DESC] (',' ident [ASC|DESC])*
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

std::string SQLParser::consumeQualifiedIdent() {
    auto name = consumeIdent();
    skipWS();
    if (!atEnd() && peek() == '.') {
        consume();
        name += "." + consumeIdent();
    }
    return name;
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

std::string SQLParser::consumeString() {
    skipWS();
    if (atEnd() || consume() != '\'')
        throw ParseError("Expected string literal at position " +
                         std::to_string(pos_));

    std::string value;
    while (!atEnd()) {
        char c = consume();
        if (c == '\'') {
            if (!atEnd() && peek() == '\'') {
                consume();
                value.push_back('\'');
                continue;
            }
            return value;
        }
        if (static_cast<unsigned char>(c) > 127)
            throw ParseError("Only ASCII string literals are supported");
        value.push_back(c);
    }
    throw ParseError("Unterminated string literal");
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
    auto from = parseTableRef();
    stmt.tableName = from.tableName;
    stmt.tableAlias = from.alias;
    while (true) {
        skipWS();
        if (tryKeyword("INNER")) {}
        if (!tryKeyword("JOIN")) break;
        JoinClause join;
        join.rightTable = parseTableRef();
        expectKeyword("ON");
        join.condition = parseExpr();
        stmt.joins.push_back(std::move(join));
    }
    skipWS();
    if (tryKeyword("WHERE"))
        stmt.whereClause = parseExpr();
    skipWS();
    if (tryKeyword("ORDER")) {
        expectKeyword("BY");
        stmt.orderByColumns = parseOrderByList();
    }

    skipWS();
    if (tryKeyword("LIMIT")) {
        stmt.limitCount = consumeInt();
        if (stmt.limitCount < 0)
            throw ParseError("LIMIT value must be non-negative");
    }
    return stmt;
}

TableRef SQLParser::parseTableRef() {
    TableRef ref;
    ref.tableName = consumeIdent();
    ref.alias = ref.tableName;
    skipWS();
    if (tryKeyword("AS")) {
        ref.alias = consumeIdent();
    } else {
        size_t saved = pos_;
        try {
            auto candidate = consumeIdent();
            if (candidate != "WHERE" && candidate != "ORDER" &&
                candidate != "LIMIT" && candidate != "JOIN" &&
                candidate != "INNER" && candidate != "ON")
                ref.alias = candidate;
            else
                pos_ = saved;
        } catch (const ParseError&) {
            pos_ = saved;
        }
    }
    return ref;
}

std::vector<std::string> SQLParser::parseProjectionList() {
    skipWS();
    if (!atEnd() && peek() == '*') {
        consume();
        return {}; // empty = SELECT *
    }
    std::vector<std::string> cols;
    cols.push_back(consumeQualifiedIdent());
    skipWS();
    while (!atEnd() && peek() == ',') {
        consume();
        cols.push_back(consumeQualifiedIdent());
        skipWS();
    }
    return cols;
}

std::vector<OrderByColumn> SQLParser::parseOrderByList() {
    std::vector<OrderByColumn> cols;
    OrderByColumn col;
    col.columnName = consumeQualifiedIdent();
    skipWS();
    // Check for ASC or DESC
    if (tryKeyword("DESC")) {
        col.direction = SortDirection::Descending;
    } else {
        tryKeyword("ASC"); // optional, default is ascending
        col.direction = SortDirection::Ascending;
    }
    cols.push_back(col);
    
    skipWS();
    while (!atEnd() && peek() == ',') {
        consume();
        OrderByColumn nextCol;
        nextCol.columnName = consumeQualifiedIdent();
        skipWS();
        if (tryKeyword("DESC")) {
            nextCol.direction = SortDirection::Descending;
        } else {
            tryKeyword("ASC");
            nextCol.direction = SortDirection::Ascending;
        }
        cols.push_back(nextCol);
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

    if (peek() == '\'')
        return ASTExpr::stringLiteral(consumeString());

    // Identifier (column name or keyword — keywords handled by tryKeyword above)
    auto id = consumeQualifiedIdent();
    return ASTExpr::columnRef(id);
}

} // namespace proto2
