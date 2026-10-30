#include "config/config_loader.hpp"
#include "security/acl.hpp"
#include <fstream>
#include <sstream>
#include <iostream>
#include <algorithm>
#include <cctype>

namespace helios {

namespace {

std::string Trim(const std::string& str) {
    auto start = str.find_first_not_of(" \t\r\n");
    if (start == std::string::npos) return "";
    auto end = str.find_last_not_of(" \t\r\n");
    return str.substr(start, end - start + 1);
}

std::string StripQuotes(const std::string& str) {
    std::string s = Trim(str);
    if (s.size() >= 2) {
        if ((s.front() == '"' && s.back() == '"') || (s.front() == '\'' && s.back() == '\'')) {
            s = s.substr(1, s.size() - 2);
        }
    }
    return s;
}

std::string ToLower(std::string str) {
    std::transform(str.begin(), str.end(), str.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    return str;
}

bool IsPowerOfTwo(size_t n) {
    return n > 0 && (n & (n - 1)) == 0;
}

struct LineInfo {
    size_t line_num{0};
    size_t indent{0};
    std::string content;
    bool is_seq_item{false};
};

// Strips comments taking care of quoted substrings
std::string StripComment(const std::string& line) {
    bool in_double_quote = false;
    bool in_single_quote = false;
    for (size_t i = 0; i < line.size(); ++i) {
        char c = line[i];
        if (c == '"' && !in_single_quote) {
            if (i == 0 || line[i - 1] != '\\') {
                in_double_quote = !in_double_quote;
            }
        } else if (c == '\'' && !in_double_quote) {
            in_single_quote = !in_single_quote;
        } else if (c == '#' && !in_double_quote && !in_single_quote) {
            return line.substr(0, i);
        }
    }
    return line;
}

size_t FindKeyColon(const std::string& str) {
    bool in_double_quote = false;
    bool in_single_quote = false;
    for (size_t i = 0; i < str.size(); ++i) {
        char c = str[i];
        if (c == '"' && !in_single_quote) {
            if (i == 0 || str[i - 1] != '\\') {
                in_double_quote = !in_double_quote;
            }
        } else if (c == '\'' && !in_double_quote) {
            in_single_quote = !in_single_quote;
        } else if (c == ':' && !in_double_quote && !in_single_quote) {
            // Check if colon is followed by whitespace or end of string
            if (i + 1 == str.size() || std::isspace(static_cast<unsigned char>(str[i + 1]))) {
                return i;
            }
        }
    }
    return std::string::npos;
}

bool ParseBlock(const std::vector<LineInfo>& lines, size_t& idx, size_t current_indent, YamlNode& out_node, std::string& err) {
    if (idx >= lines.size()) return true;

    bool is_seq = lines[idx].is_seq_item && lines[idx].indent == current_indent;

    if (is_seq) {
        out_node.type = YamlNodeType::Sequence;
        out_node.line = lines[idx].line_num;
        out_node.column = lines[idx].indent;

        while (idx < lines.size()) {
            if (lines[idx].indent < current_indent) {
                break;
            }
            if (lines[idx].indent != current_indent || !lines[idx].is_seq_item) {
                break;
            }

            const auto& line = lines[idx];
            std::string content = line.content;
            size_t colon = FindKeyColon(content);

            if (colon != std::string::npos) {
                // Sequence of mappings
                YamlNode map_node;
                map_node.type = YamlNodeType::Map;
                map_node.line = line.line_num;
                map_node.column = line.indent;

                std::string k = Trim(content.substr(0, colon));
                std::string v = Trim(content.substr(colon + 1));
                size_t child_indent = line.indent + 2;

                if (!v.empty()) {
                    YamlNode val_node;
                    val_node.type = YamlNodeType::Scalar;
                    val_node.value = StripQuotes(v);
                    val_node.line = line.line_num;
                    map_node.map[k] = std::move(val_node);
                    idx++;
                } else {
                    idx++;
                    if (idx < lines.size() && lines[idx].indent > line.indent) {
                        YamlNode val_node;
                        if (!ParseBlock(lines, idx, lines[idx].indent, val_node, err)) return false;
                        map_node.map[k] = std::move(val_node);
                    }
                }

                // Parse subsequent keys at child_indent
                while (idx < lines.size() && lines[idx].indent >= child_indent && !lines[idx].is_seq_item) {
                    const auto& sub_line = lines[idx];
                    size_t sub_colon = FindKeyColon(sub_line.content);
                    if (sub_colon == std::string::npos) {
                        err = "Line " + std::to_string(sub_line.line_num) + ": Expected key-value pair in sequence map item";
                        return false;
                    }
                    std::string sub_k = Trim(sub_line.content.substr(0, sub_colon));
                    std::string sub_v = Trim(sub_line.content.substr(sub_colon + 1));

                    if (!sub_v.empty()) {
                        YamlNode val_node;
                        val_node.type = YamlNodeType::Scalar;
                        val_node.value = StripQuotes(sub_v);
                        val_node.line = sub_line.line_num;
                        map_node.map[sub_k] = std::move(val_node);
                        idx++;
                    } else {
                        idx++;
                        if (idx < lines.size() && lines[idx].indent > sub_line.indent) {
                            YamlNode val_node;
                            if (!ParseBlock(lines, idx, lines[idx].indent, val_node, err)) return false;
                            map_node.map[sub_k] = std::move(val_node);
                        }
                    }
                }

                out_node.sequence.push_back(std::move(map_node));
            } else if (!content.empty()) {
                // Sequence of scalars
                YamlNode scalar_node;
                scalar_node.type = YamlNodeType::Scalar;
                scalar_node.value = StripQuotes(content);
                scalar_node.line = line.line_num;
                scalar_node.column = line.indent;
                out_node.sequence.push_back(std::move(scalar_node));
                idx++;
            } else {
                // Sequence item defined by nested block
                idx++;
                if (idx < lines.size() && lines[idx].indent > current_indent) {
                    YamlNode item_node;
                    if (!ParseBlock(lines, idx, lines[idx].indent, item_node, err)) return false;
                    out_node.sequence.push_back(std::move(item_node));
                }
            }
        }
    } else {
        out_node.type = YamlNodeType::Map;
        out_node.line = lines[idx].line_num;
        out_node.column = lines[idx].indent;

        while (idx < lines.size()) {
            if (lines[idx].indent < current_indent) {
                break;
            }
            if (lines[idx].indent != current_indent) {
                break;
            }
            if (lines[idx].is_seq_item) {
                break;
            }

            const auto& line = lines[idx];
            size_t colon = FindKeyColon(line.content);
            if (colon == std::string::npos) {
                err = "Line " + std::to_string(line.line_num) + ": Expected key-value pair, found: '" + line.content + "'";
                return false;
            }

            std::string key = Trim(line.content.substr(0, colon));
            std::string val_str = Trim(line.content.substr(colon + 1));

            if (!val_str.empty()) {
                YamlNode val_node;
                val_node.type = YamlNodeType::Scalar;
                val_node.value = StripQuotes(val_str);
                val_node.line = line.line_num;
                val_node.column = line.indent + colon + 1;
                out_node.map[key] = std::move(val_node);
                idx++;
            } else {
                idx++;
                if (idx < lines.size() && lines[idx].indent > current_indent) {
                    YamlNode child_node;
                    if (!ParseBlock(lines, idx, lines[idx].indent, child_node, err)) return false;
                    out_node.map[key] = std::move(child_node);
                } else {
                    YamlNode null_node;
                    null_node.type = YamlNodeType::Null;
                    out_node.map[key] = std::move(null_node);
                }
            }
        }
    }

    return true;
}

} // namespace

// ============================================================================
// YamlNode Implementation
// ============================================================================

std::string YamlNode::AsString(const std::string& default_val) const {
    if (type == YamlNodeType::Scalar) return value;
    return default_val;
}

int64_t YamlNode::AsInt(int64_t default_val) const {
    if (type != YamlNodeType::Scalar || value.empty()) return default_val;
    try {
        return std::stoll(value);
    } catch (...) {
        return default_val;
    }
}

uint64_t YamlNode::AsUInt(uint64_t default_val) const {
    if (type != YamlNodeType::Scalar || value.empty()) return default_val;
    try {
        return std::stoull(value);
    } catch (...) {
        return default_val;
    }
}

double YamlNode::AsDouble(double default_val) const {
    if (type != YamlNodeType::Scalar || value.empty()) return default_val;
    try {
        return std::stod(value);
    } catch (...) {
        return default_val;
    }
}

bool YamlNode::AsBool(bool default_val) const {
    if (type != YamlNodeType::Scalar || value.empty()) return default_val;
    std::string lower = ToLower(value);
    if (lower == "true" || lower == "yes" || lower == "on" || lower == "1") return true;
    if (lower == "false" || lower == "no" || lower == "off" || lower == "0") return false;
    return default_val;
}

const YamlNode* YamlNode::Find(const std::string& key) const {
    if (type != YamlNodeType::Map) return nullptr;
    auto it = map.find(key);
    if (it != map.end()) return &it->second;
    return nullptr;
}

const YamlNode& YamlNode::operator[](const std::string& key) const {
    static const YamlNode kNullNode;
    const auto* p = Find(key);
    return p ? *p : kNullNode;
}

// ============================================================================
// HeliosConfig Mapping
// ============================================================================

WorkerPoolConfig HeliosConfig::ToWorkerPoolConfig() const {
    WorkerPoolConfig wcfg;
    wcfg.listen_host = server.host;
    wcfg.listen_port = server.port;
    wcfg.num_workers = server.workers;
    wcfg.total_buffers = server.buffer_pool_blocks;
    wcfg.buffer_size = server.buffer_block_size;
    wcfg.ring_entries = server.io_uring_entries;
    wcfg.drain_timeout = timeouts.drain_timeout;
    wcfg.header_read_timeout = timeouts.header_read_timeout;
    wcfg.security = security;
    wcfg.tls = tls;
    if (!backend_pools.empty()) {
        wcfg.lb_algorithm = backend_pools[0].algorithm;
        wcfg.enable_connection_pooling = backend_pools[0].enable_connection_pool;
        wcfg.connection_pool_config = backend_pools[0].connection_pool_config;
    }
    return wcfg;
}

PrometheusServer::Config HeliosConfig::ToPrometheusConfig() const {
    PrometheusServer::Config pcfg;
    pcfg.host = server.host;
    pcfg.port = metrics.port;
    return pcfg;
}

AccessLoggerConfig HeliosConfig::ToAccessLoggerConfig() const {
    AccessLoggerConfig lcfg;
    lcfg.enabled = logging.enabled;
    lcfg.format = (logging.format == "combined") ? LogFormat::Combined : LogFormat::Json;
    lcfg.output = logging.output;
    lcfg.queue_capacity = logging.queue_capacity;
    lcfg.flush_interval = logging.flush_interval;
    return lcfg;
}

// ============================================================================
// ConfigLoader Implementation
// ============================================================================

bool ConfigLoader::ParseYaml(const std::string& yaml_content, YamlNode& root, std::string& error_message) {
    std::vector<LineInfo> lines;
    std::istringstream stream(yaml_content);
    std::string raw_line;
    size_t line_num = 0;

    while (std::getline(stream, raw_line)) {
        line_num++;
        std::string stripped = StripComment(raw_line);

        auto first_char = stripped.find_first_not_of(" \t");
        if (first_char == std::string::npos) {
            continue; // Blank or comment line
        }

        LineInfo info;
        info.line_num = line_num;
        info.indent = first_char;

        std::string trimmed_content = Trim(stripped.substr(first_char));
        if (trimmed_content.rfind("- ", 0) == 0) {
            info.is_seq_item = true;
            info.content = Trim(trimmed_content.substr(2));
        } else if (trimmed_content == "-") {
            info.is_seq_item = true;
            info.content = "";
        } else {
            info.is_seq_item = false;
            info.content = trimmed_content;
        }

        lines.push_back(std::move(info));
    }

    if (lines.empty()) {
        root.type = YamlNodeType::Map;
        return true;
    }

    size_t idx = 0;
    size_t root_indent = lines[0].indent;
    return ParseBlock(lines, idx, root_indent, root, error_message);
}

bool ConfigLoader::FromYaml(const YamlNode& root, HeliosConfig& config, std::string& error_message) {
    if (root.type != YamlNodeType::Map) {
        error_message = "Root YAML node must be a map/dictionary";
        return false;
    }

    // 1. Server Configuration
    if (const auto* srv = root.Find("server")) {
        if (const auto* val = srv->Find("host")) config.server.host = val->AsString("0.0.0.0");
        if (const auto* val = srv->Find("port")) config.server.port = static_cast<uint16_t>(val->AsUInt(8080));
        if (const auto* val = srv->Find("workers")) config.server.workers = static_cast<size_t>(val->AsUInt(0));
        if (const auto* val = srv->Find("cpu_affinity")) config.server.cpu_affinity = val->AsBool(true);
        if (const auto* val = srv->Find("max_connections")) config.server.max_connections = static_cast<size_t>(val->AsUInt(10000));
        if (const auto* val = srv->Find("buffer_pool_blocks")) config.server.buffer_pool_blocks = static_cast<size_t>(val->AsUInt(1024));
        if (const auto* val = srv->Find("buffer_block_size")) config.server.buffer_block_size = static_cast<size_t>(val->AsUInt(16384));
        if (const auto* val = srv->Find("io_uring_entries")) config.server.io_uring_entries = static_cast<size_t>(val->AsUInt(512));
    }

    // 2. Backend Pools Configuration
    if (const auto* pools = root.Find("backend_pools")) {
        if (pools->type == YamlNodeType::Sequence) {
            config.backend_pools.clear();
            for (const auto& pool_node : pools->sequence) {
                BackendPoolConfig pool_cfg;
                pool_cfg.name = pool_node["name"].AsString();
                pool_cfg.algorithm = pool_node["algorithm"].AsString("round_robin");

                const YamlNode* endpoints_node = pool_node.Find("endpoints");
                if (!endpoints_node) endpoints_node = pool_node.Find("backends");

                if (endpoints_node && endpoints_node->type == YamlNodeType::Sequence) {
                    for (const auto& ep_node : endpoints_node->sequence) {
                        BackendEndpointConfig ep;
                        ep.host = ep_node["host"].AsString();
                        if (ep.host.empty()) ep.host = ep_node["address"].AsString();
                        ep.port = static_cast<uint16_t>(ep_node["port"].AsUInt(0));
                        ep.weight = static_cast<uint32_t>(ep_node["weight"].AsUInt(1));
                        pool_cfg.endpoints.push_back(std::move(ep));
                    }
                }

                if (const auto* cp = pool_node.Find("connection_pool")) {
                    if (const auto* val = cp->Find("enabled")) pool_cfg.enable_connection_pool = val->AsBool(false);
                    if (const auto* val = cp->Find("max_idle_per_backend")) {
                        pool_cfg.connection_pool_config.max_idle_per_backend = static_cast<size_t>(val->AsUInt(32));
                    }
                    if (const auto* val = cp->Find("max_total_idle")) {
                        pool_cfg.connection_pool_config.max_total_idle = static_cast<size_t>(val->AsUInt(1024));
                    }
                    if (const auto* val = cp->Find("idle_timeout_ms")) {
                        pool_cfg.connection_pool_config.idle_timeout = std::chrono::milliseconds(val->AsUInt(60000));
                    }
                }

                config.backend_pools.push_back(std::move(pool_cfg));
            }
        }
    }

    // 3. Timeouts Configuration
    if (const auto* timeouts = root.Find("timeouts")) {
        if (const auto* val = timeouts->Find("idle_timeout_ms")) {
            config.timeouts.idle_timeout = std::chrono::milliseconds(val->AsUInt(30000));
        }
        if (const auto* val = timeouts->Find("header_read_timeout_ms")) {
            config.timeouts.header_read_timeout = std::chrono::milliseconds(val->AsUInt(5000));
        }
        if (const auto* val = timeouts->Find("connect_timeout_ms")) {
            config.timeouts.connect_timeout = std::chrono::milliseconds(val->AsUInt(2000));
        }
        if (const auto* val = timeouts->Find("half_close_fallback_ms")) {
            config.timeouts.half_close_fallback = std::chrono::milliseconds(val->AsUInt(5000));
        }
        if (const auto* val = timeouts->Find("drain_timeout_ms")) {
            config.timeouts.drain_timeout = std::chrono::milliseconds(val->AsUInt(15000));
        }
    }

    // 4. Health Check Configuration
    const auto* hc = root.Find("health_check");
    if (!hc) hc = root.Find("health_checks");
    if (hc) {
        if (const auto* val = hc->Find("enabled")) config.health_check.enabled = val->AsBool(false);
        if (const auto* val = hc->Find("type")) config.health_check.type = ToLower(val->AsString("tcp"));
        if (const auto* val = hc->Find("path")) config.health_check.path = val->AsString("/health");
        if (const auto* exp = hc->Find("expected_status")) {
            config.health_check.expected_status = static_cast<uint16_t>(exp->AsUInt(200));
        } else if (const auto* sc = hc->Find("status_code")) {
            config.health_check.expected_status = static_cast<uint16_t>(sc->AsUInt(200));
        }

        if (const auto* val = hc->Find("interval_ms")) {
            config.health_check.interval = std::chrono::milliseconds(val->AsUInt(5000));
        } else if (const auto* sec = hc->Find("interval_seconds")) {
            config.health_check.interval = std::chrono::milliseconds(static_cast<uint64_t>(sec->AsDouble(5.0) * 1000.0));
        }

        if (const auto* val = hc->Find("timeout_ms")) {
            config.health_check.timeout = std::chrono::milliseconds(val->AsUInt(1000));
        } else if (const auto* sec = hc->Find("timeout_seconds")) {
            config.health_check.timeout = std::chrono::milliseconds(static_cast<uint64_t>(sec->AsDouble(1.0) * 1000.0));
        }

        if (const auto* val = hc->Find("unhealthy_threshold")) config.health_check.unhealthy_threshold = static_cast<uint32_t>(val->AsUInt(3));
        if (const auto* val = hc->Find("healthy_threshold")) config.health_check.healthy_threshold = static_cast<uint32_t>(val->AsUInt(2));
    }

    // 5. Metrics Configuration
    if (const auto* metrics = root.Find("metrics")) {
        if (const auto* val = metrics->Find("enabled")) config.metrics.enabled = val->AsBool(true);
        if (const auto* val = metrics->Find("port")) config.metrics.port = static_cast<uint16_t>(val->AsUInt(9090));
        if (const auto* val = metrics->Find("path")) config.metrics.path = val->AsString("/metrics");
    }

    // 6. Logging Configuration
    if (const auto* log_node = root.Find("logging")) {
        if (const auto* val = log_node->Find("enabled")) config.logging.enabled = val->AsBool(false);
        if (const auto* val = log_node->Find("format")) config.logging.format = ToLower(val->AsString("json"));
        if (const auto* val = log_node->Find("output")) config.logging.output = val->AsString("stdout");
        if (const auto* val = log_node->Find("queue_capacity")) config.logging.queue_capacity = val->AsUInt(65536);
        if (const auto* val = log_node->Find("flush_interval_ms")) {
            config.logging.flush_interval = std::chrono::milliseconds(val->AsUInt(100));
        }
    }

    // 7. L7 Routes Configuration
    if (const auto* routes_node = root.Find("routes")) {
        if (routes_node->type == YamlNodeType::Sequence) {
            config.routes.clear();
            for (const auto& r_node : routes_node->sequence) {
                L7RouteConfig r;
                r.name = r_node["name"].AsString();
                r.host = r_node["host"].AsString();
                if (const auto* pp = r_node.Find("path_prefix")) {
                    r.path_prefix = pp->AsString("/");
                } else if (const auto* p = r_node.Find("path")) {
                    r.path_prefix = p->AsString("/");
                } else {
                    r.path_prefix = "/";
                }
                if (const auto* tp = r_node.Find("target_pool")) {
                    r.target_pool = tp->AsString();
                } else if (const auto* bp = r_node.Find("backend_pool")) {
                    r.target_pool = bp->AsString();
                }
                if (const auto* sp = r_node.Find("strip_prefix")) {
                    r.strip_prefix = sp->AsBool(false);
                }
                if (const auto* ih = r_node.Find("inject_headers")) {
                    r.inject_headers = ih->AsBool(true);
                }
                config.routes.push_back(std::move(r));
            }
        }
    }
    if (const auto* val = root.Find("default_pool")) {
        config.default_pool = val->AsString();
    }

    // Security configuration (ACL, Rate Limiting, Slowloris)
    if (const auto* sec_node = root.Find("security")) {
        if (const auto* acl_node = sec_node->Find("acl")) {
            if (const auto* en = acl_node->Find("enabled")) {
                config.security.acl.enabled = en->AsBool(false);
            }
            if (const auto* dp = acl_node->Find("default_policy")) {
                config.security.acl.default_policy = dp->AsString("allow");
            }
            if (const auto* rules_node = acl_node->Find("rules")) {
                if (rules_node->IsSequence()) {
                    for (const auto& r_node : rules_node->sequence) {
                        AclRuleConfig rule;
                        if (const auto* cidr = r_node.Find("cidr")) {
                            rule.cidr = cidr->AsString();
                        }
                        if (const auto* act = r_node.Find("action")) {
                            rule.action = act->AsString("allow");
                        }
                        config.security.acl.rules.push_back(std::move(rule));
                    }
                }
            }
        }

        if (const auto* rl_node = sec_node->Find("rate_limit")) {
            if (const auto* en = rl_node->Find("enabled")) {
                config.security.rate_limit.enabled = en->AsBool(false);
            }
            if (const auto* rps = rl_node->Find("rate_per_sec")) {
                config.security.rate_limit.rate_per_sec = rps->AsDouble(100.0);
            }
            if (const auto* burst = rl_node->Find("burst_size")) {
                config.security.rate_limit.burst_size = static_cast<uint32_t>(burst->AsUInt(20));
            }
            if (const auto* mask = rl_node->Find("cidr_mask")) {
                config.security.rate_limit.cidr_mask = static_cast<uint8_t>(mask->AsUInt(32));
            }
        }

        if (const auto* sl_node = sec_node->Find("slowloris")) {
            if (const auto* en = sl_node->Find("enabled")) {
                config.security.slowloris.enabled = en->AsBool(true);
            }
            if (const auto* to = sl_node->Find("header_read_timeout_ms")) {
                config.security.slowloris.header_read_timeout = std::chrono::milliseconds(to->AsUInt(5000));
            }
        }

        if (const auto* hdr_node = sec_node->Find("headers")) {
            if (const auto* en = hdr_node->Find("enabled")) {
                config.security.headers.enabled = en->AsBool(false);
            } else {
                config.security.headers.enabled = true;
            }
            if (const auto* srv = hdr_node->Find("server")) {
                config.security.headers.set_server_header = true;
                config.security.headers.server_value = StripQuotes(srv->AsString("helios"));
            }
            if (const auto* srv_en = hdr_node->Find("set_server_header")) {
                config.security.headers.set_server_header = srv_en->AsBool(true);
            }
            if (const auto* hsts_node = hdr_node->Find("hsts")) {
                if (hsts_node->IsMap()) {
                    if (const auto* en = hsts_node->Find("enabled")) {
                        config.security.headers.hsts_enabled = en->AsBool(false);
                    }
                    if (const auto* val = hsts_node->Find("value")) {
                        config.security.headers.hsts_value = StripQuotes(val->AsString("max-age=31536000; includeSubDomains"));
                    }
                } else {
                    config.security.headers.hsts_enabled = hsts_node->AsBool(false);
                }
            }
            if (const auto* cto = hdr_node->Find("x_content_type_options")) {
                config.security.headers.x_content_type_options = cto->AsBool(true);
            }
            if (const auto* xfo = hdr_node->Find("x_frame_options")) {
                config.security.headers.x_frame_options = StripQuotes(xfo->AsString("SAMEORIGIN"));
            }
            if (const auto* custom_node = hdr_node->Find("custom_headers")) {
                if (custom_node->IsSequence()) {
                    for (const auto& ch : custom_node->sequence) {
                        std::string name;
                        std::string val;
                        if (const auto* n = ch.Find("name")) name = StripQuotes(n->AsString());
                        else if (const auto* k = ch.Find("key")) name = StripQuotes(k->AsString());
                        if (const auto* v = ch.Find("value")) val = StripQuotes(v->AsString());
                        if (!name.empty()) {
                            config.security.headers.custom_headers.emplace_back(std::move(name), std::move(val));
                        }
                    }
                }
            }
        }
    }

    if (const auto* top_hdr = root.Find("response_headers")) {
        if (const auto* en = top_hdr->Find("enabled")) {
            config.security.headers.enabled = en->AsBool(false);
        } else {
            config.security.headers.enabled = true;
        }
        if (const auto* srv = top_hdr->Find("server")) {
            config.security.headers.set_server_header = true;
            config.security.headers.server_value = StripQuotes(srv->AsString("helios"));
        }
        if (const auto* srv_en = top_hdr->Find("set_server_header")) {
            config.security.headers.set_server_header = srv_en->AsBool(true);
        }
        if (const auto* hsts_node = top_hdr->Find("hsts")) {
            if (hsts_node->IsMap()) {
                if (const auto* en = hsts_node->Find("enabled")) {
                    config.security.headers.hsts_enabled = en->AsBool(false);
                }
                if (const auto* val = hsts_node->Find("value")) {
                    config.security.headers.hsts_value = StripQuotes(val->AsString("max-age=31536000; includeSubDomains"));
                }
            } else {
                config.security.headers.hsts_enabled = hsts_node->AsBool(false);
            }
        }
        if (const auto* cto = top_hdr->Find("x_content_type_options")) {
            config.security.headers.x_content_type_options = cto->AsBool(true);
        }
        if (const auto* xfo = top_hdr->Find("x_frame_options")) {
            config.security.headers.x_frame_options = StripQuotes(xfo->AsString("SAMEORIGIN"));
        }
        if (const auto* custom_node = top_hdr->Find("custom_headers")) {
            if (custom_node->IsSequence()) {
                for (const auto& ch : custom_node->sequence) {
                    std::string name;
                    std::string val;
                    if (const auto* n = ch.Find("name")) name = StripQuotes(n->AsString());
                    else if (const auto* k = ch.Find("key")) name = StripQuotes(k->AsString());
                    if (const auto* v = ch.Find("value")) val = StripQuotes(v->AsString());
                    if (!name.empty()) {
                        config.security.headers.custom_headers.emplace_back(std::move(name), std::move(val));
                    }
                }
            }
        }
    }

    // TLS / HTTPS Termination configuration
    if (const auto* tls_node = root.Find("tls")) {
        if (const auto* en = tls_node->Find("enabled")) {
            config.tls.enabled = en->AsBool(false);
        }
        if (const auto* port = tls_node->Find("port")) {
            config.tls.port = static_cast<uint16_t>(port->AsUInt(8443));
        }
        if (const auto* cert = tls_node->Find("cert_file")) {
            config.tls.cert_file = StripQuotes(cert->AsString());
        }
        if (const auto* key = tls_node->Find("key_file")) {
            config.tls.key_file = StripQuotes(key->AsString());
        }
        if (const auto* ver = tls_node->Find("min_version")) {
            config.tls.min_version = StripQuotes(ver->AsString("TLSv1.2"));
        }
        if (const auto* ciphers = tls_node->Find("ciphers")) {
            config.tls.ciphers = StripQuotes(ciphers->AsString());
        }
        if (const auto* ciphersuites = tls_node->Find("ciphersuites")) {
            config.tls.ciphersuites = StripQuotes(ciphersuites->AsString());
        }
        if (const auto* alpn_node = tls_node->Find("alpn")) {
            if (alpn_node->IsSequence()) {
                config.tls.alpn_protocols.clear();
                for (const auto& a_node : alpn_node->sequence) {
                    config.tls.alpn_protocols.push_back(StripQuotes(a_node.AsString()));
                }
            }
        }
    }

    return true;
}

bool ConfigLoader::Validate(const HeliosConfig& config, std::string& error_message) {
    // 1. Validate server configuration
    if (config.server.host.empty()) {
        error_message = "server.host must not be empty";
        return false;
    }

    if (!IsPowerOfTwo(config.server.buffer_block_size) || config.server.buffer_block_size < 1024) {
        error_message = "server.buffer_block_size must be a power of two and >= 1024 (got " +
                        std::to_string(config.server.buffer_block_size) + ")";
        return false;
    }

    if (!IsPowerOfTwo(config.server.io_uring_entries) || config.server.io_uring_entries < 64) {
        error_message = "server.io_uring_entries must be a power of two and >= 64 (got " +
                        std::to_string(config.server.io_uring_entries) + ")";
        return false;
    }

    if (config.server.max_connections == 0) {
        error_message = "server.max_connections must be > 0";
        return false;
    }

    // 2. Validate backend pools
    if (config.backend_pools.empty()) {
        error_message = "At least one backend pool must be defined in 'backend_pools'";
        return false;
    }

    for (size_t i = 0; i < config.backend_pools.size(); ++i) {
        const auto& pool = config.backend_pools[i];
        if (pool.name.empty()) {
            error_message = "Backend pool at index " + std::to_string(i) + " must have a non-empty 'name'";
            return false;
        }

        std::string algo = ToLower(pool.algorithm);
        if (algo != "round_robin" && algo != "weighted_round_robin" &&
            algo != "least_connections" && algo != "consistent_hash" &&
            algo != "consistent_hashing") {
            error_message = "Unknown load balancing algorithm '" + pool.algorithm + "' in pool '" + pool.name + "'";
            return false;
        }

        if (pool.endpoints.empty()) {
            error_message = "Backend pool '" + pool.name + "' must have at least one endpoint";
            return false;
        }

        for (size_t j = 0; j < pool.endpoints.size(); ++j) {
            const auto& ep = pool.endpoints[j];
            if (ep.host.empty()) {
                error_message = "Endpoint " + std::to_string(j) + " in pool '" + pool.name + "' has empty host";
                return false;
            }
            if (ep.port == 0) {
                error_message = "Endpoint " + std::to_string(j) + " in pool '" + pool.name + "' has invalid port 0";
                return false;
            }
            if (ep.weight == 0) {
                error_message = "Endpoint " + std::to_string(j) + " in pool '" + pool.name + "' must have weight >= 1";
                return false;
            }
        }
    }

    // 3. Validate timeouts
    if (config.timeouts.idle_timeout.count() <= 0 ||
        config.timeouts.header_read_timeout.count() <= 0 ||
        config.timeouts.connect_timeout.count() <= 0 ||
        config.timeouts.half_close_fallback.count() <= 0 ||
        config.timeouts.drain_timeout.count() <= 0) {
        error_message = "All timeouts in 'timeouts' section must be > 0";
        return false;
    }

    // 4. Validate health check
    if (config.health_check.enabled) {
        if (config.health_check.interval.count() < 10) {
            error_message = "health_check.interval_ms must be >= 10 ms";
            return false;
        }
        if (config.health_check.timeout.count() < 10) {
            error_message = "health_check.timeout_ms must be >= 10 ms";
            return false;
        }
        if (config.health_check.unhealthy_threshold == 0 || config.health_check.healthy_threshold == 0) {
            error_message = "health_check healthy and unhealthy thresholds must be >= 1";
            return false;
        }
        if (config.health_check.type != "tcp" && config.health_check.type != "http") {
            error_message = "health_check.type must be either 'tcp' or 'http'";
            return false;
        }
        if (config.health_check.type == "http") {
            if (config.health_check.path.empty() || config.health_check.path.front() != '/') {
                error_message = "health_check.path must start with '/' (got '" + config.health_check.path + "')";
                return false;
            }
            if (config.health_check.expected_status != 0 && (config.health_check.expected_status < 100 || config.health_check.expected_status > 599)) {
                error_message = "health_check.expected_status must be a valid HTTP status code between 100 and 599 (or 0 for any 2xx)";
                return false;
            }
        }
    }

    // 5. Validate metrics
    if (config.metrics.enabled) {
        if (config.metrics.path.empty() || config.metrics.path.front() != '/') {
            error_message = "metrics.path must start with '/' (got '" + config.metrics.path + "')";
            return false;
        }
    }

    // 6. Validate logging
    if (config.logging.enabled) {
        if (config.logging.format != "json" && config.logging.format != "combined") {
            error_message = "logging.format must be either 'json' or 'combined' (got '" + config.logging.format + "')";
            return false;
        }
        if (config.logging.queue_capacity == 0) {
            error_message = "logging.queue_capacity must be > 0";
            return false;
        }
        if (config.logging.flush_interval.count() == 0) {
            error_message = "logging.flush_interval_ms must be > 0";
            return false;
        }
    }

    // 7. Validate L7 routes if present
    for (size_t i = 0; i < config.routes.size(); ++i) {
        const auto& r = config.routes[i];
        if (r.target_pool.empty()) {
            error_message = "Route at index " + std::to_string(i) + " must specify a non-empty 'target_pool'";
            return false;
        }
        if (r.path_prefix.empty() || r.path_prefix.front() != '/') {
            error_message = "Route at index " + std::to_string(i) + " path_prefix must start with '/' (got '" + r.path_prefix + "')";
            return false;
        }
    }

    // 8. Validate security configuration
    if (config.security.acl.enabled) {
        std::string def_pol = ToLower(config.security.acl.default_policy);
        if (def_pol != "allow" && def_pol != "deny") {
            error_message = "security.acl.default_policy must be 'allow' or 'deny'";
            return false;
        }
        for (size_t i = 0; i < config.security.acl.rules.size(); ++i) {
            const auto& r = config.security.acl.rules[i];
            std::string act = ToLower(r.action);
            if (act != "allow" && act != "deny") {
                error_message = "security.acl.rules[" + std::to_string(i) + "].action must be 'allow' or 'deny'";
                return false;
            }
            auto parsed = AclRule::Parse(r.cidr, (act == "deny") ? AclAction::Deny : AclAction::Allow);
            if (!parsed) {
                error_message = "security.acl.rules[" + std::to_string(i) + "].cidr has invalid CIDR format: '" + r.cidr + "'";
                return false;
            }
        }
    }

    if (config.security.rate_limit.enabled) {
        if (config.security.rate_limit.rate_per_sec <= 0.0) {
            error_message = "security.rate_limit.rate_per_sec must be > 0";
            return false;
        }
        if (config.security.rate_limit.burst_size == 0) {
            error_message = "security.rate_limit.burst_size must be > 0";
            return false;
        }
        if (config.security.rate_limit.cidr_mask > 32) {
            error_message = "security.rate_limit.cidr_mask must be <= 32";
            return false;
        }
    }

    if (config.tls.enabled) {
        if (config.tls.cert_file.empty()) {
            error_message = "tls.cert_file must not be empty when tls is enabled";
            return false;
        }
        if (config.tls.key_file.empty()) {
            error_message = "tls.key_file must not be empty when tls is enabled";
            return false;
        }
        if (config.tls.min_version != "TLSv1.2" && config.tls.min_version != "TLSv1.3") {
            error_message = "tls.min_version must be either 'TLSv1.2' or 'TLSv1.3' (got '" + config.tls.min_version + "')";
            return false;
        }
    }

    return true;
}

ConfigResult ConfigLoader::LoadFromFile(const std::string& filepath) {
    ConfigResult result;
    std::ifstream file(filepath);
    if (!file.is_open()) {
        result.success = false;
        result.error_message = "Failed to open configuration file: " + filepath;
        return result;
    }

    std::stringstream buffer;
    buffer << file.rdbuf();
    return LoadFromString(buffer.str());
}

ConfigResult ConfigLoader::LoadFromString(const std::string& yaml_content) {
    ConfigResult result;
    YamlNode root;
    std::string err;

    if (!ParseYaml(yaml_content, root, err)) {
        result.success = false;
        result.error_message = "YAML Syntax Error: " + err;
        return result;
    }

    if (!FromYaml(root, result.config, err)) {
        result.success = false;
        result.error_message = "YAML Schema Error: " + err;
        return result;
    }

    if (!Validate(result.config, err)) {
        result.success = false;
        result.error_message = "Configuration Validation Error: " + err;
        return result;
    }

    result.success = true;
    return result;
}

} // namespace helios
