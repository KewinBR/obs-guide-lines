#include "guide-lines-overlay.hpp"
#include <OBSBasicPreview.hpp>
#include <obs-frontend-api.h>
#include <obs.h>
#include <graphics/matrix4.h>
#include <graphics/vec3.h>
#include <QPainter>
#include <QMouseEvent>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonArray>
#include <QFile>
#include <QDir>
#include <QStandardPaths>
#include <QCursor>
#include <QMenu>
#include <QAction>
#include <QMainWindow>
#include <QPushButton>
#include <QGuiApplication>
#include <cmath>

static std::vector<GuideLinesOverlay::GuideLine> copiedHorizontalGuides;
static std::vector<GuideLinesOverlay::GuideLine> copiedVerticalGuides;
static bool hasCopiedLayout = false;

GuideLinesOverlay::GuideLinesOverlay(QWidget* parent) : QWidget(parent) {
    setWindowFlags(Qt::Tool | Qt::FramelessWindowHint | Qt::WindowDoesNotAcceptFocus);
    setAttribute(Qt::WA_NoSystemBackground);
    setAttribute(Qt::WA_TranslucentBackground);
    setAttribute(Qt::WA_ShowWithoutActivating);
    setMouseTracking(true);
    setFocusPolicy(Qt::NoFocus);
    previewWidget = parent;
}

GuideLinesOverlay::~GuideLinesOverlay() {
    connectSceneSignals(nullptr);
}

void GuideLinesOverlay::setGuidesVisible(bool visible) {
    guidesVisible = visible;
    
    QMainWindow* main_window = static_cast<QMainWindow*>(obs_frontend_get_main_window());
    if (main_window) {
        QPushButton* rulerBtn = main_window->findChild<QPushButton*>("rulerToggleButton");
        if (rulerBtn) {
            QSignalBlocker blocker(rulerBtn);
            rulerBtn->setChecked(visible);
        }
    }

    updateOverlayGeometry();
}

void GuideLinesOverlay::setColors(const QColor& horiz, const QColor& vert) {
    horizontalColor = horiz;
    verticalColor = vert;
    update();
}

void GuideLinesOverlay::clearGuides() {
    horizontalGuides.clear();
    verticalGuides.clear();
    updateMask();
    update();
}

bool GuideLinesOverlay::findGuideAt(const QPoint& pos, int& index, bool& isHorizontal) {
    const float tolerance = 5.0f; // pixels tolerance for selection

    for (size_t i = 0; i < horizontalGuides.size(); ++i) {
        float y = canvasY + horizontalGuides[i].position * scale;
        if (std::abs(pos.y() - y) <= tolerance && pos.x() >= rulerSize) {
            index = static_cast<int>(i);
            isHorizontal = true;
            return true;
        }
    }

    for (size_t i = 0; i < verticalGuides.size(); ++i) {
        float x = canvasX + verticalGuides[i].position * scale;
        if (std::abs(pos.x() - x) <= tolerance && pos.y() >= rulerSize) {
            index = static_cast<int>(i);
            isHorizontal = false;
            return true;
        }
    }

    return false;
}

void GuideLinesOverlay::getSnapCandidates(std::vector<float>& hCandidates, std::vector<float>& vCandidates) {
    hCandidates.clear();
    vCandidates.clear();

    obs_video_info ovi;
    if (!obs_get_video_info(&ovi)) return;

    float baseCX = static_cast<float>(ovi.base_width);
    float baseCY = static_cast<float>(ovi.base_height);

    hCandidates.push_back(0.0f);
    hCandidates.push_back(baseCY / 2.0f);
    hCandidates.push_back(baseCY);

    vCandidates.push_back(0.0f);
    vCandidates.push_back(baseCX / 2.0f);
    vCandidates.push_back(baseCX);

    obs_source_t* sceneSource = obs_frontend_get_current_scene();
    if (!sceneSource) return;

    obs_scene_t* scene = obs_scene_from_source(sceneSource);
    if (scene) {
        struct SnapData {
            std::vector<float>* h;
            std::vector<float>* v;
        } data = { &hCandidates, &vCandidates };

        obs_scene_enum_items(scene, [](obs_scene_t*, obs_sceneitem_t* item, void* param) {
            if (!obs_sceneitem_visible(item)) return true;

            SnapData* sd = static_cast<SnapData*>(param);
            matrix4 boxTransform;
            obs_sceneitem_get_box_transform(item, &boxTransform);

            auto getTransformed = [](float x, float y, const matrix4& m) {
                vec3 p;
                vec3_set(&p, x, y, 0.0f);
                vec3_transform(&p, &p, &m);
                return p;
            };

            vec3 t[4] = {
                getTransformed(0.0f, 0.0f, boxTransform),
                getTransformed(1.0f, 0.0f, boxTransform),
                getTransformed(0.0f, 1.0f, boxTransform),
                getTransformed(1.0f, 1.0f, boxTransform)
            };

            float minX = t[0].x, maxX = t[0].x;
            float minY = t[0].y, maxY = t[0].y;

            for (int i = 1; i < 4; ++i) {
                if (t[i].x < minX) minX = t[i].x;
                if (t[i].x > maxX) maxX = t[i].x;
                if (t[i].y < minY) minY = t[i].y;
                if (t[i].y > maxY) maxY = t[i].y;
            }

            sd->v->push_back(minX);
            sd->v->push_back(maxX);
            sd->v->push_back((minX + maxX) / 2.0f);

            sd->h->push_back(minY);
            sd->h->push_back(maxY);
            sd->h->push_back((minY + maxY) / 2.0f);

            return true;
        }, &data);
    }

    obs_source_release(sceneSource);
}

void GuideLinesOverlay::updateViewportMetrics() {
    if (!previewWidget) return;

    obs_video_info ovi;
    if (!obs_get_video_info(&ovi)) {
        return;
    }

    float baseCX = static_cast<float>(ovi.base_width);
    float baseCY = static_cast<float>(ovi.base_height);

    OBSBasicPreview* preview = reinterpret_cast<OBSBasicPreview*>(previewWidget);
    if (!preview) return;

    float pixelRatio = preview->devicePixelRatioF();
    float previewScale = preview->GetScalingAmount();

    QSize targetSize = preview->size() * pixelRatio;
    bool isFixedScaling = preview->IsFixedScaling();

    float previewX = 0.0f;
    float previewY = 0.0f;

    float windowCX = static_cast<float>(targetSize.width() - 20);
    float windowCY = static_cast<float>(targetSize.height() - 20);

    if (isFixedScaling) {
        previewX = (windowCX - baseCX * previewScale) / 2.0f;
        previewY = (windowCY - baseCY * previewScale) / 2.0f;

        previewX += preview->GetScrollX();
        previewY += preview->GetScrollY();
    } else {
        float windowAspect = windowCX / windowCY;
        float baseAspect = baseCX / baseCY;
        float newCX = 0.0f;
        float newCY = 0.0f;

        if (windowAspect > baseAspect) {
            previewScale = windowCY / baseCY;
            newCX = windowCY * baseAspect;
            newCY = windowCY;
        } else {
            previewScale = windowCX / baseCX;
            newCX = windowCX;
            newCY = windowCX / baseAspect;
        }

        previewX = windowCX / 2.0f - newCX / 2.0f;
        previewY = windowCY / 2.0f - newCY / 2.0f;
    }

    previewX += 10.0f; // PREVIEW_EDGE_SIZE
    previewY += 10.0f; // PREVIEW_EDGE_SIZE

    canvasX = previewX / pixelRatio;
    canvasY = previewY / pixelRatio;
    scale = previewScale / pixelRatio;
}

void GuideLinesOverlay::updateOverlayGeometry() {
    if (!previewWidget || !previewWidget->isVisible() || !previewWidget->window()->isVisible() || previewWidget->window()->isMinimized()) {
        hide();
        return;
    }

    QPoint globalPos = previewWidget->mapToGlobal(QPoint(0, 0));
    setGeometry(QRect(globalPos, previewWidget->size()));

    updateViewportMetrics();
    
    if (activeGuideIndex == -1 && !isCreatingNew) {
        updateMask();
    }

    if (guidesVisible) {
        show();
        raise();
    } else {
        hide();
    }
}

void GuideLinesOverlay::updateMask() {
    if (!guidesVisible) {
        setMask(QRegion());
        return;
    }

    QRegion mask;
    mask += QRect(0, 0, width(), rulerSize);
    mask += QRect(0, 0, rulerSize, height());

    const int tolerance = 4;
    for (const auto& gl : horizontalGuides) {
        float y = canvasY + gl.position * scale;
        if (y >= rulerSize && y < height()) {
            mask += QRect(rulerSize, static_cast<int>(y) - tolerance, width() - rulerSize, 2 * tolerance);
        }
    }
    for (const auto& gl : verticalGuides) {
        float x = canvasX + gl.position * scale;
        if (x >= rulerSize && x < width()) {
            mask += QRect(static_cast<int>(x) - tolerance, rulerSize, 2 * tolerance, height() - rulerSize);
        }
    }

    setMask(mask);
}

void GuideLinesOverlay::onViewportChanged() {
    updateViewportMetrics();
    if (activeGuideIndex == -1 && !isCreatingNew) {
        updateMask();
    }
    update();
}

bool GuideLinesOverlay::hasCopiedGuides() {
    return hasCopiedLayout;
}

void GuideLinesOverlay::copyActiveSceneLayout() {
    copiedHorizontalGuides = horizontalGuides;
    copiedVerticalGuides = verticalGuides;
    hasCopiedLayout = true;
}

void GuideLinesOverlay::pasteActiveSceneLayout() {
    if (!hasCopiedLayout) return;
    horizontalGuides = copiedHorizontalGuides;
    verticalGuides = copiedVerticalGuides;
    updateMask();
    update();
    saveGuides(currentSceneName);
}

void GuideLinesOverlay::showContextMenu(const QPoint& globalPos, int index, bool isHorizontal) {
    QMenu menu(this);
    
    QAction* deleteAction = menu.addAction(QObject::tr("Excluir Guia"));
    
    menu.addSeparator();
    QMenu* colorMenu = menu.addMenu(QObject::tr("Cor da Guia"));
    
    struct ColorOption {
        QString name;
        QColor color;
    };
    std::vector<ColorOption> colors = {
        { QObject::tr("Ciano"), QColor(0, 190, 255) },
        { QObject::tr("Magenta"), QColor(255, 0, 128) },
        { QObject::tr("Amarelo"), QColor(255, 215, 0) },
        { QObject::tr("Verde"), QColor(0, 255, 0) },
        { QObject::tr("Branco"), QColor(255, 255, 255) }
    };
    
    std::vector<QAction*> colorActions;
    QColor current = isHorizontal ? horizontalGuides[index].color : verticalGuides[index].color;
    for (const auto& opt : colors) {
        QAction* act = colorMenu->addAction(opt.name);
        act->setCheckable(true);
        if (current == opt.color) {
            act->setChecked(true);
        }
        colorActions.push_back(act);
    }
    
    menu.addSeparator();
    QMenu* styleMenu = menu.addMenu(QObject::tr("Estilo da Guia"));
    QAction* dashAction = styleMenu->addAction(QObject::tr("Tracejado"));
    dashAction->setCheckable(true);
    QAction* solidAction = styleMenu->addAction(QObject::tr("Sólido"));
    solidAction->setCheckable(true);
    
    Qt::PenStyle currentStyle = isHorizontal ? horizontalGuides[index].style : verticalGuides[index].style;
    if (currentStyle == Qt::DashLine) {
        dashAction->setChecked(true);
    } else {
        solidAction->setChecked(true);
    }
    
    QAction* selected = menu.exec(globalPos);
    if (!selected) return;
    
    if (selected == deleteAction) {
        if (isHorizontal) {
            horizontalGuides.erase(horizontalGuides.begin() + index);
        } else {
            verticalGuides.erase(verticalGuides.begin() + index);
        }
        updateMask();
        update();
        saveGuides(currentSceneName);
    } else if (selected == dashAction) {
        if (isHorizontal) {
            horizontalGuides[index].style = Qt::DashLine;
        } else {
            verticalGuides[index].style = Qt::DashLine;
        }
        update();
        saveGuides(currentSceneName);
    } else if (selected == solidAction) {
        if (isHorizontal) {
            horizontalGuides[index].style = Qt::SolidLine;
        } else {
            verticalGuides[index].style = Qt::SolidLine;
        }
        update();
        saveGuides(currentSceneName);
    } else {
        for (size_t i = 0; i < colorActions.size(); ++i) {
            if (selected == colorActions[i]) {
                if (isHorizontal) {
                    horizontalGuides[index].color = colors[i].color;
                } else {
                    verticalGuides[index].color = colors[i].color;
                }
                update();
                saveGuides(currentSceneName);
                break;
            }
        }
    }
}

void GuideLinesOverlay::connectSceneSignals(obs_source_t* sceneSource) {
    if (currentSceneSource) {
        signal_handler_disconnect(
            obs_source_get_signal_handler(currentSceneSource),
            "item_transform",
            &GuideLinesOverlay::onItemTransform,
            this
        );
        obs_source_release(currentSceneSource);
        currentSceneSource = nullptr;
    }
    
    if (sceneSource) {
        currentSceneSource = sceneSource;
        obs_source_get_ref(currentSceneSource);
        signal_handler_connect(
            obs_source_get_signal_handler(currentSceneSource),
            "item_transform",
            &GuideLinesOverlay::onItemTransform,
            this
        );
    }
}

void GuideLinesOverlay::onItemTransform(void* data, calldata_t* params) {
    GuideLinesOverlay* overlay = static_cast<GuideLinesOverlay*>(data);
    if (overlay) {
        overlay->handleItemTransform(params);
    }
}

void GuideLinesOverlay::handleItemTransform(calldata_t* params) {
    if (!guidesVisible) return;
    
    // Disable snapping if Control key is held down (standard OBS rule)
    if (QGuiApplication::keyboardModifiers() & Qt::ControlModifier) {
        return;
    }
    
    // Snapping only occurs during user mouse dragging
    if (!(QGuiApplication::mouseButtons() & Qt::LeftButton)) {
        return;
    }
    
    static bool inside_snap = false;
    if (inside_snap) return;

    obs_sceneitem_t* item = static_cast<obs_sceneitem_t*>(calldata_ptr(params, "item"));
    if (!item) return;
    
    if (obs_sceneitem_locked(item)) return;

    inside_snap = true;

    // Snapping distance of 8 screen pixels
    float snapTolerance = 8.0f;
    if (scale > 0.0f) {
        snapTolerance /= scale;
    }

    matrix4 boxTransform;
    obs_sceneitem_get_box_transform(item, &boxTransform);

    auto getTransformed = [](float x, float y, const matrix4& m) {
        vec3 p;
        vec3_set(&p, x, y, 0.0f);
        vec3_transform(&p, &p, &m);
        return p;
    };

    vec3 t[4] = {
        getTransformed(0.0f, 0.0f, boxTransform),
        getTransformed(1.0f, 0.0f, boxTransform),
        getTransformed(0.0f, 1.0f, boxTransform),
        getTransformed(1.0f, 1.0f, boxTransform)
    };

    float minX = t[0].x, maxX = t[0].x;
    float minY = t[0].y, maxY = t[0].y;

    for (int i = 1; i < 4; ++i) {
        if (t[i].x < minX) minX = t[i].x;
        if (t[i].x > maxX) maxX = t[i].x;
        if (t[i].y < minY) minY = t[i].y;
        if (t[i].y > maxY) maxY = t[i].y;
    }

    // Snap horizontal edges of item to vertical guidelines
    float bestOffsetX = 0.0f;
    float minDiffX = snapTolerance;
    float itemVTargets[3] = { minX, maxX, (minX + maxX) / 2.0f };
    for (float target : itemVTargets) {
        for (const auto& gl : verticalGuides) {
            float diff = gl.position - target;
            if (std::abs(diff) < minDiffX) {
                minDiffX = std::abs(diff);
                bestOffsetX = diff;
            }
        }
    }

    // Snap vertical edges of item to horizontal guidelines
    float bestOffsetY = 0.0f;
    float minDiffY = snapTolerance;
    float itemHTargets[3] = { minY, maxY, (minY + maxY) / 2.0f };
    for (float target : itemHTargets) {
        for (const auto& gl : horizontalGuides) {
            float diff = gl.position - target;
            if (std::abs(diff) < minDiffY) {
                minDiffY = std::abs(diff);
                bestOffsetY = diff;
            }
        }
    }

    if (bestOffsetX != 0.0f || bestOffsetY != 0.0f) {
        vec2 pos;
        obs_sceneitem_get_pos(item, &pos);
        pos.x += bestOffsetX;
        pos.y += bestOffsetY;
        obs_sceneitem_set_pos(item, &pos);
    }

    inside_snap = false;
}

void GuideLinesOverlay::paintEvent(QPaintEvent* event) {
    Q_UNUSED(event);
    if (!guidesVisible) return;

    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing);

    QColor rulerBg(25, 25, 25, 240);
    painter.fillRect(rulerSize, 0, width() - rulerSize, rulerSize, rulerBg);
    painter.fillRect(0, rulerSize, rulerSize, height() - rulerSize, rulerBg);

    QPen borderPen(QColor(65, 65, 65));
    borderPen.setWidth(1);
    painter.setPen(borderPen);
    painter.drawLine(0, rulerSize, width(), rulerSize);
    painter.drawLine(rulerSize, 0, rulerSize, height());

    drawRulers(painter);

    for (size_t i = 0; i < horizontalGuides.size(); ++i) {
        const auto& gl = horizontalGuides[i];
        float y = canvasY + gl.position * scale;
        if (y >= rulerSize && y < height()) {
            QPen pen(gl.color);
            pen.setWidth(1);
            if (static_cast<int>(i) == activeGuideIndex && isActiveHorizontal) {
                pen.setStyle(Qt::SolidLine);
                pen.setWidth(2);
                if (isSnapped) {
                    pen.setColor(QColor(255, 215, 0));
                }
            } else if (static_cast<int>(i) == hoveredGuideIndex && isHoveredHorizontal) {
                pen.setStyle(Qt::SolidLine);
            } else {
                pen.setStyle(gl.style);
            }
            painter.setPen(pen);
            painter.drawLine(rulerSize, static_cast<int>(y), width(), static_cast<int>(y));
        }
    }

    for (size_t i = 0; i < verticalGuides.size(); ++i) {
        const auto& gl = verticalGuides[i];
        float x = canvasX + gl.position * scale;
        if (x >= rulerSize && x < width()) {
            QPen pen(gl.color);
            pen.setWidth(1);
            if (static_cast<int>(i) == activeGuideIndex && !isActiveHorizontal) {
                pen.setStyle(Qt::SolidLine);
                pen.setWidth(2);
                if (isSnapped) {
                    pen.setColor(QColor(255, 215, 0));
                }
            } else if (static_cast<int>(i) == hoveredGuideIndex && !isHoveredHorizontal) {
                pen.setStyle(Qt::SolidLine);
            } else {
                pen.setStyle(gl.style);
            }
            painter.setPen(pen);
            painter.drawLine(static_cast<int>(x), rulerSize, static_cast<int>(x), height());
        }
    }

    painter.fillRect(0, 0, rulerSize, rulerSize, QColor(40, 40, 40));
    painter.setPen(QColor(65, 65, 65));
    painter.drawRect(0, 0, rulerSize, rulerSize);
}

void GuideLinesOverlay::drawRulers(QPainter& painter) {
    QPen tickPen(QColor(150, 150, 150));
    painter.setPen(tickPen);

    QFont font = painter.font();
    font.setPixelSize(9);
    painter.setFont(font);

    double step = 100.0;
    if (scale > 2.0f) step = 10.0;
    else if (scale > 0.8f) step = 50.0;
    else if (scale > 0.4f) step = 100.0;
    else if (scale > 0.15f) step = 200.0;
    else step = 500.0;

    double subStep = step / 10.0;

    double start_x = std::floor(((rulerSize - canvasX) / scale) / subStep) * subStep;
    double end_x = (width() - canvasX) / scale;

    for (double cx = start_x; cx <= end_x; cx += subStep) {
        float x = canvasX + cx * scale;
        if (x < rulerSize || x >= width()) continue;

        long long tickIndex = std::round(cx / subStep);
        if (tickIndex % 10 == 0) {
            painter.drawLine(x, 0, x, rulerSize - 2);
            painter.drawText(x + 3, rulerSize - 4, QString::number(static_cast<int>(std::round(cx))));
        } else if (tickIndex % 5 == 0) {
            painter.drawLine(x, rulerSize / 2, x, rulerSize - 2);
        } else {
            painter.drawLine(x, rulerSize - 5, x, rulerSize - 2);
        }
    }

    double start_y = std::floor(((rulerSize - canvasY) / scale) / subStep) * subStep;
    double end_y = (height() - canvasY) / scale;

    for (double cy = start_y; cy <= end_y; cy += subStep) {
        float y = canvasY + cy * scale;
        if (y < rulerSize || y >= height()) continue;

        long long tickIndex = std::round(cy / subStep);
        if (tickIndex % 10 == 0) {
            painter.drawLine(0, y, rulerSize - 2, y);
            painter.save();
            painter.translate(2, y + 10);
            painter.rotate(-90);
            painter.drawText(0, 0, QString::number(static_cast<int>(std::round(cy))));
            painter.restore();
        } else if (tickIndex % 5 == 0) {
            painter.drawLine(rulerSize / 2, y, rulerSize - 2, y);
        } else {
            painter.drawLine(rulerSize - 5, y, rulerSize - 2, y);
        }
    }
}

void GuideLinesOverlay::mousePressEvent(QMouseEvent* event) {
    if (!guidesVisible) return;

    QPoint pos = event->pos();

    if (event->button() == Qt::RightButton) {
        int index = -1;
        bool isHorizontal = false;
        if (findGuideAt(pos, index, isHorizontal)) {
            showContextMenu(event->globalPosition().toPoint(), index, isHorizontal);
            return;
        }
    }

    if (pos.y() < rulerSize && pos.x() >= rulerSize) {
        isCreatingNew = true;
        isActiveHorizontal = true;
        activeGuideIndex = -1;
        setCursor(Qt::SplitVCursor);
        setMask(QRegion());
        getSnapCandidates(horizontalSnapCandidates, verticalSnapCandidates);
        isSnapped = false;
        return;
    }
    if (pos.x() < rulerSize && pos.y() >= rulerSize) {
        isCreatingNew = true;
        isActiveHorizontal = false;
        activeGuideIndex = -1;
        setCursor(Qt::SplitHCursor);
        setMask(QRegion());
        getSnapCandidates(horizontalSnapCandidates, verticalSnapCandidates);
        isSnapped = false;
        return;
    }

    if (findGuideAt(pos, activeGuideIndex, isActiveHorizontal)) {
        isCreatingNew = false;
        setCursor(isActiveHorizontal ? Qt::SplitVCursor : Qt::SplitHCursor);
        setMask(QRegion());
        getSnapCandidates(horizontalSnapCandidates, verticalSnapCandidates);
        isSnapped = false;
    }
}

void GuideLinesOverlay::mouseMoveEvent(QMouseEvent* event) {
    if (!guidesVisible) return;

    QPoint pos = event->pos();

    if (activeGuideIndex == -1 && !isCreatingNew) {
        if (findGuideAt(pos, hoveredGuideIndex, isHoveredHorizontal)) {
            setCursor(isHoveredHorizontal ? Qt::SplitVCursor : Qt::SplitHCursor);
        } else {
            hoveredGuideIndex = -1;
            setCursor(Qt::ArrowCursor);
        }
        update();
        return;
    }

    const float snapTolerancePixels = 6.0f;
    float snapToleranceCanvas = snapTolerancePixels / scale;
    bool snapped = false;

    if (isActiveHorizontal) {
        float canvasYVal = (pos.y() - canvasY) / scale;
        
        float bestVal = canvasYVal;
        float minDiff = snapToleranceCanvas;
        for (float candidate : horizontalSnapCandidates) {
            float diff = std::abs(canvasYVal - candidate);
            if (diff < minDiff) {
                minDiff = diff;
                bestVal = candidate;
                snapped = true;
            }
        }
        canvasYVal = bestVal;
        isSnapped = snapped;

        if (isCreatingNew) {
            if (pos.y() >= rulerSize) {
                GuideLine gl;
                gl.position = canvasYVal;
                gl.color = horizontalColor;
                gl.style = Qt::DashLine;
                horizontalGuides.push_back(gl);
                activeGuideIndex = static_cast<int>(horizontalGuides.size()) - 1;
                isCreatingNew = false;
            }
        } else if (activeGuideIndex >= 0 && activeGuideIndex < static_cast<int>(horizontalGuides.size())) {
            if (pos.y() < rulerSize) {
                horizontalGuides.erase(horizontalGuides.begin() + activeGuideIndex);
                activeGuideIndex = -1;
                setCursor(Qt::ArrowCursor);
            } else {
                horizontalGuides[activeGuideIndex].position = canvasYVal;
            }
        }
    } else {
        float canvasXVal = (pos.x() - canvasX) / scale;

        float bestVal = canvasXVal;
        float minDiff = snapToleranceCanvas;
        for (float candidate : verticalSnapCandidates) {
            float diff = std::abs(canvasXVal - candidate);
            if (diff < minDiff) {
                minDiff = diff;
                bestVal = candidate;
                snapped = true;
            }
        }
        canvasXVal = bestVal;
        isSnapped = snapped;

        if (isCreatingNew) {
            if (pos.x() >= rulerSize) {
                GuideLine gl;
                gl.position = canvasXVal;
                gl.color = verticalColor;
                gl.style = Qt::DashLine;
                verticalGuides.push_back(gl);
                activeGuideIndex = static_cast<int>(verticalGuides.size()) - 1;
                isCreatingNew = false;
            }
        } else if (activeGuideIndex >= 0 && activeGuideIndex < static_cast<int>(verticalGuides.size())) {
            if (pos.x() < rulerSize) {
                verticalGuides.erase(verticalGuides.begin() + activeGuideIndex);
                activeGuideIndex = -1;
                setCursor(Qt::ArrowCursor);
            } else {
                verticalGuides[activeGuideIndex].position = canvasXVal;
            }
        }
    }

    update();
}

void GuideLinesOverlay::mouseReleaseEvent(QMouseEvent* event) {
    Q_UNUSED(event);
    if (activeGuideIndex != -1 || isCreatingNew) {
        activeGuideIndex = -1;
        isCreatingNew = false;
        isSnapped = false;
        setCursor(Qt::ArrowCursor);
        updateMask();
        update();
        if (!currentSceneName.isEmpty()) {
            saveGuides(currentSceneName);
        }
    }
}

QString GuideLinesOverlay::getSettingsFilePath() const {
    QString configPath = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
    QDir dir(configPath);
    dir.cdUp();
    dir.mkdir("plugin_config");
    dir.cd("plugin_config");
    dir.mkdir("obs-guide-lines");
    dir.cd("obs-guide-lines");
    return dir.absoluteFilePath("guides.json");
}

void GuideLinesOverlay::loadGuides(const QString& sceneName) {
    currentSceneName = sceneName;
    clearGuides();

    obs_source_t* sceneSource = obs_frontend_get_current_scene();
    connectSceneSignals(sceneSource);
    obs_source_release(sceneSource);

    if (sceneName.isEmpty()) return;

    QFile file(getSettingsFilePath());
    if (!file.open(QIODevice::ReadOnly)) return;

    QByteArray data = file.readAll();
    QJsonDocument doc = QJsonDocument::fromJson(data);
    if (doc.isNull()) return;

    QJsonObject root = doc.object();
    if (root.contains(sceneName)) {
        QJsonObject sceneData = root[sceneName].toObject();
        
        QJsonArray hArray = sceneData["horizontal"].toArray();
        for (const auto& val : hArray) {
            GuideLine gl;
            if (val.isObject()) {
                QJsonObject obj = val.toObject();
                gl.position = static_cast<float>(obj["position"].toDouble());
                gl.color = QColor(obj["color"].toString());
                gl.style = static_cast<Qt::PenStyle>(obj["style"].toInt());
            } else {
                gl.position = static_cast<float>(val.toDouble());
                gl.color = horizontalColor;
                gl.style = Qt::DashLine;
            }
            horizontalGuides.push_back(gl);
        }

        QJsonArray vArray = sceneData["vertical"].toArray();
        for (const auto& val : vArray) {
            GuideLine gl;
            if (val.isObject()) {
                QJsonObject obj = val.toObject();
                gl.position = static_cast<float>(obj["position"].toDouble());
                gl.color = QColor(obj["color"].toString());
                gl.style = static_cast<Qt::PenStyle>(obj["style"].toInt());
            } else {
                gl.position = static_cast<float>(val.toDouble());
                gl.color = verticalColor;
                gl.style = Qt::DashLine;
            }
            verticalGuides.push_back(gl);
        }
    }
    updateMask();
    update();
}

void GuideLinesOverlay::saveGuides(const QString& sceneName) {
    if (sceneName.isEmpty()) return;

    QString filePath = getSettingsFilePath();
    QJsonObject root;

    QFile file(filePath);
    if (file.open(QIODevice::ReadOnly)) {
        QByteArray data = file.readAll();
        QJsonDocument doc = QJsonDocument::fromJson(data);
        if (!doc.isNull()) {
            root = doc.object();
        }
        file.close();
    }

    QJsonObject sceneData;
    QJsonArray hArray;
    for (const auto& gl : horizontalGuides) {
        QJsonObject obj;
        obj["position"] = gl.position;
        obj["color"] = gl.color.name(QColor::HexArgb);
        obj["style"] = static_cast<int>(gl.style);
        hArray.append(obj);
    }
    QJsonArray vArray;
    for (const auto& gl : verticalGuides) {
        QJsonObject obj;
        obj["position"] = gl.position;
        obj["color"] = gl.color.name(QColor::HexArgb);
        obj["style"] = static_cast<int>(gl.style);
        vArray.append(obj);
    }
    sceneData["horizontal"] = hArray;
    sceneData["vertical"] = vArray;

    root[sceneName] = sceneData;

    if (file.open(QIODevice::WriteOnly)) {
        QJsonDocument doc(root);
        file.write(doc.toJson());
        file.close();
    }
}
