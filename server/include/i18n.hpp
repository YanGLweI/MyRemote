#pragma once

class QCoreApplication;

// The one place Qt still speaks English in this window: the standard buttons of
// a message box or input dialog come from the framework, not from our strings.
namespace i18n {

// Translate exactly those button words, leaving every other string alone. A
// .qm catalog would say more, at the cost of a file the package must ship and
// keep in step with the Qt build.
void install_button_translator(QCoreApplication& app);

}  // namespace i18n
