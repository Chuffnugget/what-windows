import QtQuick

// Commands and readings share the same route badges. Only commands add the
// execution badge; callers supply permissions, rather than this view granting them.
Row {
    id: badges

    property bool localAllowed: false
    property bool cloudAllowed: false
    property bool showRunAccess: false
    property bool concise: false
    property bool runAllowed: false
    property string runHelp: ""
    spacing: 6

    PermissionBadge {
        text: "LOCAL"
        access: !badges.localAllowed ? PermissionBadge.Blocked
            : badges.showRunAccess ? PermissionBadge.Controls : PermissionBadge.Readings
        help: badges.showRunAccess
            ? (badges.localAllowed ? "Home Assistant can trigger this saved command over local connections." : badges.runHelp)
            : (badges.localAllowed ? "This reading is selected for sharing over local connections."
                                  : "This reading is not shared over local connections.")
    }
    PermissionBadge {
        text: "CLOUD"
        access: badges.cloudAllowed ? PermissionBadge.Cloud : PermissionBadge.Blocked
        help: badges.showRunAccess
            ? (badges.cloudAllowed ? "Home Assistant can trigger this saved command over cloud connections."
                                  : "Cloud execution is blocked. The command needs Home Assistant permission and both its own and its module's cloud permission.")
            : (badges.cloudAllowed ? "This reading is selected for cloud sharing. Its module's cloud permission must also allow sharing."
                                  : "This reading is not shared over cloud connections.")
    }
    PermissionBadge {
        visible: badges.showRunAccess
        text: badges.concise
            ? (badges.runAllowed ? "Can Run" : "Blocked")
            : (badges.runAllowed ? "Home Assistant Can Run" : "Home Assistant Blocked")
        Accessible.name: badges.runAllowed ? "Home Assistant Can Run" : "Home Assistant Blocked"
        access: badges.runAllowed ? PermissionBadge.Controls : PermissionBadge.Blocked
        help: badges.runHelp
    }
}
