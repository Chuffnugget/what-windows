#include "modules/windows/displays/Displays.h"
#include <QCoreApplication>
#include <QJsonDocument>
#include <cstdio>
int main(int argc,char **argv){QCoreApplication app(argc,argv);auto state=Displays::query();
 if(state["available"].toBool())state["validation_error"]=Displays::apply(Displays::layout(state),true);
 auto bytes=QJsonDocument(state).toJson();fwrite(bytes.constData(),1,size_t(bytes.size()),stdout);
 return state["available"].toBool()&&state["validation_error"].toString().isEmpty()?0:1;}
