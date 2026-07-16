#pragma once

#include <QVector>
#include <QWidget>

struct RssiHistorySample
{
    qint64 timestampMs = 0;
    int dbm = 0;
};

class RssiHistoryGraph : public QWidget
{
public:
    explicit RssiHistoryGraph(QWidget *parent = nullptr);

    void setSamples(const QVector<RssiHistorySample>& samples);
    void clearSamples();

    QSize minimumSizeHint() const override;

protected:
    void paintEvent(QPaintEvent *event) override;

private:
    QVector<RssiHistorySample> m_samples;
};
