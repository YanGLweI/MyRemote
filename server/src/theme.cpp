#include "theme.hpp"

#include <QApplication>
#include <QPalette>
#include <QWidget>

namespace theme {

const Palette& colors() {
    // Positional, in Palette declaration order: designated initializers need C++20
    // and an aggregate, and QColor members make Palette neither.
    static const Palette p{
        QColor(0x17, 0x1A, 0x1D),  // bg: window
        QColor(0x0C, 0x0E, 0x10),  // canvas: letterbox behind a remote desktop
        QColor(0x1F, 0x24, 0x2A),  // surface: panels, inputs
        QColor(0x27, 0x2D, 0x35),  // surface_raised: controls
        QColor(0x2F, 0x36, 0x40),  // surface_hover
        QColor(0x33, 0x3A, 0x43),  // line
        QColor(0xE6, 0xE9, 0xEC),  // text
        QColor(0x98, 0xA2, 0xAD),  // muted
        QColor(0x3E, 0x9B, 0x6E),  // accent
        // State colours are meaning, not decoration: a roster dot, a row and the
        // session pill all have to agree on what amber means.
        QColor(0x3E, 0x9B, 0x6E),  // live
        QColor(0xD9, 0xA0, 0x3C),  // reconnecting
        QColor(0x6E, 0x76, 0x81),  // stale
        QColor(0xC0, 0x55, 0x4E),  // denied
    };
    return p;
}

namespace {

// Sizes are in points, not pixels: the panel this console is expected to live on
// runs at ~198% scaling, and a pixel size would not follow the display.
constexpr int kMeta = 8;
constexpr int kSection = 11;
constexpr int kBadge = 8;

}  // namespace

QFont base_font() {
    // The platform default (Segoe UI 9pt) is kept on purpose; only the hinting is
    // pinned, because Segoe at this size reads as a blur without it.
    static const QFont font = [] {
        QFont f = QApplication::font();
        f.setHintingPreference(QFont::PreferFullHinting);
        return f;
    }();
    return font;
}

QFont section_font() {
    static const QFont font = [] {
        QFont f = base_font();
        f.setPointSize(kSection);
        f.setWeight(QFont::DemiBold);
        return f;
    }();
    return font;
}

QFont meta_font() {
    static const QFont font = [] {
        QFont f = base_font();
        f.setFamilies({QStringLiteral("Cascadia Mono"), QStringLiteral("Consolas"),
                       QStringLiteral("Courier New")});
        f.setPointSize(kMeta);
        return f;
    }();
    return font;
}

QFont badge_font() {
    static const QFont font = [] {
        QFont f = base_font();
        f.setPointSize(kBadge);
        return f;
    }();
    return font;
}

QString stylesheet() {
    const Palette& c = colors();
    // Named tokens rather than positional %1..%n: the rule blocks below are read
    // and edited far more often than they are written.
    struct Token {
        const char* name;
        QString value;
    };
    const Token subs[] = {
        {"@TEXT@", c.text.name()},         {"@BG@", c.bg.name()},
        {"@SURF@", c.surface.name()},      {"@RAISE@", c.surface_raised.name()},
        {"@HOVER@", c.surface_hover.name()}, {"@LINE@", c.line.name()},
        {"@MUTED@", c.muted.name()},       {"@ACCENT@", c.accent.name()},
    };

    QString css = QStringLiteral(
        // Colour only: setting a background on bare QWidget would override the
        // widgets that paint themselves, including the remote desktop.
        "QWidget { color: @TEXT@; }"
        "QMainWindow, QDialog, QMenu, QStatusBar, QToolTip { background: @BG@; }"

        "QStatusBar { border-top: 1px solid @LINE@; color: @MUTED@; }"
        "QStatusBar::item { border: none; }"
        "QSplitter::handle { background: @LINE@; }"
        "QSplitter::handle:horizontal { width: 1px; }"
        "QToolTip { color: @TEXT@; border: 1px solid @LINE@; padding: 4px 6px; }"

        "QLineEdit { background: @SURF@; border: 1px solid @LINE@; border-radius: 4px;"
        "  padding: 4px 8px; color: @TEXT@; }"
        "QLineEdit:focus { border: 1px solid @ACCENT@; }"

        // A QPushButton ignores background/radius unless a border is named.
        "QPushButton { background: @RAISE@; color: @TEXT@; border: 1px solid @LINE@;"
        "  border-radius: 4px; padding: 4px 10px; }"
        "QPushButton:hover { background: @HOVER@; border-color: @MUTED@; }"
        "QPushButton:pressed { background: @SURF@; }"
        "QPushButton:checked { border-color: @ACCENT@; color: @ACCENT@; }"
        "QPushButton:disabled { color: @MUTED@; background: @SURF@;"
        "  border-color: @LINE@; }"

        // A styled QComboBox keeps a white popup unless its item view is named
        // too, and its arrow container has to be flattened explicitly.
        "QComboBox { background: @RAISE@; color: @TEXT@; border: 1px solid @LINE@;"
        "  border-radius: 4px; padding: 3px 8px; }"
        "QComboBox:hover { border-color: @MUTED@; }"
        "QComboBox::drop-down { subcontrol-origin: padding;"
        "  subcontrol-position: center right; width: 16px; border-left: 1px"
        "  solid @LINE@; }"
        "QComboBox QAbstractItemView { background: @SURF@; color: @TEXT@;"
        "  border: 1px solid @LINE@; selection-background-color: @HOVER@;"
        "  selection-color: @TEXT@; outline: none; }"

        // Underline tabs: the row of tabs is context, not a set of buttons.
        "QTabWidget::pane { border: none; }"
        "QTabBar { background: @BG@; border: none; }"
        "QTabBar::tab { background: @BG@; color: @MUTED@; border: none;"
        "  border-bottom: 2px solid @BG@; padding: 6px 10px; }"
        "QTabBar::tab:hover { color: @TEXT@; }"
        "QTabBar::tab:selected { color: @TEXT@; border-bottom: 2px solid @ACCENT@; }"

        // Marks that live inside another widget — a line edit's search and clear
        // buttons, a tab's close button — must not be painted as native chips:
        // the style would drop a light plate inside a dark surface.
        "QLineEdit QToolButton, QTabBar QToolButton { border: none;"
        "  background: transparent; padding: 0; margin: 0; }"
        "QTabBar QToolButton:hover { background: @HOVER@; border-radius: 3px; }"

        "QListView, QAbstractItemView { background: @SURF@; color: @TEXT@;"
        "  border: none; outline: none;"
        "  selection-background-color: @HOVER@; selection-color: @TEXT@; }"
        "QMenu { border: 1px solid @LINE@; }"
        "QMenu::item { padding: 5px 18px; }"
        "QMenu::item:selected { background: @HOVER@; }"
        "QMenu::separator { height: 1px; background: @LINE@; margin: 4px 6px; }"

        // Native scrollbars ignore the handle unless the step buttons are zeroed.
        "QScrollBar { background: transparent; }"
        "QScrollBar:vertical { width: 10px; margin: 0; }"
        "QScrollBar:horizontal { height: 10px; margin: 0; }"
        "QScrollBar::handle { background: @MUTED@; border-radius: 4px;"
        "  min-height: 28px; min-width: 28px; }"
        "QScrollBar::handle:hover { background: @TEXT@; }"
        "QScrollBar::add-line, QScrollBar::sub-line"
        "  { width: 0; height: 0; background: none; border: none; }"
        "QScrollBar::add-page, QScrollBar::sub-page { background: none; }");

    for (const auto& sub : subs) {
        css.replace(QLatin1String(sub.name), sub.value);
    }
    return css;
}

void tint(QWidget* w, const QColor& c) {
    w->setStyleSheet(QStringLiteral("color: %1;").arg(c.name()));
}

void apply(QApplication& app) {
    // windowsvista keeps the native rendering of the parts the stylesheet does not
    // reach; it must be selected before the palette, or the style re-applies its
    // own light colours over it.
    app.setStyle(QStringLiteral("windowsvista"));
    app.setFont(base_font());

    QPalette pal = app.palette();
    pal.setColor(QPalette::Window, colors().bg);
    pal.setColor(QPalette::WindowText, colors().text);
    pal.setColor(QPalette::Base, colors().surface);
    pal.setColor(QPalette::AlternateBase, colors().surface_raised);
    pal.setColor(QPalette::Text, colors().text);
    pal.setColor(QPalette::Button, colors().surface_raised);
    pal.setColor(QPalette::ButtonText, colors().text);
    pal.setColor(QPalette::Highlight, colors().accent.darker(140));
    pal.setColor(QPalette::HighlightedText, colors().text);
    pal.setColor(QPalette::PlaceholderText, colors().muted);
    pal.setColor(QPalette::ToolTipBase, colors().surface_raised);
    pal.setColor(QPalette::ToolTipText, colors().text);
    pal.setColor(QPalette::Link, colors().accent);
    // A disabled label on a dark surface has to stay legible: dim it, do not wash
    // it to the platform's light grey.
    pal.setColor(QPalette::Disabled, QPalette::WindowText, colors().stale);
    pal.setColor(QPalette::Disabled, QPalette::Text, colors().stale);
    pal.setColor(QPalette::Disabled, QPalette::ButtonText, colors().stale);
    app.setPalette(pal);

    app.setStyleSheet(stylesheet());
}

}  // namespace theme
