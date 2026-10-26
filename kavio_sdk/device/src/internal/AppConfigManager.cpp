#include "AppConfigManager.h"

#include <cerrno>
#include <cmath>
#include <cstdlib>
#include <fcntl.h>
#include <fstream>
#include <limits>
#include <memory>
#include <regex>
#include <set>
#include <sys/stat.h>
#include <unistd.h>

namespace kavio {
namespace {
constexpr size_t kMaxConfigBytes = 1024u * 1024u;

Status ConfigError(const std::string& message) {
    return Status::Error(StatusCode::Common_InvalidArgument, message);
}

bool ParseJson(const std::string& text, Json::Value& value, std::string& error) {
    Json::CharReaderBuilder options;
    options["collectComments"] = false;
    options["allowComments"] = false;
    options["allowTrailingCommas"] = false;
    std::unique_ptr<Json::CharReader> reader(options.newCharReader());
    return reader->parse(text.data(), text.data() + text.size(), &value, &error);
}

std::string Serialize(const Json::Value& value) {
    Json::StreamWriterBuilder writer;
    writer["indentation"] = "  ";
    return Json::writeString(writer, value);
}

Status Read(const std::string& path, std::string& bytes, bool& exists) {
    struct stat info{};
    if (::stat(path.c_str(), &info) < 0) {
        exists = false;
        if (errno == ENOENT) return Status::Ok();
        return Status::Error(StatusCode::Common_InternalError, "cannot stat configuration: " + path);
    }
    exists = true;
    if (!S_ISREG(info.st_mode) || info.st_size < 0 ||
        static_cast<uint64_t>(info.st_size) > kMaxConfigBytes)
        return ConfigError("configuration file must be a regular JSON file up to 1 MiB: " + path);
    std::ifstream stream(path, std::ios::binary);
    if (!stream)
        return Status::Error(StatusCode::Common_InternalError, "cannot open configuration: " + path);
    bytes.assign(std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>());
    if (stream.bad())
        return Status::Error(StatusCode::Common_InternalError, "cannot read configuration: " + path);
    return Status::Ok();
}

Status AtomicWrite(const std::string& path, const std::string& content) {
    if (path.empty() || content.size() > kMaxConfigBytes)
        return ConfigError("invalid configuration output path or length");
    const std::string temporary = path + ".tmp";
    const int fd = ::open(temporary.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
    if (fd < 0) return Status::Error(StatusCode::Common_InternalError, "cannot create config temporary file");
    size_t offset = 0;
    bool ok = true;
    while (offset < content.size()) {
        const ssize_t written = ::write(fd, content.data() + offset, content.size() - offset);
        if (written > 0) offset += static_cast<size_t>(written);
        else if (written < 0 && errno == EINTR) continue;
        else { ok = false; break; }
    }
    if (ok && ::fsync(fd) != 0) ok = false;
    if (::close(fd) != 0) ok = false;
    if (ok && ::rename(temporary.c_str(), path.c_str()) != 0) ok = false;
    if (!ok) {
        ::unlink(temporary.c_str());
        return Status::Error(StatusCode::Common_InternalError, "failed atomically saving configuration: " + path);
    }
    return Status::Ok();
}

bool IsInteger(const Json::Value& value) { return value.isInt() || value.isUInt() || value.isInt64() || value.isUInt64(); }

bool Matches(const Json::Value& value, const Json::Value& field) {
    const std::string type = field.get("type", "").asString();
    if (type == "boolean") return value.isBool();
    if (type == "string") {
        if (!value.isString()) return false;
        const auto size = value.asString().size();
        if (field.get("required",false).asBool() && size == 0) return false;
        if (field.isMember("min_length") && size < field["min_length"].asUInt()) return false;
        if (field.isMember("max_length") && size > field["max_length"].asUInt()) return false;
        return true;
    }
    if (type == "enum") {
        if (!value.isString()) return false;
        for (const auto& option : field["options"])
            if (option.get("value",Json::Value()) == value) return true;
        if (!field.isMember("custom") || !field["custom"].isObject()) return false;
        const Json::Value& custom = field["custom"];
        const size_t length = value.asString().size();
        return !(field.get("required", false).asBool() && value.asString().empty()) &&
               !(custom.isMember("min_length") && length < custom["min_length"].asUInt()) &&
               !(custom.isMember("max_length") && length > custom["max_length"].asUInt());
    }
    if (type == "integer" && !IsInteger(value)) return false;
    if (type == "number" && !value.isNumeric()) return false;
    if (type != "number" && type != "integer") return false;
    const double number = value.asDouble();
    return std::isfinite(number) &&
        (!field.isMember("minimum") || number >= field["minimum"].asDouble()) &&
        (!field.isMember("maximum") || number <= field["maximum"].asDouble());
}

Status CheckSchemaShape(const Json::Value& schema) {
    static const std::set<std::string> root_keys{
        "definition_version", "title", "description", "fields"};
    static const std::set<std::string> field_keys{
        "key", "name", "category", "description", "type", "required", "read_only",
        "minimum", "maximum", "min_length", "max_length", "multiline", "unit",
        "placeholder", "options", "show_value", "custom", "value"};
    for (const auto& name : schema.getMemberNames())
        if (!root_keys.count(name)) return ConfigError("unknown schema property: " + name);
    if (schema.get("definition_version", "").asString() != "2.0" ||
        !schema["title"].isString() || schema["title"].asString().empty() ||
        (schema.isMember("description") && !schema["description"].isString()) ||
        !schema["fields"].isArray() || schema["fields"].empty() ||
        schema["fields"].size() > 128)
        return ConfigError("invalid schema description root");
    static const std::regex key_expression("^[A-Za-z_][A-Za-z0-9_]{0,63}$");
    std::set<std::string> keys;
    for (const auto& field : schema["fields"]) {
        if (!field.isObject()) return ConfigError("schema field must be an object");
        for (const auto& name : field.getMemberNames())
            if (!field_keys.count(name)) return ConfigError("unknown schema field property: " + name);
        for (const auto* mandatory : {"key", "name", "category", "description", "type"})
            if (!field[mandatory].isString())
                return ConfigError(std::string("invalid schema field property: ") + mandatory);
        const auto key = field["key"].asString();
        if (!std::regex_match(key, key_expression) || !keys.insert(key).second)
            return ConfigError("invalid or duplicate schema key: " + key);
        const auto type = field["type"].asString();
        if (type != "boolean" && type != "integer" && type != "number" &&
            type != "string" && type != "enum")
            return ConfigError("unsupported schema type: " + type);
        if (!field.isMember("value") || !Matches(field["value"],field))
            return ConfigError("invalid schema initial value: " + key);
        if (type == "enum") {
            if (!field["options"].isArray() || field["options"].empty() ||
                field["options"].size() > 256) return ConfigError("invalid enum options");
            for (const auto& option : field["options"])
                if (!option.isObject() || !option["value"].isString() ||
                    !option["label"].isString()) return ConfigError("invalid enum option");
        } else if (field.isMember("options") || field.isMember("custom") ||
                   field.isMember("show_value")) return ConfigError("unexpected enum attributes");
        if ((type != "number" && type != "integer") &&
            (field.isMember("minimum") || field.isMember("maximum")))
            return ConfigError("numeric limits on non-numeric field");
        if (type != "string" && (field.isMember("min_length") ||
                                 field.isMember("max_length") || field.isMember("multiline")))
            return ConfigError("string limits on non-string field");
    }
    return Status::Ok();
}
} // namespace

Status AppConfigManager::Initialize() {
    std::lock_guard<std::mutex> guard(mutex_);
    const char* definition_path = std::getenv("KAVIO_APP_SCHEMA_DESCRIPTION_PATH");
    const char* data_dir = std::getenv("KAVIO_APP_DATA_DIR");
    if (!definition_path || !*definition_path || !data_dir || !*data_dir) {
        // Non-managed IPC clients have no App configuration lifecycle.
        schema_loaded_ = true;
        enabled_ = false;
        return Status::Ok();
    }
    if (!schema_loaded_) {
        std::string data; bool exists = false;
        struct stat schema_info{};
        if (::stat(definition_path, &schema_info) == 0 && schema_info.st_size > 256 * 1024)
            return ConfigError("schema_description.json exceeds 256 KiB; use {} for no configuration");
        Status st = Read(definition_path, data, exists);
        if (!st.ok()) return st;
        if (!exists) return ConfigError("schema_description.json is missing; use {} for no configuration");
        std::string error;
        if (!ParseJson(data, schema_, error) || !schema_.isObject())
            return ConfigError("invalid schema_description.json; use {} for no configuration: " + error);
        if (!schema_.empty()) {
            const Status checked = CheckSchemaShape(schema_);
            if (!checked.ok())
                return ConfigError(checked.message + "; use {} in schema_description.json for no configuration");
            for (const auto& field : schema_["fields"])
                defaults_[field["key"].asString()] = field["value"];
            enabled_ = true;
        }
        schema_raw_ = Serialize(schema_);
        config_path_ = std::string(data_dir) + "/config.json";
        schema_loaded_ = true;
    }
    if (!enabled_) return Status::Ok();
    std::string saved; bool exists = false;
    auto st = Read(config_path_, saved, exists);
    if (!st.ok()) return st;
    if (!exists) {
        st = AtomicWrite(config_path_, Serialize(defaults_));
        if (!st.ok()) return st;
        current_ = defaults_;
        return Status::Ok();
    }
    Json::Value loaded; std::string error;
    if (!ParseJson(saved, loaded, error))
        return ConfigError("stored config.json contains invalid JSON: " + error + "; App may restore defaults");
    st = Validate(loaded);
    if (!st.ok()) return st;
    current_ = loaded;
    return Status::Ok();
}

Status AppConfigManager::Validate(const Json::Value& value) const {
    if (!value.isObject() || value.size() != defaults_.size())
        return ConfigError("config.json fields do not match schema_description.json; App may restore defaults");
    for (const auto& field : schema_["fields"]) {
        const std::string key = field["key"].asString();
        if (!value.isMember(key) || !Matches(value[key], field))
            return ConfigError("config.json field does not match schema: " + key);
    }
    return Status::Ok();
}

Status AppConfigManager::Persist(const Json::Value& next, bool save_previous) {
    const std::string content = Serialize(next);
    if (save_previous) {
        auto st = AtomicWrite(config_path_ + ".backup", Serialize(current_));
        if (!st.ok()) return st;
    }
    return AtomicWrite(config_path_, content);
}

Status AppConfigManager::Update(const Json::Value& value) {
    std::lock_guard<std::mutex> guard(mutex_);
    if (!enabled_) return Status::Error(StatusCode::Common_Unsupported,"App has no configuration");
    auto st = Validate(value);
    if (!st.ok()) return st;
    st = Persist(value, true);
    if (st.ok()) current_ = value;
    return st;
}

Status AppConfigManager::ResetToDefaults() {
    std::lock_guard<std::mutex> guard(mutex_);
    if (!schema_loaded_ || !enabled_)
        return Status::Error(StatusCode::Common_Unsupported,"App has no configuration");
    const auto st = AtomicWrite(config_path_, Serialize(defaults_));
    if (st.ok()) current_ = defaults_;
    return st;
}

Json::Value AppConfigManager::Snapshot() const { std::lock_guard<std::mutex> guard(mutex_); return current_; }
std::string AppConfigManager::DescriptionJson() const { std::lock_guard<std::mutex> guard(mutex_); return schema_raw_; }
std::string AppConfigManager::CurrentJson() const { return Serialize(Snapshot()); }
bool AppConfigManager::Enabled() const { std::lock_guard<std::mutex> guard(mutex_); return enabled_; }
} // namespace kavio
