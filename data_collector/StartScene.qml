import QtQuick
import QtQuick.Layouts
import QtQuick.Controls.Basic
import QtQuick.Dialogs
import DataCollector

Page {
    id: root
    signal started()

    FileDialog {
        id: fileDialog
        title: qsTr("Choose a video")
        nameFilters: [
            qsTr("Videos (*.mp4 *.mov *.mkv *.avi *.webm *.m4v *.mpg *.mpeg)"),
            qsTr("All files (*)")
        ]
        onAccepted: Session.loadVideo(selectedFile)
    }

    // Session may restore parameters from a previous run's log after a video
    // is loaded — refresh the fields then.
    Connections {
        target: Session
        function onVideoInfoChanged() {
            intervalField.text = Session.samplingInterval.toString()
            lengthField.text = Session.sampleLength.toString()
            sizeField.text = Session.sampleSize.toString()
        }
    }

    ColumnLayout {
        anchors.centerIn: parent
        width: Math.min(parent.width - 64, 540)
        spacing: 14

        Label {
            text: qsTr("Data Collector")
            font.pixelSize: 30
            font.bold: true
            Layout.alignment: Qt.AlignHCenter
        }

        Label {
            text: qsTr("Upload a video, then label sampled clips as positive/negative.")
            opacity: 0.7
            Layout.alignment: Qt.AlignHCenter
        }

        Button {
            text: qsTr("Choose Video…")
            enabled: !Session.busy
            Layout.alignment: Qt.AlignHCenter
            Layout.topMargin: 8
            onClicked: fileDialog.open()
        }

        ColumnLayout {
            visible: Session.videoLoaded
            spacing: 4
            Layout.fillWidth: true

            Label {
                text: Session.videoPath
                elide: Text.ElideMiddle
                horizontalAlignment: Text.AlignHCenter
                Layout.fillWidth: true
            }
            Label {
                text: qsTr("MD5: %1   ·   Duration: %2 s")
                        .arg(Session.md5)
                        .arg(Session.durationSeconds.toFixed(1))
                opacity: 0.7
                horizontalAlignment: Text.AlignHCenter
                Layout.fillWidth: true
            }
            Label {
                visible: Session.hasPreviousProgress
                text: qsTr("Previous progress found — resuming at %1 s")
                        .arg(Session.cursor.toFixed(1))
                color: "#43a047"
                horizontalAlignment: Text.AlignHCenter
                Layout.fillWidth: true
            }
        }

        GridLayout {
            columns: 2
            columnSpacing: 12
            rowSpacing: 8
            Layout.fillWidth: true
            Layout.topMargin: 8

            Label { text: qsTr("Sampling Interval (s)") }
            TextField {
                id: intervalField
                text: "5"
                validator: DoubleValidator { bottom: 0.05; decimals: 3; notation: DoubleValidator.StandardNotation }
                Layout.fillWidth: true
            }

            Label { text: qsTr("Sample Length (s)") }
            TextField {
                id: lengthField
                text: "1"
                validator: DoubleValidator { bottom: 0.05; decimals: 3; notation: DoubleValidator.StandardNotation }
                Layout.fillWidth: true
            }

            Label { text: qsTr("Sample Size") }
            TextField {
                id: sizeField
                text: "12"
                validator: IntValidator { bottom: 1; top: 64 }
                Layout.fillWidth: true
            }
        }

        RowLayout {
            visible: Session.busy
            spacing: 8
            Layout.alignment: Qt.AlignHCenter
            BusyIndicator {
                running: Session.busy
                implicitWidth: 28
                implicitHeight: 28
            }
            Label { text: Session.busyMessage }
        }

        Label {
            visible: Session.lastError.length > 0
            text: Session.lastError
            color: "#e53935"
            wrapMode: Text.Wrap
            Layout.fillWidth: true
            horizontalAlignment: Text.AlignHCenter
        }

        Button {
            text: Session.hasPreviousProgress ? qsTr("Resume") : qsTr("Start")
            highlighted: true
            enabled: Session.videoLoaded && !Session.busy
                     && intervalField.acceptableInput
                     && lengthField.acceptableInput
                     && sizeField.acceptableInput
            Layout.alignment: Qt.AlignHCenter
            Layout.topMargin: 8
            onClicked: {
                Session.samplingInterval = parseFloat(intervalField.text)
                Session.sampleLength = parseFloat(lengthField.text)
                Session.sampleSize = parseInt(sizeField.text)
                Session.startCollecting()
                root.started()
            }
        }
    }
}
