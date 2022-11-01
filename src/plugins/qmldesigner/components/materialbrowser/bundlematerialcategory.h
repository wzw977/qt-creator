/****************************************************************************
**
** Copyright (C) 2022 The Qt Company Ltd.
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

#pragma once

#include <QObject>

namespace QmlDesigner {

class BundleMaterial;

class BundleMaterialCategory : public QObject
{
    Q_OBJECT

    Q_PROPERTY(QString bundleCategoryName MEMBER m_name CONSTANT)
    Q_PROPERTY(bool bundleCategoryVisible MEMBER m_visible NOTIFY categoryVisibleChanged)
    Q_PROPERTY(bool bundleCategoryExpanded MEMBER m_expanded NOTIFY categoryExpandChanged)
    Q_PROPERTY(QList<BundleMaterial *> bundleCategoryMaterials MEMBER m_categoryMaterials
               NOTIFY bundleMaterialsModelChanged)

public:
    BundleMaterialCategory(QObject *parent, const QString &name);

    void addBundleMaterial(BundleMaterial *bundleMat);
    bool updateImportedState(const QStringList &importedMats);
    bool filter(const QString &searchText);

    QString name() const;
    bool visible() const;
    bool expanded() const;
    QList<BundleMaterial *> categoryMaterials() const;

signals:
    void categoryVisibleChanged();
    void categoryExpandChanged();
    void bundleMaterialsModelChanged();

private:
    QString m_name;
    bool m_visible = true;
    bool m_expanded = true;

    QList<BundleMaterial *> m_categoryMaterials;
};

} // namespace QmlDesigner
