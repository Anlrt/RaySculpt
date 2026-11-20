#include <QApplication>
#include <QElapsedTimer>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QKeyEvent>
#include <QMatrix4x4>
#include <QMouseEvent>
#include <QOpenGLBuffer>
#include <QOpenGLContext>
#include <QOpenGLFramebufferObject>
#include <QOpenGLFunctions_3_3_Core>
#include <QOpenGLShaderProgram>
#include <QOpenGLVertexArrayObject>
#include <QOpenGLWidget>
#include <QPainter>
#include <QPolygonF>
#include <QSurfaceFormat>
#include <QTimer>
#include <QVector3D>
#include <QWheelEvent>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <memory>
#include <numbers>
#include <vector>

namespace {
QJsonObject readJson(const QString& path) {
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) qFatal("Could not open %s", qPrintable(path));
    auto value = QJsonDocument::fromJson(file.readAll());
    if (!value.isObject()) qFatal("Expected JSON object in %s", qPrintable(path));
    return value.object();
}

QVector3D vector3(const QJsonValue& value) {
    const auto array = value.toArray();
    return {float(array.at(0).toDouble()), float(array.at(1).toDouble()), float(array.at(2).toDouble())};
}

class Mesh {
public:
    QString id, kind, label;
    QString version;
    QVector3D center, color, translation;
    QMatrix4x4 pose;
    bool movable = false, visible = true, error = false;
    std::vector<QVector3D> vertices;
    std::vector<unsigned> indices;
    QOpenGLBuffer positions{QOpenGLBuffer::VertexBuffer};
    QOpenGLBuffer elements{QOpenGLBuffer::IndexBuffer};
    QOpenGLVertexArrayObject vao;
    GLenum mode = GL_TRIANGLES;

    void upload(QOpenGLFunctions_3_3_Core& gl) {
        if (vertices.empty()) return;
        vao.create();
        vao.bind();
        positions.create();
        positions.bind();
        positions.allocate(vertices.data(), int(vertices.size()*sizeof(QVector3D)));
        gl.glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, sizeof(QVector3D), nullptr);
        gl.glEnableVertexAttribArray(0);
        if (!indices.empty()) {
            elements.create();
            elements.bind();
            elements.allocate(indices.data(), int(indices.size()*sizeof(unsigned)));
        }
        vao.release();
        positions.release();
    }

    void destroy() {
        vao.destroy();
        positions.destroy();
        elements.destroy();
    }

    void draw(QOpenGLFunctions_3_3_Core& gl) {
        if (vertices.empty()) return;
        vao.bind();
        if (indices.empty()) gl.glDrawArrays(mode, 0, GLsizei(vertices.size()));
        else gl.glDrawElements(mode, GLsizei(indices.size()), GL_UNSIGNED_INT, nullptr);
        vao.release();
    }
};

class Preview final : public QOpenGLWidget, protected QOpenGLFunctions_3_3_Core {
    std::vector<std::unique_ptr<Mesh>> objects;
    Mesh grid;
    std::unique_ptr<QOpenGLShaderProgram> shader;
    std::unique_ptr<QOpenGLFramebufferObject> selectionBuffer;
    QVector3D center;
    float yaw, pitch, scale;
    bool showGrid;
    bool moveMode = false, moving = false;
    QPointF lastPosition;
    QString selected;
    QJsonArray highlights;

    QMatrix4x4 camera() const {
        const float a = yaw * std::numbers::pi_v<float>/180.f;
        const float b = pitch * std::numbers::pi_v<float>/180.f;
        const float ca = std::cos(a), sa = std::sin(a), cb = std::cos(b), sb = std::sin(b);
        const float x = 2.f*scale/width(), y = 2.f*scale/height();
        QMatrix4x4 matrix(x*ca, x*sa, 0, 0,
                          y*sa*sb, -y*ca*sb, y*cb, 0,
                          sa*cb/100000.f, -ca*cb/100000.f, -sb/100000.f, 0,
                          0, 0, 0, 1);
        matrix.translate(-center);
        return matrix;
    }

    QSize physicalSize() const {
        return QSize(qRound(width()*devicePixelRatioF()), qRound(height()*devicePixelRatioF()));
    }

    void cleanup() {
        if (!shader) return;
        makeCurrent();
        for (auto& object : objects) object->destroy();
        grid.destroy();
        selectionBuffer.reset();
        shader.reset();
        doneCurrent();
    }

    void draw(bool ids) {
        const auto size = physicalSize();
        glViewport(0, 0, size.width(), size.height());
        glEnable(GL_DEPTH_TEST);
        glDisable(GL_BLEND);
        glDisable(GL_DITHER);
        glClearColor(ids ? 0.f : .95f, ids ? 0.f : .97f, ids ? 0.f : .98f, 1);
        glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
        shader->bind();
        const auto view = camera();
        shader->setUniformValue("camera", view);
        shader->setUniformValue("lit", false);
        if (!ids && showGrid) {
            const float power = std::pow(10.f,std::floor(std::log10(std::max(1e-5f,75.f/scale))));
            const float step = power*scale < 30.f ? 5.f*power : power;
            auto gridMatrix=view;
            gridMatrix.translate(std::round(center.x()/step)*step,std::round(center.y()/step)*step,0);
            gridMatrix.scale(step);
            shader->setUniformValue("camera",gridMatrix);
            shader->setUniformValue("color", grid.color);
            grid.draw(*this);
            glClear(GL_DEPTH_BUFFER_BIT);
        }
        for (unsigned i = 0; i < objects.size(); ++i) {
            auto& object = *objects[i];
            if (!object.visible) continue;
            auto model = view;
            model.translate(object.translation);
            model *= object.pose;
            shader->setUniformValue("camera",model);
            const unsigned id = i+1;
            const QVector3D color = ids
                ? QVector3D(float(id & 255), float((id >> 8) & 255), float((id >> 16) & 255))/255.f
                : object.error ? QVector3D(.78f,.33f,.29f)
                : (object.id == selected ? QVector3D(.37f,.67f,.8f) : object.color);
            shader->setUniformValue("color", color);
            shader->setUniformValue("lit", !ids && object.kind != "screen" && object.mode == GL_TRIANGLES);
            if (object.mode == GL_POINTS) glPointSize(8.f*float(devicePixelRatioF()));
            object.draw(*this);
        }
        shader->release();
    }

protected:
    void initializeGL() override {
        if (!initializeOpenGLFunctions()) return;
        connect(context(), &QOpenGLContext::aboutToBeDestroyed, this, [this] { cleanup(); },
                Qt::DirectConnection);
        shader = std::make_unique<QOpenGLShaderProgram>();
        if (!shader->addShaderFromSourceCode(QOpenGLShader::Vertex, R"(
            #version 330 core
            layout(location=0) in vec3 position;
            uniform mat4 camera;
            out vec3 surface;
            void main() { surface = position; gl_Position = camera * vec4(position, 1.0); }
        )") || !shader->addShaderFromSourceCode(QOpenGLShader::Fragment, R"(
            #version 330 core
            uniform vec3 color;
            uniform bool lit;
            in vec3 surface;
            out vec4 pixel;
            void main() {
                float light = 1.0;
                if (lit) {
                    vec3 normal = normalize(cross(dFdx(surface), dFdy(surface)));
                    light = .65 + .35 * abs(dot(normal, normalize(vec3(.35,.55,.76))));
                }
                pixel = vec4(color * light, 1.0);
            }
        )") || !shader->link()) {
            qWarning("%s",qPrintable(shader->log()));
            shader.reset();
            return;
        }
        for (auto& object : objects) object->upload(*this);
        grid.upload(*this);
    }

    void paintGL() override {
        if (!shader) return;
        draw(false);
        QPainter painter(this);
        painter.setPen(QColor("#526976"));
        painter.drawText(18, 26, QStringLiteral("Ортогональный вид"));
        const auto transform = camera();
        const QPointF origin(42,height()-38);
        const QVector3D axes[] = {{1,0,0},{0,1,0},{0,0,1}};
        const QColor colors[] = {QColor("#b75a54"),QColor("#4f8c69"),QColor("#4678a0")};
        for (int i = 0; i < 3; ++i) {
            const auto direction = transform.mapVector(axes[i]);
            const QPointF tip = origin + QPointF(direction.x()*width(),-direction.y()*height())*(13.f/scale);
            painter.setPen(QPen(colors[i],1.5));
            painter.drawLine(origin,tip);
            painter.drawText(tip+QPointF(3,-2),QString(QChar(char16_t(u'X'+i))));
        }
        painter.setPen(QColor("#526976"));
        const float ruler = std::pow(10.f,std::floor(std::log10(85.f/scale)));
        const float length = ruler*scale;
        const QPointF end(width()-24,height()-25);
        painter.drawLine(end-QPointF(length,0),end);
        for (const auto x : {end.x()-length,end.x()})
            painter.drawLine(QPointF(x,end.y()-3),QPointF(x,end.y()+3));
        painter.drawText(end-QPointF(length,8),QString::number(ruler)+QStringLiteral(" мм"));
        for (const auto& object : objects) {
            if (!object->visible || (object->mode != GL_POINTS && object->kind != "screen")) continue;
            const auto point = transform.map(object->pose.map(object->center)+object->translation);
            const QPointF pixel((point.x()+1)*width()/2, (1-point.y())*height()/2);
            painter.drawText(pixel + QPointF(8, -8), object->label);
        }
        painter.setPen(QPen(QColor("#bd5620"),2.4));
        for (const auto value : highlights) {
            QPolygonF line;
            for (const auto point : value.toArray()) line.append(project(vector3(point)));
            if (line.size()==1) painter.drawEllipse(line.first(),4,4);
            else painter.drawPolyline(line);
        }
    }

    void mousePressEvent(QMouseEvent* event) override {
        lastPosition = event->position();
        if (event->button() == Qt::LeftButton) {
            selected = pick(event->position());
            moving = (moveMode || (event->modifiers() & Qt::ShiftModifier)) &&
                std::any_of(objects.begin(),objects.end(),[this](const auto& object) {
                    return object->id == selected && object->movable;
                });
            update();
        }
    }

    void mouseMoveEvent(QMouseEvent* event) override {
        const auto delta = event->position() - lastPosition;
        if (event->buttons() & Qt::RightButton) {
            yaw -= float(delta.x())*.45f;
            pitch = std::clamp(pitch+float(delta.y())*.45f, -90.f, 90.f);
            update();
        } else if (event->buttons() & Qt::MiddleButton) {
            const auto transform = camera();
            const QVector3D right = transform.row(0).toVector3D().normalized();
            const QVector3D up = transform.row(1).toVector3D().normalized();
            center -= (right*float(delta.x()) - up*float(delta.y()))/scale;
            update();
        } else if (moving && event->buttons() & Qt::LeftButton) {
            const auto transform = camera();
            const auto right = transform.row(0).toVector3D().normalized();
            const auto up = transform.row(1).toVector3D().normalized();
            for (auto& object : objects)
                if (object->id == selected && object->movable)
                    object->translation += (right*float(delta.x())-up*float(delta.y()))/scale;
            update();
        }
        lastPosition = event->position();
    }

    void mouseReleaseEvent(QMouseEvent* event) override {
        if (event->button() == Qt::LeftButton) moving = false;
    }

    void wheelEvent(QWheelEvent* event) override {
        const float previous = scale;
        scale = std::clamp(scale*float(std::pow(1.18, event->angleDelta().y()/120.)), .002f, 8000.f);
        const auto delta = event->position() - QPointF(width()/2.,height()/2.);
        const auto transform = camera();
        const auto right = transform.row(0).toVector3D().normalized();
        const auto up = transform.row(1).toVector3D().normalized();
        center += (right*float(delta.x()) - up*float(delta.y()))*(1.f/previous - 1.f/scale);
        update();
    }

    void keyPressEvent(QKeyEvent* event) override {
        if (event->key() == Qt::Key_M) {
            moveMode = !moveMode;
            setCursor(moveMode ? Qt::SizeAllCursor : Qt::ArrowCursor);
            return;
        }
        if (event->key() != Qt::Key_F) {
            QOpenGLWidget::keyPressEvent(event);
            return;
        }
        const bool selectedOnly = event->modifiers() & Qt::ShiftModifier;
        const auto transform = camera();
        const auto right = transform.row(0).toVector3D().normalized();
        const auto up = transform.row(1).toVector3D().normalized();
        const auto depth = QVector3D::crossProduct(right,up);
        QVector3D low, high;
        bool found = false;
        for (const auto& object : objects) {
            if (selectedOnly && object->id != selected) continue;
            const auto bound = [&](const QVector3D& point) {
                const QVector3D p(QVector3D::dotProduct(right,point),
                                  QVector3D::dotProduct(up,point),
                                  QVector3D::dotProduct(depth,point));
                for (int axis = 0; axis < 3; ++axis) {
                    low[axis] = found ? std::min(p[axis],low[axis]) : p[axis];
                    high[axis] = found ? std::max(p[axis],high[axis]) : p[axis];
                }
                found = true;
            };
            if (!object->visible) continue;
            for (const auto& point : object->vertices) bound(object->pose.map(point)+object->translation);
        }
        if (!found) return;
        const auto mid = (low+high)*.5f;
        center = right*mid.x() + up*mid.y() + depth*mid.z();
        scale = .71f*std::min(width()/std::max(10.f,high.x()-low.x()),
                             height()/std::max(10.f,high.y()-low.y()));
        update();
    }

public:
    explicit Preview(const QJsonObject& snapshot, QWidget* parent=nullptr) : QOpenGLWidget(parent) {
        setFocusPolicy(Qt::StrongFocus);
        replaceScene(snapshot);
        for (int i = -27; i <= 27; ++i) {
            const float offset = float(i);
            grid.vertices.insert(grid.vertices.end(), {{offset,-30,0},{offset,30,0},
                                                       {-30,offset,0},{30,offset,0}});
        }
        grid.mode = GL_LINES;
        grid.color = {.85f,.89f,.91f};
    }

    void frame(const QJsonObject& snapshot) {
        const auto cam = snapshot["camera"].toObject();
        yaw = float(cam["yaw_deg"].toDouble());
        pitch = float(cam["pitch_deg"].toDouble());
        scale = float(cam["px_per_mm"].toDouble());
        center = vector3(cam["center_mm"]);
        showGrid = snapshot["grid"].toBool();
        selected=snapshot["selected"].toString();
        highlights=snapshot["highlights"].toArray();
        for (const auto value : snapshot["poses"].toArray()) {
            const auto data=value.toObject();
            const auto id=data["id"].toString();
            const auto object=std::find_if(objects.begin(),objects.end(),[&](const auto& item) {
                return item->id==id;
            });
            if (object==objects.end()) continue;
            auto& body=**object;
            body.label=data["label"].toString();
            body.visible=data["visible"].toBool();
            body.error=data["error"].toBool();
            const auto values=data["matrix"].toArray();
            if (values.size()==12) {
                float p[12];
                for (int i=0;i<12;++i) p[i]=float(values[i].toDouble());
                body.pose=QMatrix4x4(p[0],p[1],p[2],p[3],p[4],p[5],p[6],p[7],
                                     p[8],p[9],p[10],p[11],0,0,0,1);
            }
        }
        update();
    }

    void replaceScene(const QJsonObject& snapshot) {
        auto previous=std::move(objects);
        objects.clear();
        if (shader) makeCurrent();
        for (const auto value : snapshot["objects"].toArray()) {
            const auto data = value.toObject();
            const auto id=data["id"].toString(), version=data["version"].toString();
            const auto reused=std::find_if(previous.begin(),previous.end(),[&](const auto& entry) {
                return entry && entry->id==id && entry->version==version;
            });
            if (reused!=previous.end()) {
                objects.push_back(std::move(*reused));
                continue;
            }
            auto object = std::make_unique<Mesh>();
            object->id = id;
            object->version=version;
            object->kind = data["kind"].toString(object->id);
            object->label = data["label"].toString();
            object->center = vector3(data["center"]);
            object->movable = data["movable"].toBool();
            object->color = object->kind == "screen" ? QVector3D(.71f,.82f,.87f) : QVector3D(.56f,.66f,.72f);
            for (const auto point : data["vertices"].toArray()) object->vertices.push_back(vector3(point));
            for (const auto face : data["triangles"].toArray()) {
                for (const auto index : face.toArray()) object->indices.push_back(unsigned(index.toInt()));
            }
            if (object->vertices.empty()) {
                object->mode = GL_POINTS;
                object->vertices.push_back(object->center);
                object->color = {.96f,.76f,.39f};
            }
            if (shader) object->upload(*this);
            objects.push_back(std::move(object));
        }
        if (shader) {
            for (auto& object : previous) {
                if (object) object->destroy();
            }
            doneCurrent();
        }
        frame(snapshot);
    }

    ~Preview() override {
        if (context()) disconnect(context(), nullptr, this, nullptr);
        cleanup();
    }

    QString pick(const QPointF& point) {
        if (!shader || !rect().contains(point.toPoint())) return {};
        makeCurrent();
        unsigned id = 0;
        const auto size = physicalSize();
        if (!selectionBuffer || selectionBuffer->size() != size) {
            selectionBuffer.reset();
            selectionBuffer = std::make_unique<QOpenGLFramebufferObject>(
                size, QOpenGLFramebufferObject::Depth);
        }
        if (selectionBuffer->isValid() && selectionBuffer->bind()) {
            draw(true);
            unsigned char rgba[4]{};
            glReadPixels(int(point.x()*devicePixelRatioF()),
                         size.height()-1-int(point.y()*devicePixelRatioF()),
                         1, 1, GL_RGBA, GL_UNSIGNED_BYTE, rgba);
            id = unsigned(rgba[0]) | (unsigned(rgba[1]) << 8) | (unsigned(rgba[2]) << 16);
            glBindFramebuffer(GL_FRAMEBUFFER, defaultFramebufferObject());
        }
        doneCurrent();
        return id && id <= objects.size() ? objects[id-1]->id : QString{};
    }

    QPointF project(const QVector3D& world) const {
        const auto point = camera().map(world);
        return {(point.x()+1)*width()/2, (1-point.y())*height()/2};
    }

    QString kindOf(const QString& id) const {
        for (const auto& object : objects)
            if (object->id == id) return object->kind;
        return {};
    }

    int pickNumber(const QPointF& point) {
        const auto id=pick(point);
        for (std::size_t i=0;i<objects.size();++i)
            if (objects[i]->id==id) return int(i);
        return -1;
    }

    bool ready() const { return shader && isValid(); }

    int benchmark() {
        if (!shader) return 1;
        makeCurrent();
        qInfo("OpenGL renderer: %s", reinterpret_cast<const char*>(glGetString(GL_RENDERER)));
        std::vector<double> ms;
        for (int i = 0; i < 65; ++i) {
            yaw = 36.f + (i-5)*.45f;
            QElapsedTimer timer;
            timer.start();
            draw(false);
            glFinish();
            if (i >= 5) ms.push_back(timer.nsecsElapsed()/1e6);
        }
        doneCurrent();
        std::sort(ms.begin(), ms.end());
        qInfo("GPU draw p50 %.3f ms p95 %.3f ms, camera-only vertex uploads 0", (ms[29]+ms[30])/2., ms[56]);
        return ms[56] <= 16.7 ? 0 : 2;
    }

    void benchmarkPresentation() {
        auto timer = std::make_shared<QElapsedTimer>();
        auto timings = std::make_shared<std::vector<double>>();
        auto count = std::make_shared<unsigned>(0);
        connect(this,&QOpenGLWidget::frameSwapped,this,[this,timer,timings,count] {
            if (*count >= 5) timings->push_back(timer->nsecsElapsed()/1e6);
            if (++*count == 65) {
                std::sort(timings->begin(), timings->end());
                qInfo("Frame presentation p50 %.3f ms p95 %.3f ms with default synchronization",
                      ((*timings)[29]+(*timings)[30])/2., (*timings)[56]);
                QApplication::exit();
                return;
            }
            yaw += .45f;
            timer->restart();
            update();
        });
        timer->start();
        update();
    }
};
}

#ifdef CAUSTIC_GPU_WIDGET
#ifdef _WIN32
#define RAYSCULPT_EXPORT __declspec(dllexport)
#else
#define RAYSCULPT_EXPORT
#endif

extern "C" RAYSCULPT_EXPORT void* raysculpt_view_create_v1(void* owner) noexcept {
    try {
        const QJsonObject camera{{"yaw_deg",36.},{"pitch_deg",26.},{"px_per_mm",7.},
                                 {"center_mm",QJsonArray{0.,0.,0.}}};
        auto widget=new Preview(QJsonObject{{"camera",camera},{"grid",true}},static_cast<QWidget*>(owner));
        QSurfaceFormat format;
        format.setVersion(3,3);
        format.setProfile(QSurfaceFormat::CoreProfile);
        format.setDepthBufferSize(24);
        widget->setFormat(format);
        widget->setAttribute(Qt::WA_TransparentForMouseEvents);
        widget->setFocusPolicy(Qt::NoFocus);
        return widget;
    } catch (...) {
        return nullptr;
    }
}

extern "C" RAYSCULPT_EXPORT int raysculpt_view_sync_v1(void* handle,const char* text,
                                                      std::size_t length,int geometry) noexcept {
    if (!handle || !text || length>64*1024*1024) return 0;
    try {
        const auto message=QJsonDocument::fromJson(QByteArray(text,qsizetype(length)));
        if (!message.isObject()) return 0;
        auto view=static_cast<Preview*>(handle);
        if (geometry) view->replaceScene(message.object());
        else view->frame(message.object());
        return 1;
    } catch (...) {
        return 0;
    }
}

extern "C" RAYSCULPT_EXPORT int raysculpt_view_pick_v1(void* handle,double x,double y) noexcept {
    if (!handle) return -1;
    try {
        return static_cast<Preview*>(handle)->pickNumber(QPointF(x,y));
    } catch (...) {
        return -1;
    }
}

extern "C" RAYSCULPT_EXPORT int raysculpt_view_ready_v1(void* handle) noexcept {
    return handle && static_cast<Preview*>(handle)->ready();
}
#else
int main(int argc, char** argv) {
    QSurfaceFormat format;
    format.setVersion(3,3);
    format.setProfile(QSurfaceFormat::CoreProfile);
    format.setDepthBufferSize(24);
    format.setSwapInterval(argc >= 3 && std::strcmp(argv[2],"--benchmark") == 0 ? 0 : 1);
    QSurfaceFormat::setDefaultFormat(format);
    QApplication app(argc, argv);
    const auto args = app.arguments();
    if (args.size() < 2) qFatal("Usage: raysculpt_gpu_preview SNAPSHOT [--benchmark | --present | --picks REPORT SCENE | --snapshot FILE]");
    QWidget secondWindow;
    Preview view(readJson(args[1]));
    view.setWindowTitle(QStringLiteral("RaySculpt — OpenGL"));
    view.resize(1600,900);
    view.show();
    if (args.size() > 2) {
        QTimer::singleShot(200,&view,[&] {
            if (!view.isValid()) {
                qCritical("OpenGL context unavailable");
                app.exit(1);
            } else if (args[2] == "--benchmark") {
                app.exit(view.benchmark());
            } else if (args[2] == "--present") {
                view.benchmarkPresentation();
            } else if (args[2] == "--snapshot" && args.size() == 4) {
                app.exit(view.grabFramebuffer().save(args[3]) ? 0 : 1);
            } else if (args[2] == "--picks" && args.size() == 5) {
                const auto samples = readJson(args[3])["picks"].toArray();
                const auto check = [&, samples] {
                    int checked = 0, mismatches = 0;
                    for (const auto value : samples) {
                        const auto sample = value.toObject();
                        if (sample["scene"].toString() != args[4]) continue;
                        const auto xy = sample["xy_mm"].toArray();
                        const auto actual = view.pick(view.project(
                            QVector3D(float(xy[0].toDouble()),float(xy[1].toDouble()),0)));
                        ++checked;
                        if (view.kindOf(actual) != sample["visible_id"].toString()) ++mismatches;
                    }
                    qInfo("Picking samples %d, mismatches %d", checked, mismatches);
                    return view.isValid() && checked && !mismatches;
                };
                if (!check()) { app.exit(2); return; }
                view.resize(1024,768);
                if (!check()) { app.exit(2); return; }
                view.setParent(&secondWindow);
                secondWindow.resize(1024,768);
                secondWindow.show();
                view.show();
                QTimer::singleShot(200,&view,[&,check] { app.exit(check() ? 0 : 2); });
            } else app.exit(1);
        });
    }
    return app.exec();
}
#endif
