// SPDX-FileCopyrightText: Copyright 2026 GR2port
// SPDX-License-Identifier: GPL-2.0-or-later

#include <array>
#include <string>
#include <vector>
#include <QCheckBox>
#include <QDateTime>
#include <QDialog>
#include <QHeaderView>
#include <QIcon>
#include <QLabel>
#include <QLocale>
#include <QPixmap>
#include <QPushButton>
#include <QTableWidget>
#include <cmrc/cmrc.hpp>

#include "common/path_util.h"
#include "core/game_check/trophy_list.h"
#include "core/language/language.h"
#include "launcher/qt/text.h"
#include "launcher/qt/trophy_viewer.h"

CMRC_DECLARE(res);

namespace Gr2Port::Launcher {

namespace {

// The dialog's client area and the places of its controls, in pixels at 96 DPI: the Windows
// launcher's trophy viewer's.
constexpr int ClientWidth = 640;
constexpr int ClientHeight = 470;
constexpr QRect ProgressLine{QPoint{10, 10}, QPoint{629, 24}};
constexpr std::array<QRect, 3> FilterBoxes{
    QRect{QPoint{10, 32}, QPoint{209, 48}},
    QRect{QPoint{216, 32}, QPoint{425, 48}},
    QRect{QPoint{432, 32}, QPoint{629, 48}},
};
constexpr QRect ListArea{QPoint{10, 58}, QPoint{629, 428}};
constexpr QRect OkButton{QPoint{557, 438}, QPoint{629, 459}};
// The list's columns: the trophy's picture, name, description, time earned and grade.
constexpr std::array<std::string_view, 5> Headers{"Trophy", "Name", "Description", "Time Unlocked",
                                                  "Type"};
constexpr std::array<int, 5> ColumnWidths{68, 140, 228, 110, 52};
constexpr int IconSize = 48;
constexpr int GradeSize = 32;
constexpr int RowHeight = 54;

// The filters: the earned trophies, the others, and the hidden ones among the others.
constexpr std::array<std::string_view, 3> Filters{
    "Show Earned Trophies", "Show Not Earned Trophies", "Show Hidden Trophies"};
constexpr std::array<bool, 3> FilterDefaults{true, true, false};
constexpr size_t EarnedFilter = 0;
constexpr size_t NotEarnedFilter = 1;
constexpr size_t HiddenFilter = 2;

struct Viewer {
    std::vector<Trophies::Trophy> trophies;
    QTableWidget* list = nullptr;
    std::array<QCheckBox*, Filters.size()> filters{};
};

// The built-in picture of a trophy grade.
QPixmap GradePicture(char grade) {
    std::string name = "bronze";
    if (grade == 'S') {
        name = "silver";
    } else if (grade == 'G') {
        name = "gold";
    } else if (grade == 'P') {
        name = "platinum";
    }
    const cmrc::file file = cmrc::res::get_filesystem().open("src/resources/" + name + ".png");
    QPixmap picture;
    picture.loadFromData(reinterpret_cast<const uchar*>(file.begin()),
                         static_cast<uint>(file.size()));
    return picture.scaled(GradeSize, GradeSize, Qt::KeepAspectRatio, Qt::SmoothTransformation);
}

QTableWidgetItem* TextCell(const QString& text) {
    auto* const item = new QTableWidgetItem{text};
    item->setFlags(item->flags() & ~Qt::ItemIsEditable);
    return item;
}

QTableWidgetItem* PictureCell(const QPixmap& picture) {
    auto* const item = new QTableWidgetItem;
    item->setData(Qt::DecorationRole, picture);
    item->setFlags(item->flags() & ~Qt::ItemIsEditable);
    return item;
}

// A row shows when its trophy passes the filters: a hidden trophy the player has not earned
// needs the hidden filter as well.
void ShowRows(const Viewer& viewer) {
    for (size_t i = 0; i < viewer.trophies.size(); ++i) {
        const Trophies::Trophy& trophy = viewer.trophies[i];
        bool shown = viewer.filters[trophy.earned ? EarnedFilter : NotEarnedFilter]->isChecked();
        if (!trophy.earned && trophy.hidden && !viewer.filters[HiddenFilter]->isChecked()) {
            shown = false;
        }
        viewer.list->setRowHidden(static_cast<int>(i), !shown);
    }
}

void FillList(const Viewer& viewer) {
    viewer.list->setRowCount(static_cast<int>(viewer.trophies.size()));
    for (size_t i = 0; i < viewer.trophies.size(); ++i) {
        const Trophies::Trophy& trophy = viewer.trophies[i];
        const int row = static_cast<int>(i);
        QPixmap icon{QString::fromStdString(Common::FS::PathToUTF8String(trophy.icon))};
        if (!icon.isNull()) {
            icon = icon.scaled(IconSize, IconSize, Qt::KeepAspectRatio, Qt::SmoothTransformation);
        }
        // A description's line breaks are spaces in the list.
        QString detail = QString::fromStdString(trophy.detail);
        detail.replace(QLatin1Char('\n'), QLatin1Char(' '));
        QString time;
        if (trophy.earned && trophy.earned_at != 0) {
            time = QLocale::system().toString(
                QDateTime::fromSecsSinceEpoch(static_cast<qint64>(trophy.earned_at)),
                QLocale::ShortFormat);
        }
        viewer.list->setItem(row, 0, PictureCell(icon));
        viewer.list->setItem(row, 1, TextCell(QString::fromStdString(trophy.name)));
        viewer.list->setItem(row, 2, TextCell(detail));
        viewer.list->setItem(row, 3, TextCell(time));
        viewer.list->setItem(row, 4, PictureCell(GradePicture(trophy.grade)));
    }
}

} // namespace

void ShowTrophies(QWidget* owner) {
    QDialog dialog{owner};
    dialog.setWindowTitle(Tr("Trophies"));
    dialog.setFixedSize(ClientWidth, ClientHeight);
    dialog.setFont(DialogFont());

    Viewer viewer;
    size_t earned = 0;
    for (Trophies::List& list : Trophies::Read(Language::Current())) {
        for (Trophies::Trophy& trophy : list.trophies) {
            earned += trophy.earned ? 1 : 0;
            viewer.trophies.push_back(std::move(trophy));
        }
    }
    const size_t total = viewer.trophies.size();

    auto* const progress =
        new QLabel{total == 0 ? Tr("No trophies found.")
                              : Tr("Progress: {0}% ({1}/{2})",
                                   {std::to_string(earned * 100 / total), std::to_string(earned),
                                    std::to_string(total)}),
                   &dialog};
    progress->setGeometry(ProgressLine);
    for (size_t i = 0; i < Filters.size(); ++i) {
        viewer.filters[i] = new QCheckBox{Tr(Filters[i]), &dialog};
        viewer.filters[i]->setGeometry(FilterBoxes[i]);
        viewer.filters[i]->setChecked(FilterDefaults[i]);
    }

    viewer.list = new QTableWidget{0, static_cast<int>(Headers.size()), &dialog};
    viewer.list->setGeometry(ListArea);
    viewer.list->setSelectionBehavior(QAbstractItemView::SelectRows);
    viewer.list->setSelectionMode(QAbstractItemView::SingleSelection);
    viewer.list->setEditTriggers(QAbstractItemView::NoEditTriggers);
    viewer.list->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    viewer.list->setVerticalScrollMode(QAbstractItemView::ScrollPerPixel);
    viewer.list->setShowGrid(false);
    viewer.list->setWordWrap(true);
    viewer.list->setIconSize(QSize{IconSize, IconSize});
    viewer.list->verticalHeader()->hide();
    viewer.list->verticalHeader()->setDefaultSectionSize(RowHeight);
    viewer.list->horizontalHeader()->setSectionsClickable(false);
    viewer.list->horizontalHeader()->setSectionResizeMode(QHeaderView::Fixed);
    viewer.list->horizontalHeader()->setDefaultAlignment(Qt::AlignLeft | Qt::AlignVCenter);
    for (size_t i = 0; i < Headers.size(); ++i) {
        const int column = static_cast<int>(i);
        viewer.list->setHorizontalHeaderItem(column, new QTableWidgetItem{Tr(Headers[i])});
        viewer.list->setColumnWidth(column, ColumnWidths[i]);
    }
    FillList(viewer);
    ShowRows(viewer);

    auto* const ok = new QPushButton{Tr("OK"), &dialog};
    ok->setGeometry(OkButton);
    ok->setDefault(true);

    for (QCheckBox* const filter : viewer.filters) {
        QObject::connect(filter, &QCheckBox::toggled, &dialog, [&viewer] { ShowRows(viewer); });
    }
    QObject::connect(ok, &QPushButton::clicked, &dialog, &QDialog::accept);

    viewer.list->setFocus();
    dialog.exec();
}

} // namespace Gr2Port::Launcher
