#include "gui_executor.hpp"
#include <utility>

namespace orbelio {
bool QtGuiExecutor::tryPost(Task task, std::size_t) {
    if (!task) {
        return false;
    }
    // This QObject outlives the worker; its destruction drops remaining GUI completions.
    return QMetaObject::invokeMethod(this, [task = std::move(task)] { task(); }, Qt::QueuedConnection);
}
}
