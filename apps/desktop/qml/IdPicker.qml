import QtQuick
import QtQuick.Controls

ComboBox {
    property var options: []
    property string selectedId: ""
    property bool defaultChoice: false
    property string defaultLabel: "Default"
    property string emptyLabel: "Choose an option"

    model: defaultChoice ? [{ "name": defaultLabel, "id": "" }].concat(options) : options
    textRole: "name"
    valueRole: "id"
    displayText: currentIndex < 0 ? emptyLabel : currentText
    currentIndex: {
        for (let index = 0; index < model.length; ++index) {
            if (model[index].id === selectedId)
                return index
        }
        return defaultChoice ? 0 : -1
    }
}
