#pragma once
#include <functional>
#include <vector>
#include <mutex>

namespace kmp {

// Command executed on the game/OGRE thread.
// Network thread enqueues these; OnGameTick drains them.
struct GameCommand {
    std::function<void()> execute;

    GameCommand() = default;
    GameCommand(std::function<void()> fn) : execute(std::move(fn)) {}
};

// Thread-safe command queue for marshalling network thread work to game thread.
// Critical: ALL game memory writes and OGRE scene object access must go through this.
class GameCommandQueue {
public:
    GameCommandQueue() = default;
    ~GameCommandQueue() = default;

    // Enqueue command from network thread (or any thread)
    void Push(GameCommand cmd) {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_commands.push_back(std::move(cmd));
    }

    // Drain all commands on game thread
    void DrainAll(const std::function<void(GameCommand&)>& fn) {
        std::vector<GameCommand> local;
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            if (m_commands.empty()) return;
            local.swap(m_commands);
        }

        for (auto& cmd : local) {
            if (cmd.execute) {
                cmd.execute();
            }
        }
    }

    // Get pending command count (for diagnostics)
    size_t Size() const {
        std::lock_guard<std::mutex> lock(m_mutex);
        return m_commands.size();
    }

    // Clear all pending commands (use on disconnect)
    void Clear() {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_commands.clear();
    }

private:
    mutable std::mutex m_mutex;
    std::vector<GameCommand> m_commands;
};

} // namespace kmp
