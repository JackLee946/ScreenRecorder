QT       += core gui widgets
CONFIG   += c++11
TARGET    = ScreenRecorder
TEMPLATE  = app

SOURCES  += main.cpp

# 让中文字符串在 MSVC 下不乱码
msvc: QMAKE_CXXFLAGS += /utf-8
