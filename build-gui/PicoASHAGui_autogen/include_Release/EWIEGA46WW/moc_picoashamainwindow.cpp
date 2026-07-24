/****************************************************************************
** Meta object code from reading C++ file 'picoashamainwindow.h'
**
** Created by: The Qt Meta Object Compiler version 69 (Qt 6.11.1)
**
** WARNING! All changes made in this file will be lost!
*****************************************************************************/

#include "../../../../gui/picoashamainwindow.h"
#include <QtGui/qtextcursor.h>
#include <QtCore/qmetatype.h>

#include <QtCore/qtmochelpers.h>

#include <memory>


#include <QtCore/qxptype_traits.h>
#if !defined(Q_MOC_OUTPUT_REVISION)
#error "The header file 'picoashamainwindow.h' doesn't include <QObject>."
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
struct qt_meta_tag_ZN18PicoAshaMainWindowE_t {};
} // unnamed namespace

template <> constexpr inline auto PicoAshaMainWindow::qt_create_metaobjectdata<qt_meta_tag_ZN18PicoAshaMainWindowE_t>()
{
    namespace QMC = QtMocConstants;
    QtMocHelpers::StringRefStorage qt_stringData {
        "PicoAshaMainWindow",
        "hciLogPathChanged",
        "",
        "path",
        "hciLogActionBtnClicked",
        "cmdRestartBtnClicked",
        "cmdConnAllowedBtnClicked",
        "allowed",
        "cmdStreamingEnabledBtnClicked",
        "enabled",
        "cmdRemoveBondBtnClicked",
        "usbSettingsBtnClicked",
        "asha::comm::USBInfo",
        "usb_info",
        "pairWithAddress",
        "addr",
        "uint8_t",
        "addr_type",
        "onPairDialogAcceptedRejected"
    };

    QtMocHelpers::UintData qt_methods {
        // Signal 'hciLogPathChanged'
        QtMocHelpers::SignalData<void(QString const &)>(1, 2, QMC::AccessPublic, QMetaType::Void, {{
            { QMetaType::QString, 3 },
        }}),
        // Signal 'hciLogActionBtnClicked'
        QtMocHelpers::SignalData<void()>(4, 2, QMC::AccessPublic, QMetaType::Void),
        // Signal 'cmdRestartBtnClicked'
        QtMocHelpers::SignalData<void()>(5, 2, QMC::AccessPublic, QMetaType::Void),
        // Signal 'cmdConnAllowedBtnClicked'
        QtMocHelpers::SignalData<void(bool)>(6, 2, QMC::AccessPublic, QMetaType::Void, {{
            { QMetaType::Bool, 7 },
        }}),
        // Signal 'cmdStreamingEnabledBtnClicked'
        QtMocHelpers::SignalData<void(bool)>(8, 2, QMC::AccessPublic, QMetaType::Void, {{
            { QMetaType::Bool, 9 },
        }}),
        // Signal 'cmdRemoveBondBtnClicked'
        QtMocHelpers::SignalData<void()>(10, 2, QMC::AccessPublic, QMetaType::Void),
        // Signal 'usbSettingsBtnClicked'
        QtMocHelpers::SignalData<void(asha::comm::USBInfo const &)>(11, 2, QMC::AccessPublic, QMetaType::Void, {{
            { 0x80000000 | 12, 13 },
        }}),
        // Signal 'pairWithAddress'
        QtMocHelpers::SignalData<void(QByteArray const &, uint8_t)>(14, 2, QMC::AccessPublic, QMetaType::Void, {{
            { QMetaType::QByteArray, 15 }, { 0x80000000 | 16, 17 },
        }}),
        // Slot 'onPairDialogAcceptedRejected'
        QtMocHelpers::SlotData<void()>(18, 2, QMC::AccessPublic, QMetaType::Void),
    };
    QtMocHelpers::UintData qt_properties {
    };
    QtMocHelpers::UintData qt_enums {
    };
    return QtMocHelpers::metaObjectData<PicoAshaMainWindow, qt_meta_tag_ZN18PicoAshaMainWindowE_t>(QMC::MetaObjectFlag{}, qt_stringData,
            qt_methods, qt_properties, qt_enums);
}
Q_CONSTINIT const QMetaObject PicoAshaMainWindow::staticMetaObject = { {
    QMetaObject::SuperData::link<QMainWindow::staticMetaObject>(),
    qt_staticMetaObjectStaticContent<qt_meta_tag_ZN18PicoAshaMainWindowE_t>.stringdata,
    qt_staticMetaObjectStaticContent<qt_meta_tag_ZN18PicoAshaMainWindowE_t>.data,
    qt_static_metacall,
    nullptr,
    qt_staticMetaObjectRelocatingContent<qt_meta_tag_ZN18PicoAshaMainWindowE_t>.metaTypes,
    nullptr
} };

void PicoAshaMainWindow::qt_static_metacall(QObject *_o, QMetaObject::Call _c, int _id, void **_a)
{
    auto *_t = static_cast<PicoAshaMainWindow *>(_o);
    if (_c == QMetaObject::InvokeMetaMethod) {
        switch (_id) {
        case 0: _t->hciLogPathChanged((*reinterpret_cast<std::add_pointer_t<QString>>(_a[1]))); break;
        case 1: _t->hciLogActionBtnClicked(); break;
        case 2: _t->cmdRestartBtnClicked(); break;
        case 3: _t->cmdConnAllowedBtnClicked((*reinterpret_cast<std::add_pointer_t<bool>>(_a[1]))); break;
        case 4: _t->cmdStreamingEnabledBtnClicked((*reinterpret_cast<std::add_pointer_t<bool>>(_a[1]))); break;
        case 5: _t->cmdRemoveBondBtnClicked(); break;
        case 6: _t->usbSettingsBtnClicked((*reinterpret_cast<std::add_pointer_t<asha::comm::USBInfo>>(_a[1]))); break;
        case 7: _t->pairWithAddress((*reinterpret_cast<std::add_pointer_t<QByteArray>>(_a[1])),(*reinterpret_cast<std::add_pointer_t<uint8_t>>(_a[2]))); break;
        case 8: _t->onPairDialogAcceptedRejected(); break;
        default: ;
        }
    }
    if (_c == QMetaObject::IndexOfMethod) {
        if (QtMocHelpers::indexOfMethod<void (PicoAshaMainWindow::*)(QString const & )>(_a, &PicoAshaMainWindow::hciLogPathChanged, 0))
            return;
        if (QtMocHelpers::indexOfMethod<void (PicoAshaMainWindow::*)()>(_a, &PicoAshaMainWindow::hciLogActionBtnClicked, 1))
            return;
        if (QtMocHelpers::indexOfMethod<void (PicoAshaMainWindow::*)()>(_a, &PicoAshaMainWindow::cmdRestartBtnClicked, 2))
            return;
        if (QtMocHelpers::indexOfMethod<void (PicoAshaMainWindow::*)(bool )>(_a, &PicoAshaMainWindow::cmdConnAllowedBtnClicked, 3))
            return;
        if (QtMocHelpers::indexOfMethod<void (PicoAshaMainWindow::*)(bool )>(_a, &PicoAshaMainWindow::cmdStreamingEnabledBtnClicked, 4))
            return;
        if (QtMocHelpers::indexOfMethod<void (PicoAshaMainWindow::*)()>(_a, &PicoAshaMainWindow::cmdRemoveBondBtnClicked, 5))
            return;
        if (QtMocHelpers::indexOfMethod<void (PicoAshaMainWindow::*)(asha::comm::USBInfo const & )>(_a, &PicoAshaMainWindow::usbSettingsBtnClicked, 6))
            return;
        if (QtMocHelpers::indexOfMethod<void (PicoAshaMainWindow::*)(QByteArray const & , uint8_t )>(_a, &PicoAshaMainWindow::pairWithAddress, 7))
            return;
    }
}

const QMetaObject *PicoAshaMainWindow::metaObject() const
{
    return QObject::d_ptr->metaObject ? QObject::d_ptr->dynamicMetaObject() : &staticMetaObject;
}

void *PicoAshaMainWindow::qt_metacast(const char *_clname)
{
    if (!_clname) return nullptr;
    if (!strcmp(_clname, qt_staticMetaObjectStaticContent<qt_meta_tag_ZN18PicoAshaMainWindowE_t>.strings))
        return static_cast<void*>(this);
    return QMainWindow::qt_metacast(_clname);
}

int PicoAshaMainWindow::qt_metacall(QMetaObject::Call _c, int _id, void **_a)
{
    _id = QMainWindow::qt_metacall(_c, _id, _a);
    if (_id < 0)
        return _id;
    if (_c == QMetaObject::InvokeMetaMethod) {
        if (_id < 9)
            qt_static_metacall(this, _c, _id, _a);
        _id -= 9;
    }
    if (_c == QMetaObject::RegisterMethodArgumentMetaType) {
        if (_id < 9)
            *reinterpret_cast<QMetaType *>(_a[0]) = QMetaType();
        _id -= 9;
    }
    return _id;
}

// SIGNAL 0
void PicoAshaMainWindow::hciLogPathChanged(QString const & _t1)
{
    QMetaObject::activate<void>(this, &staticMetaObject, 0, nullptr, _t1);
}

// SIGNAL 1
void PicoAshaMainWindow::hciLogActionBtnClicked()
{
    QMetaObject::activate(this, &staticMetaObject, 1, nullptr);
}

// SIGNAL 2
void PicoAshaMainWindow::cmdRestartBtnClicked()
{
    QMetaObject::activate(this, &staticMetaObject, 2, nullptr);
}

// SIGNAL 3
void PicoAshaMainWindow::cmdConnAllowedBtnClicked(bool _t1)
{
    QMetaObject::activate<void>(this, &staticMetaObject, 3, nullptr, _t1);
}

// SIGNAL 4
void PicoAshaMainWindow::cmdStreamingEnabledBtnClicked(bool _t1)
{
    QMetaObject::activate<void>(this, &staticMetaObject, 4, nullptr, _t1);
}

// SIGNAL 5
void PicoAshaMainWindow::cmdRemoveBondBtnClicked()
{
    QMetaObject::activate(this, &staticMetaObject, 5, nullptr);
}

// SIGNAL 6
void PicoAshaMainWindow::usbSettingsBtnClicked(asha::comm::USBInfo const & _t1)
{
    QMetaObject::activate<void>(this, &staticMetaObject, 6, nullptr, _t1);
}

// SIGNAL 7
void PicoAshaMainWindow::pairWithAddress(QByteArray const & _t1, uint8_t _t2)
{
    QMetaObject::activate<void>(this, &staticMetaObject, 7, nullptr, _t1, _t2);
}
QT_WARNING_POP
