#pragma once

#include <Parsers/IParserBase.h>
#include <Parsers/ExpressionElementParsers.h>
#include <Parsers/ExpressionListParsers.h>
#include <Parsers/ASTNameTypePair.h>
#include <Parsers/ASTColumnDeclaration.h>
#include <Parsers/ASTIdentifier.h>
#include <Parsers/CommonParsers.h>
#include <Common/typeid_cast.h>
#include <Poco/String.h>


namespace DB
{

/** A nested table. For example, Nested(UInt32 CounterID, FixedString(2) UserAgentMajor)
  */
class ParserNestedTable : public IParserBase
{
protected:
    const char * getName() const { return "nested table"; }
    bool parseImpl(Pos & pos, Pos end, ASTPtr & node, Pos & max_parsed_pos, Expected & expected);
};


/** Parametric type or Storage. For example:
 *         FixedString(10) or
 *         Partitioned(Log, ChunkID) or
 *         Nested(UInt32 CounterID, FixedString(2) UserAgentMajor)
 * Result of parsing - ASTFunction with or without parameters.
 */
class ParserIdentifierWithParameters : public IParserBase
{
protected:
    const char * getName() const { return "identifier with parameters"; }
    bool parseImpl(Pos & pos, Pos end, ASTPtr & node, Pos & max_parsed_pos, Expected & expected);
};


/** Data type or table engine, possibly with parameters. For example, UInt8 or see examples from ParserIdentifierWithParameters
  * Parse result is ASTFunction, with or without arguments.
  */
class ParserIdentifierWithOptionalParameters : public IParserBase
{
protected:
    const char * getName() const { return "identifier with optional parameters"; }
    bool parseImpl(Pos & pos, Pos end, ASTPtr & node, Pos & max_parsed_pos, Expected & expected);
};

class ParserTypeInCastExpression : public ParserIdentifierWithOptionalParameters
{
protected:
    const char * getName() const { return "type in cast expression"; }
    bool parseImpl(Pos & pos, Pos end, ASTPtr & node, Pos & max_parsed_pos, Expected & expected);
};


template <class NameParser>
class IParserNameTypePair : public IParserBase
{
protected:
    const char * getName() const { return "name and type pair"; }
    bool parseImpl(Pos & pos, Pos end, ASTPtr & node, Pos & max_parsed_pos, Expected & expected);
};

/** The name and type are separated by a space. For example, URL String. */
using ParserNameTypePair = IParserNameTypePair<ParserIdentifier>;
/** Name and type separated by a space. The name can contain a dot. For example, Hits.URL String. */
using ParserCompoundNameTypePair = IParserNameTypePair<ParserCompoundIdentifier>;

template <class NameParser>
bool IParserNameTypePair<NameParser>::parseImpl(Pos & pos, Pos end, ASTPtr & node, Pos & max_parsed_pos, Expected & expected)
{
    NameParser name_parser;
    ParserIdentifierWithOptionalParameters type_parser;
    ParserWhitespaceOrComments ws_parser;

    Pos begin = pos;

    ASTPtr name, type;
    if (name_parser.parse(pos, end, name, max_parsed_pos, expected)
        && ws_parser.ignore(pos, end, max_parsed_pos, expected)
        && type_parser.parse(pos, end, type, max_parsed_pos, expected))
    {
        auto name_type_pair = std::make_shared<ASTNameTypePair>(StringRange(begin, pos));
        name_type_pair->name = typeid_cast<const ASTIdentifier &>(*name).name;
        name_type_pair->type = type;
        name_type_pair->children.push_back(type);
        node = name_type_pair;
        return true;
    }

    return false;
}

/** List of columns. */
class ParserNameTypePairList : public IParserBase
{
protected:
    const char * getName() const { return "name and type pair list"; }
    bool parseImpl(Pos & pos, Pos end, ASTPtr & node, Pos & max_parsed_pos, Expected & expected);
};


template <class NameParser>
class IParserColumnDeclaration : public IParserBase
{
protected:
    const char * getName() const { return "column declaration"; }
    bool parseImpl(Pos & pos, Pos end, ASTPtr & node, Pos & max_parsed_pos, Expected & expected);
};

using ParserColumnDeclaration = IParserColumnDeclaration<ParserIdentifier>;
using ParserCompoundColumnDeclaration = IParserColumnDeclaration<ParserCompoundIdentifier>;

template <class NameParser>
bool IParserColumnDeclaration<NameParser>::parseImpl(Pos & pos, Pos end, ASTPtr & node, Pos & max_parsed_pos, Expected & expected)
{
    NameParser name_parser;
    ParserIdentifierWithOptionalParameters type_parser;
    ParserWhitespaceOrComments ws;
    ParserString s_default{"DEFAULT", true, true};
    ParserString s_materialized{"MATERIALIZED", true, true};
    ParserString s_alias{"ALIAS", true, true};
    ParserTernaryOperatorExpression expr_parser;

    const auto begin = pos;

    /// mandatory column name
    ASTPtr name;
    if (!name_parser.parse(pos, end, name, max_parsed_pos, expected))
        return false;

    ws.ignore(pos, end, max_parsed_pos, expected);

    /** column name should be followed by type name if it
      *    is not immediately followed by {DEFAULT, MATERIALIZED, ALIAS}
      */
    ASTPtr type;
    const auto fallback_pos = pos;
    if (!s_default.check(pos, end, expected, max_parsed_pos) &&
        !s_materialized.check(pos, end, expected, max_parsed_pos) &&
        !s_alias.check(pos, end, expected, max_parsed_pos))
    {
        if (type_parser.parse(pos, end, type, max_parsed_pos, expected))
            ws.ignore(pos, end, max_parsed_pos, expected);
    }
    else
        pos = fallback_pos;

    /// parse {DEFAULT, MATERIALIZED, ALIAS}
    String default_specifier;
    ASTPtr default_expression;
    const auto pos_before_specifier = pos;
    if (s_default.ignore(pos, end, max_parsed_pos, expected) ||
        s_materialized.ignore(pos, end, max_parsed_pos, expected) ||
        s_alias.ignore(pos, end, max_parsed_pos, expected))
    {
        default_specifier = Poco::toUpper(std::string{pos_before_specifier, pos});

        /// should be followed by an expression
        ws.ignore(pos, end, max_parsed_pos, expected);

        if (!expr_parser.parse(pos, end, default_expression, max_parsed_pos, expected))
            return false;
    }
    else if (!type)
        return false; /// reject sole column name without type

    const auto column_declaration = std::make_shared<ASTColumnDeclaration>(StringRange{begin, pos});
    node = column_declaration;
    column_declaration->name = typeid_cast<ASTIdentifier &>(*name).name;
    if (type)
    {
        column_declaration->type = type;
        column_declaration->children.push_back(std::move(type));
    }

    if (default_expression)
    {
        column_declaration->default_specifier = default_specifier;
        column_declaration->default_expression = default_expression;
        column_declaration->children.push_back(std::move(default_expression));
    }

    return true;
}

class ParserColumnDeclarationList : public IParserBase
{
protected:
    const char * getName() const { return "column declaration list"; }
    bool parseImpl(Pos & pos, Pos end, ASTPtr & node, Pos & max_parsed_pos, Expected & expected);
};


/** ENGINE = name. */
class ParserEngine : public IParserBase
{
protected:
    const char * getName() const { return "ENGINE"; }
    bool parseImpl(Pos & pos, Pos end, ASTPtr & node, Pos & max_parsed_pos, Expected & expected);
};


/** Query like this:
  * CREATE|ATTACH TABLE [IF NOT EXISTS] [db.]name
  * (
  *     name1 type1,
  *     name2 type2,
  *     ...
  * ) ENGINE = engine
  *
  * Or:
  * CREATE|ATTACH TABLE [IF NOT EXISTS] [db.]name AS [db2.]name2 [ENGINE = engine]
  *
  * Or:
  * CREATE|ATTACH TABLE [IF NOT EXISTS] [db.]name AS ENGINE = engine SELECT ...
  *
  * Or:
  * CREATE|ATTACH DATABASE db [ENGINE = engine]
  *
  * Or:
  * CREATE|ATTACH [MATERIALIZED] VIEW [IF NOT EXISTS] [db.]name [ENGINE = engine] [POPULATE] AS SELECT ...
  */
class ParserCreateQuery : public IParserBase
{
protected:
    const char * getName() const { return "CREATE TABLE or ATTACH TABLE query"; }
    bool parseImpl(Pos & pos, Pos end, ASTPtr & node, Pos & max_parsed_pos, Expected & expected);
};

}
