#pragma once
#include <QQuickItem>
#include <QPointer>
#include "ScreenViewer.h"
// Qt SceneGraph owns render nodes/textures. CPU images stay bounded in the
// controller; texture upload occurs only for a new frame, not a window resize.
class ScreenVideoItem : public QQuickItem {
    Q_OBJECT
    Q_PROPERTY(QObject* source READ source WRITE setSource NOTIFY sourceChanged)
public:
    explicit ScreenVideoItem(QQuickItem* parent = nullptr);
    QObject* source() const { return viewer_; }
    void setSource(QObject* source);
signals:
    void sourceChanged();
protected:
    QSGNode* updatePaintNode(QSGNode* old, UpdatePaintNodeData*) override;
    void geometryChange(const QRectF& now, const QRectF& before) override;
private:
    QPointer<ScreenViewer> viewer_;
    QMetaObject::Connection frameConnection_;
};
