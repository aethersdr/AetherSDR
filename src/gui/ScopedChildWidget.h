#pragma once

#include <QPointer>
#include <QWidget>

namespace AetherSDR {

// A transient widget may be destroyed by its parent inside exec()'s nested
// event loop. Keep normal scoped cleanup without putting a Qt-owned child on
// the stack or deleting it twice after parent teardown.
template <typename Widget>
class ScopedChildWidget final {
public:
    explicit ScopedChildWidget(QWidget* parent)
        : m_widget(new Widget(parent))
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
