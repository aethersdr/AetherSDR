// Socket-free production parser/dispatcher and real Qt item-view coverage (#5503).
#include "TestSettingsProfile.h"
#include "core/AudioEngine.h"
#include "core/QsoRecorder.h"
#include "models/RadioModel.h"
#include "core/AutomationServer.h"

#include <QApplication>
#include <QHelpEvent>
#include <QJsonDocument>
#include <QListWidget>
#include <QPushButton>
#include <QScrollBar>
#include <QStandardItemModel>
#include <QSortFilterProxyModel>
#include <QTableView>
#include <QTableWidget>
#include <QToolTip>
#include <QTreeView>
#include <cstdio>
#include <memory>
#include <functional>

namespace AetherSDR {
class AutomationServerTestAccess
{
public:
    static QJsonObject request(AutomationServer& server, const QByteArray& line)
    {
        return server.handleLine(line, nullptr);
    }
};
}

namespace {
int failures = 0;
void check(bool ok, const char* description)
{
    std::printf("%s %s\n", ok ? "PASS" : "FAIL", description);
    if (!ok) {
        ++failures;
    }
}

class DeletingTipFilter final : public QObject
{
public:
    std::function<void()> destroy;
protected:
    bool eventFilter(QObject*, QEvent* event) override
    {
        if (event->type() == QEvent::ToolTip) {
            destroy();
            return true;
        }
        return false;
    }
};

class TipTable final : public QTableWidget
{
public:
    using QTableWidget::QTableWidget;
    QModelIndex lastTip;
protected:
    bool viewportEvent(QEvent* event) override
    {
        if (event->type() == QEvent::ToolTip) {
            lastTip = indexAt(static_cast<QHelpEvent*>(event)->pos());
        }
        return QTableWidget::viewportEvent(event);
    }
};
}

int main(int argc, char** argv)
{
    TestSettingsProfile profile(QStringLiteral("automation-cell"));
    if (!profile.isValid()) {
        return 1;
    }
    QApplication app(argc, argv);
    AetherSDR::AutomationServer server; // Never start(): no listener or radio.
    const auto request = [&](const QByteArray& line) {
        return AetherSDR::AutomationServerTestAccess::request(server, line);
    };
    const auto error = [&](const QByteArray& line, const QString& fragment) {
        const QJsonObject result = request(line);
        const bool refused = !result.value("ok").toBool()
            && result.value("error").toString().contains(fragment);
        if (!refused) {
            std::printf("Unexpected response: %s\n",
                        QJsonDocument(result).toJson(QJsonDocument::Compact).constData());
        }
        return refused;
    };
    TipTable table(30, 2);
    table.setObjectName(QStringLiteral("cells"));
    table.resize(250, 150);
    for (int row = 0; row < table.rowCount(); ++row) {
        auto* item = new QTableWidgetItem(QStringLiteral("row %1").arg(row));
        item->setToolTip(QStringLiteral("tip %1").arg(row));
        item->setData(Qt::AccessibleTextRole, QStringLiteral("accessible"));
        table.setItem(row, 0, item); // Qt item ownership.
    }
    table.show();
    QApplication::processEvents();
    table.item(0, 0)->setSelected(true);
    server.setReadOnly(true);
    const QJsonObject read = request("cell cells 0 0");
    check(read.value("ok").toBool() && read.value("text") == "row 0"
              && read.value("toolTip") == "tip 0"
              && read.value("accessibleText") == "accessible"
              && read.value("selected").toBool()
              && read.value("rows").toInt() == 30 && read.value("cols").toInt() == 2,
          "cell reads roles, selection and bounds in observe-only mode");
    const int scroll = table.verticalScrollBar()->value();
    check(request(R"({"cmd":"cell","target":"cells","value":"29 0"})").value("text") == "row 29"
              && table.verticalScrollBar()->value() == scroll,
          "JSON cell reads offscreen row without scrolling");
    check(error("tooltip cells cell 29 0", "read-only")
              && table.verticalScrollBar()->value() == scroll,
          "observe-only blocks tooltip before scrolling");
    server.setReadOnly(false);
    check(error("cell cells -1 0", "row -1 out of range"), "negative row refused");
    check(error("cell cells 30 0", "row 30 out of range"), "upper row bound refused");
    check(error("cell cells 0 2", "column 2 out of range"), "upper column bound refused");
    check(error("cell cells a 0", "integer"), "noninteger refused");
    check(error("cell cells 2147483648 0", "integer"), "integer overflow refused");
    check(error("cell cells 0", "integer"), "missing column refused");
    check(error("cell cells 0 0 junk", "integer"), "extra cell token refused");
    check(error("tooltip cells cell 0 0 junk", "exactly"), "extra tooltip token refused");
    check(error("tooltip cells cell 0 1", "no tooltip"), "empty tooltip refused");
    table.setRowHidden(0, true);
    check(error("tooltip cells cell 0 0", "not visible"), "hidden row refused");
    table.setRowHidden(0, false);
    table.setColumnHidden(0, true);
    check(error("tooltip cells cell 0 0", "not visible"), "hidden column refused");
    table.setColumnHidden(0, false);
    const QJsonObject shown = request("tooltip cells cell 29 0");
    check(shown.value("ok").toBool() && table.lastTip.row() == 29
              && QToolTip::text() == QStringLiteral("tip 29"),
          "scrolled tooltip reaches requested viewport cell and displays text");
    table.setColumnWidth(0, 1000);
    const QJsonObject wide = request("tooltip cells cell 29 0");
    check(wide.value("ok").toBool() && table.viewport()->rect().contains(
              table.viewport()->mapFromGlobal(QPoint(wide.value("x").toInt(), wide.value("y").toInt()))),
          "oversized cell tooltip position stays inside viewport");
    QPushButton button;
    button.setObjectName(QStringLiteral("button"));
    button.setToolTip(QStringLiteral("widget tip"));
    button.show();
    QApplication::processEvents();
    check(error("cell button 0 0", "not an item view"), "non-view target refused");
    check(request("tooltip button").value("text") == "widget tip", "widget tooltip preserved");
    check(request(R"({"cmd":"tooltip","target":"button","value":"cell literal"})").value("text") == "cell literal",
          "JSON override preserves literal cell prefix");
    request("tooltip button hide");
    QTableView empty;
    empty.setObjectName(QStringLiteral("empty"));
    check(error("cell empty 0 0", "no model"), "missing model refused");
    QListWidget list;
    list.setObjectName(QStringLiteral("list"));
    list.addItem(QStringLiteral("list item"));
    check(request("cell list 0 0").value("text") == "list item", "list model supported");
    QStandardItemModel model;
    auto* parent = new QStandardItem(QStringLiteral("parent"));
    parent->appendRow(new QStandardItem(QStringLiteral("child")));
    model.appendRow(parent); // Qt model ownership.
    QTreeView tree;
    tree.setObjectName(QStringLiteral("tree"));
    tree.setModel(&model);
    tree.setRootIndex(model.index(0, 0));
    check(request("cell tree 0 0").value("text") == "child",
          "cell is relative to the view displayed root");

    QSortFilterProxyModel proxy;
    QStandardItemModel flat;
    flat.appendRow(new QStandardItem(QStringLiteral("zebra")));
    flat.appendRow(new QStandardItem(QStringLiteral("ant")));
    proxy.setSourceModel(&flat);
    proxy.sort(0);
    QTableView sorted;
    sorted.setObjectName(QStringLiteral("sorted"));
    sorted.setModel(&proxy);
    check(request("cell sorted 0 0").value("text") == "ant", "sorted proxy uses visible row order");
    server.setAuthToken(QStringLiteral("test-token"));
    check(error("cell cells 0 0", "unauthorized"), "cell still requires authentication");
    check(request(R"({"cmd":"cell","args":"cells 0 0","token":"test-token"})").value("text") == "row 0",
          "authenticated positional form reaches same parser");
    server.setAuthToken({});
    table.setColumnWidth(0, 100);
    table.scrollToTop();
    const QMetaObject::Connection reset = QObject::connect(
        table.verticalScrollBar(), &QScrollBar::valueChanged, &table,
        [&table](int) {
            // Keep the same dimensions but replace the item identity. A raw
            // QModelIndex still addresses row 29 after this reset, so merely
            // checking visualRect would show the new tip and report the old one.
            table.clearContents();
            auto* replacement = new QTableWidgetItem(QStringLiteral("replacement"));
            replacement->setToolTip(QStringLiteral("replacement tip"));
            table.setItem(29, 0, replacement);
        });
    check(error("tooltip cells cell 29 0", "changed"), "model reset during scroll is refused");
    QObject::disconnect(reset);

    auto transient = std::make_unique<QTableWidget>(1, 1);
    transient->setObjectName(QStringLiteral("transient"));
    auto* tipItem = new QTableWidgetItem(QStringLiteral("temporary"));
    tipItem->setToolTip(QStringLiteral("temporary tip"));
    transient->setItem(0, 0, tipItem);
    transient->show();
    QApplication::processEvents();
    DeletingTipFilter filter;
    filter.destroy = [&transient] { transient.reset(); };
    transient->viewport()->installEventFilter(&filter);
    check(request("tooltip transient cell 0 0").value("targetDestroyed").toBool()
              && !transient,
          "tooltip event can destroy its target without a dangling access");
    return failures == 0 ? 0 : 1;
}
