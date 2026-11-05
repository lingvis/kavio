extern "C" {
#include <lua.h>
#include <lauxlib.h>
#include <lualib.h>
}

#include <filesystem>
#include <iostream>
#include <string>

namespace {

std::filesystem::path ExecutableDirectory(const char* argv0)
{
    std::error_code ec;
    const auto self = std::filesystem::read_symlink("/proc/self/exe", ec);
    if (!ec && !self.empty()) return self.parent_path();
    auto path = std::filesystem::absolute(argv0 ? argv0 : "kavio_lua_runner", ec);
    if (ec) return std::filesystem::current_path();
    return path.parent_path();
}

void PrependPackageField(lua_State* L, const char* field, const std::string& prefix)
{
    lua_getglobal(L, "package");
    lua_getfield(L, -1, field);
    const char* current = lua_tostring(L, -1);
    const std::string value = prefix + (current && *current ? ";" + std::string(current) : std::string());
    lua_pop(L, 1);
    lua_pushlstring(L, value.data(), value.size());
    lua_setfield(L, -2, field);
    lua_pop(L, 1);
}

void SetArgTable(lua_State* L, int argc, char** argv, int script_index, const std::filesystem::path& script)
{
    const int argument_count = argc > script_index + 1 ? argc - script_index - 1 : 0;
    lua_createtable(L, argument_count, 1);
    lua_pushstring(L, script.string().c_str());
    lua_rawseti(L, -2, 0);
    for (int i = script_index + 1; i < argc; ++i) {
        lua_pushstring(L, argv[i]);
        lua_rawseti(L, -2, i - script_index);
    }
    lua_setglobal(L, "arg");
}

}  // namespace

int main(int argc, char** argv)
{
    const bool check_only = argc >= 2 && std::string(argv[1]) == "--check";
    const int script_index = check_only ? 2 : 1;
    if (argc <= script_index) {
        std::cerr << "kavio_lua_runner: missing Lua entry script\n";
        return 2;
    }

    const auto base = ExecutableDirectory(argv[0]);
    std::filesystem::path script = argv[script_index];
    if (script.is_relative()) script = base / script;

    lua_State* L = luaL_newstate();
    if (!L) {
        std::cerr << "kavio_lua_runner: failed to create Lua state\n";
        return 3;
    }
    luaL_openlibs(L);

    const std::string base_string = base.string();
    PrependPackageField(L, "path", base_string + "/?.lua;" + base_string + "/?/init.lua");
    PrependPackageField(L, "cpath", base_string + "/?.so;" + base_string + "/?/?.so");
    SetArgTable(L, argc, argv, script_index, script);

    int result = 0;
    if (luaL_loadfile(L, script.string().c_str()) != LUA_OK) {
        const char* message = lua_tostring(L, -1);
        std::cerr << "kavio_lua_runner: " << (message ? message : "load failed") << "\n";
        result = 4;
    } else if (!check_only && lua_pcall(L, 0, LUA_MULTRET, 0) != LUA_OK) {
        const char* message = lua_tostring(L, -1);
        std::cerr << "kavio_lua_runner: " << (message ? message : "runtime error") << "\n";
        result = 5;
    }

    lua_close(L);
    return result;
}
