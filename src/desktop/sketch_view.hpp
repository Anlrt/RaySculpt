#pragma once
#include <QWidget>
#include <QTableWidget>
#include <QPainter>
#include <QMouseEvent>
#include <QWheelEvent>
#include <cmath>

// The table and canvas share coordinates in millimetres.
namespace raysculpt {
class SketchView : public QWidget {
  public:
    explicit SketchView(QTableWidget &model, QWidget *parent = nullptr) : QWidget(parent), table(model) {
        setMinimumSize(350, 230);
        connect(&table, &QTableWidget::cellChanged, this, [this] { update(); });
        connect(&table, &QTableWidget::itemSelectionChanged, this, [this] { update(); });
    }
    bool circle = false;
    QPointF center;
    double radius = 10, snap = 1;
    void fit() {
        auto pts = points();
        if (pts.isEmpty())
            return;
        auto box = pts.boundingRect();
        origin = box.center();
        scale = std::min(12., 190 / std::max({10., box.width(), box.height()}));
        update();
    }

  protected:
    void paintEvent(QPaintEvent *) override {
        QPainter p(this);
        p.fillRect(rect(), QColor("#f4f7f9"));
        double spacing = snap;
        while (spacing * scale < 15)
            spacing *= 2;
        auto zero = pixel(QPointF());
        p.setPen(QColor("#d3dee3"));
        for (double x = std::fmod(zero.x(), spacing * scale); x < width(); x += spacing * scale)
            p.drawLine(QPointF(x, 0), QPointF(x, height()));
        for (double y = std::fmod(zero.y(), spacing * scale); y < height(); y += spacing * scale)
            p.drawLine(QPointF(0, y), QPointF(width(), y));
        p.setRenderHint(QPainter::Antialiasing);
        p.setPen(QPen(QColor("#397792"), 2));
        p.setBrush(QColor(106, 177, 201, 65));
        if (circle) {
            p.drawEllipse(pixel(center), radius * scale, radius * scale);
            return;
        }
        auto pts = points();
        QPolygonF shape;
        for (int i = 0; i < pts.size(); ++i) {
            auto a = pts[i], b = pts[(i + 1) % pts.size()], d = b - a;
            shape << pixel(a);
            double len = std::hypot(d.x(), d.y()), rise = number(i, 2);
            if (std::abs(rise) < 1e-9 || len < 1e-9)
                continue;
            auto c = (a + b) / 2 +
                     QPointF(-d.y() / len, d.x() / len) * ((rise * rise - len * len / 4) / (2 * rise));
            double start = std::atan2(a.y() - c.y(), a.x() - c.x()), sweep = -4 * std::atan(2 * rise / len),
                   r = std::hypot(a.x() - c.x(), a.y() - c.y());
            for (int j = 1; j < 30; ++j) {
                double theta = start + sweep * j / 30;
                shape << pixel(c + r * QPointF(std::cos(theta), std::sin(theta)));
            }
        }
        if (shape.size() > 2)
            p.drawPolygon(shape);
        for (int i = 0; i < pts.size(); ++i) {
            auto xy = pixel(pts[i]);
            p.setBrush(table.currentRow() == i ? QColor("#d77b36") : Qt::white);
            p.drawEllipse(xy, 4, 4);
            p.drawText(xy + QPointF(6, -5), QString::number(i + 1));
        }
    }
    void mousePressEvent(QMouseEvent *e) override {
        if (circle || e->button() != Qt::LeftButton)
            return;
        auto pts = points();
        drag = -1;
        for (int i = 0; i < pts.size(); ++i)
            if ((pixel(pts[i]) - e->position()).manhattanLength() < 12) {
                drag = i;
                break;
            }
        if (drag < 0) {
            drag = table.rowCount();
            table.insertRow(drag);
            for (int j = 0; j < 3; ++j)
                table.setItem(drag, j, new QTableWidgetItem("0"));
            place(e->position());
        }
        table.selectRow(drag);
    }
    void mouseMoveEvent(QMouseEvent *e) override {
        if (drag >= 0)
            place(e->position());
    }
    void mouseReleaseEvent(QMouseEvent *) override { drag = -1; }
    void wheelEvent(QWheelEvent *e) override {
        scale = std::clamp(scale * std::pow(1.18, e->angleDelta().y() / 120.), .01, 2000.);
        update();
    }

  private:
    QTableWidget &table;
    QPointF origin;
    double scale = 5;
    int drag = -1;
    double number(int i, int j) const {
        auto item = table.item(i, j);
        return item ? item->text().toDouble() : 0;
    }
    QPolygonF points() const {
        QPolygonF pts;
        for (int i = 0; i < table.rowCount(); ++i)
            pts << QPointF(number(i, 0), number(i, 1));
        return pts;
    }
    QPointF pixel(QPointF pt) const {
        return {(pt.x() - origin.x()) * scale + width() / 2., height() / 2. - (pt.y() - origin.y()) * scale};
    }
    void place(QPointF pos) {
        if (drag < 0 || drag >= table.rowCount())
            return;
        auto x = origin.x() + (pos.x() - width() / 2.) / scale,
             y = origin.y() - (pos.y() - height() / 2.) / scale;
        table.item(drag, 0)->setText(QString::number(std::round(x / snap) * snap, 'g', 10));
        table.item(drag, 1)->setText(QString::number(std::round(y / snap) * snap, 'g', 10));
        update();
    }
};
} // namespace raysculpt
