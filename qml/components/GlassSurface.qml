import QtQuick
import QtQuick.Effects
import "../controls"

Rectangle {
    // A rectangle shadow avoids capturing and blurring the prompt contents.
    RectangularShadow {
        objectName: "promptShadow"
        anchors.fill: parent
        z: -1
        offset: Qt.vector2d(0, 6)
        blur: 22
        spread: 1
        color: "#70000000"
        cached: true
    }

    border.color: Theme.outlineStrong
    border.width: 1

    gradient: Gradient {
        GradientStop {
            position: 0
            color: Theme.glassTop
        }

        GradientStop {
            position: 0.38
            color: Theme.glassMiddle
        }

        GradientStop {
            position: 1
            color: Theme.glassBottom
        }
    }
}
