#include <radray/runtime/game_framework/world.h>

#include <algorithm>

#include <radray/runtime/game_framework/actor.h>

namespace radray {

World::World() = default;

World::World(Application* app)
    : _app(app) {
}

World::~World() noexcept {
    // 销毁所有 Actor(每个都会触发 UnregisterAllComponents)
    while (!_actors.empty()) {
        DestroyActor(_actors.back().get());
    }
}

void World::DestroyActor(Actor* actor) {
    auto it = std::find_if(_actors.begin(), _actors.end(),
                           [actor](const unique_ptr<Actor>& ptr) {
                               return ptr.get() == actor;
                           });
    if (it == _actors.end()) {
        return;
    }
    actor->UnregisterAllComponents();
    actor->OnDestroyed();
    actor->_world = nullptr;
    _actors.erase(it);
}

void World::Tick(float deltaTime) {
    for (auto& actor : _actors) {
        actor->Tick(deltaTime);
    }
}

Actor* World::SpawnActor(unique_ptr<Actor> actor) {
    if (actor == nullptr) {
        return nullptr;
    }
    Actor* raw = actor.get();
    raw->_world = this;
    _actors.push_back(std::move(actor));
    raw->RegisterAllComponents();
    raw->OnSpawned();
    return raw;
}


}  // namespace radray
