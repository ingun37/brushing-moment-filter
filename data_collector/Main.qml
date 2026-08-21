import QtQuick
import QtQuick.Controls.Basic

ApplicationWindow {
    id: window
    width: 1160
    height: 800
    minimumWidth: 760
    minimumHeight: 560
    visible: true
    title: qsTr("Data Collector")

    StackView {
        id: stack
        anchors.fill: parent
        initialItem: startComponent
    }

    Component {
        id: startComponent
        StartScene {
            onStarted: stack.push(collectComponent)
        }
    }

    Component {
        id: collectComponent
        CollectScene {
            onLeaveRequested: stack.pop()
        }
    }
}
