/****************************************************************************
**
** Copyright (C) 2016 Denis Mingulov.
** Copyright (C) 2016 The Qt Company Ltd.
** Contact: https://www.qt.io/licensing/
**
** This file is part of Qt Creator.
**
** Commercial License Usage
** Licensees holding valid commercial Qt licenses may use this file in
** accordance with the commercial license agreement provided with the
** Software or, alternatively, in accordance with the terms contained in
** a written agreement between you and The Qt Company. For licensing terms
** and conditions see https://www.qt.io/terms-conditions. For further
** information use the contact form at https://www.qt.io/contact-us.
**
** GNU General Public License Usage
** Alternatively, this file may be used under the terms of the GNU
** General Public License version 3 as published by the Free Software
** Foundation with exceptions as appearing in the file LICENSE.GPL3-EXCEPT
** included in the packaging of this file. Please review the following
** information to ensure the GNU General Public License requirements will
** be met: https://www.gnu.org/licenses/gpl-3.0.html.
**
****************************************************************************/

#include "imageview.h"

#include "exportdialog.h"
#include "multiexportdialog.h"
#include "imageviewerfile.h"

#include <coreplugin/messagemanager.h>

#include <utils/fileutils.h>
#include <utils/qtcassert.h>

#include <QMessageBox>
#include <QGraphicsRectItem>

#include <QWheelEvent>
#include <QMouseEvent>
#include <QImage>
#include <QPainter>
#include <QPixmap>

#include <QDir>
#include <QFileInfo>

#include <qmath.h>

#ifndef QT_NO_SVG
#include <QGraphicsSvgItem>
#include <QSvgRenderer>
#endif

namespace ImageViewer {
namespace Constants {
    const qreal DEFAULT_SCALE_FACTOR = 1.2;
    const qreal zoomLevels[] = { 0.25, 0.5, 0.75, 1.0, 1.5, 2.0, 4.0, 8.0 };
}

namespace Internal {

static qreal nextLevel(qreal currentLevel)
{
    auto iter = std::find_if(std::begin(Constants::zoomLevels), std::end(Constants::zoomLevels), [&](qreal val) {
        return val > currentLevel;
    });
    if (iter != std::end(Constants::zoomLevels))
        return *iter;
    return currentLevel;
}

static qreal previousLevel(qreal currentLevel)
{
    auto iter = std::find_if(std::rbegin(Constants::zoomLevels), std::rend(Constants::zoomLevels), [&](qreal val) {
        return val < currentLevel;
    });
    if (iter != std::rend(Constants::zoomLevels))
        return *iter;
    return currentLevel;
}

ImageView::ImageView(ImageViewerFile *file)
    : m_file(file)
{
    setScene(new QGraphicsScene(this));
    setTransformationAnchor(AnchorUnderMouse);
    setDragMode(ScrollHandDrag);
    setInteractive(false);
    setViewportUpdateMode(FullViewportUpdate);
    setFrameShape(QFrame::NoFrame);
    setRenderHint(QPainter::SmoothPixmapTransform);

    // Prepare background check-board pattern
    QPixmap tilePixmap(64, 64);
    tilePixmap.fill(Qt::white);
    QPainter tilePainter(&tilePixmap);
    QColor color(220, 220, 220);
    tilePainter.fillRect(0, 0, 0x20, 0x20, color);
    tilePainter.fillRect(0x20, 0x20, 0x20, 0x20, color);
    tilePainter.end();

    setBackgroundBrush(tilePixmap);
}

ImageView::~ImageView() = default;

void ImageView::reset()
{
    scene()->clear();
    resetTransform();
}

void ImageView::createScene()
{
    m_imageItem = m_file->createGraphicsItem();
    if (!m_imageItem) // failed to load
        return;
    m_imageItem->setCacheMode(QGraphicsItem::NoCache);
    m_imageItem->setZValue(0);

    // background item
    m_backgroundItem = new QGraphicsRectItem(m_imageItem->boundingRect());
    m_backgroundItem->setBrush(Qt::white);
    m_backgroundItem->setPen(Qt::NoPen);
    m_backgroundItem->setVisible(m_showBackground);
    m_backgroundItem->setZValue(-1);

    // outline
    m_outlineItem = new QGraphicsRectItem(m_imageItem->boundingRect());
    QPen outline(Qt::black, 1, Qt::DashLine);
    outline.setCosmetic(true);
    m_outlineItem->setPen(outline);
    m_outlineItem->setBrush(Qt::NoBrush);
    m_outlineItem->setVisible(m_showOutline);
    m_outlineItem->setZValue(1);

    QGraphicsScene *s = scene();
    s->addItem(m_backgroundItem);
    s->addItem(m_imageItem);
    s->addItem(m_outlineItem);

    emitScaleFactor();
}

void ImageView::drawBackground(QPainter *p, const QRectF &)
{
    p->save();
    p->resetTransform();
    p->setRenderHint(QPainter::SmoothPixmapTransform, false);
    p->drawTiledPixmap(viewport()->rect(), backgroundBrush().texture());
    p->restore();
}

QImage ImageView::renderSvg(const QSize &imageSize) const
{
    QImage image(imageSize, QImage::Format_ARGB32);
    image.fill(Qt::transparent);
    QPainter painter;
    painter.begin(&image);
#ifndef QT_NO_SVG
    auto svgItem = qgraphicsitem_cast<QGraphicsSvgItem *>(m_imageItem);
    QTC_ASSERT(svgItem, return image);
    svgItem->renderer()->render(&painter, QRectF(QPointF(), QSizeF(imageSize)));
#endif
    painter.end();
    return image;
}

bool ImageView::exportSvg(const ExportData &ed)
{
    const bool result = renderSvg(ed.size).save(ed.fileName);
    if (result) {
        const QString message = tr("Exported \"%1\", %2x%3, %4 bytes")
            .arg(QDir::toNativeSeparators(ed.fileName))
            .arg(ed.size.width()).arg(ed.size.height())
            .arg(QFileInfo(ed.fileName).size());
        Core::MessageManager::writeDisrupting(message);
    } else {
        const QString message = tr("Could not write file \"%1\".").arg(QDir::toNativeSeparators(ed.fileName));
        QMessageBox::critical(this, tr("Export Image"), message);
    }
    return result;
}

#ifndef QT_NO_SVG
static QString suggestedExportFileName(const QFileInfo &fi)
{
    return fi.absolutePath() + QLatin1Char('/') + fi.baseName()
        + QStringLiteral(".png");
}
#endif

QSize ImageView::svgSize() const
{
    QSize result;
#ifndef QT_NO_SVG
    if (const QGraphicsSvgItem *svgItem = qgraphicsitem_cast<QGraphicsSvgItem *>(m_imageItem))
        result = svgItem->boundingRect().size().toSize();
#endif // !QT_NO_SVG
    return result;
}

void ImageView::exportImage()
{
#ifndef QT_NO_SVG
    auto svgItem = qgraphicsitem_cast<QGraphicsSvgItem *>(m_imageItem);
    QTC_ASSERT(svgItem, return);

    const QFileInfo origFi = m_file->filePath().toFileInfo();
    ExportDialog exportDialog(this);
    exportDialog.setWindowTitle(tr("Export %1").arg(origFi.fileName()));
    exportDialog.setExportSize(svgSize());
    exportDialog.setExportFileName(suggestedExportFileName(origFi));

    while (exportDialog.exec() == QDialog::Accepted && !exportSvg(exportDialog.exportData())) {}
#endif // !QT_NO_SVG
}

void ImageView::exportMultiImages()
{
#ifndef QT_NO_SVG
    QTC_ASSERT(qgraphicsitem_cast<QGraphicsSvgItem *>(m_imageItem), return);

    const QFileInfo origFi = m_file->filePath().toFileInfo();
    const QSize size = svgSize();
    const QString title =
        tr("Export a Series of Images from %1 (%2x%3)")
          .arg(origFi.fileName()).arg(size.width()).arg(size.height());
    MultiExportDialog multiExportDialog;
    multiExportDialog.setWindowTitle(title);
    multiExportDialog.setExportFileName(suggestedExportFileName(origFi));
    multiExportDialog.setSvgSize(size);
    multiExportDialog.suggestSizes();

    while (multiExportDialog.exec() == QDialog::Accepted) {
        const auto exportData = multiExportDialog.exportData();
        bool ok = true;
        for (const auto &data : exportData) {
            if (!exportSvg(data)) {
                ok = false;
                break;
            }
        }
        if (ok)
            break;
    }
#endif // !QT_NO_SVG
}

void ImageView::setViewBackground(bool enable)
{
    m_showBackground = enable;
    if (m_backgroundItem)
        m_backgroundItem->setVisible(enable);
}

void ImageView::setViewOutline(bool enable)
{
    m_showOutline = enable;
    if (m_outlineItem)
        m_outlineItem->setVisible(enable);
}

void ImageView::doScale(qreal factor)
{
    scale(factor, factor);
    emitScaleFactor();
    if (auto pixmapItem = dynamic_cast<QGraphicsPixmapItem *>(m_imageItem))
        pixmapItem->setTransformationMode(
                    transform().m11() < 1 ? Qt::SmoothTransformation : Qt::FastTransformation);
}

void ImageView::wheelEvent(QWheelEvent *event)
{
    qreal factor = qPow(Constants::DEFAULT_SCALE_FACTOR, event->angleDelta().y() / 240.0);
    qreal currentScale = transform().m11();
    qreal newScale = currentScale * factor;
    // cap to 0.001 - 1000
    qreal actualFactor = qBound(0.001, factor, 1000.0);
    doScale(actualFactor);
    event->accept();
}

void ImageView::zoomIn()
{
    qreal nextZoomLevel = nextLevel(transform().m11());
    resetTransform();
    doScale(nextZoomLevel);
}

void ImageView::zoomOut()
{
    qreal previousZoomLevel = previousLevel(transform().m11());
    resetTransform();
    doScale(previousZoomLevel);
}

void ImageView::resetToOriginalSize()
{
    resetTransform();
    emitScaleFactor();
}

void ImageView::fitToScreen()
{
    fitInView(m_imageItem, Qt::KeepAspectRatio);
    emitScaleFactor();
}

void ImageView::emitScaleFactor()
{
    // get scale factor directly from the transform matrix
    qreal factor = transform().m11();
    emit scaleFactorChanged(factor);
}

void ImageView::showEvent(QShowEvent *)
{
    m_file->updateVisibility();
}

void ImageView::hideEvent(QHideEvent *)
{
    m_file->updateVisibility();
}

} // namespace Internal
} // namespace ImageView
