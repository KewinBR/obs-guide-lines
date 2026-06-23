#include <obs-module.h>
#include <obs-frontend-api.h>
#include <plugin-support.h>
#include <QMainWindow>
#include <QAction>
#include <QMenu>
#include <QEvent>
#include <QResizeEvent>
#include <QScrollBar>
#include <QPushButton>
#include <QHBoxLayout>
#include <QApplication>
#include "guide-lines-overlay.hpp"

OBS_DECLARE_MODULE()
OBS_MODULE_USE_DEFAULT_LOCALE("obs-guide-lines", "en-US")

static GuideLinesOverlay* overlayWidget = nullptr;
static obs_hotkey_id toggleHotkeyId = OBS_INVALID_HOTKEY_ID;

// Qt Event Filter to keep overlay aligned and sized to preview widget
class PreviewResizeFilter : public QObject {
public:
    explicit PreviewResizeFilter(GuideLinesOverlay* overlay) : QObject(overlay), overlay(overlay) {}

protected:
    bool eventFilter(QObject* obj, QEvent* event) override {
        Q_UNUSED(obj);
        if (event->type() == QEvent::Resize ||
            event->type() == QEvent::Move ||
            event->type() == QEvent::Show ||
            event->type() == QEvent::Hide ||
            event->type() == QEvent::WindowStateChange) {
            overlay->updateOverlayGeometry();
        }
        return QObject::eventFilter(obj, event);
    }

private:
    GuideLinesOverlay* overlay;
};

// Event Filter to intercept Right-Click Context Menu on Scenes List
class SceneMenuFilter : public QObject {
public:
    explicit SceneMenuFilter(QObject* parent = nullptr) : QObject(parent) {}

protected:
    bool eventFilter(QObject* obj, QEvent* event) override {
        if (event->type() == QEvent::Show) {
            QMenu* menu = qobject_cast<QMenu*>(obj);
            if (menu) {
                // Check if this menu is the Scenes Context Menu by looking for actionRemoveScene
                bool isSceneMenu = false;
                for (QAction* action : menu->actions()) {
                    if (action->objectName() == "actionRemoveScene") {
                        isSceneMenu = true;
                        break;
                    }
                }
                
                if (isSceneMenu) {
                    // Check if we already added our custom actions to prevent duplicates
                    bool alreadyAdded = false;
                    for (QAction* action : menu->actions()) {
                        if (action->objectName() == "actionCopyGuideLayout") {
                            alreadyAdded = true;
                            break;
                        }
                    }
                    
                    if (!alreadyAdded) {
                        menu->addSeparator();
                        
                        QAction* copyLayout = menu->addAction(QObject::tr("Copiar layout de guias"));
                        copyLayout->setObjectName("actionCopyGuideLayout");
                        
                        QAction* pasteLayout = menu->addAction(QObject::tr("Colar layout de guias"));
                        pasteLayout->setObjectName("actionPasteGuideLayout");
                        
                        if (overlayWidget) {
                            pasteLayout->setEnabled(overlayWidget->hasCopiedGuides());
                            
                            QObject::connect(copyLayout, &QAction::triggered, []() {
                                if (overlayWidget) {
                                    overlayWidget->copyActiveSceneLayout();
                                }
                            });
                            
                            QObject::connect(pasteLayout, &QAction::triggered, []() {
                                if (overlayWidget) {
                                    overlayWidget->pasteActiveSceneLayout();
                                }
                            });
                        }
                    }
                }
            }
        }
        return QObject::eventFilter(obj, event);
    }
};

// Helper to get active scene name
static QString get_active_scene_name() {
    obs_source_t* scene_source = obs_frontend_get_current_scene();
    if (scene_source) {
        QString name = QString::fromUtf8(obs_source_get_name(scene_source));
        obs_source_release(scene_source);
        return name;
    }
    return "";
}

// Toggle visibility of guides
static void toggle_guides() {
    if (overlayWidget) {
        bool visible = !overlayWidget->isGuidesVisible();
        overlayWidget->setGuidesVisible(visible);
    }
}

// Hotkey callback
static void toggle_guides_hotkey(void* data, obs_hotkey_id id, obs_hotkey_t* hotkey, bool pressed) {
    Q_UNUSED(data);
    Q_UNUSED(id);
    Q_UNUSED(hotkey);
    if (pressed) {
        toggle_guides();
    }
}

// OBS Frontend Event Handler
static void frontend_event_handler(enum obs_frontend_event event, void* private_data) {
    Q_UNUSED(private_data);

    if (event == OBS_FRONTEND_EVENT_FINISHED_LOADING) {
        QMainWindow* main_window = static_cast<QMainWindow*>(obs_frontend_get_main_window());
        if (!main_window) return;

        QWidget* preview_widget = main_window->findChild<QWidget*>("preview");
        if (!preview_widget) return;

        // Create overlay widget
        overlayWidget = new GuideLinesOverlay(preview_widget);
        overlayWidget->updateOverlayGeometry();

        // Install event filters on both main window and preview
        PreviewResizeFilter* filter = new PreviewResizeFilter(overlayWidget);
        main_window->installEventFilter(filter);
        preview_widget->installEventFilter(filter);

        // Register scene context menu filter on application
        if (qApp) {
            qApp->installEventFilter(new SceneMenuFilter(qApp));
        }

        // Find scrollbars to update viewport on scroll
        QScrollBar* x_scrollbar = main_window->findChild<QScrollBar*>("previewXScrollBar");
        QScrollBar* y_scrollbar = main_window->findChild<QScrollBar*>("previewYScrollBar");

        if (x_scrollbar) {
            QObject::connect(x_scrollbar, &QScrollBar::valueChanged, overlayWidget, &GuideLinesOverlay::onViewportChanged);
        }
        if (y_scrollbar) {
            QObject::connect(y_scrollbar, &QScrollBar::valueChanged, overlayWidget, &GuideLinesOverlay::onViewportChanged);
        }

        // Connect scalingChanged signal
        QObject::connect(preview_widget, SIGNAL(scalingChanged(float)), overlayWidget, SLOT(onViewportChanged()));
        // Connect DisplayResized signal to catch panning drag updates
        QObject::connect(preview_widget, SIGNAL(DisplayResized()), overlayWidget, SLOT(onViewportChanged()));

        // Add "Régua" button next to "Filtros" in the context toolbar
        QPushButton* filters_button = main_window->findChild<QPushButton*>("sourceFiltersButton");
        if (filters_button) {
            QWidget* parent_widget = filters_button->parentWidget();
            if (parent_widget && parent_widget->layout()) {
                QHBoxLayout* button_layout = qobject_cast<QHBoxLayout*>(parent_widget->layout());
                if (button_layout) {
                    int index = -1;
                    for (int i = 0; i < button_layout->count(); ++i) {
                        if (button_layout->itemAt(i)->widget() == filters_button) {
                            index = i;
                            break;
                        }
                    }
                    
                    if (index != -1) {
                        QPushButton* rulerBtn = new QPushButton(parent_widget);
                        rulerBtn->setObjectName("rulerToggleButton");
                        rulerBtn->setCheckable(true);
                        rulerBtn->setChecked(overlayWidget ? overlayWidget->isGuidesVisible() : true);
                        rulerBtn->setText(QObject::tr("Régua"));
                        rulerBtn->setMinimumSize(QSize(0, 22));
                        
                        QIcon eyeIcon;
                        eyeIcon.addFile(":/res/images/visible.svg", QSize(), QIcon::Normal, QIcon::On);
                        eyeIcon.addFile(":/res/images/invisible.svg", QSize(), QIcon::Normal, QIcon::Off);
                        rulerBtn->setIcon(eyeIcon);
                        
                        button_layout->insertWidget(index + 1, rulerBtn);
                        
                        QObject::connect(rulerBtn, &QPushButton::toggled, [](bool checked) {
                            if (overlayWidget) {
                                overlayWidget->setGuidesVisible(checked);
                            }
                        });
                    }
                }
            }
        }

        // Load guides for the initially loaded scene
        QString initialScene = get_active_scene_name();
        if (!initialScene.isEmpty()) {
            overlayWidget->loadGuides(initialScene);
        }

        // Add menu item in Tools -> OBS Guide Lines
        QAction* toggleAction = (QAction*)obs_frontend_add_tools_menu_qaction("OBS Guide Lines");
        if (toggleAction) {
            toggleAction->setCheckable(true);
            toggleAction->setChecked(true);
            
            QObject::connect(toggleAction, &QAction::toggled, [](bool checked) {
                if (overlayWidget) {
                    overlayWidget->setGuidesVisible(checked);
                }
            });
        }

    } else if (event == OBS_FRONTEND_EVENT_SCENE_CHANGED) {
        if (overlayWidget) {
            QString sceneName = get_active_scene_name();
            overlayWidget->loadGuides(sceneName);
        }
    }
}

bool obs_module_load(void) {
    obs_log(LOG_INFO, "OBS Guide Lines loaded (version %s)", PLUGIN_VERSION);

    obs_frontend_add_event_callback(frontend_event_handler, nullptr);

    toggleHotkeyId = obs_hotkey_register_frontend(
        "obs_guide_lines_toggle",
        "Toggle OBS Guide Lines",
        toggle_guides_hotkey,
        nullptr
    );

    return true;
}

void obs_module_unload(void) {
    obs_log(LOG_INFO, "OBS Guide Lines unloaded");
}
