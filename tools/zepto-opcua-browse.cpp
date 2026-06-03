#include "zeptodb/feeds/opcua_consumer.h"

#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <string>
#include <unordered_set>
#include <vector>

#ifdef ZEPTO_OPCUA_AVAILABLE
#include <open62541/client.h>
#include <open62541/client_config_default.h>
#include <open62541/client_highlevel.h>
#include <open62541/types.h>
#endif

namespace {

struct Options {
    std::string endpoint = "opc.tcp://localhost:4840";
    std::string root = "ns=0;i=85";
    int max_depth = 2;
    uint32_t symbol_base = 1000;
    bool emit_config = true;
    bool emit_csv = false;
};

void usage() {
    std::cerr
        << "Usage: zepto-opcua-browse [--endpoint URL] [--root NODEID]\n"
        << "                          [--max-depth N] [--symbol-base N]\n"
        << "                          [--config|--csv]\n";
}

bool parse_args(int argc, char** argv, Options& out) {
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        auto need_value = [&](const char* name) -> const char* {
            if (i + 1 >= argc) {
                std::cerr << name << " requires a value\n";
                return nullptr;
            }
            return argv[++i];
        };
        if (arg == "--endpoint") {
            const char* v = need_value("--endpoint");
            if (!v) return false;
            out.endpoint = v;
        } else if (arg == "--root") {
            const char* v = need_value("--root");
            if (!v) return false;
            out.root = v;
        } else if (arg == "--max-depth") {
            const char* v = need_value("--max-depth");
            if (!v) return false;
            out.max_depth = std::atoi(v);
            if (out.max_depth < 0) return false;
        } else if (arg == "--symbol-base") {
            const char* v = need_value("--symbol-base");
            if (!v) return false;
            out.symbol_base = static_cast<uint32_t>(std::strtoul(v, nullptr, 10));
        } else if (arg == "--config") {
            out.emit_config = true;
            out.emit_csv = false;
        } else if (arg == "--csv") {
            out.emit_config = false;
            out.emit_csv = true;
        } else if (arg == "--help" || arg == "-h") {
            usage();
            std::exit(0);
        } else {
            std::cerr << "unknown argument: " << arg << "\n";
            return false;
        }
    }
    return !out.endpoint.empty() && !out.root.empty();
}

#ifdef ZEPTO_OPCUA_AVAILABLE
std::string json_escape(const std::string& s) {
    std::string out;
    out.reserve(s.size() + 8);
    for (char c : s) {
        switch (c) {
            case '\\': out += "\\\\"; break;
            case '"': out += "\\\""; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default: out += c; break;
        }
    }
    return out;
}

struct BrowseRow {
    std::string node_id;
    std::string browse_name;
    std::string display_name;
};

std::string ua_string_to_std(const UA_String& s) {
    if (!s.data || s.length == 0) return {};
    return std::string(reinterpret_cast<const char*>(s.data), s.length);
}

std::string node_id_to_string(const UA_NodeId& id) {
    switch (id.identifierType) {
        case UA_NODEIDTYPE_NUMERIC:
            return "ns=" + std::to_string(id.namespaceIndex) + ";i=" +
                   std::to_string(id.identifier.numeric);
        case UA_NODEIDTYPE_STRING:
            return "ns=" + std::to_string(id.namespaceIndex) + ";s=" +
                   ua_string_to_std(id.identifier.string);
        default:
            return {};
    }
}

bool parse_node_id(const std::string& s, UA_NodeId& out) {
    int ns = 0;
    int consumed = 0;
    if (std::sscanf(s.c_str(), "ns=%d;%n", &ns, &consumed) != 1 || consumed <= 0) {
        return false;
    }
    const char* p = s.c_str() + consumed;
    if (p[0] == 'i' && p[1] == '=') {
        unsigned int id = 0;
        if (std::sscanf(p + 2, "%u", &id) != 1) return false;
        out = UA_NODEID_NUMERIC(static_cast<UA_UInt16>(ns), id);
        return true;
    }
    if (p[0] == 's' && p[1] == '=') {
        out = UA_NODEID_STRING_ALLOC(static_cast<UA_UInt16>(ns), p + 2);
        return true;
    }
    return false;
}

struct BrowseState {
    UA_Client* client = nullptr;
    int max_depth = 0;
    int depth = 0;
    std::vector<BrowseRow> rows;
    std::unordered_set<std::string> seen;
};

UA_StatusCode visit_child(UA_NodeId child_id,
                          UA_Boolean is_inverse,
                          UA_NodeId /*reference_type_id*/,
                          void* handle) {
    auto* state = static_cast<BrowseState*>(handle);
    if (!state || is_inverse) return UA_STATUSCODE_GOOD;

    const std::string node = node_id_to_string(child_id);
    if (node.empty() || !state->seen.insert(node).second) {
        return UA_STATUSCODE_GOOD;
    }

    UA_NodeClass node_class = UA_NODECLASS_UNSPECIFIED;
    const UA_StatusCode class_status =
        UA_Client_readNodeClassAttribute(state->client, child_id, &node_class);
    if (class_status == UA_STATUSCODE_GOOD && node_class == UA_NODECLASS_VARIABLE) {
        UA_QualifiedName browse_name;
        UA_QualifiedName_init(&browse_name);
        UA_LocalizedText display_name;
        UA_LocalizedText_init(&display_name);

        std::string browse;
        std::string display;
        if (UA_Client_readBrowseNameAttribute(
                state->client, child_id, &browse_name) == UA_STATUSCODE_GOOD) {
            browse = ua_string_to_std(browse_name.name);
        }
        if (UA_Client_readDisplayNameAttribute(
                state->client, child_id, &display_name) == UA_STATUSCODE_GOOD) {
            display = ua_string_to_std(display_name.text);
        }
        state->rows.push_back({node, browse, display});
        UA_QualifiedName_clear(&browse_name);
        UA_LocalizedText_clear(&display_name);
    }

    if (state->depth < state->max_depth) {
        ++state->depth;
        UA_Client_forEachChildNodeCall(state->client, child_id, visit_child, state);
        --state->depth;
    }
    return UA_STATUSCODE_GOOD;
}

int browse_live(const Options& opt) {
    UA_NodeId root;
    if (!parse_node_id(opt.root, root)) {
        std::cerr << "unsupported root NodeId: " << opt.root << "\n";
        return 2;
    }

    UA_Client* client = UA_Client_new();
    if (!client) {
        UA_NodeId_clear(&root);
        std::cerr << "UA_Client_new failed\n";
        return 2;
    }
    UA_ClientConfig_setDefault(UA_Client_getConfig(client));
    UA_StatusCode sc = UA_Client_connect(client, opt.endpoint.c_str());
    if (sc != UA_STATUSCODE_GOOD) {
        UA_NodeId_clear(&root);
        UA_Client_delete(client);
        std::cerr << "connect failed: 0x" << std::hex << sc << std::dec << "\n";
        return 2;
    }

    BrowseState state;
    state.client = client;
    state.max_depth = opt.max_depth;
    UA_Client_forEachChildNodeCall(client, root, visit_child, &state);

    if (opt.emit_csv) {
        std::cout << "node_id,symbol_id,browse_name,display_name\n";
        for (size_t i = 0; i < state.rows.size(); ++i) {
            std::cout << state.rows[i].node_id << ","
                      << (opt.symbol_base + static_cast<uint32_t>(i)) << ","
                      << state.rows[i].browse_name << ","
                      << state.rows[i].display_name << "\n";
        }
    } else {
        std::cout << "{\n  \"nodes\": [\n";
        for (size_t i = 0; i < state.rows.size(); ++i) {
            const auto& row = state.rows[i];
            std::cout << "    {\"node_id\":\"" << json_escape(row.node_id)
                      << "\",\"symbol_id\":" << (opt.symbol_base + i)
                      << ",\"value_scale\":10000.0}";
            if (i + 1 < state.rows.size()) std::cout << ",";
            std::cout << "\n";
        }
        std::cout << "  ]\n}\n";
    }

    UA_Client_disconnect(client);
    UA_Client_delete(client);
    UA_NodeId_clear(&root);
    return 0;
}
#endif

} // namespace

int main(int argc, char** argv) {
    Options opt;
    if (!parse_args(argc, argv, opt)) {
        usage();
        return 1;
    }

#ifdef ZEPTO_OPCUA_AVAILABLE
    return browse_live(opt);
#else
    (void)opt;
    std::cerr << "zepto-opcua-browse requires ZEPTO_USE_OPCUA=ON and open62541\n";
    return 2;
#endif
}
