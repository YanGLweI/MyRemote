#include "i18n.hpp"

#include <QCoreApplication>
#include <QString>
#include <QTranslator>

namespace i18n {

namespace {

struct Entry {
    const char* source;
    const char* chinese;
};

// The words QDialogButtonBox asks its style/theme for. Both contexts Qt uses
// for the same buttons are covered: a message box fills its button box through
// QPlatformTheme, while a plain QDialog lays out a QDialogButtonBox directly.
const Entry kButtons[] = {
    {"OK", "确定"},        {"Cancel", "取消"},   {"Close", "关闭"},
    {"Yes", "是"},         {"No", "否"},         {"Apply", "应用"},
    {"Reset", "重置"},     {"Save", "保存"},     {"Discard", "丢弃"},
    {"Retry", "重试"},     {"Ignore", "忽略"},   {"Help", "帮助"},
    {"Open", "打开"},      {"Continue", "继续"}, {"Back", "返回"},
    {"Quit", "退出"},
};

class ButtonTranslator : public QTranslator {
public:
    explicit ButtonTranslator(QObject* parent) : QTranslator(parent) {}

    // A null return means "not mine", and Qt carries on looking as if this
    // translator were not installed at all.
    QString translate(const char* context, const char* source_text,
                      const char* = nullptr, int = -1) const override {
        const QString name = QString::fromLatin1(context);
        if (name != QLatin1String("QDialogButtonBox") &&
            name != QLatin1String("QPlatformTheme")) {
            return {};
        }
        for (const Entry& entry : kButtons) {
            if (qstrcmp(source_text, entry.source) == 0) {
                return QString::fromUtf8(entry.chinese);
            }
        }
        return {};
    }

    bool isEmpty() const override { return false; }
};

}  // namespace

void install_button_translator(QCoreApplication& app) {
    // Not owned by the application: parented here so it outlives every dialog
    // the app can show and dies with the app itself.
    app.installTranslator(new ButtonTranslator(&app));
}

}  // namespace i18n
