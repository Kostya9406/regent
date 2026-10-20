QT += widgets dbus network
CONFIG += c++20 warn_on
CONFIG -= debug_and_release
TARGET = regent
TEMPLATE = app

INCLUDEPATH += src

# Hardening, as Arch's makepkg would add: bounds-checked libc calls, stack
# canaries, a read-only GOT and PLT resolved at start.
QMAKE_CXXFLAGS += -fstack-protector-strong -fstack-clash-protection
QMAKE_CXXFLAGS_RELEASE += -D_FORTIFY_SOURCE=3  # needs optimisation; makepkg's CXXFLAGS carry it too
contains(QT_ARCH, x86_64): QMAKE_CXXFLAGS += -fcf-protection  # x86 only (CET)
QMAKE_LFLAGS += -Wl,-z,relro,-z,now -Wl,-z,noexecstack
SOURCES += $$files(src/*.cpp, true)
HEADERS += $$files(src/*.h, true)

# Out-of-source: mkdir build && cd build && qmake6 .. && make
OBJECTS_DIR = $$OUT_PWD/obj
MOC_DIR = $$OUT_PWD/moc
RCC_DIR = $$OUT_PWD/rcc
DESTDIR = $$OUT_PWD

# make install: ~/.local by default; packages pass PREFIX=/usr and INSTALL_ROOT.
# The default is fixed when qmake runs: for /usr, run qmake with PREFIX=/usr
# rather than plain qmake followed by sudo make install.
isEmpty(PREFIX): PREFIX = $$(HOME)/.local
target.path = $$PREFIX/bin
desktop.files = packaging/regent.desktop
desktop.path = $$PREFIX/share/applications
icon.files = packaging/regent.svg
icon.path = $$PREFIX/share/icons/hicolor/scalable/apps
INSTALLS += target desktop icon
