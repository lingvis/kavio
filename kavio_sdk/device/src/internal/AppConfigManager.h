#pragma once

#include <kavio/Status.h>
#include <json/json.h>
#include <mutex>
#include <string>

namespace kavio {

// The sole owner of the effective configuration and its persistence.
class AppConfigManager {
public:
    Status Initialize();
    Status ResetToDefaults();
    Status Update(const Json::Value& value);
    Json::Value Snapshot() const;
    std::string DescriptionJson() const;
    std::string CurrentJson() const;
    bool Enabled() const;

private:
    Status Validate(const Json::Value& value) const;
    Status Persist(const Json::Value& next, bool save_previous);
    mutable std::mutex mutex_;
    Json::Value schema_{Json::objectValue};
    Json::Value defaults_{Json::objectValue};
    Json::Value current_{Json::objectValue};
    std::string schema_raw_ = "{}";
    std::string config_path_;
    bool enabled_ = false;
    bool schema_loaded_ = false;
};
} // namespace kavio
