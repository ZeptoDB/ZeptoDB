// ============================================================================
// ZeptoDB: q to SQL Transformation Tests
// ============================================================================
#include "zeptodb/migration/q_parser.h"
#include <gtest/gtest.h>
#include <string>

using namespace zeptodb::migration;

// ============================================================================
// Helper Function
// ============================================================================

std::string convert_q_to_sql(const std::string& q_query) {
    QLexer lexer(q_query);
    auto tokens = lexer.tokenize();

    QParser parser(tokens);
    auto ast = parser.parse();

    QToSQLTransformer transformer;
    return transformer.transform(ast);
}

// ============================================================================
// Basic SELECT Tests
// ============================================================================

TEST(QToSQLTest, SimpleSelectAll) {
    std::string q = "select from trades";
    std::string sql = convert_q_to_sql(q);

    EXPECT_NE(sql.find("SELECT *"), std::string::npos);
    EXPECT_NE(sql.find("FROM trades"), std::string::npos);
}

TEST(QToSQLTest, SelectSpecificColumns) {
    std::string q = "select sym, price, size from trades";
    std::string sql = convert_q_to_sql(q);

    EXPECT_NE(sql.find("SELECT"), std::string::npos);
    EXPECT_NE(sql.find("sym"), std::string::npos);
    EXPECT_NE(sql.find("price"), std::string::npos);
    EXPECT_NE(sql.find("size"), std::string::npos);
    EXPECT_NE(sql.find("FROM trades"), std::string::npos);
}

TEST(QToSQLTest, SelectWithWhere) {
    std::string q = "select from trades where sym=`AAPL";
    std::string sql = convert_q_to_sql(q);

    EXPECT_NE(sql.find("WHERE"), std::string::npos);
    EXPECT_NE(sql.find("sym"), std::string::npos);
    EXPECT_NE(sql.find("AAPL"), std::string::npos);
}

TEST(QToSQLTest, SelectWithGroupBy) {
    std::string q = "select sum size by sym from trades";
    std::string sql = convert_q_to_sql(q);

    EXPECT_NE(sql.find("GROUP BY"), std::string::npos);
    EXPECT_NE(sql.find("sym"), std::string::npos);
}

// ============================================================================
// Aggregation Function Tests
// ============================================================================

TEST(QToSQLTest, SumFunction) {
    std::string q = "select sum size from trades";
    std::string sql = convert_q_to_sql(q);

    EXPECT_NE(sql.find("SUM"), std::string::npos);
    EXPECT_NE(sql.find("size"), std::string::npos);
}

TEST(QToSQLTest, AvgFunction) {
    std::string q = "select avg price from trades";
    std::string sql = convert_q_to_sql(q);

    EXPECT_NE(sql.find("AVG"), std::string::npos);
    EXPECT_NE(sql.find("price"), std::string::npos);
}

TEST(QToSQLTest, MinMaxFunction) {
    std::string q = "select min price, max price from trades";
    std::string sql = convert_q_to_sql(q);

    EXPECT_NE(sql.find("MIN"), std::string::npos);
    EXPECT_NE(sql.find("MAX"), std::string::npos);
}

TEST(QToSQLTest, CountFunction) {
    std::string q = "select count i from trades";
    std::string sql = convert_q_to_sql(q);

    EXPECT_NE(sql.find("COUNT"), std::string::npos);
}

// ============================================================================
// Financial Function Tests
// ============================================================================

TEST(QToSQLTest, WavgToVWAP) {
    std::string q = "select size wavg price from trades";
    std::string sql = convert_q_to_sql(q);

    // wavg should convert to weighted average calculation
    EXPECT_NE(sql.find("SUM"), std::string::npos);
    EXPECT_NE(sql.find("size"), std::string::npos);
    EXPECT_NE(sql.find("price"), std::string::npos);
}

TEST(QToSQLTest, XbarFunction) {
    std::string q = "select xbar[300;time] from trades";
    std::string sql = convert_q_to_sql(q);

    EXPECT_NE(sql.find("xbar"), std::string::npos);
    EXPECT_NE(sql.find("time"), std::string::npos);
    EXPECT_NE(sql.find("300"), std::string::npos);
}

TEST(QToSQLTest, EMAFunction) {
    std::string q = "select ema[0.1;price] from trades";
    std::string sql = convert_q_to_sql(q);

    EXPECT_NE(sql.find("ema"), std::string::npos);
    EXPECT_NE(sql.find("price"), std::string::npos);
}

// ============================================================================
// Complex Query Tests
// ============================================================================

TEST(QToSQLTest, ComplexAggregationWithGroupBy) {
    std::string q = "select sum size, avg price, max price by sym from trades";
    std::string sql = convert_q_to_sql(q);

    EXPECT_NE(sql.find("SUM"), std::string::npos);
    EXPECT_NE(sql.find("AVG"), std::string::npos);
    EXPECT_NE(sql.find("MAX"), std::string::npos);
    EXPECT_NE(sql.find("GROUP BY"), std::string::npos);
    EXPECT_NE(sql.find("sym"), std::string::npos);
}

TEST(QToSQLTest, MultipleWhereConditions) {
    std::string q = "select from trades where sym=`AAPL";
    std::string sql = convert_q_to_sql(q);

    EXPECT_NE(sql.find("WHERE"), std::string::npos);
}

TEST(QToSQLTest, GroupByMultipleColumns) {
    std::string q = "select sum size by sym, time from trades";
    std::string sql = convert_q_to_sql(q);

    EXPECT_NE(sql.find("GROUP BY"), std::string::npos);
    EXPECT_NE(sql.find("sym"), std::string::npos);
    EXPECT_NE(sql.find("time"), std::string::npos);
}

// ============================================================================
// ASOF JOIN Tests
// ============================================================================

TEST(QToSQLTest, AsofJoin) {
    std::string q = "aj[`time`sym;trades;quotes]";
    std::string sql = convert_q_to_sql(q);

    EXPECT_NE(sql.find("ASOF JOIN"), std::string::npos);
    EXPECT_NE(sql.find("trades"), std::string::npos);
    EXPECT_NE(sql.find("quotes"), std::string::npos);
}

// ============================================================================
// Real-World Query Tests
// ============================================================================

TEST(QToSQLTest, VWAP_BySymbolTime) {
    // Classic VWAP calculation grouped by symbol and time bucket
    std::string q = "select size wavg price by sym, xbar[300;time] from trades";
    std::string sql = convert_q_to_sql(q);

    EXPECT_NE(sql.find("SUM"), std::string::npos);
    EXPECT_NE(sql.find("GROUP BY"), std::string::npos);
    EXPECT_NE(sql.find("sym"), std::string::npos);
}

TEST(QToSQLTest, FilteredAggregation) {
    // Filter trades and aggregate
    std::string q = "select sum size, avg price from trades where sym=`AAPL";
    std::string sql = convert_q_to_sql(q);

    EXPECT_NE(sql.find("WHERE"), std::string::npos);
    EXPECT_NE(sql.find("SUM"), std::string::npos);
    EXPECT_NE(sql.find("AVG"), std::string::npos);
}

TEST(QToSQLTest, MinuteBarOHLC) {
    // OHLC bars: Open, High, Low, Close
    std::string q = "select first price, max price, min price, last price by sym, xbar[60;time] from trades";
    std::string sql = convert_q_to_sql(q);

    EXPECT_NE(sql.find("FIRST"), std::string::npos);
    EXPECT_NE(sql.find("MAX"), std::string::npos);
    EXPECT_NE(sql.find("MIN"), std::string::npos);
    EXPECT_NE(sql.find("LAST"), std::string::npos);
    EXPECT_NE(sql.find("GROUP BY"), std::string::npos);
}

// ============================================================================
// Error Handling Tests
// ============================================================================

TEST(QToSQLTest, EmptyQuery) {
    EXPECT_THROW({
        convert_q_to_sql("");
    }, std::exception);
}

TEST(QToSQLTest, InvalidSyntax) {
    EXPECT_THROW({
        convert_q_to_sql("select from");
    }, std::exception);
}

// ============================================================================
// Performance Tests
// ============================================================================

TEST(QToSQLTest, LargeQueryPerformance) {
    // Generate a complex query
    std::string q = "select sum size, avg price, min price, max price, "
                   "first price, last price by sym, xbar[60;time] from trades "
                   "where sym=`AAPL";

    auto start = std::chrono::high_resolution_clock::now();

    std::string sql = convert_q_to_sql(q);

    auto end = std::chrono::high_resolution_clock::now();
    auto duration = std::chrono::duration_cast<std::chrono::microseconds>(end - start);

    // Should complete in < 10ms for typical queries
    EXPECT_LT(duration.count(), 10000) << "Conversion took " << duration.count() << "µs";

    std::cout << "Query conversion time: " << duration.count() << "µs" << std::endl;
}

// ============================================================================
// Extended Tests
// ============================================================================

TEST(QToSQLTest, InExpression) {
    std::string q = "select from trades where sym in `AAPL`MSFT";
    std::string sql = convert_q_to_sql(q);
    EXPECT_NE(sql.find("IN"), std::string::npos);
    EXPECT_NE(sql.find("AAPL"), std::string::npos);
    EXPECT_NE(sql.find("MSFT"), std::string::npos);
}

TEST(QToSQLTest, WithinExpression) {
    std::string q = "select from trades where price within (100;200)";
    std::string sql = convert_q_to_sql(q);
    EXPECT_NE(sql.find("BETWEEN"), std::string::npos);
    EXPECT_NE(sql.find("100"), std::string::npos);
    EXPECT_NE(sql.find("200"), std::string::npos);
}

TEST(QToSQLTest, LikeExpression) {
    std::string q = "select from trades where sym like \"AA%\"";
    std::string sql = convert_q_to_sql(q);
    EXPECT_NE(sql.find("LIKE"), std::string::npos);
}

TEST(QToSQLTest, MultipleWhereComma) {
    std::string q = "select from trades where sym=`AAPL, price>100";
    std::string sql = convert_q_to_sql(q);
    EXPECT_NE(sql.find("AND"), std::string::npos);
    EXPECT_NE(sql.find("AAPL"), std::string::npos);
    EXPECT_NE(sql.find("100"), std::string::npos);
}

TEST(QToSQLTest, Arithmetic) {
    std::string q = "select price * volume from trades";
    std::string sql = convert_q_to_sql(q);
    EXPECT_NE(sql.find("*"), std::string::npos);
    EXPECT_NE(sql.find("price"), std::string::npos);
}

TEST(QToSQLTest, UpdateStatement) {
    std::string q = "update vwap: price from trades where sym=`AAPL";
    QLexer lexer(q);
    auto tokens = lexer.tokenize();
    QParser parser(tokens);
    auto ast = parser.parse();
    QToSQLTransformer transformer;
    std::string sql = transformer.transform(ast);
    EXPECT_NE(sql.find("UPDATE"), std::string::npos);
    EXPECT_NE(sql.find("SET"), std::string::npos);
    EXPECT_NE(sql.find("vwap"), std::string::npos);
}

TEST(QToSQLTest, DeleteStatement) {
    std::string q = "delete from trades where price>1000";
    QLexer lexer(q);
    auto tokens = lexer.tokenize();
    QParser parser(tokens);
    auto ast = parser.parse();
    QToSQLTransformer transformer;
    std::string sql = transformer.transform(ast);
    EXPECT_NE(sql.find("DELETE"), std::string::npos);
    EXPECT_NE(sql.find("FROM trades"), std::string::npos);
    EXPECT_NE(sql.find("1000"), std::string::npos);
}

TEST(QToSQLTest, ConditionalExpression) {
    std::string q = "select $[price>100;1;0] from trades";
    std::string sql = convert_q_to_sql(q);
    EXPECT_NE(sql.find("CASE WHEN"), std::string::npos);
    EXPECT_NE(sql.find("THEN"), std::string::npos);
    EXPECT_NE(sql.find("ELSE"), std::string::npos);
}

TEST(QToSQLTest, PrevFunction) {
    std::string q = "select prev price from trades";
    std::string sql = convert_q_to_sql(q);
    EXPECT_NE(sql.find("LAG"), std::string::npos);
    EXPECT_NE(sql.find("OVER"), std::string::npos);
}

// ============================================================================
// Main
// ============================================================================

int main(int argc, char** argv) {
    testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
