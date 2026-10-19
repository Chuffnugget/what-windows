#include "modules/nvidia/NvmlReader.h"
#include <QCoreApplication>
#include <QJsonDocument>
#include <cstdio>
int main(int argc,char **argv){QCoreApplication app(argc,argv);const auto result=Nvidia::read();const auto bytes=QJsonDocument(result).toJson();fwrite(bytes.constData(),1,bytes.size(),stdout);return 0;}
