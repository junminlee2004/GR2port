// SPDX-FileCopyrightText: Copyright 2026 GR2port
// SPDX-License-Identifier: GPL-2.0-or-later

#include <QPainter>
#include <QPainterPath>
#include <QProxyStyle>
#include <QStyleFactory>
#include <QStyleOptionGroupBox>
#include <QWidget>

#include "launcher/qt/style.h"

namespace Gr2Port::Launcher {

namespace {

// A check box's side and a tab's space around its text, in pixels at 96 DPI.
constexpr int CheckSide = 13;
constexpr int TabSpace = 14;
// A group box's title starts this far from its left edge, with this much clear at each side.
constexpr int TitleLeft = 7;
constexpr int TitleGap = 2;

const QColor Ink{51, 51, 51};
const QColor HotInk{0, 120, 215};
const QColor OffInk{204, 204, 204};
const QColor PressedFill{204, 228, 247};
const QColor GroupEdge{220, 220, 220};

// A white box with a dark edge, which shows on the dialogs' white, and its tick.
void DrawCheck(const QStyleOption& option, QPainter& painter) {
    const bool enabled = option.state.testFlag(QStyle::State_Enabled);
    const bool hot = enabled && option.state.testFlag(QStyle::State_MouseOver);
    const bool pressed = enabled && option.state.testFlag(QStyle::State_Sunken);
    const QColor ink = !enabled ? OffInk : hot ? HotInk : Ink;
    painter.save();
    painter.fillRect(option.rect, ink);
    painter.fillRect(option.rect.adjusted(1, 1, -1, -1), pressed ? PressedFill : QColor{Qt::white});
    if (option.state.testFlag(QStyle::State_On) || option.state.testFlag(QStyle::State_NoChange)) {
        const QRectF box{option.rect};
        QPainterPath tick;
        tick.moveTo(box.left() + box.width() * 0.22, box.top() + box.height() * 0.52);
        tick.lineTo(box.left() + box.width() * 0.42, box.top() + box.height() * 0.72);
        tick.lineTo(box.left() + box.width() * 0.79, box.top() + box.height() * 0.28);
        painter.setRenderHint(QPainter::Antialiasing);
        painter.strokePath(tick, QPen{ink, 1.5});
    }
    painter.restore();
}

// A thin frame from the middle of the title's line down, with the title over its top edge.
void DrawGroup(const QStyleOptionGroupBox& box, QPainter& painter) {
    const int title_height = box.fontMetrics.height();
    const QRect frame = box.rect.adjusted(0, title_height / 2, 0, 0);
    painter.save();
    painter.fillRect(QRect{frame.left(), frame.top(), frame.width(), 1}, GroupEdge);
    painter.fillRect(QRect{frame.left(), frame.bottom(), frame.width(), 1}, GroupEdge);
    painter.fillRect(QRect{frame.left(), frame.top(), 1, frame.height()}, GroupEdge);
    painter.fillRect(QRect{frame.right(), frame.top(), 1, frame.height()}, GroupEdge);
    if (!box.text.isEmpty()) {
        const QRect title{box.rect.left() + TitleLeft, box.rect.top(),
                          box.fontMetrics.horizontalAdvance(box.text) + 2 * TitleGap, title_height};
        painter.fillRect(title, box.palette.window());
        painter.setPen(box.palette.color(
            box.state.testFlag(QStyle::State_Enabled) ? QPalette::Normal : QPalette::Disabled,
            QPalette::WindowText));
        painter.drawText(title.adjusted(TitleGap, 0, -TitleGap, 0),
                         Qt::AlignLeft | Qt::AlignVCenter | Qt::TextSingleLine, box.text);
    }
    painter.restore();
}

class Style : public QProxyStyle {
public:
    Style() : QProxyStyle{QStyleFactory::create(QStringLiteral("Fusion"))} {}

    int pixelMetric(PixelMetric metric, const QStyleOption* option,
                    const QWidget* widget) const override {
        switch (metric) {
        case PM_IndicatorWidth:
        case PM_IndicatorHeight:
            return CheckSide;
        case PM_TabBarTabHSpace:
            return TabSpace - (widget ? widget->property(TabSpaceTaken).toInt() : 0);
        default:
            return QProxyStyle::pixelMetric(metric, option, widget);
        }
    }

    int styleHint(StyleHint hint, const QStyleOption* option, const QWidget* widget,
                  QStyleHintReturn* data) const override {
        // A combo box drops a list of a few rows below it, with a scroll bar for the rest.
        if (hint == SH_ComboBox_Popup) {
            return 0;
        }
        return QProxyStyle::styleHint(hint, option, widget, data);
    }

    void drawPrimitive(PrimitiveElement element, const QStyleOption* option, QPainter* painter,
                       const QWidget* widget) const override {
        if (element == PE_IndicatorCheckBox || element == PE_IndicatorItemViewItemCheck) {
            DrawCheck(*option, *painter);
            return;
        }
        QProxyStyle::drawPrimitive(element, option, painter, widget);
    }

    void drawComplexControl(ComplexControl control, const QStyleOptionComplex* option,
                            QPainter* painter, const QWidget* widget) const override {
        if (control == CC_GroupBox) {
            if (const auto* const box = qstyleoption_cast<const QStyleOptionGroupBox*>(option)) {
                DrawGroup(*box, *painter);
                return;
            }
        }
        QProxyStyle::drawComplexControl(control, option, painter, widget);
    }
};

} // namespace

QStyle* MakeStyle() {
    return new Style;
}

} // namespace Gr2Port::Launcher
