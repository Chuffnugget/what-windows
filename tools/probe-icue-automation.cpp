#include "modules/icue/IcueAutomation.h"
#include <QCoreApplication>
#include <QJsonDocument>
#include <cstdio>
int main(int argc,char **argv) {
    QCoreApplication app(argc,argv);
    IcueAutomation api;
    const auto state=api.poll(true);
    const auto json=QJsonDocument(state).toJson();
    std::fwrite(json.constData(),1,json.size(),stdout);
    return state["connected"].toBool() ? 0 : 1;
}
