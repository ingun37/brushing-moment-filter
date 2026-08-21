#pragma once

#include "clips.h"

#include <memory>
#include <mutex>
#include <vector>

// Immutable current batch of clips, shared between the Session (writer, GUI
// thread) and the QML image provider (reader, possibly another thread).
class ClipStore
{
public:
    using Batch = std::shared_ptr<const std::vector<clips::Clip>>;

    void set(Batch batch)
    {
        const std::scoped_lock lock(mutex_);
        batch_ = std::move(batch);
    }

    Batch get() const
    {
        const std::scoped_lock lock(mutex_);
        return batch_;
    }

private:
    mutable std::mutex mutex_;
    Batch batch_;
};
