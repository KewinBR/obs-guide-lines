#pragma once

#include <QWidget>
#include <QColor>
#include <vector>
#include <string>
#include <map>

class GuideLinesOverlay : public QWidget {
    Q_OBJECT

public:
public:
    struct GuideLine {
        float position;
        QColor color;
        Qt::PenStyle style = Qt::DashLine;
    };

    explicit GuideLinesOverlay(QWidget* parent);
    ~GuideLinesOverlay() override;

    void setGuidesVisible(bool visible);
    bool isGuidesVisible() const { return guidesVisible; }

    void setColors(const QColor& horiz, const QColor& vert);
    QColor getHorizontalColor() const { return horizontalColor; }
    QColor getVerticalColor() const { return verticalColor; }

    void loadGuides(const QString& sceneName);
    void saveGuides(const QString& sceneName);
    void clearGuides();

    void updateViewportMetrics();
    void updateOverlayGeometry();
    void updateMask();

    void copyActiveSceneLayout();
    void pasteActiveSceneLayout();
    static bool hasCopiedGuides();

    void showContextMenu(const QPoint& globalPos, int index, bool isHorizontal);
    void connectSceneSignals(struct obs_source* sceneSource);
    void handleItemTransform(struct calldata* params);
    static void onItemTransform(void* data, struct calldata* params);

public slots:
    void onViewportChanged();

protected:
    void paintEvent(QPaintEvent* event) override;
    void mousePressEvent(QMouseEvent* event) override;
    void mouseMoveEvent(QMouseEvent* event) override;
    void mouseReleaseEvent(QMouseEvent* event) override;

private:
    bool guidesVisible = true;
    QColor horizontalColor = QColor(0, 190, 255); // Cyan default
    QColor verticalColor = QColor(255, 0, 128);   // Magenta default

    // Guides stored in canvas pixels
    std::vector<GuideLine> horizontalGuides;
    std::vector<GuideLine> verticalGuides;

    int activeGuideIndex = -1;
    bool isActiveHorizontal = false;
    bool isCreatingNew = false;

    // Viewport and scaling metrics
    float scale = 1.0f;
    float canvasX = 0.0f;
    float canvasY = 0.0f;

    int hoveredGuideIndex = -1;
    bool isHoveredHorizontal = false;

    std::vector<float> horizontalSnapCandidates;
    std::vector<float> verticalSnapCandidates;
    bool isSnapped = false;

    // Helper functions
    bool findGuideAt(const QPoint& pos, int& index, bool& isHorizontal);
    void getSnapCandidates(std::vector<float>& hCandidates, std::vector<float>& vCandidates);
    QString getSettingsFilePath() const;
    void drawRulers(QPainter& painter);

    QWidget* previewWidget = nullptr;
    struct obs_source* currentSceneSource = nullptr;
    QString currentSceneName;
    const int rulerSize = 20; // 20px rulers
};
