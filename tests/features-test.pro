QT += widgets dbus network testlib
CONFIG += c++20 console testcase
CONFIG -= app_bundle
TARGET = features-test
INCLUDEPATH += $$PWD/../src
SOURCES += $$PWD/features-test.cpp \
           $$PWD/../src/core/Alerts.cpp $$PWD/../src/core/Cgroups.cpp $$PWD/../src/core/Cli.cpp \
           $$PWD/../src/core/FlightRecorder.cpp $$PWD/../src/core/FlightReport.cpp $$PWD/../src/core/Hyprland.cpp \
           $$PWD/../src/core/Settings.cpp $$PWD/../src/core/Fs.cpp $$PWD/../src/core/Actions.cpp \
           $$PWD/../src/core/Sampler.cpp $$PWD/../src/core/HwIds.cpp $$PWD/../src/core/Benchmarks.cpp \
           $$PWD/../src/core/Startup.cpp \
           $$PWD/../src/ui/CommandPalette.cpp $$PWD/../src/ui/Theme.cpp $$PWD/../src/ui/OmarchyPalette.cpp
HEADERS += $$PWD/../src/core/Settings.h $$PWD/../src/ui/CommandPalette.h $$PWD/../src/ui/OmarchyPalette.h
OBJECTS_DIR = obj
MOC_DIR = moc
