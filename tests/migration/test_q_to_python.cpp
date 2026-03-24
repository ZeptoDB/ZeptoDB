// ============================================================================
// ZeptoDB: q to Python Transformation Tests
// ============================================================================
#include "zeptodb/migration/q_to_python.h"
#include <gtest/gtest.h>
#include <string>

using namespace zeptodb::migration;

std::string q_to_py(const std::string& q_query) {
    QLexer lexer(q_query);
    auto tokens = lexer.tokenize();
    QParser parser(tokens);
    auto ast = parser.parse();
    QToPythonTransformer transformer;
    return transformer.transform(ast);
}

// ============================================================================
// Basic SELECT
// ============================================================================

TEST(QToPythonTest, SelectAll) {
    auto py = q_to_py("select from trades");
    EXPECT_NE(py.find("DataFrame(db, \"trades\")"), std::string::npos);
}

TEST(QToPythonTest, SelectColumns) {
    auto py = q_to_py("select price, volume from trades");
    EXPECT_NE(py.find("DataFrame(db, \"trades\")"), std::string::npos);
    EXPECT_NE(py.find("select("), std::string::npos);
    EXPECT_NE(py.find("price"), std::string::npos);
    EXPECT_NE(py.find("volume"), std::string::npos);
}

// ============================================================================
// WHERE
// ============================================================================

TEST(QToPythonTest, SelectWithWhere) {
    auto py = q_to_py("select from trades where sym=`AAPL");
    EXPECT_NE(py.find("filter("), std::string::npos);
    EXPECT_NE(py.find("=="), std::string::npos);
    EXPECT_NE(py.find("AAPL"), std::string::npos);
}

// ============================================================================
// GROUP BY
// ============================================================================

TEST(QToPythonTest, GroupBy) {
    auto py = q_to_py("select sum size by sym from trades");
    EXPECT_NE(py.find("group_by("), std::string::npos);
    EXPECT_NE(py.find("\"sym\""), std::string::npos);
    EXPECT_NE(py.find("agg("), std::string::npos);
    EXPECT_NE(py.find("sum"), std::string::npos);
}

// ============================================================================
// Aggregation Functions
// ============================================================================

TEST(QToPythonTest, AvgFunction) {
    auto py = q_to_py("select avg price from trades");
    EXPECT_NE(py.find("mean"), std::string::npos);
    EXPECT_NE(py.find("price"), std::string::npos);
}

TEST(QToPythonTest, MinMaxFunction) {
    auto py = q_to_py("select min price, max price from trades");
    EXPECT_NE(py.find("min"), std::string::npos);
    EXPECT_NE(py.find("max"), std::string::npos);
}

TEST(QToPythonTest, CountFunction) {
    auto py = q_to_py("select count i from trades");
    EXPECT_NE(py.find("count"), std::string::npos);
}

// ============================================================================
// Financial Functions
// ============================================================================

TEST(QToPythonTest, WavgToVWAP) {
    auto py = q_to_py("select size wavg price from trades");
    EXPECT_NE(py.find("vwap"), std::string::npos);
    EXPECT_NE(py.find("price"), std::string::npos);
    EXPECT_NE(py.find("size"), std::string::npos);
}

TEST(QToPythonTest, XbarFunction) {
    auto py = q_to_py("select from trades where xbar[300;timestamp]");
    EXPECT_NE(py.find("xbar"), std::string::npos);
    EXPECT_NE(py.find("300"), std::string::npos);
}

TEST(QToPythonTest, EMAFunction) {
    auto py = q_to_py("select ema[20;price] from trades");
    EXPECT_NE(py.find("ema"), std::string::npos);
    EXPECT_NE(py.find("20"), std::string::npos);
    EXPECT_NE(py.find("price"), std::string::npos);
}

// ============================================================================
// ASOF JOIN
// ============================================================================

TEST(QToPythonTest, AsofJoin) {
    auto py = q_to_py("aj[`time`sym;trades;quotes]");
    EXPECT_NE(py.find("asof_join"), std::string::npos);
    EXPECT_NE(py.find("trades"), std::string::npos);
    EXPECT_NE(py.find("quotes"), std::string::npos);
    EXPECT_NE(py.find("\"time\""), std::string::npos);
    EXPECT_NE(py.find("\"sym\""), std::string::npos);
}

// ============================================================================
// Multi-line Script
// ============================================================================

TEST(QToPythonTest, ScriptTransform) {
    std::string q_script =
        "/ daily VWAP\n"
        "t: select from trades where sym=`AAPL\n"
        "v: select size wavg price by sym from t\n";

    QToPythonTransformer transformer;
    auto py = transformer.transform_script(q_script);

    EXPECT_NE(py.find("import zepto_py"), std::string::npos);
    EXPECT_NE(py.find("t ="), std::string::npos);
    EXPECT_NE(py.find("v ="), std::string::npos);
    EXPECT_NE(py.find("filter("), std::string::npos);
    EXPECT_NE(py.find("vwap"), std::string::npos);
    EXPECT_NE(py.find("# daily VWAP"), std::string::npos);
}

TEST(QToPythonTest, ScriptComments) {
    std::string q_script = "/ this is a comment\n";
    QToPythonTransformer transformer;
    auto py = transformer.transform_script(q_script);
    EXPECT_NE(py.find("# this is a comment"), std::string::npos);
}

TEST(QToPythonTest, ScriptUnsupportedFallback) {
    std::string q_script = "\\l /path/to/script.q\n";
    QToPythonTransformer transformer;
    auto py = transformer.transform_script(q_script);
    EXPECT_NE(py.find("# TODO:"), std::string::npos);
}

// ============================================================================
// Complex Patterns
// ============================================================================

TEST(QToPythonTest, VWAPBySymbolTime) {
    auto py = q_to_py("select size wavg price by sym from trades");
    EXPECT_NE(py.find("group_by("), std::string::npos);
    EXPECT_NE(py.find("\"sym\""), std::string::npos);
    EXPECT_NE(py.find("vwap"), std::string::npos);
}

TEST(QToPythonTest, MultiColumnGroupBy) {
    auto py = q_to_py("select sum size by sym, time from trades");
    EXPECT_NE(py.find("group_by("), std::string::npos);
    EXPECT_NE(py.find("\"sym\""), std::string::npos);
    EXPECT_NE(py.find("\"time\""), std::string::npos);
}

// ============================================================================
// Arithmetic
// ============================================================================

TEST(QToPythonTest, Arithmetic) {
    auto py = q_to_py("select price * volume from trades");
    EXPECT_NE(py.find("*"), std::string::npos);
    EXPECT_NE(py.find("price"), std::string::npos);
    EXPECT_NE(py.find("volume"), std::string::npos);
}

TEST(QToPythonTest, ArithmeticDivide) {
    // q uses % for division
    auto py = q_to_py("select price % volume from trades");
    EXPECT_NE(py.find("/"), std::string::npos);
}

// ============================================================================
// IN / WITHIN / LIKE
// ============================================================================

TEST(QToPythonTest, InExpression) {
    auto py = q_to_py("select from trades where sym in `AAPL`MSFT`GOOG");
    EXPECT_NE(py.find("is_in"), std::string::npos);
    EXPECT_NE(py.find("AAPL"), std::string::npos);
    EXPECT_NE(py.find("MSFT"), std::string::npos);
}

TEST(QToPythonTest, WithinExpression) {
    auto py = q_to_py("select from trades where price within (100;200)");
    EXPECT_NE(py.find("is_between"), std::string::npos);
    EXPECT_NE(py.find("100"), std::string::npos);
    EXPECT_NE(py.find("200"), std::string::npos);
}

TEST(QToPythonTest, LikeExpression) {
    auto py = q_to_py("select from trades where sym like \"AA*\"");
    EXPECT_NE(py.find("contains"), std::string::npos);
    EXPECT_NE(py.find("AA"), std::string::npos);
}

// ============================================================================
// Multiple WHERE conditions
// ============================================================================

TEST(QToPythonTest, MultipleWhere) {
    auto py = q_to_py("select from trades where sym=`AAPL, price>100");
    EXPECT_NE(py.find("filter("), std::string::npos);
    EXPECT_NE(py.find("AAPL"), std::string::npos);
    EXPECT_NE(py.find("100"), std::string::npos);
    EXPECT_NE(py.find("&"), std::string::npos);
}

// ============================================================================
// UPDATE / DELETE / EXEC
// ============================================================================

TEST(QToPythonTest, Update) {
    auto py = q_to_py("update vwap: price from trades where sym=`AAPL");
    EXPECT_NE(py.find("with_columns"), std::string::npos);
    EXPECT_NE(py.find("vwap"), std::string::npos);
    EXPECT_NE(py.find("filter"), std::string::npos);
}

TEST(QToPythonTest, Delete) {
    auto py = q_to_py("delete from trades where price>1000");
    EXPECT_NE(py.find("filter"), std::string::npos);
    EXPECT_NE(py.find("1000"), std::string::npos);
}

TEST(QToPythonTest, Exec) {
    auto py = q_to_py("exec price from trades where sym=`AAPL");
    EXPECT_NE(py.find("to_series"), std::string::npos);
    EXPECT_NE(py.find("price"), std::string::npos);
}

// ============================================================================
// Conditional: $[cond;true;false]
// ============================================================================

TEST(QToPythonTest, Conditional) {
    auto py = q_to_py("select $[price>100;1;0] from trades");
    EXPECT_NE(py.find("if"), std::string::npos);
    EXPECT_NE(py.find("else"), std::string::npos);
}

// ============================================================================
// Function definition
// ============================================================================

TEST(QToPythonTest, FunctionDef) {
    QToPythonTransformer transformer;
    std::string q_script = "f: {[x;y] x+y}\n";
    auto py = transformer.transform_script(q_script);
    EXPECT_NE(py.find("f ="), std::string::npos);
    EXPECT_NE(py.find("lambda"), std::string::npos);
    EXPECT_NE(py.find("x"), std::string::npos);
    EXPECT_NE(py.find("y"), std::string::npos);
}

// ============================================================================
// prev / next
// ============================================================================

TEST(QToPythonTest, PrevFunction) {
    auto py = q_to_py("select prev price from trades");
    EXPECT_NE(py.find("shift"), std::string::npos);
    EXPECT_NE(py.find("price"), std::string::npos);
}

// ============================================================================
// Negative numbers
// ============================================================================

TEST(QToPythonTest, NegativeNumber) {
    auto py = q_to_py("select from trades where price>-100");
    EXPECT_NE(py.find("-100"), std::string::npos);
}

// ============================================================================
// not
// ============================================================================

TEST(QToPythonTest, NotExpression) {
    auto py = q_to_py("select from trades where not sym=`AAPL");
    EXPECT_NE(py.find("~"), std::string::npos);
}
