#pragma once

#include <orbelio/core/executor.hpp>
#include <QObject>

namespace orbelio {
class QtGuiExecutor final : public QObject, public IExecutor {
public:
    bool tryPost(Task task, std::size_t retainedBytes = 0) override;
};
}
