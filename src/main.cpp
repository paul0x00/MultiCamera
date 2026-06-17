#include <QApplication>

#include "AppTheme.h"
#include "MainWindow.h"

int main(int argc, char *argv[]) {
    // Qt6 默认已启用高分屏缩放；该属性仅在 Qt5 下需要显式设置。
#if QT_VERSION < QT_VERSION_CHECK(6, 0, 0)
    QApplication::setAttribute(Qt::AA_EnableHighDpiScaling);
    QApplication::setAttribute(Qt::AA_UseHighDpiPixmaps);
#endif
    QApplication app(argc, argv);
    app.setStyleSheet(theme::styleSheet());

    MainWindow window;
    window.show();

    return app.exec();
}
