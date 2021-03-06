/*
 * Copyright (C) 2017 Canonical, Ltd.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU Lesser General Public License as published by
 * the Free Software Foundation; version 3.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#include "backend/backend_cups.h"
#include "cups/devicesearcher.h"
#include "cups/jobloader.h"
#include "cups/printerdriverloader.h"
#include "cups/printerloader.h"
#include "utils.h"

#include <cups/http.h>
#include <cups/ipp.h>
#include <cups/ppd.h>

#include <QLocale>
#include <QThread>
#include <QTimeZone>

#define __CUPS_ADD_OPTION(dest, name, value) dest->num_options = \
    cupsAddOption(name, value, dest->num_options, &dest->options);

#define __CUPS_ATTR_EXISTS(map, attr, type) map.contains(attr) \
    && map.value(attr).canConvert<type>()

PrinterCupsBackend::PrinterCupsBackend(IppClient *client, QPrinterInfo info,
                                       OrgCupsCupsdNotifierInterface *notifier,
                                       QObject *parent)
    : PrinterBackend(info.printerName(), parent)
    , m_knownQualityOptions({
        "Quality", "PrintQuality", "HPPrintQuality", "StpQuality",
        "OutputMode",})
    , m_extendedAttributeNames({
        QStringLiteral("StateMessage"), QStringLiteral("DeviceUri"),
        QStringLiteral("IsShared"), QStringLiteral("Copies"),
    })
    , m_client(client)
    , m_info(info)
    , m_notifier(notifier)
    , m_cupsSubscriptionId(-1)
{
    m_type = PrinterEnum::PrinterType::CupsType;
    connect(m_notifier, SIGNAL(JobCompleted(const QString&, const QString&,
                                            const QString&, uint,
                                            const QString&, bool, uint, uint,
                                            const QString&, const QString&, uint)),
            this, SIGNAL(jobCompleted(const QString&, const QString&,
                                      const QString&, uint, const QString&,
                                      bool, uint, uint, const QString&,
                                      const QString&, uint)));
    connect(m_notifier, SIGNAL(JobCreated(const QString&, const QString&,
                                          const QString&, uint, const QString&,
                                          bool, uint, uint, const QString&,
                                          const QString&, uint)),
            this, SIGNAL(jobCreated(const QString&, const QString&,
                                    const QString&, uint, const QString&, bool,
                                    uint, uint, const QString&, const QString&,
                                    uint)));
    connect(m_notifier, SIGNAL(JobState(const QString&, const QString&,
                                        const QString&, uint, const QString&,
                                        bool, uint, uint, const QString&,
                                        const QString&, uint)),
            this, SIGNAL(jobState(const QString&, const QString&,
                                  const QString&, uint, const QString&, bool,
                                  uint, uint, const QString&, const QString&,
                                  uint)));
    connect(m_notifier, SIGNAL(PrinterAdded(const QString&, const QString&,
                                            const QString&, uint,
                                            const QString&, bool)),
            this, SIGNAL(printerAdded(const QString&, const QString&,
                                      const QString&, uint,
                                      const QString&, bool)));
    connect(m_notifier, SIGNAL(PrinterDeleted(const QString&, const QString&,
                                              const QString&, uint,
                                              const QString&, bool)),
            this, SIGNAL(printerDeleted(const QString&, const QString&,
                                        const QString&, uint,
                                        const QString&, bool)));
    connect(m_notifier, SIGNAL(PrinterModified(const QString&, const QString&,
                                               const QString&, uint,
                                               const QString&, bool)),
            this, SIGNAL(printerModified(const QString&, const QString&,
                                         const QString&, uint,
                                         const QString&, bool)));
    connect(m_notifier, SIGNAL(PrinterStateChanged(const QString&,
                                                   const QString&,
                                                   const QString&, uint,
                                                   const QString&, bool)),
            this, SIGNAL(printerStateChanged(const QString&, const QString&,
                                             const QString&, uint,
                                             const QString&, bool)));

}

PrinterCupsBackend::~PrinterCupsBackend()
{
    Q_FOREACH(auto dest, m_dests) {
        if (dest)
            cupsFreeDests(1, dest);
    }
    Q_FOREACH(auto ppd, m_ppds) {
        if (ppd)
            ppdClose(ppd);
    }

    cancelSubscription();
    Q_EMIT cancelWorkers();
}

QString PrinterCupsBackend::printerAdd(const QString &name,
                                       const QString &uri,
                                       const QString &ppdFile,
                                       const QString &info,
                                       const QString &location)
{
    if (!m_client->printerAdd(name, uri, ppdFile, info, location)) {
        return m_client->getLastError();
    }
    return QString();
}

QString PrinterCupsBackend::printerAddWithPpd(const QString &name,
                                              const QString &uri,
                                              const QString &ppdFileName,
                                              const QString &info,
                                              const QString &location)
{
    if (!m_client->printerAddWithPpdFile(name, uri, ppdFileName, info, location)) {
        return m_client->getLastError();
    }
    return QString();
}

bool PrinterCupsBackend::holdsDefinition() const
{
    return !m_info.isNull();
}

QString PrinterCupsBackend::printerDelete(const QString &name)
{
    if (!m_client->printerDelete(name)) {
        return m_client->getLastError();
    }
    return QString();
}

QString PrinterCupsBackend::printerSetDefault(const QString &name)
{
    if (!m_client->printerSetDefault(name)) {
        return m_client->getLastError();
    }
    return QString();
}

QString PrinterCupsBackend::printerSetEnabled(const QString &name,
                                              const bool enabled)
{
    if (!m_client->printerSetEnabled(name, enabled)) {
        return m_client->getLastError();
    }
    return QString();
}

QString PrinterCupsBackend::printerSetAcceptJobs(
        const QString &name,
        const bool accept,
        const QString &reason)
{
    if (!m_client->printerSetAcceptJobs(name, accept, reason)) {
        return m_client->getLastError();
    }
    return QString();
}

QString PrinterCupsBackend::printerSetCopies(const QString &name,
                                             const int &copies)
{
    if (!m_client->printerSetCopies(name, copies)) {
        return m_client->getLastError();
    }
    return QString();
}

QString PrinterCupsBackend::printerSetShared(const QString &name,
                                             const bool shared)
{
    if (!m_client->printerSetShared(name, shared)) {
        return m_client->getLastError();
    }
    return QString();
}

QString PrinterCupsBackend::printerSetInfo(const QString &name,
                                           const QString &info)
{
    if (!m_client->printerClassSetInfo(name, info)) {
        return m_client->getLastError();
    }
    return QString();
}

QString PrinterCupsBackend::printerAddOption(const QString &name,
                                             const QString &option,
                                             const QStringList &values)
{
    if (!m_client->printerClassSetOption(name, option, values)) {
        return m_client->getLastError();
    }

    return QString();
}

QVariant PrinterCupsBackend::printerGetOption(const QString &name,
                                              const QString &option) const
{
    auto res = printerGetOptions(name, QStringList({option}));
    return res[option];
}

QMap<QString, QVariant> PrinterCupsBackend::printerGetOptions(
        const QString &name, const QStringList &options) const
{
    QMap<QString, QVariant> ret;

    cups_dest_t *dest = getDest(name);
    ppd_file_t* ppd = getPpd(name);

    // Used to store extended attributes, which we should request maximum once.
    QMap<QString, QVariant> extendedAttributesResults;

    /* Goes through known extended attributes. If one is being asked for,
    ask for all of them right away. */
    Q_FOREACH(const QString &extendedOption, m_extendedAttributeNames) {
        if (options.contains(extendedOption)) {
            extendedAttributesResults = m_client->printerGetAttributes(
                name, QStringList({
                    QStringLiteral("device-uri"),
                    QStringLiteral("printer-uri-supported"),
                    QStringLiteral("printer-state-message"),
                    QStringLiteral("copies-default"),
                })
            );
            break;
        }
    }

    Q_FOREACH(const QString &option, options) {
        if (option == QStringLiteral("DefaultColorModel") && ppd) {
            ColorModel model;
            ppd_option_t *ppdColorModel = ppdFindOption(ppd, "ColorModel");
            if (ppdColorModel) {
                ppd_choice_t* def = ppdFindChoice(ppdColorModel,
                                                  ppdColorModel->defchoice);
                if (def) {
                    model = Utils::parsePpdColorModel(def->choice,
                                                      def->text,
                                                      "ColorModel");
                }
            }
            ret[option] = QVariant::fromValue(model);
        } else if (option == QStringLiteral("DefaultPrintQuality") && ppd) {
            PrintQuality quality;
            Q_FOREACH(const QString opt, m_knownQualityOptions) {
                ppd_option_t *ppdQuality = ppdFindOption(ppd, opt.toUtf8());
                if (ppdQuality) {
                    ppd_choice_t* def = ppdFindChoice(ppdQuality,
                                                      ppdQuality->defchoice);
                    if (def) {
                        quality = Utils::parsePpdPrintQuality(def->choice,
                                                              def->text, opt);
                    }
                }
            }
            ret[option] = QVariant::fromValue(quality);
        } else if (option == QStringLiteral("SupportedPrintQualities") && ppd) {
            QList<PrintQuality> qualities;
            Q_FOREACH(const QString &opt, m_knownQualityOptions) {
                ppd_option_t *qualityOpt = ppdFindOption(ppd, opt.toUtf8());
                if (qualityOpt) {
                    for (int i = 0; i < qualityOpt->num_choices; ++i) {
                        qualities.append(
                            Utils::parsePpdPrintQuality(
                                qualityOpt->choices[i].choice,
                                qualityOpt->choices[i].text,
                                opt
                            )
                        );
                    }
                }
            }
            ret[option] = QVariant::fromValue(qualities);
        } else if (option == QStringLiteral("SupportedColorModels") && ppd) {
            QList<ColorModel> models;
            ppd_option_t *colorModels = ppdFindOption(ppd, "ColorModel");
            if (colorModels) {
                for (int i = 0; i < colorModels->num_choices; ++i) {
                    models.append(
                        Utils::parsePpdColorModel(
                            colorModels->choices[i].choice,
                            colorModels->choices[i].text,
                            QStringLiteral("ColorModel")
                        )
                    );
                }
            }
            ret[option] = QVariant::fromValue(models);
        } else if (option == QStringLiteral("AcceptJobs") && dest) {
            // "true" if the destination is accepting new jobs, "false" if not.
            QString res = cupsGetOption("printer-is-accepting-jobs",
                                        dest->num_options, dest->options);
            ret[option] = res.contains("true");
        } else if (option == QStringLiteral("StateReasons") && dest) {
            ret[option] = cupsGetOption("printer-state-reasons",
                                        dest->num_options, dest->options);
        } else if (option == QStringLiteral("StateMessage")) {
            ret[option] = extendedAttributesResults["printer-state-message"];
        } else if (option == QStringLiteral("DeviceUri")) {
            auto res = extendedAttributesResults;
            if (!res["printer-uri-supported"].toString().isEmpty()) {
                ret[option] = res["printer-uri-supported"];
            }
            if (!res["device-uri"].toString().isEmpty()) {
                ret[option] = res["device-uri"];
            }
        } else if (option == QStringLiteral("Copies")) {
            ret[option] = extendedAttributesResults[QStringLiteral("copies-default")];
        } else if (option == QStringLiteral("Shared") && dest) {
            ret[option] = cupsGetOption("printer-is-shared",
                                        dest->num_options, dest->options);
        }
    }
    return ret;
}

cups_dest_t* PrinterCupsBackend::makeDest(const QString &name,
                                          const PrinterJob *options)
{
    cups_dest_t *dest = getDest(name);

    if (options->collate()) {
        __CUPS_ADD_OPTION(dest, "Collate", "True");
    } else {
        __CUPS_ADD_OPTION(dest, "Collate", "False");
    }

    __CUPS_ADD_OPTION(dest, "copies", QString::number(options->copies()).toLocal8Bit());
    __CUPS_ADD_OPTION(dest, "ColorModel", options->getColorModel().name.toLocal8Bit());
    __CUPS_ADD_OPTION(dest, "Duplex", Utils::duplexModeToPpdChoice(options->getDuplexMode()).toLocal8Bit());

    if (options->landscape()) {
        __CUPS_ADD_OPTION(dest, "landscape", "");
    }

    if (options->printRangeMode() == PrinterEnum::PrintRange::PageRange
            && !options->printRange().isEmpty()) {
        __CUPS_ADD_OPTION(dest, "page-ranges", options->printRange().toLocal8Bit());
    }

    PrintQuality quality = options->getPrintQuality();
    __CUPS_ADD_OPTION(dest, quality.originalOption.toLocal8Bit(),
                      quality.name.toLocal8Bit());

    if (options->reverse()) {
        __CUPS_ADD_OPTION(dest, "OutputOrder", "Reverse");
    } else {
        __CUPS_ADD_OPTION(dest, "OutputOrder", "Normal");
    }

    // Always scale to fit the page for now
    __CUPS_ADD_OPTION(dest, "fit-to-page", "True");

    return dest;
}

void PrinterCupsBackend::cancelJob(const QString &name, const int jobId)
{
    int ret = cupsCancelJob(name.toLocal8Bit(), jobId);

    if (!ret) {
        qWarning() << "Failed to cancel job:" << jobId << "for" << name;
    }
}

void PrinterCupsBackend::holdJob(const QString &name, const int jobId)
{
    if (!m_client->printerHoldJob(name, jobId)) {
        qWarning() << "Failed to hold job:" << jobId << "for" << name;
    }
}

void PrinterCupsBackend::releaseJob(const QString &name, const int jobId)
{
    if (!m_client->printerReleaseJob(name, jobId)) {
        qWarning() << "Failed to release job:" << jobId << "for" << name;
    }
}

int PrinterCupsBackend::printFileToDest(const QString &filepath,
                                        const QString &title,
                                        const cups_dest_t *dest)
{
    qDebug() << "Printing:" << filepath << title << dest->name << dest->num_options;
    return cupsPrintFile(dest->name,
                         filepath.toLocal8Bit(),
                         title.toLocal8Bit(),
                         dest->num_options,
                         dest->options);
}


QList<cups_job_t *> PrinterCupsBackend::getCupsJobs(const QString &name)
{
    QList<cups_job_t *> list;
    cups_job_t *jobs;

    // Get a list of the jobs that are 'mine' and only active ones
    // https://www.cups.org/doc/api-cups.html#cupsGetJobs
    int count;
    if (name.isEmpty()) {
        count = cupsGetJobs(&jobs, NULL, 1, CUPS_WHICHJOBS_ACTIVE);
    } else {
        count = cupsGetJobs(&jobs, name.toLocal8Bit(), 1, CUPS_WHICHJOBS_ACTIVE);
    }

    for (int i=0; i < count; i++) {
        list.append(&jobs[i]);
    }

    return list;
}

QMap<QString, QVariant> PrinterCupsBackend::printerGetJobAttributes(
    const QString &name, const int jobId)
{
    Q_UNUSED(name);
    QMap<QString, QVariant> rawMap = m_client->printerGetJobAttributes(name, jobId);
    QMap<QString, QVariant> map;

    // Filter attributes to know values
    // Do this here so we can use things such as m_knownQualityOptions

    if (__CUPS_ATTR_EXISTS(rawMap, "Collate", bool)) {
        map.insert("Collate", rawMap.value("Collate"));
    } else {
        map.insert("Collate", QVariant(true));
    }

    if (__CUPS_ATTR_EXISTS(rawMap, "copies", int)) {
        map.insert("copies", rawMap.value("copies"));
    } else {
        map.insert("copies", QVariant(1));
    }

    if (__CUPS_ATTR_EXISTS(rawMap, "ColorModel", QString)) {
        map.insert("ColorModel", rawMap.value("ColorModel"));
    } else {
        map.insert("ColorModel", QVariant(""));
    }

    if (__CUPS_ATTR_EXISTS(rawMap, "date-time-at-completed", QDateTime)) {
        map.insert("CompletedTime", rawMap.value("date-time-at-completed"));
    } else {
        map.insert("CompletedTime", QVariant(QDateTime()));
    }

    if (__CUPS_ATTR_EXISTS(rawMap, "date-time-at-creation", QDateTime)) {
        map.insert("CreationTime", rawMap.value("date-time-at-creation"));
    } else {
        map.insert("CreationTime", QVariant(QDateTime()));
    }

    if (__CUPS_ATTR_EXISTS(rawMap, "Duplex", QString)) {
        map.insert("Duplex", rawMap.value("Duplex"));
    } else {
        map.insert("Duplex", QVariant(""));
    }

    // Try job-media-sheets-completed first as it should include duplex
    // if it doesn't exist fallback to job-impressions-completed
    if (__CUPS_ATTR_EXISTS(rawMap, "job-media-sheets-completed", int)) {
        map.insert("impressionsCompleted", rawMap.value("job-media-sheets-completed"));
    } else if (__CUPS_ATTR_EXISTS(rawMap, "job-impressions-completed", int)) {
        map.insert("impressionsCompleted", rawMap.value("job-impressions-completed"));
    } else {
        map.insert("impressionsCompleted", QVariant(0));
    }

    if (__CUPS_ATTR_EXISTS(rawMap, "landscape", bool)) {
        map.insert("landscape", rawMap.value("landscape"));
    } else {
        map.insert("landscape", QVariant(false));
    }

    if (__CUPS_ATTR_EXISTS(rawMap, "page-ranges", QList<QVariant>)) {
        QList<QVariant> range = rawMap.value("page-ranges").toList();
        QStringList rangeStrings;

        Q_FOREACH(QVariant var, range) {
            rangeStrings << var.toString();
        }

        map.insert("page-ranges", QVariant(rangeStrings));
    } else {
        map.insert("page-ranges", QVariant(QStringList()));
    }

    if (__CUPS_ATTR_EXISTS(rawMap, "date-time-at-processing", QDateTime)) {
        map.insert("ProcessingTime", rawMap.value("date-time-at-processing"));
    } else {
        map.insert("ProcessingTime", QVariant(QDateTime()));
    }

    Q_FOREACH(QString qualityOption, m_knownQualityOptions) {
        if (rawMap.contains(qualityOption)
                && rawMap.value(qualityOption).canConvert<QString>()) {
            map.insert("quality", rawMap.value(qualityOption).toString());
        }
    }

    if (!map.contains("quality")) {
        map.insert("quality", QVariant(""));
    }

    if (__CUPS_ATTR_EXISTS(rawMap, "OutputOrder", QString)) {
        map.insert("OutputOrder", rawMap.value("OutputOrder"));
    } else {
        map.insert("OutputOrder", "Normal");
    }

    if (__CUPS_ATTR_EXISTS(rawMap, "job-k-octets", int)) {
        map.insert("Size", rawMap.value("job-k-octets"));
    } else {
        map.insert("Size", QVariant(0));
    }

    // If there is a state then get it, as there could have been a signal
    // flood. Which then means a forceJobRefresh is able to update the state
    if (__CUPS_ATTR_EXISTS(rawMap, "job-state", int)) {
        map.insert("State", rawMap.value("job-state").toInt());
    }

    if (__CUPS_ATTR_EXISTS(rawMap, "job-originating-user-name", QString)) {
        map.insert("User", rawMap.value("job-originating-user-name"));
    } else {
        map.insert("User", QVariant(""));
    }

    // Generate a list of messages
    // TODO: for now just using job-printer-state-message, are there others?
    QStringList messages;

    if (__CUPS_ATTR_EXISTS(rawMap, "job-printer-state-message", QString)) {
        messages << rawMap.value("job-printer-state-message").toString();
    }

    map.insert("messages", QVariant(messages));

    return map;
}


QList<QSharedPointer<PrinterJob>> PrinterCupsBackend::printerGetJobs()
{
    auto jobs = getCupsJobs();
    QList<QSharedPointer<PrinterJob>> list;

    Q_FOREACH(auto job, jobs) {
        // Note: extended attributes are not loaded here
        // they are loaded in JobLoader
        auto newJob = QSharedPointer<PrinterJob>(
            new PrinterJob(QString::fromUtf8(job->dest), this, job->id)
        );
        newJob->setState(static_cast<PrinterEnum::JobState>(job->state));
        newJob->setTitle(QString::fromLocal8Bit(job->title));

        list.append(newJob);
    }
    if (!list.isEmpty())
        cupsFreeJobs(list.size(), jobs.first());

    return list;
}

QSharedPointer<PrinterJob> PrinterCupsBackend::printerGetJob(
        const QString &printerName, const int jobId)
{
    auto jobs = getCupsJobs(printerName);
    cups_job_t *cupsJob = Q_NULLPTR;
    QSharedPointer<PrinterJob> job(Q_NULLPTR);

    for (int i=0; i < jobs.size(); i++) {
        if (jobs.at(i)->id == jobId) {
            cupsJob = jobs.at(i);
            break;
        }
    }

    if (cupsJob) {
        job = QSharedPointer<PrinterJob>(
            new PrinterJob(QString::fromUtf8(cupsJob->dest), this, cupsJob->id)
        );

        job->setState(static_cast<PrinterEnum::JobState>(cupsJob->state));
        job->setTitle(QString::fromLocal8Bit(cupsJob->title));
    }

    if (!jobs.size())
        cupsFreeJobs(jobs.size(), jobs.first());

    return job;
}

QString PrinterCupsBackend::printerName() const
{
    return m_printerName;
}

QString PrinterCupsBackend::description() const
{
    return m_info.description();
}

QString PrinterCupsBackend::location() const
{
    return m_info.location();
}

QString PrinterCupsBackend::makeAndModel() const
{
    return m_info.makeAndModel();
}

bool PrinterCupsBackend::isRemote() const
{
    return m_info.isRemote();
}

PrinterEnum::State PrinterCupsBackend::state() const
{
    switch (m_info.state()) {
    case QPrinter::Active:
        return PrinterEnum::State::ActiveState;
    case QPrinter::Aborted:
        return PrinterEnum::State::AbortedState;
    case QPrinter::Error:
        return PrinterEnum::State::ErrorState;
    case QPrinter::Idle:
    default:
        return PrinterEnum::State::IdleState;
    }
}

QList<QPageSize> PrinterCupsBackend::supportedPageSizes() const
{
    return m_info.supportedPageSizes();
}

QPageSize PrinterCupsBackend::defaultPageSize() const
{
    return m_info.defaultPageSize();
}

bool PrinterCupsBackend::supportsCustomPageSizes() const
{
    return m_info.supportsCustomPageSizes();
}

QPageSize PrinterCupsBackend::minimumPhysicalPageSize() const
{
    return m_info.minimumPhysicalPageSize();
}

QPageSize PrinterCupsBackend::maximumPhysicalPageSize() const
{
    return m_info.maximumPhysicalPageSize();
}

QList<int> PrinterCupsBackend::supportedResolutions() const
{
    return m_info.supportedResolutions();
}

PrinterEnum::DuplexMode PrinterCupsBackend::defaultDuplexMode() const
{
    return Utils::qDuplexModeToDuplexMode(m_info.defaultDuplexMode());
}

QList<PrinterEnum::DuplexMode> PrinterCupsBackend::supportedDuplexModes() const
{
    QList<PrinterEnum::DuplexMode> list;
    Q_FOREACH(const QPrinter::DuplexMode mode, m_info.supportedDuplexModes()) {
        if (mode != QPrinter::DuplexAuto) {
            list.append(Utils::qDuplexModeToDuplexMode(mode));
        }
    }

    if (list.isEmpty())
        list.append(PrinterEnum::DuplexMode::DuplexNone);

    return list;
}

QList<QSharedPointer<Printer>> PrinterCupsBackend::availablePrinters()
{
    return QList<QSharedPointer<Printer>>();
}

QStringList PrinterCupsBackend::availablePrinterNames()
{
    return QPrinterInfo::availablePrinterNames();
}

QSharedPointer<Printer> PrinterCupsBackend::getPrinter(const QString &printerName)
{
    QPrinterInfo info = QPrinterInfo::printerInfo(printerName);
    return QSharedPointer<Printer>(new Printer(new PrinterCupsBackend(m_client, info, m_notifier)));
}

QString PrinterCupsBackend::defaultPrinterName()
{
    return QPrinterInfo::defaultPrinterName();
}

void PrinterCupsBackend::requestJobExtendedAttributes(
        QSharedPointer<Printer> printer, QSharedPointer<PrinterJob> job)
{
    QPair<QString, int> pair(printer->name(), job->jobId());

    if (m_activeJobRequests.contains(pair)) {
        return;
    }

    auto thread = new QThread;
    auto loader = new JobLoader(this, printer->name(), job->jobId());
    loader->moveToThread(thread);
    connect(thread, SIGNAL(started()), loader, SLOT(load()));
    connect(loader, SIGNAL(finished()), thread, SLOT(quit()));
    connect(loader, SIGNAL(finished()), loader, SLOT(deleteLater()));
    connect(loader, SIGNAL(loaded(QString, int, QMap<QString, QVariant>)),
            this, SIGNAL(jobLoaded(QString, int, QMap<QString, QVariant>)));
    connect(loader, SIGNAL(loaded(QString, int, QMap<QString, QVariant>)),
            this, SLOT(onJobLoaded(QString, int, QMap<QString, QVariant>)));
    connect(thread, SIGNAL(finished()), thread, SLOT(deleteLater()));

    m_activeJobRequests << pair;

    thread->start();
}

void PrinterCupsBackend::requestPrinter(const QString &printerName)
{
    if (m_activePrinterRequests.contains(printerName)) {
        return;
    }

    auto thread = new QThread;
    auto loader = new PrinterLoader(printerName, m_client, m_notifier);
    loader->moveToThread(thread);
    connect(thread, SIGNAL(started()), loader, SLOT(load()));
    connect(loader, SIGNAL(finished()), thread, SLOT(quit()));
    connect(loader, SIGNAL(finished()), loader, SLOT(deleteLater()));
    connect(loader, SIGNAL(loaded(QSharedPointer<Printer>)),
            this, SIGNAL(printerLoaded(QSharedPointer<Printer>)));
    connect(loader, SIGNAL(loaded(QSharedPointer<Printer>)),
            this, SLOT(onPrinterLoaded(QSharedPointer<Printer>)));
    connect(thread, SIGNAL(finished()), thread, SLOT(deleteLater()));

    m_activePrinterRequests << printerName;

    thread->start();
}

void PrinterCupsBackend::requestPrinterDrivers()
{
    auto thread = new QThread;
    auto loader = new PrinterDriverLoader();
    loader->moveToThread(thread);
    connect(loader, SIGNAL(error(const QString&)),
            this, SIGNAL(printerDriversFailedToLoad(const QString&)));
    connect(this, SIGNAL(requestPrinterDriverCancel()), loader, SLOT(cancel()));
    connect(thread, SIGNAL(started()), loader, SLOT(process()));
    connect(loader, SIGNAL(finished()), thread, SLOT(quit()));
    connect(loader, SIGNAL(finished()), loader, SLOT(deleteLater()));
    connect(loader, SIGNAL(loaded(const QList<PrinterDriver>&)),
            this, SIGNAL(printerDriversLoaded(const QList<PrinterDriver>&)));
    connect(thread, SIGNAL(finished()), thread, SLOT(deleteLater()));

    thread->start();
}

void PrinterCupsBackend::cancelPrinterDriverRequest()
{
    Q_EMIT requestPrinterDriverCancel();
}

void PrinterCupsBackend::searchForDevices()
{

    auto thread = new QThread;
    auto searcher = new DeviceSearcher();
    searcher->moveToThread(thread);
    connect(thread, SIGNAL(started()), searcher, SLOT(load()));
    connect(searcher, SIGNAL(finished()), thread, SLOT(quit()));
    connect(searcher, SIGNAL(finished()), searcher, SLOT(deleteLater()));
    connect(searcher, SIGNAL(finished()), this, SIGNAL(deviceSearchFinished()));
    connect(searcher, SIGNAL(loaded(const Device&)),
            this, SIGNAL(deviceFound(const Device&)));
    connect(thread, SIGNAL(finished()), thread, SLOT(deleteLater()));
    thread->start();
}

void PrinterCupsBackend::refresh()
{
    if (m_printerName.isEmpty()) {
        throw std::invalid_argument("Trying to refresh unnamed printer.");
    } else {
        m_info = QPrinterInfo::printerInfo(m_printerName);
    }
}

void PrinterCupsBackend::createSubscription()
{
    m_cupsSubscriptionId = m_client->createSubscription();;
}

void PrinterCupsBackend::cancelSubscription()
{
    if (m_cupsSubscriptionId > 0)
        m_client->cancelSubscription(m_cupsSubscriptionId);
}

QString PrinterCupsBackend::getPrinterInstance(const QString &name) const
{
    const auto parts = name.splitRef(QLatin1Char('/'));
    QString instance;
    if (parts.size() > 1)
        instance = parts.at(1).toString();

    return instance;
}

QString PrinterCupsBackend::getPrinterName(const QString &name) const
{
    return name.splitRef(QLatin1Char('/')).first().toString();
}

cups_dest_t* PrinterCupsBackend::getDest(const QString &name) const
{
    QString printerName = getPrinterName(name);
    QString instance = getPrinterInstance(name);

    if (m_dests.contains(name)) {
        return m_dests[name];
    } else {
        m_dests[name] = m_client->getDest(printerName, instance);
        return m_dests[name];
    }
}

ppd_file_t* PrinterCupsBackend::getPpd(const QString &name) const
{
    QString printerName = getPrinterName(name);
    QString instance = getPrinterInstance(name);

    if (m_ppds.contains(name)) {
        return m_ppds[name];
    } else {
        m_ppds[name] = m_client->getPpdFile(printerName, instance);
        return m_ppds[name];
    }
}

bool PrinterCupsBackend::isExtendedAttribute(const QString &attributeName) const
{
    return m_extendedAttributeNames.contains(attributeName);
}

void PrinterCupsBackend::onJobLoaded(QString printerName, int jobId,
                                     QMap<QString, QVariant> attributes)
{
    Q_UNUSED(attributes);

    QPair<QString, int> pair(printerName, jobId);
    m_activeJobRequests.remove(pair);
}

void PrinterCupsBackend::onPrinterLoaded(QSharedPointer<Printer> printer)
{
    m_activePrinterRequests.remove(printer->name());
}

