#include <module.h>
#include <filesystem>
#include <utils/flog.h>

ModuleManager::Module_t ModuleManager::registerStatic(
    ModuleManager::ModuleInfo_t* info,
    void (*init)(),
    ModuleManager::Instance* (*createInstance)(std::string),
    void (*deleteInstance)(ModuleManager::Instance*),
    void (*end)()
) {
    Module_t mod;
    mod.info = info;
    mod.init = init;
    mod.createInstance = createInstance;
    mod.deleteInstance = deleteInstance;
    mod.end = end;
    // Use the info pointer as a stable identity — satisfies the handle-equality
    // check in operator== without involving dlopen.
    mod.handle = reinterpret_cast<decltype(mod.handle)>(info);

    if (info == nullptr || info->name == nullptr) {
        flog::error("registerStatic called with null info");
        mod.handle = NULL;
        return mod;
    }
    if (modules.find(info->name) != modules.end()) {
        flog::warn("Static module '{0}' already registered", info->name);
        return modules[info->name];
    }
    init();
    modules[info->name] = mod;
    return mod;
}

ModuleManager::Module_t ModuleManager::loadModule(std::string path) {
#ifdef SDRPP_STATIC_MODULES
    // iOS: every module is statically linked. The "path" coming from config is
    // a base name like "network_source.dylib" — strip the extension and look
    // it up in the registry that registerStaticModules() populated at startup.
    Module_t mod{};
    std::string name = std::filesystem::path(path).stem().string();
    auto it = modules.find(name);
    if (it == modules.end()) {
        flog::error("Static module '{0}' not registered", name);
        mod.handle = NULL;
        return mod;
    }
    return it->second;
#else
    Module_t mod;

    // On Android, the path has to be relative; do not make it absolute.
#ifndef __ANDROID__
    if (!std::filesystem::exists(path)) {
        flog::error("{0} does not exist", path);
        mod.handle = NULL;
        return mod;
    }
    if (!std::filesystem::is_regular_file(path)) {
        flog::error("{0} isn't a loadable module", path);
        mod.handle = NULL;
        return mod;
    }
#endif
#ifdef _WIN32
    mod.handle = LoadLibraryA(path.c_str());
    if (mod.handle == NULL) {
        flog::error("Couldn't load {0}. Error code: {1}", path, (int)GetLastError());
        return mod;
    }
    mod.info = (ModuleInfo_t*)GetProcAddress(mod.handle, "_INFO_");
    mod.init = (void (*)())GetProcAddress(mod.handle, "_INIT_");
    mod.createInstance = (Instance * (*)(std::string)) GetProcAddress(mod.handle, "_CREATE_INSTANCE_");
    mod.deleteInstance = (void (*)(Instance*))GetProcAddress(mod.handle, "_DELETE_INSTANCE_");
    mod.end = (void (*)())GetProcAddress(mod.handle, "_END_");
#else
    mod.handle = dlopen(path.c_str(), RTLD_LAZY | RTLD_LOCAL);
    if (mod.handle == NULL) {
        flog::error("Couldn't load {0}: {1}", path, dlerror());
        return mod;
    }
    mod.info = (ModuleInfo_t*)dlsym(mod.handle, "_INFO_");
    mod.init = (void (*)())dlsym(mod.handle, "_INIT_");
    mod.createInstance = (Instance * (*)(std::string)) dlsym(mod.handle, "_CREATE_INSTANCE_");
    mod.deleteInstance = (void (*)(Instance*))dlsym(mod.handle, "_DELETE_INSTANCE_");
    mod.end = (void (*)())dlsym(mod.handle, "_END_");
#endif
    if (mod.info == NULL) {
        flog::error("{0} is missing _INFO_ symbol", path);
        mod.handle = NULL;
        return mod;
    }
    if (mod.init == NULL) {
        flog::error("{0} is missing _INIT_ symbol", path);
        mod.handle = NULL;
        return mod;
    }
    if (mod.createInstance == NULL) {
        flog::error("{0} is missing _CREATE_INSTANCE_ symbol", path);
        mod.handle = NULL;
        return mod;
    }
    if (mod.deleteInstance == NULL) {
        flog::error("{0} is missing _DELETE_INSTANCE_ symbol", path);
        mod.handle = NULL;
        return mod;
    }
    if (mod.end == NULL) {
        flog::error("{0} is missing _END_ symbol", path);
        mod.handle = NULL;
        return mod;
    }
    if (modules.find(mod.info->name) != modules.end()) {
        flog::error("{0} has the same name as an already loaded module", path);
        mod.handle = NULL;
        return mod;
    }
    for (auto const& [name, _mod] : modules) {
        if (mod.handle == _mod.handle) { return _mod; }
    }
    mod.init();
    modules[mod.info->name] = mod;
    return mod;
#endif
}

int ModuleManager::createInstance(std::string name, std::string module) {
    if (modules.find(module) == modules.end()) {
        flog::error("Module '{0}' doesn't exist", module);
        return -1;
    }
    if (instances.find(name) != instances.end()) {
        flog::error("A module instance with the name '{0}' already exists", name);
        return -1;
    }
    int maxCount = modules[module].info->maxInstances;
    if (countModuleInstances(module) >= maxCount && maxCount > 0) {
        flog::error("Maximum number of instances reached for '{0}'", module);
        return -1;
    }
    Instance_t inst;
    inst.module = modules[module];
    inst.instance = inst.module.createInstance(name);
    instances[name] = inst;
    onInstanceCreated.emit(name);
    return 0;
}

int ModuleManager::deleteInstance(std::string name) {
    if (instances.find(name) == instances.end()) {
        flog::error("Tried to remove non-existent instance '{0}'", name);
        return -1;
    }
    onInstanceDelete.emit(name);
    Instance_t inst = instances[name];
    inst.module.deleteInstance(inst.instance);
    instances.erase(name);
    onInstanceDeleted.emit(name);
    return 0;
}

int ModuleManager::deleteInstance(ModuleManager::Instance* instance) {
    flog::error("Delete instance not implemented");
    return -1;
}

int ModuleManager::enableInstance(std::string name) {
    if (instances.find(name) == instances.end()) {
        flog::error("Cannot enable '{0}', instance doesn't exist", name);
        return -1;
    }
    instances[name].instance->enable();
    return 0;
}

int ModuleManager::disableInstance(std::string name) {
    if (instances.find(name) == instances.end()) {
        flog::error("Cannot disable '{0}', instance doesn't exist", name);
        return -1;
    }
    instances[name].instance->disable();
    return 0;
}

bool ModuleManager::instanceEnabled(std::string name) {
    if (instances.find(name) == instances.end()) {
        flog::error("Cannot check if '{0}' is enabled, instance doesn't exist", name);
        return false;
    }
    return instances[name].instance->isEnabled();
}

void ModuleManager::postInit(std::string name) {
    if (instances.find(name) == instances.end()) {
        flog::error("Cannot post-init '{0}', instance doesn't exist", name);
        return;
    }
    instances[name].instance->postInit();
}

std::string ModuleManager::getInstanceModuleName(std::string name) {
    if (instances.find(name) == instances.end()) {
        flog::error("Cannot get module name of'{0}', instance doesn't exist", name);
        return "";
    }
    return std::string(instances[name].module.info->name);
}

int ModuleManager::countModuleInstances(std::string module) {
    if (modules.find(module) == modules.end()) {
        flog::error("Cannot count instances of '{0}', Module doesn't exist", module);
        return -1;
    }
    ModuleManager::Module_t mod = modules[module];
    int count = 0;
    for (auto const& [name, instance] : instances) {
        if (instance.module == mod) { count++; }
    }
    return count;
}

void ModuleManager::doPostInitAll() {
    for (auto& [name, inst] : instances) {
        flog::info("Running post-init for {0}", name);
        inst.instance->postInit();
    }
}
