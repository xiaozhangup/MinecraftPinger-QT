#include <QApplication>

#include "MainWindow.h"

int main(int argc, char *argv[]) {
    QApplication app(argc, argv);

    MainWindow w;
    w.setMinimumHeight(240);
    w.setMinimumWidth(240);
    w.adjustSize();
    // Keep a reasonable default width while letting height fit content.
    w.resize(std::max(420, w.sizeHint().width()), std::max(w.minimumHeight(), w.sizeHint().height()));
    w.show();

    return app.exec();
}
