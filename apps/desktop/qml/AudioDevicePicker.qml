import QtQuick
import QtQuick.Controls

IdPicker {
    property var devices: []
    property string selectedDeviceId: ""
    defaultLabel: "Default device"
    defaultChoice: true
    // A missing saved ID must remain visible rather than impersonating Default.
    options: {
        if (selectedDeviceId.length > 0) {
            for (let item of devices)
                if (item.id === selectedDeviceId) return devices
            return [{ "name": "Saved device unavailable — scan or choose another", "id": selectedDeviceId }].concat(devices)
        }
        return devices
    }
    selectedId: selectedDeviceId
}
