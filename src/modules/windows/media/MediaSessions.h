#pragma once
#include <QJsonArray>

// Called only by the dedicated media worker. Track metadata is opt-in.
QJsonArray readMediaSessions(bool includeTitles);
