#include "rssihistorygraph.h"

#include <algorithm>

#include <QPainter>
#include <QPainterPath>
#include <QPalette>

namespace
{
constexpr int graph_top_dbm = -30;
constexpr int graph_bottom_dbm = -100;
constexpr qint64 graph_history_ms = 60'000;

QColor lineColor(int dbm)
{
    if (dbm >= -60) return QColor(35, 145, 65);
    if (dbm >= -70) return QColor(190, 145, 20);
    if (dbm >= -80) return QColor(220, 105, 20);
    return QColor(190, 45, 45);
}
}

RssiHistoryGraph::RssiHistoryGraph(QWidget *parent)
    : QWidget(parent)
{
    setMinimumHeight(105);
    setToolTip("60-second BLE RSSI history. RSSI describes radio signal only; "
               "it does not prove audio quality.");
}

void RssiHistoryGraph::setSamples(const QVector<RssiHistorySample>& samples)
{
    m_samples = samples;
    update();
}

void RssiHistoryGraph::clearSamples()
{
    m_samples.clear();
    update();
}

QSize RssiHistoryGraph::minimumSizeHint() const
{
    return QSize(260, 105);
}

void RssiHistoryGraph::paintEvent(QPaintEvent *event)
{
    Q_UNUSED(event);

    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing);
    painter.fillRect(rect(), palette().brush(QPalette::Base));

    const QRectF plot = QRectF(rect()).adjusted(38.0, 8.0, -8.0, -22.0);
    if (plot.width() <= 0.0 || plot.height() <= 0.0) return;

    const QColor gridColor = palette().color(QPalette::Mid);
    const QColor textColor = palette().color(QPalette::Text);
    painter.setFont(QFont(painter.font().family(), 7));
    painter.setPen(QPen(gridColor, 1.0));

    for (int dbm : {-40, -60, -80, -100}) {
        const double fraction = static_cast<double>(graph_top_dbm - dbm) /
                                (graph_top_dbm - graph_bottom_dbm);
        const double y = plot.top() + fraction * plot.height();
        painter.drawLine(QPointF(plot.left(), y), QPointF(plot.right(), y));
        painter.setPen(textColor);
        painter.drawText(QRectF(0.0, y - 8.0, plot.left() - 4.0, 16.0),
                         Qt::AlignRight | Qt::AlignVCenter,
                         QString::number(dbm));
        painter.setPen(QPen(gridColor, 1.0));
    }

    painter.setPen(textColor);
    painter.drawText(QRectF(plot.left(), plot.bottom() + 3.0, 60.0, 16.0),
                     Qt::AlignLeft, "60 s ago");
    painter.drawText(QRectF(plot.right() - 35.0, plot.bottom() + 3.0, 35.0, 16.0),
                     Qt::AlignRight, "now");

    if (m_samples.isEmpty()) {
        painter.drawText(plot, Qt::AlignCenter, "unavailable");
        return;
    }

    const qint64 newestMs = m_samples.constLast().timestampMs;
    QPainterPath path;
    bool havePoint = false;
    QPointF lastPoint;
    for (const auto& sample : m_samples) {
        const qint64 ageMs = std::clamp(newestMs - sample.timestampMs,
                                        qint64(0), graph_history_ms);
        const double x = plot.right() -
                         static_cast<double>(ageMs) / graph_history_ms * plot.width();
        const int clampedDbm = std::clamp(sample.dbm,
                                          graph_bottom_dbm, graph_top_dbm);
        const double fraction = static_cast<double>(graph_top_dbm - clampedDbm) /
                                (graph_top_dbm - graph_bottom_dbm);
        const double y = plot.top() + fraction * plot.height();
        lastPoint = QPointF(x, y);
        if (!havePoint) {
            path.moveTo(lastPoint);
            havePoint = true;
        } else {
            path.lineTo(lastPoint);
        }
    }

    const QColor color = lineColor(m_samples.constLast().dbm);
    painter.setPen(QPen(color, 2.0));
    painter.drawPath(path);
    painter.setBrush(color);
    painter.drawEllipse(lastPoint, 3.0, 3.0);
}
