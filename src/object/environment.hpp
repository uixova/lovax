#ifndef ENVIRONMENT_HPP
#define ENVIRONMENT_HPP

#include <unordered_map>
#include <string>
#include <memory>
#include "object.hpp"

namespace Lume {

class Environment {
private:
    std::unordered_map<std::string, Ref<Object>> store;
    std::shared_ptr<Environment> outer; // Outer scope (e.g. the global environment)

public:
    // Default constructor for the global environment
    Environment() : outer(nullptr) {}

    // Constructor for a local environment (new scope)
    Environment(std::shared_ptr<Environment> outerEnv) : outer(outerEnv) {}

    // Finds the variable; returns nullptr if missing (evaluator raises the error with line info)
    Ref<Object> get(const std::string& name) {
        auto it = store.find(name);
        if (it != store.end()) {
            return it->second;
        }
        if (outer != nullptr) {
            return outer->get(name);
        }
        return nullptr;
    }

    // 'set' semantics (RFC-001): defines/overwrites in the CURRENT scope
    Ref<Object> define(const std::string& name, Ref<Object> val) {
        store[name] = val;
        return val;
    }

    // Module export: exposes everything defined in this scope (for file modules)
    const std::unordered_map<std::string, Ref<Object>>& entries() const {
        return store;
    }

    // Bare assignment semantics (RFC-001): walks the scope chain for the variable,
    // updates it where found. Returns false if not found (evaluator raises the error).
    // This also lets closures update counters in outer scopes.
    bool assign(const std::string& name, Ref<Object> val) {
        auto it = store.find(name);
        if (it != store.end()) {
            it->second = val;
            return true;
        }
        if (outer != nullptr) {
            return outer->assign(name, val);
        }
        return false;
    }
};

} // namespace Lume

#endif // ENVIRONMENT_HPP
