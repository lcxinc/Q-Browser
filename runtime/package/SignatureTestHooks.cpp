#include "SignatureTestHooks.h"

#ifdef Q_BROWSER_SIGNATURE_TESTING

#include <utility>

namespace qbrowser_signature_testing
{
namespace
{
SignatureTestHooks hooks;
}

void setSignatureTestHooks(SignatureTestHooks newHooks)
{
    hooks = std::move(newHooks);
}

void resetSignatureTestHooks()
{
    hooks = {};
}

const SignatureTestHooks &signatureTestHooks()
{
    return hooks;
}
}

#endif
