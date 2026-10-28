#pragma once
// Some legacy C SDK headers may define true/false as integer macros.
// Isolate these macros while parsing spdlog/fmt and this header; restore the
// caller's macro state afterwards. Never patch the bundled spdlog sources.
#if defined(true)
#pragma push_macro("true")
#undef true
#define KAVIO_LOGGING_RESTORE_TRUE_MACRO
#endif
#if defined(false)
#pragma push_macro("false")
#undef false
#define KAVIO_LOGGING_RESTORE_FALSE_MACRO
#endif
// Kavio unified header-only logger: UTC daily log files, stable startup session ID,
// 72-hour retention. Each DLL or executable owns its own local file sink.
// C++14; no separate logging library is required.
#ifndef SPDLOG_HEADER_ONLY
#define SPDLOG_HEADER_ONLY
#endif
#ifdef _WIN32
#ifndef SPDLOG_WCHAR_FILENAMES
#define SPDLOG_WCHAR_FILENAMES
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <unistd.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <dirent.h>
#endif
// VS2015 projects may use the system source code page (e.g. CP936).
// fmt 11 otherwise rejects non-UTF-8 execution encodings at compile time.
// FMT_UNICODE=0 disables that encoding assertion; it does NOT convert log
// messages. Callers must still supply UTF-8 bytes for UTF-8 log files.
// Do not add /utf-8 to the entire legacy project: that may change the
// interpretation of its existing non-UTF-8 source files.
#if defined(_MSC_VER) && !defined(FMT_UNICODE)
#define FMT_UNICODE 0
#endif
#if defined(_MSC_VER)
#pragma warning(push)
// The bundled fmt headers include UTF-8 box drawings in comments.
#pragma warning(disable: 4819)
#endif
#include <spdlog/spdlog.h>
#include <spdlog/pattern_formatter.h>
#include <spdlog/sinks/basic_file_sink.h>
#include <spdlog/details/os.h>
#if defined(_MSC_VER)
#pragma warning(pop)
#endif
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <exception>
#include <memory>
#include <mutex>
#include <regex>
#include <string>
#include <thread>
#include <utility>

namespace kavio { namespace logging {
enum class Level : uint32_t { Trace = 0, Debug = 1, Info = 2, Warn = 3, Error = 4, Critical = 5, Off = 6 };
enum class Runtime { App, KavioWin, Platform, AppHost };
enum class Error { None, InvalidName, AlreadyInitialized, MissingLogDirectory, OpenFailed };
struct Result {
    Error code;
    std::string message;
    // Explicit constructors work with the VS2015/v140 C++14 compiler.
    Result() : code(Error::None) {}
    Result(Error error, const std::string& text) : code(error), message(text) {}
    bool ok() const noexcept { return code == Error::None; }
    static Result Ok() { return Result(); }
    static Result Fail(Error code, const std::string& message) { return Result(code, message); }
};

namespace detail {
inline spdlog::level::level_enum ToSpdlog(Level level) noexcept {
    switch (level) {
    case Level::Trace: return spdlog::level::trace;
    case Level::Debug: return spdlog::level::debug;
    case Level::Info: return spdlog::level::info;
    case Level::Warn: return spdlog::level::warn;
    case Level::Error: return spdlog::level::err;
    case Level::Critical: return spdlog::level::critical;
    default: return spdlog::level::off;
    }
}
inline bool ValidName(const std::string& name) noexcept {
    if (name.empty() || name.size() > 64) return false;
    for (unsigned char c : name)
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
              (c >= '0' && c <= '9') || c == '_' || c == '-')) return false;
    return true;
}
#ifdef _WIN32
inline std::wstring ReadEnvironment(const wchar_t* key) {
    DWORD length = ::GetEnvironmentVariableW(key, nullptr, 0);
    if (!length || length > 32768) return {};
    std::wstring out(length, L'\0');
    DWORD used = ::GetEnvironmentVariableW(key, &out[0], length);
    if (!used || used >= length) return {};
    out.resize(used); return out;
}
inline spdlog::filename_t Directory(Runtime mode) {
    if (mode == Runtime::App || mode == Runtime::AppHost)
        return ReadEnvironment(L"KAVIO_APP_LOG_DIR");
    if (mode == Runtime::KavioWin) {
        const auto appdata = ReadEnvironment(L"APPDATA");
        return appdata.empty() ? std::wstring() : appdata + L"\\Kavio\\log";
    }
    return {};
}
#else
inline spdlog::filename_t Directory(Runtime mode) {
    if (mode == Runtime::App || mode == Runtime::AppHost) {
        const char* value = std::getenv("KAVIO_APP_LOG_DIR");
        return value ? std::string(value) : std::string();
    }
    if (mode == Runtime::Platform) return "./logs";
    return {};
}
#endif
inline spdlog::filename_t Join(const spdlog::filename_t& dir, const std::string& file) {
#ifdef _WIN32
    const std::wstring part(file.begin(), file.end());
    return dir.empty() ? part : (dir.back() == L'\\' || dir.back() == L'/'
        ? dir + part : dir + L"\\" + part);
#else
    return dir.empty() ? file : (dir.back() == '/' ? dir + file : dir + "/" + file);
#endif
}
inline uint32_t ProcessId() noexcept {
#ifdef _WIN32
    return ::GetCurrentProcessId();
#else
    return static_cast<uint32_t>(::getpid());
#endif
}
inline std::string NewSessionId(const std::chrono::system_clock::time_point& now) {
    const auto millis = std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()).count();
    const std::time_t sec = std::chrono::system_clock::to_time_t(now);
    std::tm utc{};
#ifdef _WIN32
    ::gmtime_s(&utc, &sec);
#else
    ::gmtime_r(&sec, &utc);
#endif
    char text[64];
    std::snprintf(text, sizeof(text), "%04d%02d%02d_%02d%02d%02d_%03u_%u",
        utc.tm_year + 1900, utc.tm_mon + 1, utc.tm_mday, utc.tm_hour, utc.tm_min,
        utc.tm_sec, static_cast<unsigned>(millis % 1000), static_cast<unsigned>(ProcessId()));
    return text;
}
inline std::string UtcDay(const std::chrono::system_clock::time_point& now) {
    const std::time_t sec = std::chrono::system_clock::to_time_t(now);
    std::tm utc{};
#ifdef _WIN32
    ::gmtime_s(&utc, &sec);
#else
    ::gmtime_r(&sec, &utc);
#endif
    char day[64];
    std::snprintf(day, sizeof(day), "%04d%02d%02d",
        utc.tm_year + 1900, utc.tm_mon + 1, utc.tm_mday);
    return day;
}
inline bool SessionFilePid(const std::string& file, uint32_t& pid) {
    // Match only files created by this SDK, never other files in the same directory.
    static const std::regex pattern(
        "^[A-Za-z0-9_-]{1,64}_[0-9]{8}_[0-9]{6}_[0-9]{3}_([0-9]+)(?:_[0-9]+)?\\.log$");
    std::smatch match;
    if (!std::regex_match(file, match, pattern)) return false;
    const unsigned long long value = std::strtoull(match[1].str().c_str(), nullptr, 10);
    if (value == 0 || value > 0xffffffffULL) return false;
    pid = static_cast<uint32_t>(value);
    return true;
}
inline void CleanupExpired(const spdlog::filename_t& directory, const std::string& current) noexcept {
    // Age is based on last write. The caller passes the file currently open by this logger.
    // Closed files from previous days are eligible even when the originating PID is alive.
    try {
#ifdef _WIN32
        const auto pattern = Join(directory, "*.log");
        WIN32_FIND_DATAW data{};
        HANDLE find = ::FindFirstFileW(pattern.c_str(), &data);
        if (find == INVALID_HANDLE_VALUE) return;
        FILETIME now{};
        ::GetSystemTimeAsFileTime(&now);
        ULARGE_INTEGER nowticks{};
        nowticks.LowPart = now.dwLowDateTime; nowticks.HighPart = now.dwHighDateTime;
        const unsigned long long maxage = 72ULL * 3600ULL * 10000000ULL;
        do {
            if (data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
            const std::wstring wide(data.cFileName);
            const std::string name(wide.begin(), wide.end()); // SDK filenames are ASCII
            uint32_t pid = 0;
            if (name == current || !SessionFilePid(name, pid)) continue;
            ULARGE_INTEGER modified{};
            modified.LowPart = data.ftLastWriteTime.dwLowDateTime;
            modified.HighPart = data.ftLastWriteTime.dwHighDateTime;
            if (nowticks.QuadPart >= modified.QuadPart &&
                nowticks.QuadPart - modified.QuadPart > maxage)
                ::DeleteFileW(Join(directory, name).c_str());
        } while (::FindNextFileW(find, &data));
        ::FindClose(find);
#else
        DIR* dir = ::opendir(directory.c_str());
        if (!dir) return;
        const std::time_t now = std::time(nullptr);
        while (auto* entry = ::readdir(dir)) {
            const std::string name(entry->d_name);
            uint32_t pid = 0;
            if (name == current || !SessionFilePid(name, pid)) continue;
            const std::string path = Join(directory, name);
            struct stat st{};
            if (::lstat(path.c_str(), &st) != 0 || !S_ISREG(st.st_mode)) continue;
            if (now >= st.st_mtime && now - st.st_mtime > 72 * 3600)
                ::unlink(path.c_str());
        }
        ::closedir(dir);
#endif
    } catch (...) { } // Cleanup must never impact application startup or log calls.
}
struct State {
    std::mutex mutex;
    std::condition_variable cv;
    std::thread worker;
    bool stop = false;
    Runtime mode = Runtime::App;
    spdlog::filename_t directory;
    std::string name;
    std::string session;  // Always the startup ID, unchanged across daily files.
    std::string day;
    std::string filename;
    std::shared_ptr<spdlog::logger> logger;
    ~State() { Close(); }

    // Requires mutex. Open the replacement sink first: a failed open leaves the
    // existing sink usable instead of silently discarding further log messages.
    void OpenFor(const std::chrono::system_clock::time_point& now) {
        const std::string base = NewSessionId(now);
        std::string id = base;
        unsigned collision = 0;
        while (spdlog::details::os::path_exists(Join(directory, name + "_" + id + ".log")))
            id = base + "_" + std::to_string(++collision);
        const std::string next_name = name + "_" + id + ".log";
        const auto sink = std::make_shared<spdlog::sinks::basic_file_sink_mt>(
            Join(directory, next_name), false);
        auto next = std::make_shared<spdlog::logger>(name, sink);
        const auto configured_level = logger ? logger->level() : spdlog::level::info;
        next->set_level(spdlog::level::trace);
        next->flush_on(spdlog::level::err);
        next->set_formatter(std::unique_ptr<spdlog::formatter>(new spdlog::pattern_formatter(
            "[%Y-%m-%dT%H:%M:%S.%eZ] [%P:%t] [%l] [%s:%#] %v",
            spdlog::pattern_time_type::utc)));
        // Every daily segment carries the same startup ID. A future log-query API
        // can reconstruct one launch across multiple UTC calendar days.
        next->info("[KavioLogging] session_id={} file={}",
                   session.empty() ? id : session, next_name);
        next->flush();
        next->set_level(configured_level);
        if (logger) logger->flush();
        logger = std::move(next);
        filename = next_name;
        day = UtcDay(now);
        if (session.empty()) session = id;
    }
    // Requires mutex; invoked on every write so midnight rollover does not
    // depend on the scheduling of the flush/retention worker.
    void CheckDay(const std::chrono::system_clock::time_point& now) {
        if (logger && day != UtcDay(now)) {
            try { OpenFor(now); } catch (...) { /* Keep the existing open sink. */ }
        }
    }
    void StartWorker() {
        stop = false;
        worker = std::thread([this]() {
            std::unique_lock<std::mutex> lock(mutex);
            auto next_cleanup = std::chrono::steady_clock::now() + std::chrono::hours(1);
            while (!stop) {
                cv.wait_for(lock, std::chrono::seconds(1), [this]() { return stop; });
                if (stop) break;
                // Roll at UTC midnight even without an application log write.
                CheckDay(std::chrono::system_clock::now());
                const auto snapshot = logger;
                const bool cleanup = std::chrono::steady_clock::now() >= next_cleanup;
                const auto dir = directory;
                const auto active = filename;
                if (cleanup) next_cleanup = std::chrono::steady_clock::now() + std::chrono::hours(1);
                lock.unlock();
                try { if (snapshot) snapshot->flush(); } catch (...) { }
                if (cleanup) CleanupExpired(dir, active);
                lock.lock();
            }
        });
    }
    void Close() noexcept {
        {
            std::lock_guard<std::mutex> lock(mutex);
            stop = true;
            cv.notify_all();
        }
        if (worker.joinable()) worker.join();
        std::lock_guard<std::mutex> lock(mutex);
        try { if (logger) logger->flush(); } catch (...) { }
        logger.reset();
        session.clear(); filename.clear(); directory.clear(); day.clear(); name.clear();
    }
};
inline State& Global() { static State state; return state; }
} // namespace detail

class Logger final {
public:
    static Result Init(const std::string& name = "main", Level level = Level::Info) {
        return Initialize(name, Runtime::App, level);
    }
    static Result InitSystem(const std::string& name, Runtime runtime, Level level = Level::Info) {
        if (runtime == Runtime::App) return Result::Fail(Error::InvalidName, "Use Init() for Apps");
        return Initialize(name, runtime, level);
    }
    static void SetLevel(Level level) noexcept {
        try {
            auto& s = detail::Global(); std::lock_guard<std::mutex> lock(s.mutex);
            if (s.logger) s.logger->set_level(detail::ToSpdlog(level));
        } catch (...) { }
    }
    static void Flush() noexcept {
        try {
            auto& s = detail::Global(); std::lock_guard<std::mutex> lock(s.mutex);
            if (s.logger) s.logger->flush();
        } catch (...) { }
    }
    static void Shutdown() noexcept { detail::Global().Close(); }
    static std::string SessionId() {
        auto& s = detail::Global(); std::lock_guard<std::mutex> lock(s.mutex);
        return s.session;
    }
    static std::string CurrentFileName() {
        auto& s = detail::Global(); std::lock_guard<std::mutex> lock(s.mutex);
        return s.filename;
    }
    static void WriteRaw(Level level, const char* module, const char* file,
                         uint32_t line, const char* message) noexcept {
        try {
            auto& s = detail::Global(); std::lock_guard<std::mutex> lock(s.mutex);
            if (!s.logger || !message) return;
            s.CheckDay(std::chrono::system_clock::now());
            const spdlog::source_loc loc{file ? file : "", static_cast<int>(line), module ? module : ""};
            s.logger->log(loc, detail::ToSpdlog(level), "[{}] {}", module ? module : "App", message);
        } catch (...) { }
    }
    template<typename... Args>
    static void Write(Level level, const spdlog::source_loc& loc,
                      spdlog::format_string_t<Args...> format, Args&&... args) noexcept {
        try {
            auto& s = detail::Global(); std::lock_guard<std::mutex> lock(s.mutex);
            if (!s.logger) return;
            s.CheckDay(std::chrono::system_clock::now());
            s.logger->log(loc, detail::ToSpdlog(level), format, std::forward<Args>(args)...);
        } catch (...) { }
    }
private:
    static Result Initialize(const std::string& name, Runtime mode, Level level) {
        if (!detail::ValidName(name))
            return Result::Fail(Error::InvalidName, "Invalid log name");
        auto& s = detail::Global();
        std::lock_guard<std::mutex> lock(s.mutex);
        if (s.logger)
            return Result::Fail(Error::AlreadyInitialized, "Logger already initialized");
        const auto directory = detail::Directory(mode);
        if (directory.empty())
            return Result::Fail(Error::MissingLogDirectory, "Runtime log directory unavailable");
        try {
            s.mode = mode;
            s.directory = directory;
            s.name = name;
            s.OpenFor(std::chrono::system_clock::now());
            s.logger->set_level(detail::ToSpdlog(level));
            detail::CleanupExpired(directory, s.filename);
            s.StartWorker();
            return Result::Ok();
        } catch (const std::exception& ex) {
            s.logger.reset(); s.directory.clear(); s.session.clear();
            s.filename.clear(); s.day.clear(); s.name.clear();
            return Result::Fail(Error::OpenFailed, ex.what());
        } catch (...) {
            s.logger.reset(); s.directory.clear(); s.session.clear();
            s.filename.clear(); s.day.clear(); s.name.clear();
            return Result::Fail(Error::OpenFailed, "Unable to initialize logger");
        }
    }
};
}} // namespace kavio::logging

#define KAVIO_LOG_SOURCE spdlog::source_loc{__FILE__, __LINE__, SPDLOG_FUNCTION}
#define KLOGT(...) ::kavio::logging::Logger::Write(::kavio::logging::Level::Trace, KAVIO_LOG_SOURCE, __VA_ARGS__)
#define KLOGD(...) ::kavio::logging::Logger::Write(::kavio::logging::Level::Debug, KAVIO_LOG_SOURCE, __VA_ARGS__)
#define KLOGI(...) ::kavio::logging::Logger::Write(::kavio::logging::Level::Info, KAVIO_LOG_SOURCE, __VA_ARGS__)
#define KLOGW(...) ::kavio::logging::Logger::Write(::kavio::logging::Level::Warn, KAVIO_LOG_SOURCE, __VA_ARGS__)
#define KLOGE(...) ::kavio::logging::Logger::Write(::kavio::logging::Level::Error, KAVIO_LOG_SOURCE, __VA_ARGS__)
#define KLOGC(...) ::kavio::logging::Logger::Write(::kavio::logging::Level::Critical, KAVIO_LOG_SOURCE, __VA_ARGS__)

#ifdef KAVIO_LOGGING_RESTORE_FALSE_MACRO
#pragma pop_macro("false")
#undef KAVIO_LOGGING_RESTORE_FALSE_MACRO
#endif
#ifdef KAVIO_LOGGING_RESTORE_TRUE_MACRO
#pragma pop_macro("true")
#undef KAVIO_LOGGING_RESTORE_TRUE_MACRO
#endif
