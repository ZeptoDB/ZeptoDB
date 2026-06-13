#include "zeptodb/server/msgpack_ingest.h"

#include "zeptodb/ingestion/tick_plant.h"

#include <cmath>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <limits>
#include <string_view>
#include <utility>
#include <vector>

namespace zeptodb::server {
namespace {

enum class ValueKind {
    Nil,
    Bool,
    Int,
    UInt,
    Double,
    String,
    Array,
    Map,
};

struct Value {
    ValueKind kind = ValueKind::Nil;
    int64_t i = 0;
    uint64_t u = 0;
    double d = 0.0;
    bool b = false;
    std::string s;
    std::vector<Value> array;
    std::vector<std::string> map_keys;
    std::vector<Value> map_values;
};

class Decoder {
public:
    explicit Decoder(std::string_view body) : body_(body) {}

    bool parse(Value* out, std::string* error) {
        if (!out) return false;
        if (!parse_value(0, out, error)) return false;
        if (pos_ != body_.size()) {
            set_error(error, "Trailing bytes after MessagePack document");
            return false;
        }
        return true;
    }

private:
    static constexpr size_t kMaxDepth = 16;

    bool read_u8(uint8_t* out) {
        if (pos_ >= body_.size()) return false;
        *out = static_cast<uint8_t>(body_[pos_++]);
        return true;
    }

    bool read_bytes(size_t n, std::string_view* out) {
        if (n > body_.size() - pos_) return false;
        *out = std::string_view(body_.data() + pos_, n);
        pos_ += n;
        return true;
    }

    bool read_u16(uint16_t* out) {
        std::string_view bytes;
        if (!read_bytes(2, &bytes)) return false;
        *out = static_cast<uint16_t>(
            (static_cast<uint16_t>(static_cast<uint8_t>(bytes[0])) << 8) |
             static_cast<uint16_t>(static_cast<uint8_t>(bytes[1])));
        return true;
    }

    bool read_u32(uint32_t* out) {
        std::string_view bytes;
        if (!read_bytes(4, &bytes)) return false;
        *out = (static_cast<uint32_t>(static_cast<uint8_t>(bytes[0])) << 24) |
               (static_cast<uint32_t>(static_cast<uint8_t>(bytes[1])) << 16) |
               (static_cast<uint32_t>(static_cast<uint8_t>(bytes[2])) << 8) |
                static_cast<uint32_t>(static_cast<uint8_t>(bytes[3]));
        return true;
    }

    bool read_u64(uint64_t* out) {
        std::string_view bytes;
        if (!read_bytes(8, &bytes)) return false;
        uint64_t v = 0;
        for (char c : bytes) {
            v = (v << 8) | static_cast<uint64_t>(static_cast<uint8_t>(c));
        }
        *out = v;
        return true;
    }

    bool parse_array(size_t depth, uint32_t len, Value* out, std::string* error) {
        if (len > body_.size() - pos_) {
            set_error(error, "MessagePack array length exceeds remaining body");
            return false;
        }
        out->kind = ValueKind::Array;
        out->array.clear();
        out->array.reserve(len);
        for (uint32_t i = 0; i < len; ++i) {
            Value child;
            if (!parse_value(depth + 1, &child, error)) return false;
            out->array.push_back(std::move(child));
        }
        return true;
    }

    bool parse_map(size_t depth, uint32_t len, Value* out, std::string* error) {
        if (len > (body_.size() - pos_) / 2) {
            set_error(error, "MessagePack map length exceeds remaining body");
            return false;
        }
        out->kind = ValueKind::Map;
        out->map_keys.clear();
        out->map_values.clear();
        out->map_keys.reserve(len);
        out->map_values.reserve(len);
        for (uint32_t i = 0; i < len; ++i) {
            Value key;
            if (!parse_value(depth + 1, &key, error)) return false;
            if (key.kind != ValueKind::String) {
                set_error(error, "MessagePack map keys must be strings");
                return false;
            }
            Value val;
            if (!parse_value(depth + 1, &val, error)) return false;
            out->map_keys.push_back(std::move(key.s));
            out->map_values.push_back(std::move(val));
        }
        return true;
    }

    bool parse_str(uint32_t len, Value* out, std::string* error) {
        std::string_view bytes;
        if (!read_bytes(len, &bytes)) {
            set_error(error, "Truncated MessagePack string");
            return false;
        }
        out->kind = ValueKind::String;
        out->s.assign(bytes.data(), bytes.size());
        return true;
    }

    bool parse_value(size_t depth, Value* out, std::string* error) {
        if (depth > kMaxDepth) {
            set_error(error, "MessagePack nesting is too deep");
            return false;
        }

        uint8_t tag = 0;
        if (!read_u8(&tag)) {
            set_error(error, "Empty or truncated MessagePack body");
            return false;
        }

        if (tag <= 0x7f) {
            out->kind = ValueKind::UInt;
            out->u = tag;
            return true;
        }
        if ((tag & 0xf0) == 0x80) return parse_map(depth, tag & 0x0f, out, error);
        if ((tag & 0xf0) == 0x90) return parse_array(depth, tag & 0x0f, out, error);
        if ((tag & 0xe0) == 0xa0) return parse_str(tag & 0x1f, out, error);
        if (tag >= 0xe0) {
            out->kind = ValueKind::Int;
            out->i = static_cast<int8_t>(tag);
            return true;
        }

        switch (tag) {
            case 0xc0:
                out->kind = ValueKind::Nil;
                return true;
            case 0xc2:
            case 0xc3:
                out->kind = ValueKind::Bool;
                out->b = tag == 0xc3;
                return true;
            case 0xca: {
                uint32_t bits = 0;
                if (!read_u32(&bits)) {
                    set_error(error, "Truncated MessagePack float32");
                    return false;
                }
                float f = 0.0f;
                static_assert(sizeof(f) == sizeof(bits));
                std::memcpy(&f, &bits, sizeof(f));
                out->kind = ValueKind::Double;
                out->d = static_cast<double>(f);
                return true;
            }
            case 0xcb: {
                uint64_t bits = 0;
                if (!read_u64(&bits)) {
                    set_error(error, "Truncated MessagePack float64");
                    return false;
                }
                double d = 0.0;
                static_assert(sizeof(d) == sizeof(bits));
                std::memcpy(&d, &bits, sizeof(d));
                out->kind = ValueKind::Double;
                out->d = d;
                return true;
            }
            case 0xcc: {
                uint8_t v = 0;
                if (!read_u8(&v)) return truncated_int(error);
                out->kind = ValueKind::UInt;
                out->u = v;
                return true;
            }
            case 0xcd: {
                uint16_t v = 0;
                if (!read_u16(&v)) return truncated_int(error);
                out->kind = ValueKind::UInt;
                out->u = v;
                return true;
            }
            case 0xce: {
                uint32_t v = 0;
                if (!read_u32(&v)) return truncated_int(error);
                out->kind = ValueKind::UInt;
                out->u = v;
                return true;
            }
            case 0xcf: {
                uint64_t v = 0;
                if (!read_u64(&v)) return truncated_int(error);
                out->kind = ValueKind::UInt;
                out->u = v;
                return true;
            }
            case 0xd0: {
                uint8_t v = 0;
                if (!read_u8(&v)) return truncated_int(error);
                out->kind = ValueKind::Int;
                out->i = static_cast<int8_t>(v);
                return true;
            }
            case 0xd1: {
                uint16_t v = 0;
                if (!read_u16(&v)) return truncated_int(error);
                out->kind = ValueKind::Int;
                out->i = static_cast<int16_t>(v);
                return true;
            }
            case 0xd2: {
                uint32_t v = 0;
                if (!read_u32(&v)) return truncated_int(error);
                out->kind = ValueKind::Int;
                out->i = static_cast<int32_t>(v);
                return true;
            }
            case 0xd3: {
                uint64_t v = 0;
                if (!read_u64(&v)) return truncated_int(error);
                out->kind = ValueKind::Int;
                out->i = static_cast<int64_t>(v);
                return true;
            }
            case 0xd9: {
                uint8_t len = 0;
                if (!read_u8(&len)) {
                    set_error(error, "Truncated MessagePack str8 length");
                    return false;
                }
                return parse_str(len, out, error);
            }
            case 0xda: {
                uint16_t len = 0;
                if (!read_u16(&len)) {
                    set_error(error, "Truncated MessagePack str16 length");
                    return false;
                }
                return parse_str(len, out, error);
            }
            case 0xdb: {
                uint32_t len = 0;
                if (!read_u32(&len)) {
                    set_error(error, "Truncated MessagePack str32 length");
                    return false;
                }
                return parse_str(len, out, error);
            }
            case 0xdc: {
                uint16_t len = 0;
                if (!read_u16(&len)) {
                    set_error(error, "Truncated MessagePack array16 length");
                    return false;
                }
                return parse_array(depth, len, out, error);
            }
            case 0xdd: {
                uint32_t len = 0;
                if (!read_u32(&len)) {
                    set_error(error, "Truncated MessagePack array32 length");
                    return false;
                }
                return parse_array(depth, len, out, error);
            }
            case 0xde: {
                uint16_t len = 0;
                if (!read_u16(&len)) {
                    set_error(error, "Truncated MessagePack map16 length");
                    return false;
                }
                return parse_map(depth, len, out, error);
            }
            case 0xdf: {
                uint32_t len = 0;
                if (!read_u32(&len)) {
                    set_error(error, "Truncated MessagePack map32 length");
                    return false;
                }
                return parse_map(depth, len, out, error);
            }
            default:
                set_error(error, "Unsupported MessagePack type tag");
                return false;
        }
    }

    bool truncated_int(std::string* error) const {
        set_error(error, "Truncated MessagePack integer");
        return false;
    }

    static void set_error(std::string* error, std::string msg) {
        if (error) *error = std::move(msg);
    }

    std::string_view body_;
    size_t pos_ = 0;
};

int64_t msgpack_ingest_now_ns() {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
}

bool scaled_to_i64(double value,
                   double scale,
                   const std::string& column,
                   int64_t* out,
                   std::string* error)
{
    const double scaled = value * scale;
    if (!std::isfinite(scaled) ||
        scaled < static_cast<double>(std::numeric_limits<int64_t>::min()) ||
        scaled > static_cast<double>(std::numeric_limits<int64_t>::max())) {
        if (error) *error = "Column '" + column + "' value is out of int64 range";
        return false;
    }
    *out = static_cast<int64_t>(scaled);
    return true;
}

bool value_to_i64(const Value& value,
                  double scale,
                  const std::string& column,
                  int64_t* out,
                  std::string* error)
{
    switch (value.kind) {
        case ValueKind::Int:
            if (scale == 1.0) {
                *out = value.i;
                return true;
            }
            return scaled_to_i64(static_cast<double>(value.i), scale, column, out, error);
        case ValueKind::UInt:
            if (value.u > static_cast<uint64_t>(std::numeric_limits<int64_t>::max())) {
                if (error) *error = "Column '" + column + "' value is out of int64 range";
                return false;
            }
            if (scale == 1.0) {
                *out = static_cast<int64_t>(value.u);
                return true;
            }
            return scaled_to_i64(static_cast<double>(value.u), scale, column, out, error);
        case ValueKind::Double:
            return scaled_to_i64(value.d, scale, column, out, error);
        default:
            if (error) *error = "Column '" + column + "' must contain numeric values";
            return false;
    }
}

bool value_to_symbol_id(zeptodb::sql::QueryExecutor& executor,
                        const Value& value,
                        const std::string& column,
                        zeptodb::SymbolId* out,
                        std::string* error)
{
    if (value.kind == ValueKind::String) {
        *out = executor.intern_symbol_for_ingest(value.s);
        return true;
    }

    int64_t id = 0;
    if (!value_to_i64(value, 1.0, column, &id, error)) return false;
    if (id < 0 ||
        id > static_cast<int64_t>(std::numeric_limits<zeptodb::SymbolId>::max())) {
        if (error) *error = "Column '" + column + "' symbol id is out of uint32 range";
        return false;
    }
    *out = static_cast<zeptodb::SymbolId>(id);
    return true;
}

const Value* find_column(const Value& root, const std::string& name) {
    if (root.kind != ValueKind::Map || name.empty()) return nullptr;
    for (size_t i = 0; i < root.map_keys.size(); ++i) {
        if (root.map_keys[i] == name) return &root.map_values[i];
    }
    return nullptr;
}

const Value* find_symbol_column(const Value& root, const std::string& requested) {
    if (const Value* col = find_column(root, requested)) return col;
    if (requested == "sym") return find_column(root, "symbol");
    if (requested == "symbol") return find_column(root, "sym");
    return nullptr;
}

bool require_array(const Value* value,
                   const std::string& column,
                   const std::vector<Value>** out,
                   std::string* error)
{
    if (!value) {
        if (error) *error = "Missing MessagePack column '" + column + "'";
        return false;
    }
    if (value->kind != ValueKind::Array) {
        if (error) *error = "Column '" + column + "' must be an array";
        return false;
    }
    *out = &value->array;
    return true;
}

bool optional_array(const Value* value,
                    const std::string& column,
                    const std::vector<Value>** out,
                    std::string* error)
{
    if (!value) {
        *out = nullptr;
        return true;
    }
    if (value->kind != ValueKind::Array) {
        if (error) *error = "Column '" + column + "' must be an array";
        return false;
    }
    *out = &value->array;
    return true;
}

bool same_length(const std::vector<Value>& values,
                 size_t expected,
                 const std::string& column,
                 std::string* error)
{
    if (values.size() == expected) return true;
    if (error) {
        *error = "Column '" + column + "' length " +
                 std::to_string(values.size()) + " does not match row count " +
                 std::to_string(expected);
    }
    return false;
}

bool decode_ticks(zeptodb::sql::QueryExecutor& executor,
                  const Value& root,
                  const MsgpackIngestOptions& options,
                  std::vector<zeptodb::ingestion::TickMessage>* out,
                  std::string* error)
{
    if (root.kind != ValueKind::Map) {
        if (error) *error = "MessagePack body must be a map of column arrays";
        return false;
    }

    const std::vector<Value>* symbol_col = nullptr;
    const std::vector<Value>* price_col = nullptr;
    const std::vector<Value>* volume_col = nullptr;
    const std::vector<Value>* timestamp_col = nullptr;
    const std::vector<Value>* msg_type_col = nullptr;

    if (!require_array(find_symbol_column(root, options.symbol_column),
                       options.symbol_column, &symbol_col, error) ||
        !require_array(find_column(root, options.price_column),
                       options.price_column, &price_col, error) ||
        !require_array(find_column(root, options.volume_column),
                       options.volume_column, &volume_col, error) ||
        !optional_array(find_column(root, options.timestamp_column),
                        options.timestamp_column, &timestamp_col, error) ||
        !optional_array(find_column(root, options.msg_type_column),
                        options.msg_type_column, &msg_type_col, error)) {
        return false;
    }

    const size_t rows = symbol_col->size();
    if (!same_length(*price_col, rows, options.price_column, error) ||
        !same_length(*volume_col, rows, options.volume_column, error) ||
        (timestamp_col && !same_length(*timestamp_col, rows, options.timestamp_column, error)) ||
        (msg_type_col && !same_length(*msg_type_col, rows, options.msg_type_column, error))) {
        return false;
    }

    const int64_t base_ts = msgpack_ingest_now_ns();
    out->clear();
    out->reserve(rows);

    for (size_t row = 0; row < rows; ++row) {
        zeptodb::ingestion::TickMessage msg{};
        if ((*symbol_col)[row].kind == ValueKind::Nil) {
            if (error) *error = "Column '" + options.symbol_column + "' contains nil";
            return false;
        }
        if (!value_to_symbol_id(executor, (*symbol_col)[row],
                                options.symbol_column, &msg.symbol_id, error) ||
            !value_to_i64((*price_col)[row], options.price_scale,
                          options.price_column, &msg.price, error) ||
            !value_to_i64((*volume_col)[row], options.volume_scale,
                          options.volume_column, &msg.volume, error)) {
            return false;
        }

        if (timestamp_col && (*timestamp_col)[row].kind != ValueKind::Nil) {
            if (!value_to_i64((*timestamp_col)[row], 1.0,
                              options.timestamp_column, &msg.recv_ts, error)) {
                return false;
            }
        } else {
            msg.recv_ts = base_ts + static_cast<int64_t>(row);
        }

        if (msg_type_col && (*msg_type_col)[row].kind != ValueKind::Nil) {
            int64_t msg_type = 0;
            if (!value_to_i64((*msg_type_col)[row], 1.0,
                              options.msg_type_column, &msg_type, error)) {
                return false;
            }
            if (msg_type < 0 || msg_type > std::numeric_limits<uint8_t>::max()) {
                if (error) *error = "Column '" + options.msg_type_column +
                                    "' msg_type is out of uint8 range";
                return false;
            }
            msg.msg_type = static_cast<uint8_t>(msg_type);
        }

        msg.price_is_float = 0;
        out->push_back(msg);
    }
    return true;
}

} // namespace

MsgpackIngestResult ingest_msgpack_columns(
    zeptodb::sql::QueryExecutor& executor,
    const std::string& body,
    const MsgpackIngestOptions& options)
{
    MsgpackIngestResult result;
    if (body.empty()) {
        result.error = "Empty MessagePack body";
        return result;
    }
    if (!std::isfinite(options.price_scale) ||
        !std::isfinite(options.volume_scale)) {
        result.error = "price_scale and volume_scale must be finite";
        return result;
    }

    Value root;
    std::string error;
    Decoder decoder(body);
    if (!decoder.parse(&root, &error)) {
        result.error = error;
        return result;
    }

    std::vector<zeptodb::ingestion::TickMessage> ticks;
    if (!decode_ticks(executor, root, options, &ticks, &error)) {
        result.error = error;
        return result;
    }

    auto ingest = executor.ingest_tick_batch(options.table_name, std::move(ticks));
    result.rows = ingest.inserted;
    result.failed = ingest.failed;
    if (!ingest.ok()) {
        result.error = ingest.error;
        return result;
    }

    result.ok = true;
    return result;
}

} // namespace zeptodb::server
