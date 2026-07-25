import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

ApplicationWindow {
    id: root
    visible: true
    title: "Aether Companion (spike)"

    header: ToolBar {
        RowLayout {
            anchors.fill: parent
            anchors.leftMargin: 16
            Label {
                text: discoveryModel.listening
                      ? "Listening for radios on UDP 4992…"
                      : "Discovery socket failed to bind"
                font.pixelSize: 16
                Layout.fillWidth: true
            }
        }
    }

    ListView {
        anchors.fill: parent
        anchors.margins: 12
        spacing: 8
        model: discoveryModel

        delegate: Frame {
            id: card
            width: ListView.view.width

            required property string radioModel
            required property string nickname
            required property string callsign
            required property string version
            required property string status
            required property string address

            ColumnLayout {
                anchors.fill: parent
                spacing: 2
                Label {
                    text: card.radioModel
                          + (card.nickname ? "  ·  " + card.nickname : "")
                    font.pixelSize: 20
                    font.bold: true
                }
                Label {
                    text: (card.callsign ? card.callsign + "  ·  " : "")
                          + card.address + "  ·  v" + card.version
                    font.pixelSize: 14
                    opacity: 0.7
                }
                Label {
                    text: card.status
                    font.pixelSize: 14
                    color: card.status === "Available" ? "#2e7d32" : "#c62828"
                }
            }
        }

        Label {
            anchors.centerIn: parent
            visible: parent.count === 0
            text: "No radios found yet.\nPhone must be on the same WiFi as the radio."
            horizontalAlignment: Text.AlignHCenter
            opacity: 0.6
            font.pixelSize: 16
        }
    }
}
