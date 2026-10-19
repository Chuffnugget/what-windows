#pragma once
#include <QStringList>

// Reports contain only the public hardware fields. Keep the section and device
// beside missing fields so a generic "Unavailable" remains useful in the UI.
inline QStringList specificationIssues(const QString &report) {
    QStringList issues;
    QString section, device;
    bool sectionNext = true;
    for (const auto &line : report.split('\n')) {
        const auto text = line.trimmed();
        if (text.isEmpty()) {
            sectionNext = true;
            device.clear();
            continue;
        }
        if (text.startsWith("Some Windows queries were unavailable:"))
            continue; // The individual query errors below contain the reasons.
        if (text.startsWith("Unavailable Windows query:")) {
            issues.append(text);
            continue;
        }
        const bool missing = text.contains("unavailable", Qt::CaseInsensitive)
            || text.contains("unknown", Qt::CaseInsensitive)
            || text.contains("could not", Qt::CaseInsensitive);
        if (sectionNext && !missing) {
            section = text;
            sectionNext = false;
            continue;
        }
        if (missing) {
            QString context = line.startsWith(' ') && !device.isEmpty() ? device : section;
            auto detail = context.isEmpty() ? text : context + ": " + text;
            if (!text.contains('(') && !text.contains("query:", Qt::CaseInsensitive))
                detail += " (Windows or the device did not supply this value.)";
            issues.append(detail);
        }
        if (!line.startsWith(' '))
            device = text.section(" | ", 0, 0);
        sectionNext = false;
    }
    issues.removeDuplicates();
    return issues;
}
