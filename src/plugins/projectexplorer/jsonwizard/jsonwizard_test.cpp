/****************************************************************************
**
** Copyright (C) 2017 The Qt Company Ltd.
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

#include "jsonwizardfactory.h"

#include <coreplugin/icore.h>

#include <projectexplorer/projectexplorer.h>

#include <QJsonDocument>
#include <QJsonArray>
#include <QJsonObject>
#include <QTest>
#include <QCheckBox>
#include <QLineEdit>
#include <QComboBox>
#include <QListView>

#include <functional>

using namespace Utils;

namespace ProjectExplorer {

static QJsonObject createWidget(const QString &type, const QString &nameSuffix, const QJsonObject &data)
{
    return QJsonObject{
        {"name", QJsonValue(nameSuffix + type)},
        {"type", type},
        {"trDisplayName", QJsonValue(nameSuffix + "DisplayName")},
        {"data", data}
    };
}

static QJsonObject createFieldPageJsonObject(const QJsonArray &widgets)
{
    return QJsonObject{
        {"name", "testpage"},
        {"trDisplayName", "mytestpage"},
        {"typeId", "Fields"},
        {"data", widgets}
    };
}

QJsonObject createGeneralWizard(const QJsonObject &pages)
{
    return QJsonObject {
        {"category", "TestCategory"},
        {"enabled", true},
        {"id", "mytestwizard"},
        {"trDisplayName", "mytest"},
        {"trDisplayCategory", "mytestcategory"},
        {"trDescription", "this is a test wizard"},
        {"generators",
            QJsonObject{
                {"typeId", "File"},
                {"data",
                    QJsonObject{
                        {"source", "myFile.txt"}
                    }
                }
            }
        },
        {"pages", pages}
    };
}

QCheckBox *findCheckBox(Wizard *wizard, const QString &objectName)
{
    return wizard->findChild<QCheckBox *>(objectName + "CheckBox");
}

QLineEdit *findLineEdit(Wizard *wizard, const QString &objectName)
{
    return wizard->findChild<QLineEdit *>(objectName + "LineEdit");
}

QComboBox *findComboBox(Wizard *wizard, const QString &objectName)
{
    return wizard->findChild<QComboBox *>(objectName + "ComboBox");
};

struct FactoryDeleter { void operator()(JsonWizardFactory *f) { f->deleteLater(); } };

using FactoryPtr = std::unique_ptr<JsonWizardFactory, FactoryDeleter>;

void ProjectExplorerPlugin::testJsonWizardsEmptyWizard()
{
    QString errorMessage;
    const QJsonObject wizard = createGeneralWizard(QJsonObject());

    const FactoryPtr factory(JsonWizardFactory::createWizardFactory(wizard.toVariantMap(), {}, &errorMessage));
    QVERIFY(factory == nullptr);
    QCOMPARE(qPrintable(errorMessage), "Page has no typeId set.");
}

void ProjectExplorerPlugin::testJsonWizardsEmptyPage()
{
    QString errorMessage;
    const QJsonObject pages = createFieldPageJsonObject(QJsonArray());
    const QJsonObject wizard = createGeneralWizard(pages);

    const FactoryPtr factory(JsonWizardFactory::createWizardFactory(wizard.toVariantMap(), {}, &errorMessage));
    QVERIFY(factory == nullptr);
    QCOMPARE(qPrintable(errorMessage), "When parsing fields of page \"PE.Wizard.Page.Fields\": ");
}

void ProjectExplorerPlugin::testJsonWizardsUnusedKeyAtFields_data()
{
    const QPair<QString, QJsonValue> wrongData = {"wrong", false};

    QTest::addColumn<QJsonObject>("wrongDataJsonObect");
    QTest::newRow("Label") << QJsonObject({{wrongData, {"trText", "someText"}}});
    QTest::newRow("Spacer") << QJsonObject({wrongData});
    QTest::newRow("LineEdit") << QJsonObject({wrongData});
    QTest::newRow("TextEdit") << QJsonObject({wrongData});
    QTest::newRow("PathChooser") << QJsonObject({wrongData});
    QTest::newRow("CheckBox") << QJsonObject({wrongData});
    QTest::newRow("ComboBox") << QJsonObject({{wrongData, {"items", QJsonArray()}}});
}

void ProjectExplorerPlugin::testJsonWizardsUnusedKeyAtFields()
{
    QString fieldType(QString::fromLatin1(QTest::currentDataTag()));
    QFETCH(QJsonObject, wrongDataJsonObect);
    QString errorMessage;
    const QJsonObject pages = QJsonObject{
        {"name", "testpage"},
        {"trDisplayName", "mytestpage"},
        {"typeId", "Fields"},
        {"data", createWidget(fieldType, "WrongKey", wrongDataJsonObect)},
    };
    const QJsonObject wizard = createGeneralWizard(pages);

    QTest::ignoreMessage(QtWarningMsg, QRegularExpression("has unsupported keys: wrong"));
    const FactoryPtr factory(JsonWizardFactory::createWizardFactory(wizard.toVariantMap(), {}, &errorMessage));
    QVERIFY(factory);
    QVERIFY(errorMessage.isEmpty());
}

void ProjectExplorerPlugin::testJsonWizardsCheckBox()
{
    QString errorMessage;

    QWidget parent;
    const QJsonArray widgets({
        createWidget("CheckBox", "Default", QJsonObject()),
        createWidget("CheckBox", "Checked", QJsonObject({{"checked", true}})),
        createWidget("CheckBox", "UnChecked", QJsonObject({{"checked", false}})),
        createWidget("CheckBox", "SpecialValueUnChecked", QJsonObject(
         {{"checked", false}, {"checkedValue", "SpecialCheckedValue"}, {"uncheckedValue", "SpecialUnCheckedValue"}})
        ),
        createWidget("CheckBox", "SpecialValueChecked", QJsonObject(
         {{"checked", true}, {"checkedValue", "SpecialCheckedValue"}, {"uncheckedValue", "SpecialUnCheckedValue"}})
        )
    });
    const QJsonObject pages = createFieldPageJsonObject(widgets);
    const QJsonObject wizardObject = createGeneralWizard(pages);
    const FactoryPtr factory(JsonWizardFactory::createWizardFactory(wizardObject.toVariantMap(), {}, &errorMessage));
    QVERIFY2(factory, qPrintable(errorMessage));

    Wizard *wizard = factory->runWizard({}, &parent, Id(), QVariantMap());

    QVERIFY(!findCheckBox(wizard, "Default")->isChecked());
    QCOMPARE(wizard->field("DefaultCheckBox"), QVariant(false));

    QVERIFY(findCheckBox(wizard, "Checked")->isChecked());
    QCOMPARE(wizard->field("CheckedCheckBox"), QVariant(true));

    QVERIFY(!findCheckBox(wizard, "UnChecked")->isChecked());
    QCOMPARE(wizard->field("UnCheckedCheckBox"), QVariant(false));

    QVERIFY(!findCheckBox(wizard, "SpecialValueUnChecked")->isChecked());
    QCOMPARE(qPrintable(wizard->field("SpecialValueUnCheckedCheckBox").toString()), "SpecialUnCheckedValue");

    QVERIFY(findCheckBox(wizard, "SpecialValueChecked")->isChecked());
    QCOMPARE(qPrintable(wizard->field("SpecialValueCheckedCheckBox").toString()), "SpecialCheckedValue");
}

void ProjectExplorerPlugin::testJsonWizardsLineEdit()
{
    QString errorMessage;

    QWidget parent;
    const QJsonArray widgets({
         createWidget("LineEdit", "Default", QJsonObject()),
         createWidget("LineEdit", "WithText", QJsonObject({{"trText", "some text"}}))
    });
    const QJsonObject pages = createFieldPageJsonObject(widgets);
    const QJsonObject wizardObject = createGeneralWizard(pages);
    const FactoryPtr factory(JsonWizardFactory::createWizardFactory(wizardObject.toVariantMap(), {}, &errorMessage));
    QVERIFY2(factory, qPrintable(errorMessage));

    Wizard *wizard = factory->runWizard({}, &parent, Id(), QVariantMap());
    QVERIFY(findLineEdit(wizard, "Default"));
    QVERIFY(findLineEdit(wizard, "Default")->text().isEmpty());
    QCOMPARE(qPrintable(findLineEdit(wizard, "WithText")->text()), "some text");

    QVERIFY(!wizard->page(0)->isComplete());
    findLineEdit(wizard, "Default")->setText("enable isComplete");
    QVERIFY(wizard->page(0)->isComplete());
}

void ProjectExplorerPlugin::testJsonWizardsComboBox()
{
    QString errorMessage;
    QWidget parent;

    const QJsonArray items({"abc", "cde", "fgh"});
    QJsonObject disabledComboBoxObject = createWidget("ComboBox", "Disabled", QJsonObject({ {{"disabledIndex", 2}, {"items", items}} }));
    disabledComboBoxObject.insert("enabled", false);
    const QJsonArray widgets({
        createWidget("ComboBox", "Default", QJsonObject({ {{"items", items}} })),
        createWidget("ComboBox", "Index2", QJsonObject({ {{"index", 2}, {"items", items}} })),
        disabledComboBoxObject
    });

    const QJsonObject pages = createFieldPageJsonObject(widgets);
    const QJsonObject wizardObject = createGeneralWizard(pages);
    const FactoryPtr factory(JsonWizardFactory::createWizardFactory(wizardObject.toVariantMap(), {}, &errorMessage));
    QVERIFY2(factory, qPrintable(errorMessage));
    Wizard *wizard = factory->runWizard({}, &parent, Id(), QVariantMap());

    QComboBox *defaultComboBox = findComboBox(wizard, "Default");
    QVERIFY(defaultComboBox);
    QCOMPARE(defaultComboBox->count(), items.count());
    QCOMPARE(qPrintable(defaultComboBox->currentText()), "abc");

    defaultComboBox->setCurrentIndex(2);
    QCOMPARE(qPrintable(defaultComboBox->currentText()), "fgh");

    QComboBox *index2ComboBox = findComboBox(wizard, "Index2");
    QVERIFY(index2ComboBox);
    QCOMPARE(qPrintable(index2ComboBox->currentText()), "fgh");

    QComboBox *disabledComboBox = findComboBox(wizard, "Disabled");
    QVERIFY(disabledComboBox);
    QCOMPARE(qPrintable(disabledComboBox->currentText()), "fgh");
}

static QString iconInsideResource(const QString &relativePathToIcon)
{
    return Core::ICore::resourcePath().resolvePath(relativePathToIcon).toString();
}

void ProjectExplorerPlugin::testJsonWizardsIconList()
{
    QString errorMessage;
    QWidget parent;

    const QJsonArray items({
        QJsonObject{
           {"trKey", "item no1"},
           {"condition", true},
           {"icon", iconInsideResource("templates/wizards/global/lib.png")}
        },
        QJsonObject{
           {"trKey", "item no2"},
           {"condition", false},
           {"icon", "not_existing_path"}

        },
        QJsonObject{
            {"trKey", "item no3"},
            {"condition", true},
            {"trToolTip", "MyToolTip"},
            {"icon", iconInsideResource("templates/wizards/global/lib.png")}
        }
    });

    const QJsonArray widgets({
        createWidget("IconList", "Fancy", QJsonObject{{"index", -1}, {"items", items}})
    });

    const QJsonObject pages = createFieldPageJsonObject(widgets);
    const QJsonObject wizardObject = createGeneralWizard(pages);
    const FactoryPtr factory(JsonWizardFactory::createWizardFactory(wizardObject.toVariantMap(), {}, &errorMessage));
    QVERIFY2(factory, qPrintable(errorMessage));
    Wizard *wizard = factory->runWizard({}, &parent, Id(), QVariantMap());

    auto view = wizard->findChild<QListView *>("FancyIconList");
    QCOMPARE(view->model()->rowCount(), 2);
    QVERIFY(view->model()->index(0,0).data(Qt::DecorationRole).canConvert<QIcon>());
    QIcon icon = view->model()->index(0,0).data(Qt::DecorationRole).value<QIcon>();
    QVERIFY(!icon.isNull());
    QVERIFY(!wizard->page(0)->isComplete());
}

} // ProjectExplorer
