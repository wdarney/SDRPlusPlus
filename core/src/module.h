#pragma once
#include <string>
#include <map>
#include <json.hpp>
#include <utils/event.h>

#ifdef _WIN32
#ifdef SDRPP_IS_CORE
#define SDRPP_EXPORT extern "C" __declspec(dllexport)
#else
#define SDRPP_EXPORT extern "C" __declspec(dllimport)
#endif
#else
#define SDRPP_EXPORT extern
#endif

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
#define MOD_EXPORT           extern "C" __declspec(dllexport)
#define SDRPP_MOD_EXTENTSION ".dll"
#else
#include <dlfcn.h>
#define MOD_EXPORT extern "C"
#ifdef __APPLE__
#define SDRPP_MOD_EXTENTSION ".dylib"
#else
#define SDRPP_MOD_EXTENTSION ".so"
#endif
#endif

// Statically linked module builds can define SDRPP_MODULE_TOKEN=<name>_ to avoid
// entry-point collisions. Desktop dynamic modules leave the exported names
// unscoped for dlsym/GetProcAddress compatibility.
#ifdef SDRPP_MODULE_TOKEN
#define _SDRPP_TOK_CAT2(a, b) a##b
#define _SDRPP_TOK_CAT(a, b)  _SDRPP_TOK_CAT2(a, b)
#define _INIT_                _SDRPP_TOK_CAT(SDRPP_MODULE_TOKEN, _INIT_)
#define _CREATE_INSTANCE_     _SDRPP_TOK_CAT(SDRPP_MODULE_TOKEN, _CREATE_INSTANCE_)
#define _DELETE_INSTANCE_     _SDRPP_TOK_CAT(SDRPP_MODULE_TOKEN, _DELETE_INSTANCE_)
#define _END_                 _SDRPP_TOK_CAT(SDRPP_MODULE_TOKEN, _END_)
#define _INFO_                _SDRPP_TOK_CAT(SDRPP_MODULE_TOKEN, _INFO_)
#endif

class ModuleManager {
public:
    struct ModuleInfo_t {
        const char* name;
        const char* description;
        const char* author;
        const int versionMajor;
        const int versionMinor;
        const int versionBuild;
        const int maxInstances;
    };

    class Instance {
    public:
        virtual ~Instance() {}
        virtual void postInit() = 0;
        virtual void enable() = 0;
        virtual void disable() = 0;
        virtual bool isEnabled() = 0;
    };

    struct Module_t {
#ifdef _WIN32
        HMODULE handle;
#else
        void* handle;
#endif
        ModuleManager::ModuleInfo_t* info;
        void (*init)();
        ModuleManager::Instance* (*createInstance)(std::string name);
        void (*deleteInstance)(ModuleManager::Instance* instance);
        void (*end)();

        friend bool operator==(const Module_t& a, const Module_t& b) {
            if (a.handle != b.handle) { return false; }
            if (a.info != b.info) { return false; }
            if (a.init != b.init) { return false; }
            if (a.createInstance != b.createInstance) { return false; }
            if (a.deleteInstance != b.deleteInstance) { return false; }
            if (a.end != b.end) { return false; }
            return true;
        }
    };

    struct Instance_t {
        ModuleManager::Module_t module;
        ModuleManager::Instance* instance;
    };

    // Resolve a module by name (the ".dylib" suffix is tolerated for backward
    // compatibility with config files). The module must already be registered
    // via registerStatic() — see core/src/static_modules.cpp (generated).
    ModuleManager::Module_t loadModule(std::string path);

    ModuleManager::Module_t registerStatic(
        ModuleInfo_t* info,
        void (*init)(),
        Instance* (*createInstance)(std::string),
        void (*deleteInstance)(Instance*),
        void (*end)()
    );

    int createInstance(std::string name, std::string module);
    int deleteInstance(std::string name);
    int deleteInstance(ModuleManager::Instance* instance);

    int enableInstance(std::string name);
    int disableInstance(std::string name);
    bool instanceEnabled(std::string name);
    void postInit(std::string name);
    std::string getInstanceModuleName(std::string name);

    int countModuleInstances(std::string module);

    void doPostInitAll();

    Event<std::string> onInstanceCreated;
    Event<std::string> onInstanceDelete;
    Event<std::string> onInstanceDeleted;

    std::map<std::string, ModuleManager::Module_t> modules;
    std::map<std::string, ModuleManager::Instance_t> instances;
};

#define SDRPP_MOD_INFO MOD_EXPORT const ModuleManager::ModuleInfo_t _INFO_
