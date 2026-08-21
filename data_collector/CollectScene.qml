import QtQuick
import QtQuick.Layouts
import QtQuick.Controls.Basic
import DataCollector

Page {
    id: root
    signal leaveRequested()

    // One bool per clip of the current batch; true = selected = positive.
    property var selections: new Array(Session.clipModel.length).fill(false)

    function toggle(index) {
        const next = root.selections.slice()
        next[index] = !next[index]
        root.selections = next
    }

    function selectedIndices() {
        const out = []
        for (let i = 0; i < root.selections.length; ++i)
            if (root.selections[i])
                out.push(i)
        return out
    }

    Connections {
        target: Session
        function onBatchChanged() {
            root.selections = new Array(Session.clipModel.length).fill(false)
        }
        function onCursorChanged() {
            cursorField.text = Session.cursor.toFixed(1)
        }
    }

    header: ToolBar {
        RowLayout {
            anchors.fill: parent
            anchors.leftMargin: 12
            anchors.rightMargin: 12
            spacing: 8

            Label { text: qsTr("Interval (s)") }
            TextField {
                id: intervalField
                text: Session.samplingInterval.toString()
                implicitWidth: 64
                validator: DoubleValidator { bottom: 0.05; decimals: 3; notation: DoubleValidator.StandardNotation }
                onEditingFinished: if (acceptableInput) Session.samplingInterval = parseFloat(text)
            }

            Label { text: qsTr("Length (s)") }
            TextField {
                id: lengthField
                text: Session.sampleLength.toString()
                implicitWidth: 64
                validator: DoubleValidator { bottom: 0.05; decimals: 3; notation: DoubleValidator.StandardNotation }
                onEditingFinished: if (acceptableInput) Session.sampleLength = parseFloat(text)
            }

            Label { text: qsTr("Size") }
            TextField {
                id: sizeField
                text: Session.sampleSize.toString()
                implicitWidth: 48
                validator: IntValidator { bottom: 1; top: 64 }
                onEditingFinished: if (acceptableInput) Session.sampleSize = parseInt(text)
            }

            Label { text: qsTr("Cursor (s)") }
            TextField {
                id: cursorField
                text: Session.cursor.toFixed(1)
                implicitWidth: 80
                validator: DoubleValidator { bottom: 0; decimals: 3; notation: DoubleValidator.StandardNotation }
                onEditingFinished: if (acceptableInput) Session.cursor = parseFloat(text)
            }

            Button {
                text: qsTr("Resample")
                enabled: !Session.busy
                onClicked: Session.resample()
            }

            Item { Layout.fillWidth: true }

            Label {
                text: qsTr("%1 / %2 s")
                        .arg(Session.cursor.toFixed(0))
                        .arg(Session.durationSeconds.toFixed(0))
                opacity: 0.7
            }
        }
    }

    GridView {
        id: grid
        anchors.fill: parent
        anchors.margins: 10
        visible: !Session.atEnd
        clip: true
        model: Session.clipModel

        readonly property int columns: Math.max(1, Math.floor(width / 348))
        cellWidth: Math.floor(width / columns)
        cellHeight: Math.floor((cellWidth - 12) * 9 / 16) + 40

        delegate: Item {
            id: cell
            required property var modelData
            required property int index

            width: grid.cellWidth
            height: grid.cellHeight

            property int frame: 0
            readonly property bool selected: root.selections[index] === true

            Timer {
                interval: Math.max(15, cell.modelData.frameIntervalMs)
                running: cell.modelData.frameCount > 1 && cell.visible
                repeat: true
                onTriggered: cell.frame = (cell.frame + 1) % cell.modelData.frameCount
            }

            Rectangle {
                anchors.fill: parent
                anchors.margins: 5
                radius: 8
                color: "transparent"
                border.width: cell.selected ? 3 : 1
                border.color: cell.selected ? "#43a047" : "#66888888"

                ColumnLayout {
                    anchors.fill: parent
                    anchors.margins: 6
                    spacing: 2

                    Image {
                        Layout.fillWidth: true
                        Layout.fillHeight: true
                        fillMode: Image.PreserveAspectFit
                        cache: false
                        source: "image://clips/" + Session.epoch + "/" + cell.index + "/" + cell.frame
                    }

                    RowLayout {
                        Layout.fillWidth: true
                        Label {
                            text: qsTr("t = %1 s").arg(cell.modelData.startSeconds.toFixed(2))
                            font.pixelSize: 12
                            opacity: 0.7
                        }
                        Item { Layout.fillWidth: true }
                        Label {
                            text: cell.selected ? qsTr("positive") : qsTr("negative")
                            font.pixelSize: 12
                            font.bold: cell.selected
                            color: cell.selected ? "#43a047" : "#888888"
                        }
                    }
                }

                MouseArea {
                    anchors.fill: parent
                    enabled: !Session.busy
                    onClicked: root.toggle(cell.index)
                }
            }
        }
    }

    // Termination view
    ColumnLayout {
        anchors.centerIn: parent
        visible: Session.atEnd
        spacing: 16

        Label {
            text: qsTr("End of video — all iterations saved.")
            font.pixelSize: 22
            Layout.alignment: Qt.AlignHCenter
        }
        Button {
            text: qsTr("Upload another video")
            highlighted: true
            Layout.alignment: Qt.AlignHCenter
            onClicked: {
                Session.reset()
                root.leaveRequested()
            }
        }
    }

    // Busy overlay
    Rectangle {
        anchors.fill: parent
        visible: Session.busy
        color: "#66000000"

        ColumnLayout {
            anchors.centerIn: parent
            spacing: 8
            BusyIndicator {
                running: Session.busy
                Layout.alignment: Qt.AlignHCenter
            }
            Label {
                text: Session.busyMessage
                color: "white"
                Layout.alignment: Qt.AlignHCenter
            }
        }
    }

    footer: ToolBar {
        RowLayout {
            anchors.fill: parent
            anchors.leftMargin: 12
            anchors.rightMargin: 12
            spacing: 12

            Button {
                text: qsTr("Back")
                enabled: !Session.busy
                onClicked: root.leaveRequested()
            }

            Label {
                text: qsTr("Saving to %1").arg(Session.outputDir)
                elide: Text.ElideMiddle
                opacity: 0.6
                Layout.fillWidth: true
            }

            Label {
                visible: Session.lastError.length > 0
                text: Session.lastError
                color: "#e53935"
                elide: Text.ElideRight
                Layout.maximumWidth: 400
            }

            Label {
                visible: !Session.atEnd
                text: qsTr("%1 positive / %2 clips")
                        .arg(root.selectedIndices().length)
                        .arg(Session.clipModel.length)
            }

            Button {
                text: qsTr("Next")
                highlighted: true
                enabled: !Session.busy && !Session.atEnd && Session.clipModel.length > 0
                onClicked: Session.submitSelection(root.selectedIndices())
            }
        }
    }
}
