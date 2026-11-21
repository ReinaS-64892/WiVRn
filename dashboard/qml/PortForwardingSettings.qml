pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Layouts
import QtQuick.Controls as Controls
import org.kde.kirigami as Kirigami
import io.github.wivrn.wivrn

ColumnLayout {
    id: root
    Layout.fillWidth: true

    function load() {
        rules.clear();
        for (const rule of Settings.portForwarding) {
            rules.append({protocol: rule.protocol, source: rule.source,
                          listenPort: rule["listen-port"], destinationPort: rule["destination-port"],
                          bidirectional: !!rule.bidirectional});
        }
    }

    function sync() {
        let entries = [];
        for (let i = 0; i < rules.count; ++i) {
            const rule = rules.get(i);
            entries.push({protocol: rule.protocol, source: rule.source,
                          "listen-port": rule.listenPort, "destination-port": rule.destinationPort,
                          bidirectional: rule.bidirectional});
        }
        Settings.portForwarding = entries;
    }

    component PortField: Controls.TextField {
        property int value: 1
        signal valueModified()

        implicitWidth: 100
        maximumLength: 5
        selectByMouse: true
        inputMethodHints: Qt.ImhDigitsOnly
        horizontalAlignment: TextInput.AlignRight
        text: value.toString()
        validator: IntValidator { bottom: 1; top: 65535 }

        onTextEdited: {
            const port = Number(text);
            if (acceptableInput && Number.isInteger(port) && port >= 1 && port <= 65535) {
                value = port;
                valueModified();
            }
        }
        onValueChanged: text = value.toString()
        onActiveFocusChanged: {
            if (!activeFocus)
                text = value.toString();
        }
        onEditingFinished: text = value.toString()
    }

    ListModel { id: rules }

    Kirigami.Heading {
        text: i18n("Port forwarding")
        level: 2
    }
    Controls.Label {
        Layout.fillWidth: true
        wrapMode: Text.WordWrap
        text: i18n("Forward local application traffic between the connected headset and PC. Changes take effect after reconnecting the headset. Application ports are accessible only through localhost.")
    }
    Repeater {
        model: rules
        delegate: Controls.Frame {
            id: entry
            required property int index
            required property string protocol
            required property string source
            required property int listenPort
            required property int destinationPort
            required property bool bidirectional
            Layout.fillWidth: true

            ColumnLayout {
                anchors.fill: parent
                RowLayout {
                    Controls.ComboBox {
                        model: ["UDP", "TCP"]
                        currentIndex: entry.protocol === "tcp" ? 1 : 0
                        onActivated: { rules.setProperty(entry.index, "protocol", currentIndex === 1 ? "tcp" : "udp"); root.sync(); }
                    }
                    Controls.ComboBox {
                        Layout.fillWidth: true
                        model: [i18n("Headset → PC"), i18n("PC → Headset")]
                        currentIndex: entry.source === "pc" ? 1 : 0
                        onActivated: { rules.setProperty(entry.index, "source", currentIndex === 1 ? "pc" : "headset"); root.sync(); }
                    }
                    Controls.ToolButton {
                        icon.name: "edit-delete"
                        text: i18n("Remove rule")
                        display: Controls.AbstractButton.IconOnly
                        Controls.ToolTip.text: text
                        Controls.ToolTip.visible: hovered
                        onClicked: { rules.remove(entry.index); root.sync(); }
                        Accessible.name: text
                    }
                }
                RowLayout {
                    Controls.Label { text: i18n("Listen:") }
                    PortField {
                        value: entry.listenPort
                        onValueModified: { rules.setProperty(entry.index, "listenPort", value); root.sync(); }
                    }
                    Controls.Label { text: i18n("Destination:") }
                    PortField {
                        value: entry.destinationPort
                        onValueModified: { rules.setProperty(entry.index, "destinationPort", value); root.sync(); }
                    }
                }
                Controls.CheckBox {
                    text: i18n("Bidirectional (also return replies)")
                    checked: entry.bidirectional
                    onToggled: { rules.setProperty(entry.index, "bidirectional", checked); root.sync(); }
                }
            }
        }
    }

    Controls.Button {
        text: i18n("Add forwarding rule")
        icon.name: "list-add"
        enabled: rules.count < 64
        onClicked: {
            rules.append({protocol: "udp", source: "headset", listenPort: 9000, destinationPort: 9000, bidirectional: false});
            root.sync();
        }
    }
    Kirigami.InlineMessage {
        Layout.fillWidth: true
        visible: Settings.portForwardingError.length > 0
        text: Settings.portForwardingError
        type: Kirigami.MessageType.Error
    }
}
