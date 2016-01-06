/****************************************************************************
**
** Copyright (C) 2015 The Qt Company Ltd.
** Contact: http://www.qt.io/licensing/
**
** This file is part of the tools applications of the QtSerialBus module.
**
** $QT_BEGIN_LICENSE:LGPL3$
** Commercial License Usage
** Licensees holding valid commercial Qt licenses may use this file in
** accordance with the commercial license agreement provided with the
** Software or, alternatively, in accordance with the terms contained in
** a written agreement between you and The Qt Company. For licensing terms
** and conditions see http://www.qt.io/terms-conditions. For further
** information use the contact form at http://www.qt.io/contact-us.
**
** GNU Lesser General Public License Usage
** Alternatively, this file may be used under the terms of the GNU Lesser
** General Public License version 3 as published by the Free Software
** Foundation and appearing in the file LICENSE.LGPLv3 included in the
** packaging of this file. Please review the following information to
** ensure the GNU Lesser General Public License version 3 requirements
** will be met: https://www.gnu.org/licenses/lgpl.html.
**
** GNU General Public License Usage
** Alternatively, this file may be used under the terms of the GNU
** General Public License version 2.0 or later as published by the Free
** Software Foundation and appearing in the file LICENSE.GPL included in
** the packaging of this file. Please review the following information to
** ensure the GNU General Public License version 2.0 requirements will be
** met: http://www.gnu.org/licenses/gpl-2.0.html.
**
** $QT_END_LICENSE$
**
****************************************************************************/

#include "canbusutil.h"

static const qint8 MAXNORMALPAYLOADSIZE = 8;
static const qint8 MAXEXTENDEDPAYLOADSIZE = 64;

CanBusUtil::CanBusUtil(QTextStream& output, QCoreApplication& app, QObject *parent)
  : QObject(parent),
    canBus(QCanBus::instance()),
    output(output),
    app(app),
    readTask(new ReadTask(output, this))
{
}

bool CanBusUtil::start(int argc, char *argv[])
{
    if (!canBus) {
        output << "Unable to create QCanBus" << endl;
        return false;
    }

    if (!parseArgs(argc, argv))
        return false;

    if (!connectCanDevice())
        return false;

    if (listening) {
        connect(canDevice.data(), &QCanBusDevice::framesReceived, readTask.data(), &ReadTask::checkMessages);
    } else {
        if (!sendData())
            return false;
        QTimer::singleShot(0, &app, SLOT(quit()));
    }

    return true;
}

void CanBusUtil::printUsage()
{
    output << "Usage: canbusutil [options] <plugin> <device> [data]" << endl
           << "-l             start listening CAN data on device" << endl
           << "--list-plugins lists all available plugins" << endl
           << "<plugin>       plugin name to use. See --list-plugins." << endl
           << "<device>       device to use" << endl
           << "[data]         Data to send if -l is not specified. Format: " << endl
           << "                   <id>#{R|payload}   (CAN 2.0 frames)," << endl
           << "                   <id>##{payload}    (CAN FD frames)," << endl
           << "               where {payload} has 0..8 (0..64 CAN FD) ASCII hex-value pairs," << endl
           << "               e.g. 1#1a2b3c" << endl;
}

void CanBusUtil::printPlugins()
{
    QList<QByteArray> plugins = canBus->plugins();
    output << "Plugins: " << endl;
    for (int i = 0; i < plugins.size(); i++)
        output << plugins.at(i) << endl;
}

void CanBusUtil::printDataUsage()
{
    output << "Invalid [data] field, use format: " << endl
           << "    <id>#{R|payload}   (CAN 2.0 frames)," << endl
           << "    <id>##{payload}    (CAN FD frames)," << endl
           << "{payload} has 0..8 (0..64 CAN FD) ASCII hex-value pairs" << endl;
}


bool CanBusUtil::parseArgs(int argc, char *argv[])
{
    QString arg1(argv[1]);

    if (arg1 == QStringLiteral("--list-plugins")) {
        printPlugins();
        return false;
    }

    if (argc != 4) {
        printUsage();
        return false;
    }

    listening = arg1 == QStringLiteral("-l");
    if (listening) {
        pluginName = QString(argv[2]);
        deviceName = QString(argv[3]);
    } else {
        pluginName = QString(arg1);
        deviceName = QString(argv[2]);
        data = QString(argv[3]);
    }
    return true;
}

bool CanBusUtil::parseDataField(qint32& id, QString& payload)
{
    int hashMarkPos = data.indexOf('#');
    if (hashMarkPos < 0) {
        output << "No hash mark found!" << endl;
        printDataUsage();
        return false;
    }

    id = data.left(hashMarkPos).toInt();
    payload = data.right(data.length() - hashMarkPos - 1);
    if (payload.size() == 0) {
        output << "Payload size zero!" << endl;
        printDataUsage();
        return false;
    }
    return true;
}

bool CanBusUtil::parsePayloadField(QString payload, bool& rtrFrame, QByteArray& bytes)
{
    bool fdFrame = false;

    if (payload[0].toUpper() == 'R') {
        rtrFrame = true;
        return true;
    } else if (payload[0] == '#') {
        fdFrame = true;
        payload = payload.right(payload.size() - 1);
    }

    rtrFrame = false;

    if (payload.size() % 2 == 0) {
        for (int i=0; i < payload.size(); i+=2) {
            bool numberConverOk = true;
            quint8 high = QString(payload[i]).toInt(&numberConverOk, 16);
            if (!numberConverOk) {
                output << "Could not convert '" << QString(payload[i]) << "' to a number"<< endl;
                printDataUsage();
                return false;
            }

            quint8 low = QString(payload[i+1]).toInt(&numberConverOk, 16);
            if (!numberConverOk) {
                output << "Could not convert '" << QString(payload[i+1]) << "' to a number" << endl;
                printDataUsage();
                return false;
            }

            quint8 byte = (high << 4) | (low);
            bytes.append(byte);
        }
        qint8 size = bytes.size();
        qint8 maxsize = fdFrame ? MAXEXTENDEDPAYLOADSIZE : MAXNORMALPAYLOADSIZE;
        if (size > maxsize) {
            output << "Warning! payload size too great. Size: " << size << ", max allowed size in frame: " << maxsize << endl
                   << "Clipping payload to fit frame..." << endl;
            size = maxsize;
            bytes = bytes.left(size);
        }
    } else {
        output << "Payload size not multiple of two!" << endl;
        printDataUsage();
        return false;
    }
    return true;
}

bool CanBusUtil::connectCanDevice()
{
    bool foundPlugin = false;
    QList<QByteArray> plugins = canBus->plugins();
    for (int i = 0; i < plugins.size(); i++)
    {
        if (plugins.at(i) == pluginName) {
            canDevice = canBus->createDevice(plugins.at(i), deviceName);
            if (!canDevice) {
                output << "Unable to create QCanBusDevice with device name: " << deviceName << endl;
                return false;
            }
            connect(canDevice.data(), &QCanBusDevice::errorOccurred, readTask.data(), &ReadTask::receiveError);
            if (!canDevice->connectDevice()) {
                output << "Unable to connect QCanBusDevice with device name: " << deviceName << endl;
                return false;
            }
            foundPlugin = true;
        }
    }

    if (!foundPlugin) {
        output << "Could not find suitable plugin." << endl;
        printPlugins();
        return false;
    }
    return true;
}

bool CanBusUtil::sendData()
{
    qint32 id;
    QString payload;
    bool rtrFrame;
    QByteArray bytes;
    QCanBusFrame frame;

    if (parseDataField(id, payload) == false) {
        return false;
    }

    if (parsePayloadField(payload, rtrFrame, bytes) == false) {
        return false;
    }

    if (id > 0x7FF)  { //11 bits
        if (id > 0x1FFFFFFF) { // 29 bits
            id = 0x1FFFFFFF;
            output << "Warning! Id does not fit into Extended Frame Format, setting id to: " << id << endl;
        }
        frame.setExtendedFrameFormat(true);
    }

    if (rtrFrame) {
        frame.setFrameType(QCanBusFrame::RemoteRequestFrame);
    } else {
        frame.setPayload(bytes);
    }

    frame.setFrameId(id);
    return canDevice->writeFrame(frame);
}
