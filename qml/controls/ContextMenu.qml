import QtQuick
import QtQuick.Controls.Basic

Menu {
    id: menu

    property PopupMotion popupMotion: PopupMotion { popup: menu }
    enter: popupMotion.enter
    exit: popupMotion.exit
    implicitWidth: 240
    padding: Theme.spaceXs

    background: Rectangle {
        color: Theme.surface
        border.color: Theme.outline
    }
}
