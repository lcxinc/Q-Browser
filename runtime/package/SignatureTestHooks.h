#pragma once

#ifdef Q_BROWSER_SIGNATURE_TESTING

#include <QByteArrayView>

#include <functional>

namespace qbrowser_signature_testing
{
struct SignatureTestHooks final
{
    bool failAfterPrivatePemWrite = false;
    std::function<void(QByteArrayView)> afterPrivateBioCleanseBeforeFree;
};

void setSignatureTestHooks(SignatureTestHooks hooks);
void resetSignatureTestHooks();
[[nodiscard]] const SignatureTestHooks &signatureTestHooks();
}

#endif
