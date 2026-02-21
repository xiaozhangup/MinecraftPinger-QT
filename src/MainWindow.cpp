#include "MainWindow.h"

#include <algorithm>
#include <cmath>
#include <vector>

#include <QAbstractSocket>
#include <QByteArray>
#include <QDnsLookup>
#include <QFont>
#include <QFontMetrics>
#include <QHostAddress>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonValue>
#include <QLineEdit>
#include <QPainter>
#include <QPixmap>
#include <QPushButton>
#include <QTcpSocket>
#include <QTimer>
#include <QNetworkProxy>

#include <QColor>
#include <QApplication>
#include <QEvent>
#include <QHBoxLayout>
#include <QLabel>
#include <QMouseEvent>
#include <QPoint>
#include <QResizeEvent>
#include <QShowEvent>
#include <QStyle>
#include <QToolButton>
#include <QtGlobal>
#include <QGraphicsDropShadowEffect>
#include <QGuiApplication>
#include <QScreen>
#include <QSizePolicy>
#include <QVBoxLayout>
#include <QWindow>
#include <QWidget>

#if defined(HAVE_X11_SHAPE)
    #include <QGuiApplication>
    #include <X11/Xlib.h>
    #include <X11/extensions/shape.h>

    // X11 headers define CursorShape as a macro which breaks Qt::CursorShape.
    #ifdef CursorShape
        #undef CursorShape
    #endif
#endif

namespace {
Qt::Edges hitTestEdges(const QPoint &pos, const QRect &rect, int border) {
    Qt::Edges edges{};

    if (pos.x() <= rect.left() + border) {
        edges |= Qt::LeftEdge;
    } else if (pos.x() >= rect.right() - border) {
        edges |= Qt::RightEdge;
    }

    if (pos.y() <= rect.top() + border) {
        edges |= Qt::TopEdge;
    } else if (pos.y() >= rect.bottom() - border) {
        edges |= Qt::BottomEdge;
    }

    return edges;
}

Qt::CursorShape cursorForEdges(Qt::Edges edges) {
    const bool left = edges.testFlag(Qt::LeftEdge);
    const bool right = edges.testFlag(Qt::RightEdge);
    const bool top = edges.testFlag(Qt::TopEdge);
    const bool bottom = edges.testFlag(Qt::BottomEdge);

    if ((left && top) || (right && bottom)) {
        return Qt::SizeFDiagCursor;
    }
    if ((right && top) || (left && bottom)) {
        return Qt::SizeBDiagCursor;
    }
    if (left || right) {
        return Qt::SizeHorCursor;
    }
    if (top || bottom) {
        return Qt::SizeVerCursor;
    }
    return Qt::ArrowCursor;
}
} // namespace

QIcon themedOrStandardIcon(QStyle *style, const QString &themeName, QStyle::StandardPixmap fallback) {
    QIcon icon = QIcon::fromTheme(themeName);
    if (!icon.isNull()) {
        return icon;
    }
    return style ? style->standardIcon(fallback) : QIcon();
}

class ProgressRing final : public QWidget {
public:
    explicit ProgressRing(QWidget *parent = nullptr) : QWidget(parent) {
        setAttribute(Qt::WA_TransparentForMouseEvents, true);
        setAttribute(Qt::WA_NoSystemBackground, true);
        setAttribute(Qt::WA_TranslucentBackground, true);
    }

    void setProgress(double p) {
        m_indeterminate = false;
        m_progress = std::clamp(p, 0.0, 1.0);
        update();
    }

    void setIndeterminate(bool on) {
        if (m_indeterminate == on) {
            return;
        }
        m_indeterminate = on;
        update();
    }

    bool isIndeterminate() const {
        return m_indeterminate;
    }

    void advanceSpinner() {
        // Roughly one rotation per ~1s when tick is 50ms.
        m_phaseDeg += 18.0;
        if (m_phaseDeg >= 360.0) {
            m_phaseDeg -= 360.0;
        }
        update();
    }

protected:
    void paintEvent(QPaintEvent *) override {
        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing, true);

        const int w = width();
        const int h = height();
        const int side = std::min(w, h);
        const int pad = 2;
        const QRectF rect((w - side) / 2.0 + pad,
                          (h - side) / 2.0 + pad,
                          side - pad * 2.0,
                          side - pad * 2.0);

        QPen bgPen(QColor(255, 255, 255, 70));
        bgPen.setWidthF(3.0);
        bgPen.setCapStyle(Qt::RoundCap);

        QPen fgPen(QColor(255, 255, 255, 220));
        fgPen.setWidthF(3.0);
        fgPen.setCapStyle(Qt::RoundCap);

        if (m_indeterminate) {
            // A single white dot rotating on a faint track.
            painter.setPen(bgPen);
            painter.drawArc(rect, 90 * 16, -360 * 16);

            constexpr double kPi = 3.14159265358979323846;
            const QPointF c = rect.center();
            const double radius = rect.width() / 2.0;
            const double angleRad = (m_phaseDeg - 90.0) * kPi / 180.0;
            const QPointF p(c.x() + radius * std::cos(angleRad), c.y() + radius * std::sin(angleRad));

            painter.setPen(Qt::NoPen);
            painter.setBrush(QColor(255, 255, 255, 230));
            painter.drawEllipse(p, 2.3, 2.3);
            return;
        }

        painter.setPen(bgPen);
        painter.drawArc(rect, 90 * 16, -360 * 16);

        painter.setPen(fgPen);
        const int span = static_cast<int>(-360.0 * 16.0 * m_progress);
        painter.drawArc(rect, 90 * 16, span);
    }

private:
    double m_progress = 0.0;
    bool m_indeterminate = false;
    double m_phaseDeg = 0.0;
};

QByteArray writeVarInt(qint32 value) {
    QByteArray out;
    quint32 v = static_cast<quint32>(value);
    do {
        quint8 temp = static_cast<quint8>(v & 0x7F);
        v >>= 7;
        if (v != 0) {
            temp |= 0x80;
        }
        out.append(static_cast<char>(temp));
    } while (v != 0);
    return out;
}

bool readVarInt(QByteArray &buffer, qint32 &value) {
    value = 0;
    int numRead = 0;
    quint8 readByte = 0;
    do {
        if (buffer.isEmpty()) {
            return false;
        }
        readByte = static_cast<quint8>(buffer.at(0));
        buffer.remove(0, 1);

        const int v = (readByte & 0x7F);
        value |= (v << (7 * numRead));

        numRead++;
        if (numRead > 5) {
            return false;
        }
    } while ((readByte & 0x80) != 0);
    return true;
}

QByteArray writeString(const QString &s) {
    const QByteArray utf8 = s.toUtf8();
    return writeVarInt(utf8.size()) + utf8;
}

QString flattenMinecraftText(const QJsonValue &v) {
    if (v.isString()) {
        return v.toString();
    }
    if (!v.isObject()) {
        return {};
    }
    const QJsonObject obj = v.toObject();
    QString result;
    if (obj.contains("text") && obj.value("text").isString()) {
        result += obj.value("text").toString();
    }
    if (obj.contains("extra") && obj.value("extra").isArray()) {
        const QJsonArray extra = obj.value("extra").toArray();
        for (const QJsonValue &child : extra) {
            result += flattenMinecraftText(child);
        }
    }
    return result;
}

bool parseHostPort(const QString &input, QString &host, quint16 &port, bool &hasExplicitPort) {
    QString s = input.trimmed();
    if (s.isEmpty()) {
        return false;
    }

    hasExplicitPort = false;

    // IPv6 in brackets: [::1]:25565
    if (s.startsWith('[')) {
        const int end = s.indexOf(']');
        if (end < 0) {
            return false;
        }
        host = s.mid(1, end - 1);
        port = 25565;
        if (end + 1 < s.size() && s.at(end + 1) == ':') {
            bool ok = false;
            const int p = s.mid(end + 2).toInt(&ok);
            if (!ok || p <= 0 || p > 65535) {
                return false;
            }
            port = static_cast<quint16>(p);
            hasExplicitPort = true;
        }
        return !host.isEmpty();
    }

    // host:port (avoid treating IPv6 without brackets)
    const int colonCount = s.count(':');
    if (colonCount == 1) {
        const int idx = s.indexOf(':');
        host = s.left(idx).trimmed();
        bool ok = false;
        const int p = s.mid(idx + 1).trimmed().toInt(&ok);
        if (!ok || p <= 0 || p > 65535) {
            return false;
        }
        port = static_cast<quint16>(p);
        hasExplicitPort = true;
        return !host.isEmpty();
    }

    host = s;
    port = 25565;
    return !host.isEmpty();
}

MainWindow::MainWindow(QWidget *parent) : QMainWindow(parent) {
    setWindowTitle("MinecraftPinger");

    // Rounded corners + shadow are easiest with a frameless, translucent top-level window.
    setWindowFlags(windowFlags() | Qt::FramelessWindowHint);
    setAttribute(Qt::WA_TranslucentBackground, true);

    // Make sure the QMainWindow itself doesn't paint an opaque background.
    setStyleSheet("QMainWindow { background: transparent; }");

    m_root = new QWidget(this);
    m_root->setContentsMargins(0, 0, 0, 0);
    m_root->setMouseTracking(true);
    m_root->installEventFilter(this);

    m_rootLayout = new QVBoxLayout(m_root);
    m_rootLayout->setContentsMargins(m_shadowMargin, m_shadowMargin, m_shadowMargin, m_shadowMargin);
    m_rootLayout->setSpacing(0);

    m_frame = new QWidget(m_root);
    m_frame->setObjectName("roundedFrame");
    // Follow system theme colors.
    m_frame->setStyleSheet(QString(
                                  "#roundedFrame {"
                                  "  background-color: palette(window);"
                                  "  border-radius: %1px;"
                                  "}")
                                  .arg(m_radius));
    m_frame->setMouseTracking(true);
    m_frame->installEventFilter(this);

    m_shadow = new QGraphicsDropShadowEffect(m_frame);
    m_shadow->setBlurRadius(m_shadowBlur);
    m_shadow->setOffset(0, m_shadowOffsetY);
    m_shadow->setColor(QColor(0, 0, 0, 120));
    m_frame->setGraphicsEffect(m_shadow);

    auto *frameLayout = new QVBoxLayout(m_frame);
    frameLayout->setContentsMargins(0, 0, 0, 0);
    frameLayout->setSpacing(0);

    // Title bar (custom buttons)
    m_titleBar = new QWidget(m_frame);
    m_titleBar->setObjectName("titleBar");
    m_titleBar->setFixedHeight(40);
    m_titleBar->setMouseTracking(true);
    m_titleBar->installEventFilter(this);

    m_titleBarLayout = new QHBoxLayout(m_titleBar);
    m_titleBarLayout->setContentsMargins(12, 8, 8, 8);
    m_titleBarLayout->setSpacing(6);

    // Put query controls into the title bar.
    m_serverInput = new QLineEdit(m_titleBar);
    m_serverInput->setPlaceholderText(tr("输入 Minecraft Java 服务器 IP/域名（可选 :端口）"));
    m_serverInput->setClearButtonEnabled(true);
    m_serverInput->setText(QStringLiteral("happylandmc.cc"));
    m_serverInput->setCursorPosition(m_serverInput->text().size());
    m_serverInput->setFixedHeight(24);
    m_serverInput->setMinimumWidth(240);

    m_queryButton = new QPushButton(tr("查询"), m_titleBar);
    m_queryButton->setFixedHeight(24);
    m_queryButton->setDefault(true);
    m_queryButton->setAutoDefault(true);

    m_btnMinimize = new QToolButton(m_titleBar);
    m_btnMaximize = new QToolButton(m_titleBar);
    m_btnClose = new QToolButton(m_titleBar);

    for (auto *btn : {m_btnMinimize, m_btnMaximize, m_btnClose}) {
        btn->setAutoRaise(true);
        btn->setCursor(Qt::ArrowCursor);
        btn->setFixedSize(28, 24);
    }

    m_btnMinimize->setToolTip(tr("Minimize"));
    m_btnMaximize->setToolTip(tr("Maximize"));
    m_btnClose->setToolTip(tr("Close"));

    updateTitleBarIcons();

    connect(m_btnMinimize, &QToolButton::clicked, this, &QWidget::showMinimized);
    connect(m_btnMaximize, &QToolButton::clicked, this, [this] {
        if (isMaximized()) {
            showNormal();
        } else {
            showMaximized();
        }
    });
    connect(m_btnClose, &QToolButton::clicked, this, &QWidget::close);

    m_titleBarLayout->addWidget(m_serverInput, 0);
    m_titleBarLayout->addWidget(m_queryButton, 0);
    m_titleBarLayout->addStretch(1);
    m_titleBarLayout->addWidget(m_btnMinimize);
    m_titleBarLayout->addWidget(m_btnMaximize);
    m_titleBarLayout->addWidget(m_btnClose);

    // Content (Minecraft query UI)
    m_content = new QWidget(m_frame);
    m_content->setMouseTracking(true);
    m_content->installEventFilter(this);

    auto *contentLayout = new QVBoxLayout(m_content);
    contentLayout->setContentsMargins(16, 16, 16, 16);
    contentLayout->setSpacing(12);
    contentLayout->setSizeConstraint(QLayout::SetMinimumSize);

    // Result card
    auto *card = new QWidget(m_content);
    card->setObjectName("resultCard");
    card->setStyleSheet(
        "#resultCard {"
        "  background-color: palette(base);"
        "  border: 1px solid palette(mid);"
        "  border-radius: 10px;"
        "}");

    auto *cardLayout = new QHBoxLayout(card);
    cardLayout->setContentsMargins(12, 12, 12, 12);
    cardLayout->setSpacing(12);

    m_iconLabel = new QLabel(card);
    m_iconLabel->setFixedSize(64, 64);
    m_iconLabel->setScaledContents(true);

    auto *rightInfo = new QWidget(card);
    auto *rightInfoLayout = new QVBoxLayout(rightInfo);
    rightInfoLayout->setContentsMargins(0, 0, 0, 0);
    rightInfoLayout->setSpacing(6);

    m_nameLabel = new QLabel(tr("请输入服务器并点击查询"), rightInfo);
    m_nameLabel->setWordWrap(true);
    QFont nameFont = m_nameLabel->font();
    nameFont.setPointSize(std::max(10, nameFont.pointSize() + 2));
    nameFont.setBold(true);
    m_nameLabel->setFont(nameFont);

    m_playersLabel = new QLabel(QString(), rightInfo);
    m_playersLabel->setStyleSheet("color: white;");
    m_playersLabel->setAlignment(Qt::AlignHCenter);
    m_playersLabel->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);

    rightInfoLayout->addWidget(m_nameLabel);
    rightInfoLayout->addWidget(m_playersLabel);
    rightInfoLayout->addStretch(1);

    cardLayout->addWidget(m_iconLabel, 0);
    cardLayout->addWidget(rightInfo, 1);
    contentLayout->addWidget(card, 1);

    m_resultCard = card;

    resetResultUi();

    frameLayout->addWidget(m_titleBar);
    frameLayout->addWidget(m_content);

    m_rootLayout->addWidget(m_frame);
    setCentralWidget(m_root);

    updateWindowVisuals();
    updateInputRegion();

    // Catch mouse events even when child widgets handle them.
    if (qApp) {
        qApp->installEventFilter(this);
    }

    // Ensure we still get hover/move events over child widgets (for resize cursors).
    for (auto *child : findChildren<QWidget *>()) {
        child->setMouseTracking(true);
    }

    // Network
    m_socket = new QTcpSocket(this);
    m_socket->setProxy(QNetworkProxy::NoProxy);
    m_timeoutTimer = new QTimer(this);
    m_timeoutTimer->setSingleShot(true);
    m_timeoutTimer->setInterval(6000);

    connect(m_queryButton, &QPushButton::clicked, this, &MainWindow::startQuery);
    connect(m_serverInput, &QLineEdit::returnPressed, this, &MainWindow::startQuery);

    connect(m_socket, &QTcpSocket::connected, this, &MainWindow::onSocketConnected);
    connect(m_socket, &QTcpSocket::readyRead, this, &MainWindow::onSocketReadyRead);
    connect(m_socket, &QTcpSocket::disconnected, this, &MainWindow::onSocketDisconnected);
    connect(m_socket,
            QOverload<QAbstractSocket::SocketError>::of(&QAbstractSocket::errorOccurred),
            this,
            &MainWindow::onSocketError);
    connect(m_timeoutTimer, &QTimer::timeout, this, &MainWindow::onQueryTimeout);

    // Auto refresh + ring (placed inside the server info card)
    m_progressRing = new ProgressRing(m_resultCard);
    m_progressRing->setFixedSize(22, 22);
    m_progressRing->hide();

    m_autoRefreshTimer = new QTimer(this);
    m_autoRefreshTimer->setSingleShot(true);
    m_autoRefreshTimer->setInterval(m_refreshIntervalMs);
    connect(m_autoRefreshTimer, &QTimer::timeout, this, &MainWindow::onAutoRefreshTimeout);

    m_refreshTickTimer = new QTimer(this);
    m_refreshTickTimer->setInterval(m_refreshTickMs);
    connect(m_refreshTickTimer, &QTimer::timeout, this, &MainWindow::onRefreshTick);
}

void MainWindow::resetResultUi() {
    if (m_iconLabel) {
        QPixmap placeholder(64, 64);
        placeholder.fill(Qt::transparent);
        m_iconLabel->setPixmap(placeholder);
    }
    if (m_nameLabel) {
        m_nameLabel->setText(tr("等待查询…"));
    }
    if (m_playersLabel) {
        m_playersLabel->setText(QString());
    }
}

void MainWindow::startQuery() {
    if (!m_serverInput || !m_queryButton || !m_socket) {
        return;
    }

    QString host;
    quint16 port = 25565;
    bool hasExplicitPort = false;
    if (!parseHostPort(m_serverInput->text(), host, port, hasExplicitPort)) {
        m_nameLabel->setText(tr("地址格式不正确"));
        m_playersLabel->setText(tr("示例：example.com 或 example.com:25565 或 [::1]:25565"));
        return;
    }

    startQueryFor(host, port, hasExplicitPort);
}

void MainWindow::startQueryFor(const QString &host, quint16 port, bool hasExplicitPort) {
    if (!m_queryButton || !m_socket) {
        return;
    }
    if (!m_queryButton->isEnabled()) {
        // Avoid overlapping queries.
        return;
    }

    // Any manual/auto query cancels the current countdown; we'll restart it after we receive data.
    stopAutoRefreshCountdown();
    m_pendingAutoRefresh = false;

    m_targetHost = host;
    m_targetPort = port;
    m_targetHasExplicitPort = hasExplicitPort;
    m_triedProtocolFallback = false;
    m_protocolVersion = 760;
    m_receivedStatusThisQuery = false;

    m_queryButton->setEnabled(false);
    m_queryButton->setText(tr("查询中…"));

    // Show an indeterminate spinner while waiting for server response.
    if (m_progressRing) {
        auto *ring = static_cast<ProgressRing *>(m_progressRing);
        ring->setIndeterminate(true);
        m_progressRing->show();
        updateProgressRingGeometry();
    }
    if (m_refreshTickTimer && !m_refreshTickTimer->isActive()) {
        m_refreshTickTimer->start();
    }

    m_readBuffer.clear();
    // Keep old info while refreshing to avoid empty gaps.
    if (!m_hasLastSuccessfulQuery) {
        resetResultUi();
    }

    m_socket->abort();
    m_socket->setProxy(QNetworkProxy::NoProxy);
    resolveAndConnect(host, port, hasExplicitPort);
    if (m_timeoutTimer) {
        m_timeoutTimer->start();
    }
}

void MainWindow::onSocketConnected() {
    sendHandshakeAndRequest(m_targetHost, m_targetPort);
}

void MainWindow::resolveAndConnect(const QString &host, quint16 port, bool hasExplicitPort) {
    // If user provided a port explicitly, connect directly.
    if (hasExplicitPort) {
        m_socket->connectToHost(host, port);
        return;
    }

    // If host looks like an IP address, SRV doesn't apply.
    QHostAddress addr;
    if (addr.setAddress(host)) {
        m_socket->connectToHost(host, port);
        return;
    }

    // Try Minecraft SRV record: _minecraft._tcp.<host>
    auto *dns = new QDnsLookup(QDnsLookup::SRV, QStringLiteral("_minecraft._tcp.%1").arg(host), this);
    connect(dns, &QDnsLookup::finished, this, [this, dns, host, port] {
        dns->deleteLater();
        if (dns->error() == QDnsLookup::NoError && !dns->serviceRecords().isEmpty()) {
            const auto rec = dns->serviceRecords().constFirst();
            const QString target = rec.target().isEmpty() ? host : rec.target();
            const quint16 srvPort = rec.port() == 0 ? port : rec.port();
            m_targetHost = target;
            m_targetPort = srvPort;
            m_socket->connectToHost(m_targetHost, m_targetPort);
            return;
        }
        // Fallback: connect to original host:25565
        m_socket->connectToHost(host, port);
    });
    dns->lookup();
}

void MainWindow::sendHandshakeAndRequest(const QString &host, quint16 port) {
    if (!m_socket) {
        return;
    }

    // Protocol version can cause some servers/proxies to close the connection.
    // We start modern, and may fallback on RemoteHostClosed.
    const qint32 protocol = m_protocolVersion;

    QByteArray handshake;
    handshake += writeVarInt(0x00);                 // packet id
    handshake += writeVarInt(protocol);             // protocol
    handshake += writeString(host);                 // server address
    handshake += QByteArray(1, static_cast<char>((port >> 8) & 0xFF));
    handshake += QByteArray(1, static_cast<char>(port & 0xFF));
    handshake += writeVarInt(0x01); // next state: status

    const QByteArray handshakePacket = writeVarInt(handshake.size()) + handshake;

    QByteArray request;
    request += writeVarInt(0x00); // status request packet id
    const QByteArray requestPacket = writeVarInt(request.size()) + request;

    m_socket->write(handshakePacket);
    m_socket->write(requestPacket);
    m_socket->flush();
}

void MainWindow::onSocketReadyRead() {
    if (!m_socket) {
        return;
    }
    m_readBuffer += m_socket->readAll();

    // Parse: [length VarInt][packetId VarInt][json string]
    // We may not have all bytes yet.
    QByteArray buf = m_readBuffer;

    qint32 packetLength = 0;
    if (!readVarInt(buf, packetLength)) {
        return;
    }
    if (packetLength < 0 || buf.size() < packetLength) {
        return;
    }

    QByteArray packetData = buf.left(packetLength);
    buf.remove(0, packetLength);

    // Commit consumption
    m_readBuffer = buf;

    qint32 packetId = 0;
    if (!readVarInt(packetData, packetId) || packetId != 0x00) {
        m_nameLabel->setText(tr("响应格式不正确"));
        onQueryTimeout();
        return;
    }

    qint32 jsonLen = 0;
    if (!readVarInt(packetData, jsonLen) || jsonLen < 0 || packetData.size() < jsonLen) {
        return;
    }
    const QByteArray jsonUtf8 = packetData.left(jsonLen);
    processStatusResponseJson(jsonUtf8);
    m_receivedStatusThisQuery = true;

    if (m_timeoutTimer) {
        m_timeoutTimer->stop();
    }

    // Mark UI as completed before initiating disconnect to avoid a synchronous
    // disconnected() signal overwriting the successful result.
    m_queryButton->setEnabled(true);
    m_queryButton->setText(tr("查询"));

    // If an auto refresh fired while we were querying, run one more refresh immediately.
    if (m_pendingAutoRefresh && m_hasLastSuccessfulQuery) {
        m_pendingAutoRefresh = false;
        stopAutoRefreshCountdown();
        startQueryFor(m_lastHost, m_lastPort, m_lastHasExplicitPort);
        return;
    }

    m_socket->disconnectFromHost();
}

void MainWindow::processStatusResponseJson(const QByteArray &jsonUtf8) {
    QJsonParseError err{};
    const QJsonDocument doc = QJsonDocument::fromJson(jsonUtf8, &err);
    if (err.error != QJsonParseError::NoError || !doc.isObject()) {
        m_nameLabel->setText(tr("解析失败"));
        m_playersLabel->setText(QString::fromUtf8(jsonUtf8.left(200)));
        return;
    }
    const QJsonObject obj = doc.object();

    // MOTD
    QString motd;
    if (obj.contains("description")) {
        motd = flattenMinecraftText(obj.value("description"));
    }
    if (motd.trimmed().isEmpty()) {
            motd = tr("(无 MOTD)");
    }
    m_nameLabel->setText(motd);

    // Players
    int online = -1;
    int max = -1;
    if (obj.contains("players") && obj.value("players").isObject()) {
        const QJsonObject players = obj.value("players").toObject();
        online = players.value("online").toInt(-1);
        max = players.value("max").toInt(-1);
    }
    if (online >= 0 && max >= 0) {
        m_playersLabel->setText(tr("%1 / %2 在线").arg(online).arg(max));
    } else {
        m_playersLabel->setText(tr("人数未知"));
    }

    // Icon
    if (obj.contains("favicon") && obj.value("favicon").isString()) {
        const QString fav = obj.value("favicon").toString();
        const QString prefix = QStringLiteral("data:image/png;base64,");
        if (fav.startsWith(prefix)) {
            const QByteArray pngBytes = QByteArray::fromBase64(fav.mid(prefix.size()).toUtf8());
            QPixmap pix;
            if (pix.loadFromData(pngBytes, "PNG")) {
                m_iconLabel->setPixmap(pix.scaled(m_iconLabel->size(), Qt::KeepAspectRatio, Qt::SmoothTransformation));
            }
        }
    }

    // Remember as last successful query and enable auto refresh.
    m_hasLastSuccessfulQuery = true;
    m_lastHost = m_targetHost;
    m_lastPort = m_targetPort;
    m_lastHasExplicitPort = m_targetHasExplicitPort;

    // Start the 10s countdown AFTER we have shown fresh data.
    scheduleNextAutoRefresh();

    // Auto resize width so MOTD can be fully visible.
    // Defer to the next event loop iteration so layouts/sizeHints are up-to-date.
    QTimer::singleShot(0, this, [this] { autoResizeWidthToFitMotd(); });
}

void MainWindow::autoResizeWidthToFitMotd() {
    if (isMaximized() || isFullScreen()) {
        return;
    }
    if (!m_root || !m_rootLayout || !m_nameLabel) {
        return;
    }

    const QString motd = m_nameLabel->text().trimmed();
    if (motd.isEmpty()) {
        return;
    }

    QScreen *scr = nullptr;
    if (windowHandle() && windowHandle()->screen()) {
        scr = windowHandle()->screen();
    } else {
        scr = QGuiApplication::primaryScreen();
    }

    const int availableW = scr ? scr->availableGeometry().width() : 1200;
    const int maxWindowW = std::max(400, static_cast<int>(availableW * 0.90));
    const int minWindowW = std::max(minimumWidth(), 0);

    // Compute target width from text metrics so it works even before layouts update (e.g. first auto query).
    const QFontMetrics fmName(m_nameLabel->font());
    int motdTextW = 0;
    const QStringList lines = motd.split('\n');
    for (const QString &line : lines) {
        motdTextW = std::max(motdTextW, fmName.horizontalAdvance(line));
    }

    int playersTextW = 0;
    if (m_playersLabel) {
        const QFontMetrics fmPlayers(m_playersLabel->font());
        playersTextW = fmPlayers.horizontalAdvance(m_playersLabel->text());
    }

    const int rightInfoW = std::max(motdTextW, playersTextW) + 8; // small padding

    int cardLR = 0;
    int cardSpacing = 12;
    if (m_resultCard && m_resultCard->layout()) {
        const QMargins cm = m_resultCard->layout()->contentsMargins();
        cardLR = cm.left() + cm.right();
        cardSpacing = m_resultCard->layout()->spacing();
        if (cardSpacing < 0) {
            cardSpacing = 12;
        }
    }

    const int iconW = m_iconLabel ? m_iconLabel->width() : 64;
    const int desiredCardW = cardLR + iconW + cardSpacing + rightInfoW;

    int contentLR = 0;
    if (m_content && m_content->layout()) {
        const QMargins m = m_content->layout()->contentsMargins();
        contentLR = m.left() + m.right();
    }
    int rootLR = 0;
    if (m_rootLayout) {
        const QMargins m = m_rootLayout->contentsMargins();
        rootLR = m.left() + m.right();
    }

    // Try single-line first (no wrap). If it would exceed the screen cap, fall back to wrapping.
    int desiredW = rootLR + contentLR + desiredCardW;
    if (desiredW > maxWindowW) {
        m_nameLabel->setWordWrap(true);
        desiredW = maxWindowW;
    } else {
        m_nameLabel->setWordWrap(false);
    }

    // Force a relayout after changing wrap mode.
    m_nameLabel->updateGeometry();
    if (m_rootLayout) {
        m_rootLayout->invalidate();
        m_rootLayout->activate();
    }
    if (m_frame && m_frame->layout()) {
        m_frame->layout()->invalidate();
        m_frame->layout()->activate();
    }
    if (m_content && m_content->layout()) {
        m_content->layout()->invalidate();
        m_content->layout()->activate();
    }

    desiredW = std::clamp(desiredW, minWindowW, maxWindowW);
    if (desiredW > 0 && desiredW != width()) {
        resize(desiredW, height());
    }
}

void MainWindow::onAutoRefreshTimeout() {
    if (!m_hasLastSuccessfulQuery) {
        return;
    }

    // Countdown finished; fire a refresh. During query we show an indeterminate spinner.
    m_refreshCountdownActive = false;
    if (m_progressRing) {
        auto *ring = static_cast<ProgressRing *>(m_progressRing);
        ring->setIndeterminate(true);
        m_progressRing->show();
        updateProgressRingGeometry();
    }

    if (!m_queryButton || !m_queryButton->isEnabled()) {
        // A query is already running; queue one refresh.
        m_pendingAutoRefresh = true;
        return;
    }
    startQueryFor(m_lastHost, m_lastPort, m_lastHasExplicitPort);
}

void MainWindow::onRefreshTick() {
    if (!m_progressRing || !m_hasLastSuccessfulQuery) {
        return;
    }

    auto *ring = static_cast<ProgressRing *>(m_progressRing);
    const bool queryInProgress = (m_queryButton && !m_queryButton->isEnabled());

    if (!m_refreshCountdownActive) {
        if (queryInProgress && ring->isIndeterminate() && m_progressRing->isVisible()) {
            ring->advanceSpinner();
        }
        return;
    }

    m_refreshElapsedMs = std::min(m_refreshIntervalMs, m_refreshElapsedMs + m_refreshTickMs);
    const double p = (m_refreshIntervalMs <= 0) ? 0.0 : (static_cast<double>(m_refreshElapsedMs) / m_refreshIntervalMs);
    ring->setProgress(p);
}

void MainWindow::scheduleNextAutoRefresh() {
    if (!m_hasLastSuccessfulQuery) {
        return;
    }

    m_refreshCountdownActive = true;
    m_refreshElapsedMs = 0;
    if (m_progressRing) {
        auto *ring = static_cast<ProgressRing *>(m_progressRing);
        ring->setIndeterminate(false);
        ring->setProgress(0.0);
        m_progressRing->show();
        updateProgressRingGeometry();
    }

    if (m_refreshTickTimer && !m_refreshTickTimer->isActive()) {
        m_refreshTickTimer->start();
    }
    if (m_autoRefreshTimer) {
        m_autoRefreshTimer->start(m_refreshIntervalMs);
    }
}

void MainWindow::stopAutoRefreshCountdown() {
    m_refreshCountdownActive = false;
    m_refreshElapsedMs = 0;
    if (m_autoRefreshTimer) {
        m_autoRefreshTimer->stop();
    }
    if (m_progressRing) {
        m_progressRing->hide();
        auto *ring = static_cast<ProgressRing *>(m_progressRing);
        ring->setIndeterminate(false);
        ring->setProgress(0.0);
    }
}

void MainWindow::updateProgressRingGeometry() {
    if (!m_progressRing || !m_resultCard) {
        return;
    }
    const int margin = 10;
    const QSize s = m_progressRing->size();
    const int x = m_resultCard->width() - s.width() - margin;
    const int y = m_resultCard->height() - s.height() - margin;
    m_progressRing->move(std::max(0, x), std::max(0, y));
}

void MainWindow::onSocketError() {
    if (m_timeoutTimer) {
        m_timeoutTimer->stop();
    }
    if (m_nameLabel) {
        m_nameLabel->setText(tr("连接失败"));
    }
    if (m_playersLabel && m_socket) {
        m_playersLabel->setText(m_socket->errorString());
    }

    // Fallback: some servers close status connection if protocol is unexpected.
    if (m_socket && m_socket->error() == QAbstractSocket::RemoteHostClosedError &&
        m_readBuffer.isEmpty() && !m_triedProtocolFallback) {
        m_triedProtocolFallback = true;
        m_protocolVersion = 47; // 1.8
        m_nameLabel->setText(tr("远程关闭连接，正在用兼容模式重试…"));
        m_playersLabel->setText(tr("协议版本回退为 1.8 状态握手"));
        m_socket->abort();
        resolveAndConnect(m_targetHost, m_targetPort, true);
        if (m_timeoutTimer) {
            m_timeoutTimer->start();
        }
        return;
    }
    if (m_queryButton) {
        m_queryButton->setEnabled(true);
        m_queryButton->setText(tr("查询"));
    }

    if (m_hasLastSuccessfulQuery) {
        scheduleNextAutoRefresh();
    }
}

void MainWindow::onSocketDisconnected() {
    // If disconnected without any data, provide a helpful hint.
    // But never overwrite a successful status parse.
    if (!m_queryButton || !m_nameLabel || !m_playersLabel) {
        return;
    }

    const bool inProgress = !m_queryButton->isEnabled() || (m_timeoutTimer && m_timeoutTimer->isActive());
    if (inProgress && !m_receivedStatusThisQuery && m_readBuffer.isEmpty()) {
        m_nameLabel->setText(tr("连接已断开"));
        m_playersLabel->setText(tr("可能是服务器/防火墙拒绝了 25565 端口连接"));
        m_queryButton->setEnabled(true);
        m_queryButton->setText(tr("查询"));
    }
}

void MainWindow::onQueryTimeout() {
    if (m_socket) {
        m_socket->abort();
    }
    if (m_nameLabel) {
        m_nameLabel->setText(tr("查询超时"));
    }
    if (m_playersLabel) {
        m_playersLabel->setText(tr("请检查地址/端口或网络"));
    }
    if (m_queryButton) {
        m_queryButton->setEnabled(true);
        m_queryButton->setText(tr("查询"));
    }

    if (m_hasLastSuccessfulQuery) {
        scheduleNextAutoRefresh();
    }
}

bool MainWindow::eventFilter(QObject *watched, QEvent *event) {
    const bool allowResize = !isMaximized() && !isFullScreen();

    auto globalPointFromMouse = [](QMouseEvent *mouseEvent) -> QPoint {
#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
        return mouseEvent->globalPosition().toPoint();
#else
        return mouseEvent->globalPos();
#endif
    };

    auto hitTestFrameEdges = [&](QMouseEvent *mouseEvent) -> Qt::Edges {
        if (!allowResize || !m_frame) {
            return {};
        }
        const QPoint framePos = m_frame->mapFromGlobal(globalPointFromMouse(mouseEvent));
        return hitTestEdges(framePos, m_frame->rect(), m_resizeBorder);
    };

    auto beginResize = [&](QMouseEvent *mouseEvent, Qt::Edges edges) -> bool {
        if (!allowResize || !edges) {
            return false;
        }
        if (windowHandle() && windowHandle()->startSystemResize(edges)) {
            mouseEvent->accept();
            return true;
        }
        m_resizing = true;
        m_resizeEdges = edges;
        m_resizeStartGeometry = frameGeometry();
        m_resizePressGlobal = globalPointFromMouse(mouseEvent);
        if (m_root) {
            m_root->grabMouse();
        }
        mouseEvent->accept();
        return true;
    };

    auto updateResizeCursor = [&](QMouseEvent *mouseEvent) {
        if (!allowResize || m_resizing || (mouseEvent->buttons() != Qt::NoButton)) {
            return;
        }
        const Qt::Edges edges = hitTestFrameEdges(mouseEvent);
        if (edges) {
            setCursor(cursorForEdges(edges));
        } else {
            unsetCursor();
        }
    };

    // Handle mouse interactions for this window even when child widgets are the event target.
    if (event->type() == QEvent::MouseMove || event->type() == QEvent::MouseButtonPress ||
        event->type() == QEvent::MouseButtonRelease || event->type() == QEvent::MouseButtonDblClick ||
        event->type() == QEvent::Leave) {
        auto *w = qobject_cast<QWidget *>(watched);
        if (!w || w->window() != this) {
            return QMainWindow::eventFilter(watched, event);
        }

        const auto isOnTitleButtons = [&](const QPoint &globalPoint) {
            for (auto *btn : {m_btnMinimize, m_btnMaximize, m_btnClose}) {
                if (!btn) {
                    continue;
                }
                if (btn->rect().contains(btn->mapFromGlobal(globalPoint))) {
                    return true;
                }
            }

            // Treat query controls as interactive titlebar widgets (no drag/maximize).
            for (auto *wgt : {static_cast<QWidget *>(m_serverInput), static_cast<QWidget *>(m_queryButton)}) {
                if (!wgt) {
                    continue;
                }
                if (wgt->isVisible() && wgt->rect().contains(wgt->mapFromGlobal(globalPoint))) {
                    return true;
                }
            }
            return false;
        };

        const auto isInTitleBar = [&](const QPoint &globalPoint) {
            if (!m_titleBar) {
                return false;
            }
            return QRect(m_titleBar->mapToGlobal(QPoint(0, 0)), m_titleBar->size()).contains(globalPoint);
        };

        if (event->type() == QEvent::Leave) {
            if (!m_resizing) {
                unsetCursor();
            }
            return QMainWindow::eventFilter(watched, event);
        }

        auto *mouseEvent = static_cast<QMouseEvent *>(event);
        const QPoint globalPoint = globalPointFromMouse(mouseEvent);

        // Cursor update for resizing (rounded frame edges).
        if (event->type() == QEvent::MouseMove && !m_resizing && (mouseEvent->buttons() == Qt::NoButton)) {
            if (isOnTitleButtons(globalPoint)) {
                unsetCursor();
                return QMainWindow::eventFilter(watched, event);
            }
            updateResizeCursor(mouseEvent);
            return QMainWindow::eventFilter(watched, event);
        }

        // Resize start.
        if (event->type() == QEvent::MouseButtonPress && mouseEvent->button() == Qt::LeftButton && allowResize &&
            !isOnTitleButtons(globalPoint)) {
            const Qt::Edges edges = hitTestFrameEdges(mouseEvent);
            if (beginResize(mouseEvent, edges)) {
                return true;
            }
        }

        // Titlebar double-click: maximize/restore.
        if (event->type() == QEvent::MouseButtonDblClick && mouseEvent->button() == Qt::LeftButton &&
            isInTitleBar(globalPoint) && !isOnTitleButtons(globalPoint)) {
            if (isMaximized()) {
                showNormal();
            } else {
                showMaximized();
            }
            mouseEvent->accept();
            return true;
        }

        // Drag move from title bar.
        if (event->type() == QEvent::MouseButtonPress && mouseEvent->button() == Qt::LeftButton &&
            isInTitleBar(globalPoint) && !isOnTitleButtons(globalPoint)) {
            if (windowHandle() && windowHandle()->startSystemMove()) {
                mouseEvent->accept();
                return true;
            }
            m_dragging = true;
            m_dragOffset = globalPoint - frameGeometry().topLeft();
            mouseEvent->accept();
            return true;
        }

        if (event->type() == QEvent::MouseMove && m_dragging && (mouseEvent->buttons() & Qt::LeftButton)) {
            move(globalPoint - m_dragOffset);
            mouseEvent->accept();
            return true;
        }

        if (event->type() == QEvent::MouseButtonRelease && mouseEvent->button() == Qt::LeftButton) {
            if (m_dragging) {
                m_dragging = false;
                mouseEvent->accept();
                return true;
            }
            if (m_resizing) {
                m_resizing = false;
                m_resizeEdges = {};
                if (m_root) {
                    m_root->releaseMouse();
                }
                unsetCursor();
                mouseEvent->accept();
                return true;
            }
        }
    }

    // Keep ring pinned to the result card.
    if (watched == m_resultCard && event->type() == QEvent::Resize) {
        updateProgressRingGeometry();
    }

    // Manual resize fallback (global mouse move while resizing)
    if (m_resizing && allowResize && event->type() == QEvent::MouseMove) {
        auto *mouseEvent = static_cast<QMouseEvent *>(event);
        const QPoint globalPoint = globalPointFromMouse(mouseEvent);

        const QPoint delta = globalPoint - m_resizePressGlobal;
        QRect newGeom = m_resizeStartGeometry;

        const int minW = minimumWidth();
        const int minH = minimumHeight();

        if (m_resizeEdges.testFlag(Qt::RightEdge)) {
            newGeom.setWidth(std::max(minW, m_resizeStartGeometry.width() + delta.x()));
        }
        if (m_resizeEdges.testFlag(Qt::BottomEdge)) {
            newGeom.setHeight(std::max(minH, m_resizeStartGeometry.height() + delta.y()));
        }
        if (m_resizeEdges.testFlag(Qt::LeftEdge)) {
            const int proposedLeft = m_resizeStartGeometry.left() + delta.x();
            const int maxLeft = m_resizeStartGeometry.right() - minW + 1;
            const int clampedLeft = std::min(proposedLeft, maxLeft);
            newGeom.setLeft(clampedLeft);
        }
        if (m_resizeEdges.testFlag(Qt::TopEdge)) {
            const int proposedTop = m_resizeStartGeometry.top() + delta.y();
            const int maxTop = m_resizeStartGeometry.bottom() - minH + 1;
            const int clampedTop = std::min(proposedTop, maxTop);
            newGeom.setTop(clampedTop);
        }

        setGeometry(newGeom);
        updateInputRegion();
        mouseEvent->accept();
        return true;
    }

    return QMainWindow::eventFilter(watched, event);
}

void MainWindow::changeEvent(QEvent *event) {
    if (event->type() == QEvent::WindowStateChange) {
        updateWindowVisuals();
        updateInputRegion();
    } else if (event->type() == QEvent::StyleChange || event->type() == QEvent::ThemeChange ||
               event->type() == QEvent::PaletteChange || event->type() == QEvent::ApplicationPaletteChange) {
        updateTitleBarIcons();
    }
    QMainWindow::changeEvent(event);
}

void MainWindow::updateTitleBarIcons() {
    if (!m_btnMinimize || !m_btnMaximize || !m_btnClose) {
        return;
    }

    m_btnMinimize->setIcon(themedOrStandardIcon(style(), QStringLiteral("window-minimize"),
                                               QStyle::SP_TitleBarMinButton));

    const bool maximized = isMaximized() || isFullScreen();
    if (maximized) {
        m_btnMaximize->setIcon(themedOrStandardIcon(style(), QStringLiteral("window-restore"),
                                                   QStyle::SP_TitleBarNormalButton));
    } else {
        m_btnMaximize->setIcon(themedOrStandardIcon(style(), QStringLiteral("window-maximize"),
                                                   QStyle::SP_TitleBarMaxButton));
    }

    m_btnClose->setIcon(themedOrStandardIcon(style(), QStringLiteral("window-close"),
                                            QStyle::SP_TitleBarCloseButton));
}

void MainWindow::showEvent(QShowEvent *event) {
    QMainWindow::showEvent(event);
    updateInputRegion();

    // Auto query default server on first show.
    if (!m_autoQueryOnStartupDone) {
        m_autoQueryOnStartupDone = true;
        QTimer::singleShot(0, this, [this] { startQuery(); });
    }
}

void MainWindow::resizeEvent(QResizeEvent *event) {
    QMainWindow::resizeEvent(event);
    updateInputRegion();
    updateProgressRingGeometry();
}

void MainWindow::updateInputRegion() {
#if defined(HAVE_X11_SHAPE)
    // Only X11 (xcb) supports making the shadow area click-through via input shape.
    if (QGuiApplication::platformName() != QStringLiteral("xcb")) {
        return;
    }
    if (!windowHandle() || !m_frame) {
        return;
    }

    Display *display = XOpenDisplay(nullptr);
    if (!display) {
        return;
    }

    const WId wid = windowHandle()->winId();
    const QRect frameRect = m_frame->geometry();
    const QRegion inputRegion = (isMaximized() || isFullScreen()) ? QRegion(rect()) : QRegion(frameRect);

    const auto rects = inputRegion.rects();
    std::vector<XRectangle> xrects;
    xrects.reserve(rects.size());
    for (const QRect &r : rects) {
        XRectangle xr;
        xr.x = static_cast<short>(r.x());
        xr.y = static_cast<short>(r.y());
        xr.width = static_cast<unsigned short>(std::max(0, r.width()));
        xr.height = static_cast<unsigned short>(std::max(0, r.height()));
        xrects.push_back(xr);
    }

    if (!xrects.empty()) {
        XShapeCombineRectangles(display,
                                static_cast<::Window>(wid),
                                ShapeInput,
                                0,
                                0,
                                xrects.data(),
                                static_cast<int>(xrects.size()),
                                ShapeSet,
                                YXBanded);
        XFlush(display);
    }

    XCloseDisplay(display);
#else
    (void)0;
#endif
}

void MainWindow::updateWindowVisuals() {
    const bool maximized = isMaximized() || isFullScreen();

    if (m_rootLayout) {
        const int margin = maximized ? 0 : m_shadowMargin;
        m_rootLayout->setContentsMargins(margin, margin, margin, margin);
    }

    if (m_shadow) {
        m_shadow->setEnabled(!maximized);
    }

    if (m_frame) {
        const int radius = maximized ? 0 : m_radius;
        m_frame->setStyleSheet(QString(
                                       "#roundedFrame {"
                                       "  background-color: palette(window);"
                                       "  border-radius: %1px;"
                                       "}")
                                       .arg(radius));
    }

    if (m_btnMaximize) {
        updateTitleBarIcons();
    }
}
