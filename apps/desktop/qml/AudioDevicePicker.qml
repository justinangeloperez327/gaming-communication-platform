import QtQuick
import QtQuick.Controls

ComboBox {
    property var devices: []
    property string selectedDeviceId: ""
    property string defaultLabel: "Default device"

    model: [{ "name": defaultLabel, "id": "" }].concat(devices)
    textRole: "name"
    valueRole: "id"
    currentIndex: {
        for (let index = 0; index < model.length; ++index) {
            if (model[index].id === selectedDeviceId)
                return index
        }
        return 0
    }
}
