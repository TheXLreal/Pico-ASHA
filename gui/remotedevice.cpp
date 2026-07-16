#include <QBitArray>
#include <QByteArray>
#include <QGridLayout>

#include <algorithm>
#include <limits>

#include "remotedevice.h"

namespace
{
constexpr qint64 rssi_average_window_ms = 10'000;
constexpr qint64 rssi_history_window_ms = 60'000;
constexpr uint16_t invalid_hci_handle = 0xffff;
}

RemoteDevice::RemoteDevice(QWidget *parent)
    : QGroupBox(parent)
{
    setupUI();
    setDefaultValues();
    setGreyedOut(true);
}

void RemoteDevice::setRemoteInfo(const asha::comm::RemoteInfo &remote)
{
    setConnID(remote.conn_id);
    setHCIHandle(remote.hci_handle);
    setAddr(remote.addr);
    setPairedBonded(remote.paired);
    setPsm(remote.psm);
    setL2CID(remote.l2cap_id);
    setDeviceName(remote.dev_name);
    setMfgName(remote.mfg_name);
    setModelName(remote.model_name);
    setFwVersion(remote.fw_vers);
    setSwVersion(remote.sw_vers);
    setSide(remote.side);
    setMode(remote.mode);
    setAudioStreaming(remote.audio_streaming);
    setCurrVolume(remote.curr_vol);
    setCurrBattery(remote.curr_battery);
}

void RemoteDevice::setROPInfo(const char* rop)
{
    QBitArray device_cap = QBitArray::fromBits(rop + 1, 8);
    QBitArray codecs = QBitArray::fromBits(rop + 15, 16);

    setSide(device_cap.testBit(0) ? Side::Right : Side::Left);
    setMode(device_cap.testBit(1) ? Mode::Binaural : Mode::Mono);
    setG24KHz(codecs.testBit(2));
}

RemoteDevice::CachedProps RemoteDevice::cachedProps()
{
    return m_cachedProps;
}

void RemoteDevice::setCachedProps(CachedProps const& props)
{
    setDeviceName(props.deviceName);
    setModelName(props.modelName);
    setMfgName(props.mfgName);
    setFwVersion(props.fwVersion);
    setSwVersion(props.swVersion);
    setG24KHz(props.g24kHZ);
    setMode(props.mode);
    setSide(props.side);
}

void RemoteDevice::setConnID(uint16_t connID)
{
    m_connIDLabel.setText(QString::number(connID));
}

void RemoteDevice::setHCIHandle(uint16_t hciHandle)
{
    if (m_hciHandle != hciHandle) {
        resetRssiHistory();
        m_hciHandle = hciHandle;
    }
    m_hciConnHandleLabel.setText(
        QString("0x%1").arg(static_cast<int>(hciHandle), 4, 16, QLatin1Char('0')));
}

void RemoteDevice::setAddr(const uint8_t *addr)
{
    QByteArray addrBytes(reinterpret_cast<const char*>(addr), 6);
    m_addrLabel.setText(addrBytes.toHex(':'));
    m_cachedProps.addr = addrBytes;
}

void RemoteDevice::setPairedBonded(bool paired)
{
    m_pairedAndBondedLabel.setText(paired ? "Yes" : "No");
}

void RemoteDevice::setPsm(uint16_t psm)
{
    m_psmLabel.setText(QString("0x%1").arg((int)psm, 2, 16));
}

void RemoteDevice::setL2CID(uint16_t cid)
{
    m_l2CIDLabel.setText(QString("0x%1").arg((int)cid, 2, 16));
}

void RemoteDevice::setDeviceName(QString const& deviceName)
{
    m_deviceNameLabel.setText(deviceName);
    m_cachedProps.deviceName = deviceName;
}

void RemoteDevice::setMfgName(QString const& mfgName)
{
    m_mfgNameLabel.setText(mfgName);
    m_cachedProps.mfgName = mfgName;
}

void RemoteDevice::setModelName(QString const& modelName)
{
    m_modelNameLabel.setText(modelName);
    m_cachedProps.modelName = modelName;
}

void RemoteDevice::setFwVersion(QString const& fwVersion)
{
    m_fwVersionLabel.setText(fwVersion);
    m_cachedProps.fwVersion = fwVersion;
}

void RemoteDevice::setSwVersion(QString const& swVersion)
{
    m_swVersionLabel.setText(swVersion);
    m_cachedProps.swVersion = swVersion;
}

void RemoteDevice::setSide(Side side)
{
    QString sideStr = side == Side::Left ? "Left"
                      : side == Side::Right ? "Right" : "Side Unknown";
    setTitle(sideStr);
    m_cachedProps.side = side;
}

void RemoteDevice::setSide(asha::comm::CSide side)
{
    using namespace asha::comm;
    Side s = side == CSide::Left ? Side::Left
                : side == CSide::Right ? Side::Right : Side::SideUnset;
    setSide(s);
}

void RemoteDevice::setMode(Mode mode)
{
    QString modeStr = mode == Mode::Mono ? "Mono"
                      : mode == Mode::Binaural ? "Binaural" : "Unknown";
    m_modeLabel.setText(modeStr);
    m_cachedProps.mode = mode;
}

void RemoteDevice::setMode(asha::comm::CMode mode)
{
    using namespace asha::comm;
    Mode m = mode == CMode::Mono ? Mode::Mono
                : mode == CMode::Binaural ? Mode::Binaural : Mode::ModeUnset;
    setMode(m);
}

void RemoteDevice::setAudioStreaming(bool audioStreaming)
{
    m_audioStreamingLabel.setText(audioStreaming ? "Yes" : "No");
}

void RemoteDevice::setCurrVolume(int8_t currVol)
{
    m_currVolumeLabel.setText(QString::number(currVol));
}

void RemoteDevice::setG24KHz(bool enabled)
{
    m_g72224Label.setText(enabled ? "Supported" : "Unsupported");
    m_cachedProps.g24kHZ = enabled;
}

void RemoteDevice::setCurrBattery(uint8_t currBattery)
{
    //m_currBatteryLabel.setText(QString::number(currBattery));
    m_currBatteryBar.setValue(currBattery);
}

uint16_t RemoteDevice::hciHandle() const
{
    return m_hciHandle;
}

void RemoteDevice::addRssiSample(uint64_t timestampUs, int dbm)
{
    if (m_hciHandle == invalid_hci_handle || dbm < -127 || dbm > 20) return;

    const qint64 timestampMs = static_cast<qint64>(timestampUs / 1000U);
    if (!m_rssiHistory.isEmpty() &&
        timestampMs < m_rssiHistory.constLast().timestampMs) {
        // Firmware reboot/timestamp reset without a matching GUI lifecycle
        // event must not mix two histories.
        resetRssiHistory();
    }

    m_rssiHistory.append({timestampMs, dbm});
    const qint64 cutoffMs = timestampMs - rssi_history_window_ms;
    while (!m_rssiHistory.isEmpty() &&
           m_rssiHistory.constFirst().timestampMs < cutoffMs) {
        m_rssiHistory.removeFirst();
    }

    updateRssiLabels();
    m_rssiGraph.setSamples(m_rssiHistory);
}

void RemoteDevice::resetRssiHistory()
{
    m_rssiHistory.clear();
    m_rssiGraph.clearSamples();
    m_rssiCurrentLabel.setText("unavailable");
    m_rssiAverageLabel.setText("unavailable");
    m_rssiMinimumLabel.setText("unavailable");
    m_rssiQualityLabel.setText("unavailable");
    m_rssiQualityLabel.setStyleSheet("font-weight: normal");
}

void RemoteDevice::updateRssiLabels()
{
    if (m_rssiHistory.isEmpty()) {
        resetRssiHistory();
        return;
    }

    const auto& current = m_rssiHistory.constLast();
    const qint64 cutoffMs = current.timestampMs - rssi_average_window_ms;
    qint64 sum = 0;
    int count = 0;
    int minimum = std::numeric_limits<int>::max();
    for (auto it = m_rssiHistory.crbegin(); it != m_rssiHistory.crend(); ++it) {
        if (it->timestampMs < cutoffMs) break;
        sum += it->dbm;
        minimum = std::min(minimum, it->dbm);
        ++count;
    }

    m_rssiCurrentLabel.setText(QString("%1 dBm").arg(current.dbm));
    m_rssiAverageLabel.setText(
        QString("%1 dBm").arg(static_cast<double>(sum) / count, 0, 'f', 1));
    m_rssiMinimumLabel.setText(QString("%1 dBm").arg(minimum));
    m_rssiQualityLabel.setText(rssiQuality(current.dbm));

    const char* color = current.dbm >= -60 ? "green"
                        : current.dbm >= -70 ? "#9a7400"
                        : current.dbm >= -80 ? "darkorange" : "red";
    m_rssiQualityLabel.setStyleSheet(
        QString("font-weight: bold; color: %1").arg(color));
}

QString RemoteDevice::rssiQuality(int dbm)
{
    if (dbm >= -60) return "good";
    if (dbm >= -70) return "fair";
    if (dbm >= -80) return "weak";
    return "very weak";
}

void RemoteDevice::setDefaultValues()
{
    setTitle("Side Unknown");

    m_connIDLabel.setText("0");
    m_hciConnHandleLabel.setText("0x00");
    m_addrLabel.setText("00:00:00:00:00:00");
    m_pairedAndBondedLabel.setText("No");
    m_deviceNameLabel.setText("");
    m_mfgNameLabel.setText("");
    m_modelNameLabel.setText("");
    m_fwVersionLabel.setText("");
    m_swVersionLabel.setText("");
    m_modeLabel.setText("Unknown");
    m_g72224Label.setText("No");
    m_psmLabel.setText("0x00");
    m_l2CIDLabel.setText("0x00");
    m_audioStreamingLabel.setText("No");
    m_currVolumeLabel.setText("-128");
    m_currBatteryBar.setValue(0);
    m_hciHandle = invalid_hci_handle;
    resetRssiHistory();
}

bool RemoteDevice::isDefaultValues()
{
    return m_connIDLabel.text() == "0";
}

void RemoteDevice::setGreyedOut(bool enabled)
{
    greyedOut = enabled;
    if (greyedOut) {
        setStyleSheet("color: grey");
    } else {
        setStyleSheet("");
    }
}

void RemoteDevice::setupUI()
{
    auto addRowToLayout = [this] (const char* header, QLabel* val) {
        val->setStyleSheet("font-weight: normal");
        this->m_formLayout.addRow(header, val);
    };

    setStyleSheet("font-weight: bold");
    setAlignment(Qt::AlignHCenter);

    addRowToLayout("Connection ID", &m_connIDLabel);
    addRowToLayout("HCI Conn Handle", &m_hciConnHandleLabel);
    addRowToLayout("Address", &m_addrLabel);
    addRowToLayout("Paired and Bonded", &m_pairedAndBondedLabel);
    addRowToLayout("Device Name", &m_deviceNameLabel);
    addRowToLayout("Make", &m_mfgNameLabel);
    addRowToLayout("Model", &m_modelNameLabel);
    addRowToLayout("FW Version", &m_fwVersionLabel);
    addRowToLayout("SW Version", &m_swVersionLabel);
    addRowToLayout("Mode", &m_modeLabel);
    addRowToLayout("24 kHz Support", &m_g72224Label);
    addRowToLayout("PSM", &m_psmLabel);
    addRowToLayout("L2CAP CID", &m_l2CIDLabel);
    addRowToLayout("Audio Streaming", &m_audioStreamingLabel);
    addRowToLayout("Current Volume", &m_currVolumeLabel);

    m_currBatteryBar.setRange(0, 10);
    m_currBatteryBar.setFormat("%v/%m");
    this->m_formLayout.addRow("Current Battery", &m_currBatteryBar);

    m_rssiGroup.setTitle("BLE RSSI (radio signal only)");
    m_rssiGroup.setToolTip("RSSI describes BLE radio signal strength only. "
                           "It does not prove ASHA audio quality.");
    auto rssiLayout = new QGridLayout;
    rssiLayout->addWidget(new QLabel("Current"), 0, 0);
    rssiLayout->addWidget(&m_rssiCurrentLabel, 0, 1);
    rssiLayout->addWidget(new QLabel("10 s average"), 1, 0);
    rssiLayout->addWidget(&m_rssiAverageLabel, 1, 1);
    rssiLayout->addWidget(new QLabel("10 s minimum"), 2, 0);
    rssiLayout->addWidget(&m_rssiMinimumLabel, 2, 1);
    rssiLayout->addWidget(new QLabel("Signal"), 3, 0);
    rssiLayout->addWidget(&m_rssiQualityLabel, 3, 1);
    rssiLayout->addWidget(&m_rssiGraph, 4, 0, 1, 2);
    m_rssiGroup.setLayout(rssiLayout);
    m_formLayout.addRow(&m_rssiGroup);

    setLayout(&m_formLayout);
}
