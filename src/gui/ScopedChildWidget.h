#pragma once

#include <QPointer>
#include <QWidget>
#include <utility>

namespace AetherSDR {

// A transient widget may be destroyed by its parent inside exec()'s nested
// event loop. Keep normal scoped cleanup without putting a Qt-owned child on
// the stack or deleting it twice after parent teardown.
template <typename Widget>
class ScopedChildWidget final {
public:
    template <typename... Args>
    explicit ScopedChildWidget(Args&&... args)
        : m_widget(new Widget(std::forward<Args>(args)...))
    {
    }

    ~ScopedChildWidget()
    {
        // QPointer is cleared if the parent already destroyed the widget.
        delete m_widget.data();
    }

    ScopedChildWidget(const ScopedChildWidget&) = delete;
    ScopedChildWidget& operator=(const ScopedChildWidget&) = delete;

    Widget* get() const { return m_widget.data(); }
    explicit operator bool() const { return !m_widget.isNull(); }

private:
    QPointer<Widget> m_widget;
};

} // namespace AetherSDR
