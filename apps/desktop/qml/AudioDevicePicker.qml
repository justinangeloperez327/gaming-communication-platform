import QtQuick
import QtQuick.Controls

IdPicker {
    property var devices: []
    property string selectedDeviceId: ""
    defaultLabel: "Default device"
    defaultChoice: true
    options: devices
    selectedId: selectedDeviceId
}
