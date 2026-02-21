#pragma once

#include <QMainWindow>

class QPoint;
class QObject;
class QEvent;
class QGraphicsDropShadowEffect;
class QHBoxLayout;
class QLabel;
class QLineEdit;
class QPushButton;
class QDnsLookup;
class QTcpSocket;
class QTimer;
class QToolButton;
class QWidget;
class QVBoxLayout;

class MainWindow final : public QMainWindow {
    Q_OBJECT

public:
    explicit MainWindow(QWidget *parent = nullptr);

protected:
    bool eventFilter(QObject *watched, QEvent *event) override;
    void changeEvent(QEvent *event) override;
    void showEvent(QShowEvent *event) override;
    void resizeEvent(QResizeEvent *event) override;

private:
    void updateWindowVisuals();
    void updateTitleBarIcons();
    void updateInputRegion();

    void startQuery();
    void startQueryFor(const QString &host, quint16 port, bool hasExplicitPort);
    void resetResultUi();

    void onSocketConnected();
    void onSocketReadyRead();
    void onSocketError();
    void onSocketDisconnected();
    void onQueryTimeout();

    void onAutoRefreshTimeout();
    void onRefreshTick();
    void updateProgressRingGeometry();

    void scheduleNextAutoRefresh();
    void stopAutoRefreshCountdown();

    void autoResizeWidthToFitMotd();

    void resolveAndConnect(const QString &host, quint16 port, bool hasExplicitPort);

    void sendHandshakeAndRequest(const QString &host, quint16 port);
    void processStatusResponseJson(const QByteArray &jsonUtf8);

    QWidget *m_root = nullptr;
    QVBoxLayout *m_rootLayout = nullptr;
    QWidget *m_frame = nullptr;
    QWidget *m_titleBar = nullptr;
    QWidget *m_content = nullptr;
    QHBoxLayout *m_titleBarLayout = nullptr;
    QToolButton *m_btnMinimize = nullptr;
    QToolButton *m_btnMaximize = nullptr;
    QToolButton *m_btnClose = nullptr;

    QGraphicsDropShadowEffect *m_shadow = nullptr;

    int m_radius = 12;
    int m_shadowBlur = 28;
    int m_shadowOffsetY = 8;
    int m_shadowMargin = 40;
    int m_resizeBorder = 8;

    bool m_resizing = false;
    Qt::Edges m_resizeEdges{};
    QRect m_resizeStartGeometry;
    QPoint m_resizePressGlobal;

    bool m_dragging = false;
    QPoint m_dragOffset;

    // ---- App UI (Minecraft status) ----
    QLineEdit *m_serverInput = nullptr;
    QPushButton *m_queryButton = nullptr;
    QLabel *m_iconLabel = nullptr;
    QLabel *m_nameLabel = nullptr;
    QLabel *m_playersLabel = nullptr;
    QWidget *m_resultCard = nullptr;

    // ---- Query state ----
    QTcpSocket *m_socket = nullptr;
    QTimer *m_timeoutTimer = nullptr;
    QByteArray m_readBuffer;

    QString m_targetHost;
    quint16 m_targetPort = 25565;
    bool m_targetHasExplicitPort = false;

    qint32 m_protocolVersion = 760;
    bool m_triedProtocolFallback = false;

    bool m_receivedStatusThisQuery = false;

    // ---- Auto refresh + progress ring ----
    QWidget *m_progressRing = nullptr;
    QTimer *m_autoRefreshTimer = nullptr;
    QTimer *m_refreshTickTimer = nullptr;
    int m_refreshIntervalMs = 10000;
    int m_refreshTickMs = 50;
    int m_refreshElapsedMs = 0;

    bool m_refreshCountdownActive = false;
    bool m_pendingAutoRefresh = false;

    bool m_hasLastSuccessfulQuery = false;
    QString m_lastHost;
    quint16 m_lastPort = 25565;
    bool m_lastHasExplicitPort = false;

    bool m_autoQueryOnStartupDone = false;
};
