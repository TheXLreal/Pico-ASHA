/****************************************************************************
** Meta object code from reading C++ file 'picoashacomm.h'
**
** Created by: The Qt Meta Object Compiler version 69 (Qt 6.11.1)
**
** WARNING! All changes made in this file will be lost!
*****************************************************************************/

#include "../../../../gui/picoashacomm.h"
#include <QtGui/qtextcursor.h>
#include <QtCore/qmetatype.h>

#include <QtCore/qtmochelpers.h>

#include <memory>


#include <QtCore/qxptype_traits.h>
#if !defined(Q_MOC_OUTPUT_REVISION)
#error "The header file 'picoashacomm.h' doesn't include <QObject>."
#elif Q_MOC_OUTPUT_REVISION != 69
#error "This file was generated using the moc from 6.11.1. It"
#error "cannot be used with the include files from this version of Qt."
#error "(The moc has changed too much.)"
#endif

#ifndef Q_CONSTINIT
#define Q_CONSTINIT
#endif

QT_WARNING_PUSH
QT_WARNING_DISABLE_DEPRECATED
QT_WARNING_DISABLE_GCC("-Wuseless-cast")
namespace {
struct qt_meta_tag_ZN12PicoAshaCommE_t {};
} // unnamed namespace

template <> constexpr inline auto PicoAshaComm::qt_create_metaobjectdata<qt_meta_tag_ZN12PicoAshaCommE_t>()
{
    namespace QMC = QtMocConstants;
    QtMocHelpers::StringRefStorage qt_stringData {
        "PicoAshaComm",
        "paFirmwareVersChanged",
        "",
        "remoteErrorChanged",
        "hciLoggingPathChanged",
        "hciLoggingEnabledChanged",
        "errMsgChanged",
        "onConnectTimer",
        "onIntroTimer",
        "onSerialError",
        "QSerialPort::SerialPortError",
        "error",
        "onSerialReadyRead",
        "onHciLogPathChanged",
        "path",
        "onHciLogActionBtnClicked",
        "onCmdRestartBtnClicked",
        "onCmdConnAllowedBtnClicked",
        "allowed",
        "onCmdStreamingEnabledBtnClicked",
        "enabled",
        "onCmdRemoveBondBtnClicked",
        "onUsbSettingsBtnClicked",
        "asha::comm::USBInfo",
        "usb_info",
        "onPairWithAddress",
        "addr",
        "uint8_t",
        "addr_type"
    };

    QtMocHelpers::UintData qt_methods {
        // Signal 'paFirmwareVersChanged'
        QtMocHelpers::SignalData<void()>(1, 2, QMC::AccessPublic, QMetaType::Void),
        // Signal 'remoteErrorChanged'
        QtMocHelpers::SignalData<void()>(3, 2, QMC::AccessPublic, QMetaType::Void),
        // Signal 'hciLoggingPathChanged'
        QtMocHelpers::SignalData<void()>(4, 2, QMC::AccessPublic, QMetaType::Void),
        // Signal 'hciLoggingEnabledChanged'
        QtMocHelpers::SignalData<void()>(5, 2, QMC::AccessPublic, QMetaType::Void),
        // Signal 'errMsgChanged'
        QtMocHelpers::SignalData<void()>(6, 2, QMC::AccessPublic, QMetaType::Void),
        // Slot 'onConnectTimer'
        QtMocHelpers::SlotData<void()>(7, 2, QMC::AccessPublic, QMetaType::Void),
        // Slot 'onIntroTimer'
        QtMocHelpers::SlotData<void()>(8, 2, QMC::AccessPublic, QMetaType::Void),
        // Slot 'onSerialError'
        QtMocHelpers::SlotData<void(QSerialPort::SerialPortError)>(9, 2, QMC::AccessPublic, QMetaType::Void, {{
            { 0x80000000 | 10, 11 },
        }}),
        // Slot 'onSerialReadyRead'
        QtMocHelpers::SlotData<void()>(12, 2, QMC::AccessPublic, QMetaType::Void),
        // Slot 'onHciLogPathChanged'
        QtMocHelpers::SlotData<void(QString const &)>(13, 2, QMC::AccessPublic, QMetaType::Void, {{
            { QMetaType::QString, 14 },
        }}),
        // Slot 'onHciLogActionBtnClicked'
        QtMocHelpers::SlotData<void()>(15, 2, QMC::AccessPublic, QMetaType::Void),
        // Slot 'onCmdRestartBtnClicked'
        QtMocHelpers::SlotData<void()>(16, 2, QMC::AccessPublic, QMetaType::Void),
        // Slot 'onCmdConnAllowedBtnClicked'
        QtMocHelpers::SlotData<void(bool)>(17, 2, QMC::AccessPublic, QMetaType::Void, {{
            { QMetaType::Bool, 18 },
        }}),
        // Slot 'onCmdStreamingEnabledBtnClicked'
        QtMocHelpers::SlotData<void(bool)>(19, 2, QMC::AccessPublic, QMetaType::Void, {{
            { QMetaType::Bool, 20 },
        }}),
        // Slot 'onCmdRemoveBondBtnClicked'
        QtMocHelpers::SlotData<void()>(21, 2, QMC::AccessPublic, QMetaType::Void),
        // Slot 'onUsbSettingsBtnClicked'
        QtMocHelpers::SlotData<void(asha::comm::USBInfo const &)>(22, 2, QMC::AccessPublic, QMetaType::Void, {{
            { 0x80000000 | 23, 24 },
        }}),
        // Slot 'onPairWithAddress'
        QtMocHelpers::SlotData<void(QByteArray const &, uint8_t)>(25, 2, QMC::AccessPublic, QMetaType::Void, {{
            { QMetaType::QByteArray, 26 }, { 0x80000000 | 27, 28 },
        }}),
    };
    QtMocHelpers::UintData qt_properties {
    };
    QtMocHelpers::UintData qt_enums {
    };
    return QtMocHelpers::metaObjectData<PicoAshaComm, qt_meta_tag_ZN12PicoAshaCommE_t>(QMC::MetaObjectFlag{}, qt_stringData,
            qt_methods, qt_properties, qt_enums);
}
Q_CONSTINIT const QMetaObject PicoAshaComm::staticMetaObject = { {
    QMetaObject::SuperData::link<QObject::staticMetaObject>(),
    qt_staticMetaObjectStaticContent<qt_meta_tag_ZN12PicoAshaCommE_t>.stringdata,
    qt_staticMetaObjectStaticContent<qt_meta_tag_ZN12PicoAshaCommE_t>.data,
    qt_static_metacall,
    nullptr,
    qt_staticMetaObjectRelocatingContent<qt_meta_tag_ZN12PicoAshaCommE_t>.metaTypes,
    nullptr
} };

void PicoAshaComm::qt_static_metacall(QObject *_o, QMetaObject::Call _c, int _id, void **_a)
{
    auto *_t = static_cast<PicoAshaComm *>(_o);
    if (_c == QMetaObject::InvokeMetaMethod) {
        switch (_id) {
        case 0: _t->paFirmwareVersChanged(); break;
        case 1: _t->remoteErrorChanged(); break;
        case 2: _t->hciLoggingPathChanged(); break;
        case 3: _t->hciLoggingEnabledChanged(); break;
        case 4: _t->errMsgChanged(); break;
        case 5: _t->onConnectTimer(); break;
        case 6: _t->onIntroTimer(); break;
        case 7: _t->onSerialError((*reinterpret_cast<std::add_pointer_t<QSerialPort::SerialPortError>>(_a[1]))); break;
        case 8: _t->onSerialReadyRead(); break;
        case 9: _t->onHciLogPathChanged((*reinterpret_cast<std::add_pointer_t<QString>>(_a[1]))); break;
        case 10: _t->onHciLogActionBtnClicked(); break;
        case 11: _t->onCmdRestartBtnClicked(); break;
        case 12: _t->onCmdConnAllowedBtnClicked((*reinterpret_cast<std::add_pointer_t<bool>>(_a[1]))); break;
        case 13: _t->onCmdStreamingEnabledBtnClicked((*reinterpret_cast<std::add_pointer_t<bool>>(_a[1]))); break;
        case 14: _t->onCmdRemoveBondBtnClicked(); break;
        case 15: _t->onUsbSettingsBtnClicked((*reinterpret_cast<std::add_pointer_t<asha::comm::USBInfo>>(_a[1]))); break;
        case 16: _t->onPairWithAddress((*reinterpret_cast<std::add_pointer_t<QByteArray>>(_a[1])),(*reinterpret_cast<std::add_pointer_t<uint8_t>>(_a[2]))); break;
        default: ;
        }
    }
    if (_c == QMetaObject::IndexOfMethod) {
        if (QtMocHelpers::indexOfMethod<void (PicoAshaComm::*)()>(_a, &PicoAshaComm::paFirmwareVersChanged, 0))
            return;
        if (QtMocHelpers::indexOfMethod<void (PicoAshaComm::*)()>(_a, &PicoAshaComm::remoteErrorChanged, 1))
            return;
        if (QtMocHelpers::indexOfMethod<void (PicoAshaComm::*)()>(_a, &PicoAshaComm::hciLoggingPathChanged, 2))
            return;
        if (QtMocHelpers::indexOfMethod<void (PicoAshaComm::*)()>(_a, &PicoAshaComm::hciLoggingEnabledChanged, 3))
            return;
        if (QtMocHelpers::indexOfMethod<void (PicoAshaComm::*)()>(_a, &PicoAshaComm::errMsgChanged, 4))
            return;
    }
}

const QMetaObject *PicoAshaComm::metaObject() const
{
    return QObject::d_ptr->metaObject ? QObject::d_ptr->dynamicMetaObject() : &staticMetaObject;
}

void *PicoAshaComm::qt_metacast(const char *_clname)
{
    if (!_clname) return nullptr;
    if (!strcmp(_clname, qt_staticMetaObjectStaticContent<qt_meta_tag_ZN12PicoAshaCommE_t>.strings))
        return static_cast<void*>(this);
    return QObject::qt_metacast(_clname);
}

int PicoAshaComm::qt_metacall(QMetaObject::Call _c, int _id, void **_a)
{
    _id = QObject::qt_metacall(_c, _id, _a);
    if (_id < 0)
        return _id;
    if (_c == QMetaObject::InvokeMetaMethod) {
        if (_id < 17)
            qt_static_metacall(this, _c, _id, _a);
        _id -= 17;
    }
    if (_c == QMetaObject::RegisterMethodArgumentMetaType) {
        if (_id < 17)
            *reinterpret_cast<QMetaType *>(_a[0]) = QMetaType();
        _id -= 17;
    }
    return _id;
}

// SIGNAL 0
void PicoAshaComm::paFirmwareVersChanged()
{
    QMetaObject::activate(this, &staticMetaObject, 0, nullptr);
}

// SIGNAL 1
void PicoAshaComm::remoteErrorChanged()
{
    QMetaObject::activate(this, &staticMetaObject, 1, nullptr);
}

// SIGNAL 2
void PicoAshaComm::hciLoggingPathChanged()
{
    QMetaObject::activate(this, &staticMetaObject, 2, nullptr);
}

// SIGNAL 3
void PicoAshaComm::hciLoggingEnabledChanged()
{
    QMetaObject::activate(this, &staticMetaObject, 3, nullptr);
}

// SIGNAL 4
void PicoAshaComm::errMsgChanged()
{
    QMetaObject::activate(this, &staticMetaObject, 4, nullptr);
}
QT_WARNING_POP
