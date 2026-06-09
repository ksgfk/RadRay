#include <radray/runtime/game_framework/world.h>

#include <algorithm>

#include <radray/runtime/game_framework/actor.h>
#include <radray/runtime/renderer/scene.h>

namespace radray {

World::World()
    : _scene(make_unique<Scene>()) {}

World::~World() noexcept {
    // Destroy all actors (triggers UnregisterAllComponents for each)
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

void World::AddActorInternal(unique_ptr<Actor> actor) {
    Actor* raw = actor.get();
    raw->_world = this;
    _actors.push_back(std::move(actor));
    raw->RegisterAllComponents();
    raw->OnSpawned();
}

}  // namespace radray
