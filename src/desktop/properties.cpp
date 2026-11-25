#include "window.hpp"
#include "jobs.hpp"
#include <QtWidgets>
#include <BRepGProp.hxx>
#include <GProp_GProps.hxx>
#include <numbers>

namespace raysculpt {
void Window::properties() {
    while (fields->rowCount()) {
        auto row = fields->takeRow(0);
        for (auto item : {row.labelItem, row.fieldItem})
            if (item) {
                if (auto widget = item->widget()) {
                    widget->hide();
                    widget->deleteLater();
                }
                delete item;
            }
    }
    auto o = object(scene, selected);
    if (o.isEmpty())
        return;
    auto k = o["kind"].toString();
    QString id = selected;
    auto change = [this, id](auto apply) {
        guard([&] {
            auto s = scene;
            auto item = object(s, id);
            if (item.isEmpty())
                return;
            apply(item);
            put(s, item);
            commit(s);
        });
    };
    auto text = new QLineEdit(o["name"].toString());
    fields->addRow("Имя", text);
    connect(text, &QLineEdit::editingFinished, this, [=] {
        auto name = text->text().trimmed();
        if (!name.isEmpty())
            change([&](auto &item) { item["name"] = name; });
    });
    fields->addRow("Тип", new QLabel(label(k)));
    auto check = [&](QString title, QString field) {
        auto box = new QCheckBox;
        box->setObjectName("property." + field);
        box->setChecked(o[field].toBool());
        fields->addRow(title, box);
        connect(box, &QCheckBox::clicked, this,
                [=](bool on) { change([&](auto &item) { item[field] = on; }); });
    };
    auto numeric = [&](QString title, double value, double low, double high, QString unit, bool enabled,
                       std::function<void(double)> apply) {
        auto spin = new QDoubleSpinBox;
        spin->setDecimals(4);
        spin->setRange(low, high);
        spin->setSuffix(unit);
        spin->setValue(value);
        spin->setEnabled(enabled);
        spin->setKeyboardTracking(false);
        fields->addRow(title, spin);
        connect(spin, &QDoubleSpinBox::editingFinished, this, [=] {
            if (std::abs(spin->value() - value) > 1e-8)
                apply(spin->value());
        });
    };
    if (k == "screen") {
        auto b = new QPushButton(o.contains("image") ? "Заменить изображение…" : "Назначить изображение…");
        b->setEnabled(!scene.contains("turntable"));
        fields->addRow(b);
        connect(b, &QPushButton::clicked, this, [this] { guard([this] { chooseImage(); }); });
    }
    auto dims = o["dims"].toObject();
    QMap<QString, QString> names{{"w", "Ширина"}, {"d", "Глубина"}, {"h", "Высота"}, {"r", "Радиус"}};
    for (auto key : dims.keys())
        numeric(names.value(key, key), dims[key].toDouble(), .001, 1e6, " мм", !o.contains("profile"),
                [=](double v) {
                    change([&](auto &item) {
                        auto d = item["dims"].toObject();
                        d[key] = v;
                        item["dims"] = d;
                    });
                });
    if (solid(k)) {
        check("Справочная геометрия", "reference");
        auto all = scene["materials"].toObject();
        auto materials = new QComboBox;
        for (auto j = all.begin(); j != all.end(); ++j)
            materials->addItem(j.value().toObject()["name"].toString(), j.key());
        materials->setCurrentIndex(materials->findData(o["material"].toString()));
        fields->addRow("Материал", materials);
        connect(materials, &QComboBox::activated, this, [=](int i) {
            auto key = materials->itemData(i).toString();
            change([&](auto &item) {
                item["material"] = key;
                item["line"] = all[key].toObject()["default"];
            });
        });
        auto m = all[o["material"].toString()].toObject();
        auto lines = new QComboBox;
        lines->addItems(m["indices"].toObject().keys());
        lines->setCurrentText(o["line"].toString());
        fields->addRow("Длина волны, нм", lines);
        connect(lines, &QComboBox::textActivated, this,
                [=](QString key) { change([&](auto &item) { item["line"] = key; }); });
        fields->addRow("Преломление n", new QLabel(QString::number(indexOf(scene, o), 'g', 8)));
        auto rho = m["density"].toDouble();
        if (rho > 0)
            fields->addRow("Плотность", new QLabel(QString::number(rho) + " г/см³"));
        double volume = view->volume(id);
        if (volume > 0) {
            fields->addRow("Объём", new QLabel(QString::number(volume / 1000., 'g', 6) + " см³"));
            if (rho > 0)
                fields->addRow("Масса", new QLabel(QString::number(volume * rho / 1000., 'g', 6) + " г"));
        }
    }
    if (!o.contains("parent"))
        check("Зафиксировано", "locked");
    else
        fields->addRow("Связано с", new QLabel(object(scene, o["parent"].toString())["name"].toString()));
    auto movable = !o["locked"].toBool() && !o.contains("parent");
    for (QString field : {"pos", "rot"})
        for (int i = 0; i < 3; ++i) {
            bool angle = field == "rot";
            auto title = QString(angle ? "Угол " : "") + QString("XYZ").mid(i, 1);
            numeric(title, o[field].toArray().at(i).toDouble(), angle ? -360. : -1e6, angle ? 360. : 1e6,
                    angle ? " °" : " мм", movable, [=, this](double v) {
                        guard([&] {
                            auto s = scene;
                            auto item = object(s, id);
                            auto a = item[field].toArray();
                            a[i] = v;
                            item[field] = a;
                            moveBody(s, id, pose(item));
                            commit(s);
                        });
                    });
        }
}
void Window::material() {
    auto all = scene["materials"].toObject();
    QDialog dlg(this);
    dlg.setWindowTitle("Справочник материалов");
    QFormLayout f(&dlg);
    QComboBox choices;
    choices.addItem("Новый материал…", "");
    for (auto i = all.begin(); i != all.end(); ++i)
        choices.addItem(i.value().toObject()["name"].toString(), i.key());
    f.addRow("Материал", &choices);
    QLineEdit name, source;
    QDoubleSpinBox density;
    density.setRange(0, 100);
    density.setDecimals(4);
    QPlainTextEdit indices;
    f.addRow("Название", &name);
    f.addRow("Плотность, г/см³", &density);
    f.addRow("Длина волны и n, по строкам", &indices);
    f.addRow("Источник", &source);
    connect(&choices, &QComboBox::currentIndexChanged, &dlg, [&] {
        auto m = all[choices.currentData().toString()].toObject();
        name.setText(m["name"].toString());
        source.setText(m["source"].toString());
        density.setValue(m["density"].toDouble());
        QStringList list;
        auto ix = m["indices"].toObject();
        for (auto i = ix.begin(); i != ix.end(); ++i)
            list << i.key() + " " + QString::number(i.value().toDouble(), 'g', 9);
        indices.setPlainText(list.join('\n'));
    });
    choices.setCurrentIndex(std::max(0, choices.findData(object(scene, selected)["material"].toString())));
    QDialogButtonBox buttons(QDialogButtonBox::Ok | QDialogButtonBox::Cancel);
    f.addRow(&buttons);
    connect(&buttons, &QDialogButtonBox::accepted, &dlg, &QDialog::accept);
    connect(&buttons, &QDialogButtonBox::rejected, &dlg, &QDialog::reject);
    if (dlg.exec() != QDialog::Accepted)
        return;
    if (name.text().trimmed().isEmpty())
        fail("Введите название материала");
    QJsonObject ix;
    QString first;
    for (auto line : indices.toPlainText().split('\n', Qt::SkipEmptyParts)) {
        auto pair = line.simplified().split(' ');
        bool ok = false;
        auto n = pair.size() == 2 ? pair[1].toDouble(&ok) : 0;
        if (!ok || !std::isfinite(n) || n <= 1)
            fail("Введите длину волны и показатель преломления > 1");
        if (first.isEmpty())
            first = pair[0];
        ix[pair[0]] = n;
    }
    if (ix.isEmpty())
        fail("Требуется хотя бы одна длина волны");
    auto key = choices.currentData().toString();
    if (key.isEmpty())
        key = QUuid::createUuid().toString(QUuid::Id128);
    auto previous = all[key].toObject()["default"].toString();
    all[key] = QJsonObject{{"name", name.text().trimmed()},
                           {"indices", ix},
                           {"default", ix.contains(previous) ? previous : first},
                           {"source", source.text()},
                           {"density", density.value() > 0 ? QJsonValue(density.value()) : QJsonValue::Null}};
    auto s = scene;
    s["materials"] = all;
    for (auto v : s["objects"].toArray()) {
        auto item = v.toObject();
        if (item["material"] == key && !ix.contains(item["line"].toString())) {
            item["line"] = first;
            put(s, item);
        }
    }
    validateDocument(s);
    storeMaterial(key, all[key].toObject());
    commit(s);
}
} // namespace raysculpt
