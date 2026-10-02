// ╔════════════════════════════════════════════════════════════════════════════════╗
// ║   Compat.h                                                                     ║
// ║   Small Qt 5.15 / Qt 6 shims                                                   ║
// ╚════════════════════════════════════════════════════════════════════════════════╝

#pragma once

#include <QMouseEvent>
#include <QWheelEvent>
#include <QtGlobal>

inline QPointF eventPos(const QMouseEvent* e)
{
#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
    return e->position();
#else
    return e->localPos();
#endif
}

inline QPointF eventPos(const QWheelEvent* e)
{
    return e->position();   // Qt 5.14+
}
