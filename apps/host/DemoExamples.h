#pragma once

#include <QString>

class QWidget;

// These examples use in-memory sample data and trusted, compiled widgets.
// They do not execute packages or grant additional browser capabilities.
QWidget *createDemoExample(const QString &id, QWidget *parent);
