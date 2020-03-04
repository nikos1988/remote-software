/******************************************************************************
 *
 * Copyright (C) 2020 Markus Zehnder <business@markuszehnder.ch>
 * Copyright (C) 2018-2020 Marton Borzak <hello@martonborzak.com>
 *
 * This file is part of the YIO-Remote software project.
 *
 * YIO-Remote software is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * YIO-Remote software is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with YIO-Remote software. If not, see <https://www.gnu.org/licenses/>.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *****************************************************************************/

#include "softwareupdate.h"

#include <QDataStream>
#include <QDebug>
#include <QJsonDocument>
#include <QLoggingCategory>

#include "config.h"
#include "notifications.h"
#include "standbycontrol.h"

static const QString UPDATE_BASENAME = "latest.";
static const QString META_FILENAME = "latest.version";

static Q_LOGGING_CATEGORY(CLASS_LC, "softwareupdate");

SoftwareUpdate *SoftwareUpdate::s_instance = nullptr;

SoftwareUpdate::SoftwareUpdate(const QVariantMap &cfg, BatteryFuelGauge *batteryFuelGauge, QObject *parent)
    : QObject(parent),
      m_batteryFuelGauge(batteryFuelGauge),
      m_autoUpdate(cfg.value("autoUpdate", false).toBool()),
      m_updateUrl(cfg.value("updateUrl", "https://update.yio.app/update").toUrl()),
      m_downloadDir(cfg.value("downloadDir", "/tmp/yio").toString()) {
    Q_ASSERT(m_batteryFuelGauge);

    s_instance = this;
    m_downloadDir.makeAbsolute();

    int checkIntervallSec = cfg.value("checkInterval", 3600).toInt();  // 1 hour
    if (checkIntervallSec < 600) {
        qCInfo(CLASS_LC) << "Adjusting check intervall to 600s because configured value is too short!";
        checkIntervallSec = 600;
    }

    qCDebug(CLASS_LC) << "Auto update:" << m_autoUpdate << ", url:" << m_updateUrl.toString()
                      << ", download dir:" << m_downloadDir.path();

    m_checkForUpdateTimer.setInterval(checkIntervallSec * 1000);
    connect(&m_checkForUpdateTimer, &QTimer::timeout, this, &SoftwareUpdate::onCheckForUpdateTimerTimeout);

    connect(&m_manager, &QNetworkAccessManager::finished, this, &SoftwareUpdate::checkForUpdateFinished);

    connect(&m_fileDownload, &FileDownload::downloadProgress, this, &SoftwareUpdate::onDownloadProgress);
    connect(&m_fileDownload, &FileDownload::downloadComplete, this, &SoftwareUpdate::onDownloadComplete);
    connect(&m_fileDownload, &FileDownload::downloadFailed, this, &SoftwareUpdate::onDownloadFailed);
}

SoftwareUpdate::~SoftwareUpdate() {
    s_instance = nullptr;
    if (m_checkForUpdateTimer.isActive()) {
        m_checkForUpdateTimer.stop();
    }
}

void SoftwareUpdate::start() {
    if (m_autoUpdate) {
        // start update checker timer
        setAutoUpdate(true);

        // check for update as well after startup delay.
        // WiFi might not yet be ready and update check might delay initial screen loading!
        QTimer::singleShot(60 * 1000, this, &SoftwareUpdate::checkForUpdate);
    }
}

QObject *SoftwareUpdate::getQMLInstance(QQmlEngine *engine, QJSEngine *scriptEngine) {
    Q_UNUSED(scriptEngine)
    Q_ASSERT(s_instance);

    QObject *instance = s_instance;
    engine->setObjectOwnership(instance, QQmlEngine::CppOwnership);
    return instance;
}

void SoftwareUpdate::setAutoUpdate(bool update) {
    m_autoUpdate = update;
    emit autoUpdateChanged();
    qCDebug(CLASS_LC) << "Autoupdate:" << m_autoUpdate;

    if (update == false) {
        qCDebug(CLASS_LC) << "Stopping software update timer";
        m_checkForUpdateTimer.stop();
    } else if (!m_checkForUpdateTimer.isActive()) {
        qCDebug(CLASS_LC) << "Starting software update timer. Interval:" << m_checkForUpdateTimer.interval() / 1000
                          << "s";
        m_checkForUpdateTimer.start();
    }
}

void SoftwareUpdate::onCheckForUpdateTimerTimeout() { checkForUpdate(); }

void SoftwareUpdate::checkForUpdate() {
    // TODO(zehnm) enhance StandbyControl with isWifiAvailable() to encapsulate standby logic.
    //             This allows enhanced standby logic in the future (loose coupling).
    // only check update if standby mode is not WIFI_OFF
    if (StandbyControl::getInstance()->mode() == StandbyControl::WIFI_OFF) {
        qCDebug(CLASS_LC) << "Update check skipped: WiFi not available";
        return;
    }

    if (m_checkOrDownloadActive) {
        return;
    }
    m_checkOrDownloadActive = true;

    qCDebug(CLASS_LC) << "Checking for update. Current version:" << currentVersion();

    QNetworkRequest request(m_updateUrl);
    request.setRawHeader("Content-Type", "application/json");
    request.setRawHeader("Accept", "application/json");

    // TODO(zehm) use GET with query parameters
    QJsonObject json;
    json.insert("version", currentVersion());
    json.insert("device_id", getDeviceType());

    QJsonDocument jsonDoc(json);
    QByteArray    jsonData = jsonDoc.toJson();

    m_manager.post(request, jsonData);  // QNetworkReply will be deleted in checkForUpdateFinished slot!
}

void SoftwareUpdate::checkForUpdateFinished(QNetworkReply *reply) {
    if (reply->error() != QNetworkReply::NoError) {
        qCWarning(CLASS_LC) << "Network reply error: " << reply->errorString();
        Notifications::getInstance()->add(true, tr("Cannot connect to the update server."));
        m_checkOrDownloadActive = false;
        reply->deleteLater();
        return;
    }

    QByteArray    responseData = reply->readAll();
    QJsonDocument jsonResponse = QJsonDocument::fromJson(responseData);
    QJsonObject   jsonObject = jsonResponse.object();

    if (jsonObject.contains("error")) {
        m_downloadUrl.clear();
        m_newVersion.clear();
        qCWarning(CLASS_LC) << "Error" << jsonObject["error"].toString();
        Notifications::getInstance()->add(true, tr("Software update:") + " " + jsonObject["error"].toString());
        m_checkOrDownloadActive = false;
        reply->deleteLater();
        return;
    }

    m_downloadUrl.setUrl(jsonObject["url"].toString());
    m_newVersion = jsonObject["latest"].toString();

    // TODO(zehnm) validate returned data! Never trust json!

    if (!m_downloadUrl.isValid()) {
        qCWarning(CLASS_LC) << "Invalid download URL:" << m_downloadUrl;
        // TODO(zehnm) abort / error handling
    }

    qCDebug(CLASS_LC) << "Url:" << m_downloadUrl << "Version:" << m_newVersion;
    m_updateAvailable = false;

    if (isAlreadyDownloaded(m_newVersion)) {
        qCInfo(CLASS_LC) << "Update has already been downloaded:" << m_newVersion;
    } else {
        // check if it's a newer version than we have
        if (isNewerVersion(currentVersion(), m_newVersion)) {
            m_updateAvailable = true;

            // send a notification
            QObject *param = this;
            Notifications::getInstance()->add(
                false, tr("New software is available"), tr("Download"),
                [](QObject *param) { qobject_cast<SoftwareUpdate *>(param)->startDownload(); }, param);
        }
    }

    emit updateAvailableChanged();
    m_checkOrDownloadActive = false;
    reply->deleteLater();
}

bool SoftwareUpdate::startDownload() {
    if (m_downloadUrl.isEmpty() || m_newVersion.isEmpty()) {
        qCWarning(CLASS_LC) << "Ignoring download: no download URL or version set!";
        m_updateAvailable = false;
        emit updateAvailableChanged();
        return false;
    }

    // start software update procedure if there's enough battery. Free space check is within FileDownload.
    if (!m_downloadDir.mkpath(m_downloadDir.path())) {
        qCCritical(CLASS_LC) << "Error creating download directory" << m_downloadDir.path();
        return false;
    }

    if (m_batteryFuelGauge->getLevel() < 50) {
        Notifications::getInstance()->add(true, tr("The remote needs to be at least 50% battery to perform updates."));
        return false;
    }

    if (m_checkOrDownloadActive) {
        return false;
    }
    m_checkOrDownloadActive = true;

    QObject *obj = Config::getInstance()->getQMLObject("loader_second");
    obj->setProperty("source", "qrc:/basic_ui/settings/SoftwareupdateDownloading.qml");

    // TODO(zehnm) determine required size from update request?
    int requiredMB = 100;

    QString fileName = UPDATE_BASENAME + QFileInfo(m_downloadUrl.path()).suffix();
    m_fileDownload.download(m_downloadUrl, m_downloadDir, fileName, requiredMB);

    return true;
}

void SoftwareUpdate::onDownloadProgress(int id, qint64 bytesReceived, qint64 bytesTotal, const QString &speed) {
    Q_UNUSED(id)

    m_bytesReceived = bytesReceived;
    emit bytesReceivedChanged();

    m_bytesTotal = bytesTotal;
    emit bytesTotalChanged();

    m_downloadSpeed = speed;
    emit downloadSpeedChanged();
}

void SoftwareUpdate::onDownloadComplete(int id) {
    // create meta file containing version string
    QFile metafile(m_downloadDir.path() + "/" + META_FILENAME);
    if (!metafile.open(QIODevice::WriteOnly | QIODevice::Truncate | QIODevice::Text)) {
        onDownloadFailed(id, metafile.errorString());
        return;
    }

    QTextStream out(&metafile);
    out << m_newVersion;
    metafile.close();

    emit downloadComplete();
    emit installAvailable();

    m_checkOrDownloadActive = false;
}

void SoftwareUpdate::onDownloadFailed(int id, QString errorMsg) {
    Q_UNUSED(id)
    qCWarning(CLASS_LC) << "Download of update failed:" << errorMsg;

    Notifications::getInstance()->add(true, tr("Update download failed: %1").arg(errorMsg));
    emit downloadFailed();

    m_checkOrDownloadActive = false;
}

bool SoftwareUpdate::installAvailable() { return m_downloadDir.exists(META_FILENAME); }

bool SoftwareUpdate::performUpdate() {
    qCWarning(CLASS_LC) << "performUpdate() NOT YET IMPLEMENTED";
    return false;
}

bool SoftwareUpdate::startDockUpdate() {
    qCWarning(CLASS_LC) << "startDockUpdate() NOT YET IMPLEMENTED";
    return false;
}

bool SoftwareUpdate::isAlreadyDownloaded(const QString &version) {
    QFile metafile(m_downloadDir.path() + "/" + META_FILENAME);

    if (!metafile.open(QIODevice::ReadOnly | QIODevice::Text)) {
        return false;
    }

    QTextStream in(&metafile);
    return version.compare(in.readLine(100), Qt::CaseInsensitive) == 0;
}

bool SoftwareUpdate::isNewerVersion(const QString &currentVersion, const QString &updateVersion) {
    QStringList newVersionDigits = updateVersion.split(".");
    QString     none = QString("%1").arg(newVersionDigits[0].toInt(), 2, 10, QChar('0'));
    QString     ntwo = QString("%1").arg(newVersionDigits[1].toInt(), 2, 10, QChar('0'));
    QString     nthree = QString("%1").arg(newVersionDigits[2].toInt(), 2, 10, QChar('0'));

    int combinedNewVersion;
    combinedNewVersion = (none.append(ntwo).append(nthree)).toInt();

    QStringList currentVersionDigits = currentVersion.split(".");
    QString     cone = QString("%1").arg(currentVersionDigits[0].toInt(), 2, 10, QChar('0'));
    QString     ctwo = QString("%1").arg(currentVersionDigits[1].toInt(), 2, 10, QChar('0'));
    QString     cthree = QString("%1").arg(currentVersionDigits[2].toInt(), 2, 10, QChar('0'));

    int combinedCurrentVersion;
    combinedCurrentVersion = (cone.append(ctwo).append(cthree)).toInt();

    qCDebug(CLASS_LC) << "New version:" << combinedNewVersion << "Current version:" << combinedCurrentVersion;

    // check if it's a newer version than we have
    return combinedNewVersion > combinedCurrentVersion;
}

QString SoftwareUpdate::getDeviceType() {
    // Quick and dirty for now
    QString cpu =
#if defined(Q_PROCESSOR_X86_64)
        ":x86_64";
#elif defined(Q_PROCESSOR_X86_32)
        ":x86_32";
#elif defined(Q_PROCESSOR_ARM)
        ":arm";
#else
        "";
#endif

#if defined(Q_OS_ANDROID)
    return QString("android") + cpu;
#elif defined(Q_OS_IOS)
    return "ios";
#elif defined(Q_OS_LINUX)
#if defined(Q_PROCESSOR_ARM)
    // TODO(zehnm) further check if running on yio remote hardware or RPi!
    return "remote";
#else
    return QString("linux") + cpu;
#endif
#elif defined(Q_OS_WIN64)
    return "windows:64";
#elif defined(Q_OS_WIN32)
        return "windows:32";
#elif defined(Q_OS_MACOS)
        return "mac";
#else
        return "other";
#endif
}
