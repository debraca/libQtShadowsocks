/*
 * controller.cpp - the source file of Controller class
 *
 * Copyright (C) 2014-2017 Symeon Huang <hzwhuang@gmail.com>
 *
 * This file is part of the libQtShadowsocks.
 *
 * libQtShadowsocks is free software; you can redistribute it and/or modify
 * it under the terms of the GNU Lesser General Public License as published
 * by the Free Software Foundation; either version 3 of the License, or
 * (at your option) any later version.
 *
 * libQtShadowsocks is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 * GNU Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public License
 * along with libQtShadowsocks; see the file LICENSE. If not, see
 * <http://www.gnu.org/licenses/>.
 */

#include <QHostInfo>
#include <QTcpSocket>
#include <botan/init.h>
#include "controller.h"
#include "encryptor.h"

using namespace QSS;

Controller::Controller(const Profile &_profile,
                       bool is_local,
                       bool auto_ban,
                       QObject *parent) :
    QObject(parent),
    profile(_profile),
    valid(true),
    isLocal(is_local),
    autoBan(auto_ban)
{
    try {
        Botan::LibraryInitializer::initialize("thread_safe");
    } catch (std::exception &e) {
        qCritical("%s", e.what());
    }

    /*
     * the default QHostAddress constructor will construct "::" as AnyIPv6
     * we explicitly use Any to enable dual stack
     * which is the case in other shadowsocks ports
     */
    if (profile.serverAddress() == "::") {
        serverAddress = Address(QHostAddress::Any, profile.serverPort());
    } else {
        serverAddress = Address(profile.serverAddress(), profile.serverPort());
        serverAddress.lookUp();
    }

    tcpServer = new TcpServer(profile.method(),
                              profile.password(),
                              profile.timeout(),
                              isLocal,
                              autoBan,
                              profile.otaEnabled(),
                              serverAddress,
                              this);

    //FD_SETSIZE which is the maximum value on *nix platforms. (1024 by default)
    tcpServer->setMaxPendingConnections(FD_SETSIZE);
    udpRelay = new UdpRelay(profile.method(),
                            profile.password(),
                            isLocal,
                            autoBan,
                            profile.otaEnabled(),
                            serverAddress,
                            this);
    httpProxy = new HttpProxy(this);

    connect(tcpServer, &TcpServer::acceptError,
            this, &Controller::onTcpServerError);
    connect(tcpServer, &TcpServer::info, this, &Controller::info);
    connect(tcpServer, &TcpServer::debug, this, &Controller::debug);
    connect(tcpServer, &TcpServer::bytesRead, this, &Controller::onBytesRead);
    connect(tcpServer, &TcpServer::bytesSend, this, &Controller::onBytesSend);
    connect(tcpServer, &TcpServer::latencyAvailable,
            this, &Controller::tcpLatencyAvailable);

    connect(udpRelay, &UdpRelay::info, this, &Controller::info);
    connect(udpRelay, &UdpRelay::debug, this, &Controller::debug);
    connect(udpRelay, &UdpRelay::bytesRead, this, &Controller::onBytesRead);
    connect(udpRelay, &UdpRelay::bytesSend, this, &Controller::onBytesSend);

    connect(httpProxy, &HttpProxy::info, this, &Controller::info);

    connect(&serverAddress, &Address::lookedUp,
            this, &Controller::onServerAddressLookedUp);
}

Controller::~Controller()
{
    if (tcpServer->isListening()) {
        stop();
    }
    Botan::LibraryInitializer::deinitialize();
}

bool Controller::start()
{
    if (!valid) {
        emit info("Controller is not valid. Maybe improper setup?");
        return false;
    }

    bool listen_ret = false;

    QString sstr("TCP server listen at port ");
    if (isLocal) {
        emit info("Running in local mode.");
        sstr.append(QString::number(profile.localPort()));
        listen_ret = tcpServer->listen(
                    getLocalAddr(),
                    profile.httpProxy() ? 0 : profile.localPort());
        if (listen_ret) {
            listen_ret = udpRelay->listen(getLocalAddr(), profile.localPort());
            if (profile.httpProxy() && listen_ret) {
                emit info("SOCKS5 port is "
                          + QString::number(tcpServer->serverPort()));
                if (httpProxy->httpListen(getLocalAddr(),
                                          profile.localPort(),
                                          tcpServer->serverPort())) {
                    emit info("Running as a HTTP proxy server");
                } else {
                    emit info("HTTP proxy server listen failed.");
                    listen_ret = false;
                }
            }
        }
    } else {
        emit info("Running in server mode.");
        sstr.append(QString::number(profile.serverPort()));
        listen_ret = tcpServer->listen(serverAddress.getFirstIP(),
                                       profile.serverPort());
        if (listen_ret) {
            listen_ret = udpRelay->listen(serverAddress.getFirstIP(),
                                       profile.serverPort());
        }
    }

    if (listen_ret) {
        emit info(sstr);
        emit runningStateChanged(true);
        if (profile.otaEnabled()) {
            emit info("One-time message authentication is enabled.");
        }
    } else {
        emit info("TCP server listen failed.");
    }

    return listen_ret;
}

void Controller::stop()
{
    httpProxy->close();
    tcpServer->close();
    udpRelay->close();
    emit runningStateChanged(false);
    emit debug("Stopped.");
}

QHostAddress Controller::getLocalAddr()
{
    QHostAddress addr(QString::fromStdString(profile.localAddress()));
    if (!addr.isNull()) {
        return addr;
    } else {
        emit info("Can't get address from " +
                  QString::fromStdString(profile.localAddress()) +
                  ". Using localhost instead.");
        return QHostAddress::LocalHost;
    }
}

void Controller::onTcpServerError(QAbstractSocket::SocketError err)
{
    emit info("TCP server error: " + tcpServer->errorString());

    //can't continue if address is already in use
    if (err == QAbstractSocket::AddressInUseError) {
        stop();
    }
}

void Controller::onBytesRead(const qint64 &r)
{
    if (r != -1) {//-1 means read failed. don't count
        bytesReceived += r;
        emit newBytesReceived(r);
        emit bytesReceivedChanged(bytesReceived);
    }
}

void Controller::onBytesSend(const qint64 &s)
{
    if (s != -1) {//-1 means write failed. don't count
        bytesSent += s;
        emit newBytesSent(s);
        emit bytesSentChanged(bytesSent);
    }
}

void Controller::onServerAddressLookedUp(const bool success, const QString err)
{
    if (!success) {
        emit info("Shadowsocks server DNS lookup failed: " + err);
    }
}
