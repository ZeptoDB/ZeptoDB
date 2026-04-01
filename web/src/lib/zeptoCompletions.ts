import { CompletionContext, type Completion } from "@codemirror/autocomplete";

const ZEPTO_FUNCTIONS: Completion[] = [
  { label: "xbar", type: "function", detail: "time bucketing", info: "xbar(interval, column)" },
  { label: "vwap", type: "function", detail: "volume-weighted avg price", info: "vwap(price, size)" },
  { label: "ema", type: "function", detail: "exponential moving avg", info: "ema(alpha, column)" },
  { label: "wma", type: "function", detail: "weighted moving avg", info: "wma(window, column)" },
  { label: "mavg", type: "function", detail: "moving average", info: "mavg(window, column)" },
  { label: "msum", type: "function", detail: "moving sum", info: "msum(window, column)" },
  { label: "mmin", type: "function", detail: "moving min", info: "mmin(window, column)" },
  { label: "mmax", type: "function", detail: "moving max", info: "mmax(window, column)" },
  { label: "deltas", type: "function", detail: "row-to-row diff", info: "deltas(column)" },
  { label: "ratios", type: "function", detail: "row-to-row ratio", info: "ratios(column)" },
  { label: "fills", type: "function", detail: "forward fill nulls", info: "fills(column)" },
];

const SQL_SNIPPETS: Completion[] = [
  { label: "SELECT", type: "keyword", apply: "SELECT $$ FROM " },
  { label: "INSERT INTO", type: "keyword", apply: "INSERT INTO $$ VALUES " },
  { label: "CREATE TABLE", type: "keyword", apply: "CREATE TABLE $$ (\n  \n)" },
  { label: "ASOF JOIN", type: "keyword", apply: "ASOF JOIN $$ ON " },
  { label: "WINDOW", type: "keyword", apply: "WINDOW $$ AS (PARTITION BY  ORDER BY )" },
  { label: "GROUP BY", type: "keyword" },
  { label: "ORDER BY", type: "keyword" },
  { label: "LIMIT", type: "keyword" },
  { label: "WHERE", type: "keyword" },
  { label: "HAVING", type: "keyword" },
  { label: "DESCRIBE", type: "keyword" },
  { label: "SHOW TABLES", type: "keyword" },
  { label: "EXPLAIN", type: "keyword", apply: "EXPLAIN " },
  { label: "INTERVAL", type: "keyword", apply: "INTERVAL '5 minutes'" },
];

const ALL = [...ZEPTO_FUNCTIONS, ...SQL_SNIPPETS];

export function zeptoCompletions(ctx: CompletionContext) {
  const word = ctx.matchBefore(/\w*/);
  if (!word || (word.from === word.to && !ctx.explicit)) return null;
  return { from: word.from, options: ALL };
}
