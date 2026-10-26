#pragma once
#include <QObject>
#include <QString>

// Local preferences only. No account credentials or media consent belong here.
class DesktopSettings final : public QObject {
    Q_OBJECT
    Q_PROPERTY(bool rememberServer READ rememberServer WRITE setRememberServer NOTIFY changed)
    Q_PROPERTY(QString serverOrigin READ serverOrigin NOTIFY changed)
    Q_PROPERTY(QString status READ status NOTIFY changed)
public:
    explicit DesktopSettings(QString fileName, QObject* parent = nullptr);
    static QString defaultFileName();
    static bool validDeviceId(const QString& id);
    bool rememberServer() const { return rememberServer_; }
    QString serverOrigin() const { return serverOrigin_; }
    QString inputId() const { return inputId_; }
    QString outputId() const { return outputId_; }
    bool pushToTalk() const { return pushToTalk_; }
    QString status() const { return status_; }
    void setRememberServer(bool remember);
    void saveServerOrigin(const QString& origin);
    void saveAudio(const QString& input, const QString& output, bool pushToTalk);
    void reset();
signals:
    void changed();
private:
    void load();
    void save();
    const QString fileName_;
    QString inputId_, outputId_, serverOrigin_, status_;
    bool rememberServer_ = false, pushToTalk_ = false, writable_ = true;
};
