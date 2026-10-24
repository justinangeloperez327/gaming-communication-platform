#include "ScreenVideoItem.h"
#include <QQuickWindow>
#include <QSGSimpleTextureNode>
#include <QSGTransformNode>
#include <QSGTexture>
#include <QMatrix4x4>
#include <algorithm>
namespace {
struct VideoNode final : QSGTransformNode {
    QSGSimpleTextureNode* quad;
    qint64 serial = 0;
    VideoNode() : quad(new QSGSimpleTextureNode) {
        quad->setOwnsTexture(true); quad->setFiltering(QSGTexture::Linear); appendChildNode(quad);
    }
};
}
ScreenVideoItem::ScreenVideoItem(QQuickItem* parent) : QQuickItem(parent) {
    setFlag(ItemHasContents, true);
    connect(this, &QQuickItem::windowChanged, this, [](QQuickWindow* window) {
        if (window) { window->setPersistentSceneGraph(false); window->setPersistentGraphics(false); }
    });
}
void ScreenVideoItem::setSource(QObject* source) {
    auto* viewer = qobject_cast<ScreenViewer*>(source);
    if (viewer_ == viewer) return;
    disconnect(frameConnection_); viewer_ = viewer;
    if (viewer) frameConnection_ = connect(viewer, &ScreenViewer::frameChanged, this, [this] { update(); });
    update(); emit sourceChanged();
}
void ScreenVideoItem::geometryChange(const QRectF& now, const QRectF& before) { QQuickItem::geometryChange(now, before); update(); }
QSGNode* ScreenVideoItem::updatePaintNode(QSGNode* old, UpdatePaintNodeData*) {
    const auto image = viewer_ ? viewer_->image() : QImage{};
    if (image.isNull() || !window() || width() <= 0 || height() <= 0) { delete old; return nullptr; }
    auto* node = static_cast<VideoNode*>(old);
    if (!node) node = new VideoNode;
    if (node->serial != image.cacheKey()) {
        auto* texture = window()->createTextureFromImage(image);
        if (!texture) { delete node; return nullptr; }
        // setOwnsTexture(true) makes setTexture release the previous texture.
        node->quad->setTexture(texture); node->serial = image.cacheKey();
    }
    const auto rotation = viewer_->rotation(); const bool quarter = rotation == 90 || rotation == 270;
    const auto scale = std::min(width() / (quarter ? image.height() : image.width()), height() / (quarter ? image.width() : image.height()));
    node->quad->setRect(-image.width() / 2.0, -image.height() / 2.0, image.width(), image.height());
    QMatrix4x4 transform; transform.translate(static_cast<float>(width() / 2), static_cast<float>(height() / 2));
    transform.rotate(static_cast<float>(rotation), 0, 0, 1); transform.scale(static_cast<float>(scale)); node->setMatrix(transform);
    return node;
}
