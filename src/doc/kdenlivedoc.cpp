/***************************************************************************
 *   Copyright (C) 2007 by Jean-Baptiste Mardelle (jb@kdenlive.org)        *
 *                                                                         *
 *   This program is free software; you can redistribute it and/or modify  *
 *   it under the terms of the GNU General Public License as published by  *
 *   the Free Software Foundation; either version 2 of the License, or     *
 *   (at your option) any later version.                                   *
 *                                                                         *
 *   This program is distributed in the hope that it will be useful,       *
 *   but WITHOUT ANY WARRANTY; without even the implied warranty of        *
 *   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the         *
 *   GNU General Public License for more details.                          *
 *                                                                         *
 *   You should have received a copy of the GNU General Public License     *
 *   along with this program; if not, write to the                         *
 *   Free Software Foundation, Inc.,                                       *
 *   51 Franklin Street, Fifth Floor, Boston, MA  02110-1301  USA          *
 ***************************************************************************/


#include "kdenlivedoc.h"
#include "documentchecker.h"
#include "documentvalidator.h"
#include "mltcontroller/clipcontroller.h"
#include "mltcontroller/producerqueue.h"
#include <config-kdenlive.h>
#include "kdenlivesettings.h"
#include "renderer.h"
#include "mainwindow.h"
#include "project/clipmanager.h"
#include "project/projectcommands.h"
#include "bin/bincommands.h"
#include "effectslist/initeffects.h"
#include "dialogs/profilesdialog.h"
#include "titler/titlewidget.h"
#include "project/notesplugin.h"
#include "project/dialogs/noteswidget.h"
#include "core.h"
#include "bin/bin.h"
#include "bin/projectclip.h"
#include "utils/KoIconUtils.h"
#include "mltcontroller/bincontroller.h"
#include "mltcontroller/effectscontroller.h"
#include "timeline/transitionhandler.h"

#include <KMessageBox>
#include <KRecentDirs>
#include <klocalizedstring.h>
#include <KIO/CopyJob>
#include <KIO/JobUiDelegate>
#include <KBookmarkManager>
#include <KBookmark>

#include <QCryptographicHash>
#include <QFile>
#include <QDebug>
#include <QFileDialog>
#include <QInputDialog>
#include <QDomImplementation>
#include <QUndoGroup>
#include <QTimer>
#include <QUndoStack>
#include <QTextEdit>

#include <mlt++/Mlt.h>
#include <KJobWidgets/KJobWidgets>
#include <QStandardPaths>

#include <locale>
#ifdef Q_OS_MAC
#include <xlocale.h>
#endif

DocUndoStack::DocUndoStack(QUndoGroup *parent) : QUndoStack(parent)
{
}

//TODO: custom undostack everywhere do that 
void DocUndoStack::push(QUndoCommand *cmd)
{
    if (index() < count()) 
        emit invalidate();
    QUndoStack::push(cmd);
}

const double DOCUMENTVERSION = 0.95;

KdenliveDoc::KdenliveDoc(const QUrl &url, const QUrl &projectFolder, QUndoGroup *undoGroup, const QString &profileName, const QMap <QString, QString>& properties, const QMap <QString, QString>& metadata, const QPoint &tracks, Render *render, NotesPlugin *notes, bool *openBackup, MainWindow *parent) :
    QObject(parent),
    m_autosave(NULL),
    m_url(url),
    m_width(0),
    m_height(0),
    m_render(render),
    m_notesWidget(notes->widget()),
    m_modified(false),
    m_projectFolder(projectFolder)
{
    // init m_profile struct
    m_commandStack = new DocUndoStack(undoGroup);
    m_profile.frame_rate_num = 0;
    m_profile.frame_rate_den = 0;
    m_profile.width = 0;
    m_profile.height = 0;
    m_profile.progressive = 0;
    m_profile.sample_aspect_num = 0;
    m_profile.sample_aspect_den = 0;
    m_profile.display_aspect_num = 0;
    m_profile.display_aspect_den = 0;
    m_profile.colorspace = 0;
    m_clipManager = new ClipManager(this);
    connect(m_clipManager, SIGNAL(displayMessage(QString,int)), parent, SLOT(slotGotProgressInfo(QString,int)));
    connect(this, SIGNAL(updateCompositionMode(int)), parent, SLOT(slotUpdateCompositeAction(int)));
    bool success = false;
    connect(m_commandStack, SIGNAL(indexChanged(int)), this, SLOT(slotModified()));
    connect(m_commandStack, SIGNAL(invalidate()), this, SLOT(checkPreviewStack()));
    connect(m_render, SIGNAL(setDocumentNotes(QString)), this, SLOT(slotSetDocumentNotes(QString)));
    connect(pCore->producerQueue(), &ProducerQueue::switchProfile, this, &KdenliveDoc::switchProfile);
    //connect(m_commandStack, SIGNAL(cleanChanged(bool)), this, SLOT(setModified(bool)));

    // Init clip modification tracker
    m_modifiedTimer.setInterval(1500);
    connect(&m_fileWatcher, &KDirWatch::dirty, this, &KdenliveDoc::slotClipModified);
    connect(&m_fileWatcher, &KDirWatch::deleted, this, &KdenliveDoc::slotClipMissing);
    connect(&m_modifiedTimer, &QTimer::timeout, this, &KdenliveDoc::slotProcessModifiedClips);

    // init default document properties
    m_documentProperties[QStringLiteral("zoom")] = '7';
    m_documentProperties[QStringLiteral("verticalzoom")] = '1';
    m_documentProperties[QStringLiteral("zonein")] = '0';
    m_documentProperties[QStringLiteral("zoneout")] = QStringLiteral("100");
    m_documentProperties[QStringLiteral("enableproxy")] = QString::number((int) KdenliveSettings::enableproxy());
    m_documentProperties[QStringLiteral("proxyparams")] = KdenliveSettings::proxyparams();
    m_documentProperties[QStringLiteral("proxyextension")] = KdenliveSettings::proxyextension();
    m_documentProperties[QStringLiteral("previewparams")] = KdenliveSettings::previewparams();
    m_documentProperties[QStringLiteral("previewextension")] = KdenliveSettings::previewextension();
    m_documentProperties[QStringLiteral("generateproxy")] = QString::number((int) KdenliveSettings::generateproxy());
    m_documentProperties[QStringLiteral("proxyminsize")] = QString::number(KdenliveSettings::proxyminsize());
    m_documentProperties[QStringLiteral("generateimageproxy")] = QString::number((int) KdenliveSettings::generateimageproxy());
    m_documentProperties[QStringLiteral("proxyimageminsize")] = QString::number(KdenliveSettings::proxyimageminsize());
    m_documentProperties[QStringLiteral("documentid")] = QString::number(QDateTime::currentMSecsSinceEpoch());

    // Load properties
    QMapIterator<QString, QString> i(properties);
    while (i.hasNext()) {
        i.next();
        m_documentProperties[i.key()] = i.value();
    }

    // Load metadata
    QMapIterator<QString, QString> j(metadata);
    while (j.hasNext()) {
        j.next();
        m_documentMetadata[j.key()] = j.value();
    }

    if (QLocale().decimalPoint() != QLocale::system().decimalPoint()) {
        setlocale(LC_NUMERIC, "");
        QLocale systemLocale = QLocale::system();
        systemLocale.setNumberOptions(QLocale::OmitGroupSeparator);
        QLocale::setDefault(systemLocale);
        // locale conversion might need to be redone
        initEffects::parseEffectFiles(pCore->binController()->mltRepository(), setlocale(LC_NUMERIC, NULL));
    }
    *openBackup = false;
    if (url.isValid()) {
        QFile file(url.toLocalFile());
        if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) {
            // The file cannot be opened
            if (KMessageBox::warningContinueCancel(parent, i18n("Cannot open the project file,\nDo you want to open a backup file?"), i18n("Error opening file"), KGuiItem(i18n("Open Backup"))) == KMessageBox::Continue) {
                *openBackup = true;
            }
            //KMessageBox::error(parent, KIO::NetAccess::lastErrorString());
        }
        else {
            qDebug()<<" // / processing file open";
            QString errorMsg;
            int line;
            int col;
            QDomImplementation::setInvalidDataPolicy(QDomImplementation::DropInvalidChars);
            success = m_document.setContent(&file, false, &errorMsg, &line, &col);
            file.close();

            if (!success) {
                // It is corrupted
                int answer = KMessageBox::warningYesNoCancel (parent, i18n("Cannot open the project file, error is:\n%1 (line %2, col %3)\nDo you want to open a backup file?", errorMsg, line, col), i18n("Error opening file"), KGuiItem(i18n("Open Backup")), KGuiItem(i18n("Recover")));
                if (answer == KMessageBox::Yes) {
                    *openBackup = true;
                }
                else if (answer == KMessageBox::No) {
                    // Try to recover broken file produced by Kdenlive 0.9.4
                    if (file.open(QIODevice::ReadOnly | QIODevice::Text)) {
                        int correction = 0;
                        QString playlist = file.readAll();
                        while (!success && correction < 2) {
                            int errorPos = 0;
                            line--;
                            col = col - 2;
                            for (int j = 0; j < line && errorPos < playlist.length(); ++j) {
                                errorPos = playlist.indexOf(QStringLiteral("\n"), errorPos);
                                errorPos++;
                            }
                            errorPos += col;
                            if (errorPos >= playlist.length()) break;
                            playlist.remove(errorPos, 1);
                            line = 0;
                            col = 0;
                            success = m_document.setContent(playlist, false, &errorMsg, &line, &col);
                            correction++;
                        }
                        if (!success) {
                            KMessageBox::sorry(parent, i18n("Cannot recover this project file"));
                        }
                        else {
                            // Document was modified, ask for backup
                            QDomElement mlt = m_document.documentElement();
                            mlt.setAttribute(QStringLiteral("modified"), 1);
                        }
                    }
                }
            }
            else {
                qDebug()<<" // / processing file open: validate";
                parent->slotGotProgressInfo(i18n("Validating"), 0);
                qApp->processEvents();
                DocumentValidator validator(m_document, url);
                success = validator.isProject();
                if (!success) {
                    // It is not a project file
                    parent->slotGotProgressInfo(i18n("File %1 is not a Kdenlive project file", m_url.path()), 100);
                    if (KMessageBox::warningContinueCancel(parent, i18n("File %1 is not a valid project file.\nDo you want to open a backup file?", m_url.path()), i18n("Error opening file"), KGuiItem(i18n("Open Backup"))) == KMessageBox::Continue) {
                        *openBackup = true;
                    }
                } else {
                    /*
                     * Validate the file against the current version (upgrade
                     * and recover it if needed). It is NOT a passive operation
                     */
                    // TODO: backup the document or alert the user?
                    success = validator.validate(DOCUMENTVERSION);
                    if (success && !KdenliveSettings::gpu_accel()) {
                        success = validator.checkMovit();
                    }
                    if (success) { // Let the validator handle error messages
                        qDebug()<<" // / processing file validate ok";
                        parent->slotGotProgressInfo(i18n("Check missing clips"), 0);
                        qApp->processEvents();
                        DocumentChecker d(m_url, m_document);
                        success = !d.hasErrorInClips();
                        if (success) {
                            loadDocumentProperties();
                            if (m_document.documentElement().attribute(QStringLiteral("modified")) == QLatin1String("1")) setModified(true);
                            if (validator.isModified()) setModified(true);
                        }
                    }
                }
            }
        }
    }
    initCacheDirs();

    // Something went wrong, or a new file was requested: create a new project
    if (!success) {
        m_url.clear();
        m_profile = ProfilesDialog::getVideoProfile(profileName);
        m_document = createEmptyDocument(tracks.x(), tracks.y());
        updateProjectProfile(false);
    }

    // Ask to create the project directory if it does not exist
    QFileInfo checkProjectFolder(m_projectFolder.toString(QUrl::RemoveFilename | QUrl::RemoveScheme));
    if (!QFile::exists(m_projectFolder.path()) && checkProjectFolder.isWritable()) {
        int create = KMessageBox::questionYesNo(parent, i18n("Project directory %1 does not exist. Create it?", m_projectFolder.path()));
        if (create == KMessageBox::Yes) {
            QDir projectDir(m_projectFolder.path());
            bool ok = projectDir.mkpath(m_projectFolder.path());
            if (!ok) {
                KMessageBox::sorry(parent, i18n("The directory %1, could not be created.\nPlease make sure you have the required permissions.", m_projectFolder.path()));
            }
        }
    }

    // Make sure the project folder is usable
    if (m_projectFolder.isEmpty() || !QFile::exists(m_projectFolder.path())) {
        KMessageBox::information(parent, i18n("Document project folder is invalid, setting it to the default one: %1", KdenliveSettings::defaultprojectfolder()));
        m_projectFolder = QUrl::fromLocalFile(KdenliveSettings::defaultprojectfolder());
    }

    // Make sure that the necessary folders exist
    QDir dir(m_projectFolder.path());
    dir.mkdir(QStringLiteral("titles"));

    updateProjectFolderPlacesEntry();
}

void KdenliveDoc::slotSetDocumentNotes(const QString &notes)
{
    m_notesWidget->setHtml(notes);
}


KdenliveDoc::~KdenliveDoc()
{
    if (m_url.isEmpty()) {
        // Document was never saved, delete cache folder
        QString documentId = QDir::cleanPath(getDocumentProperty(QStringLiteral("documentid")));
        bool ok;
        documentId.toLong(&ok);
        if (ok && !documentId.isEmpty()) {
            QString kdenliveCacheDir = QStandardPaths::writableLocation(QStandardPaths::CacheLocation);
            QDir cacheDir(kdenliveCacheDir + "/" + documentId);
            if (cacheDir.exists() && cacheDir.dirName() == documentId) {
                cacheDir.removeRecursively();
            }
        }
    }
    delete m_commandStack;
    //qDebug() << "// DEL CLP MAN";
    delete m_clipManager;
    //qDebug() << "// DEL CLP MAN done";
    if (m_autosave) {
        if (!m_autosave->fileName().isEmpty()) m_autosave->remove();
        delete m_autosave;
    }
}

int KdenliveDoc::setSceneList()
{
    //m_render->resetProfile(m_profile);
    pCore->bin()->isLoading = true;
    pCore->producerQueue()->abortOperations();
    if (m_render->setSceneList(m_document.toString(), m_documentProperties.value(QStringLiteral("position")).toInt()) == -1) {
        // INVALID MLT Consumer, something is wrong
        return -1;
    }
    pCore->bin()->isLoading = false;

    bool ok = false;
    QDir thumbsFolder = getCacheDir(CacheThumbs, &ok);
    if (ok)
        pCore->binController()->checkThumbnails(thumbsFolder);
    m_documentProperties.remove(QStringLiteral("position"));
    pCore->monitorManager()->activateMonitor(Kdenlive::ClipMonitor, true);
    return 0;
}

QDomDocument KdenliveDoc::createEmptyDocument(int videotracks, int audiotracks)
{
    QList <TrackInfo> tracks;
    // Tracks are added «backwards», so we need to reverse the track numbering
    // mbt 331: http://www.kdenlive.org/mantis/view.php?id=331
    // Better default names for tracks: Audio 1 etc. instead of blank numbers
    for (int i = 0; i < audiotracks; ++i) {
        TrackInfo audioTrack;
        audioTrack.type = AudioTrack;
        audioTrack.isMute = false;
        audioTrack.isBlind = true;
        audioTrack.isLocked = false;
        audioTrack.trackName = i18n("Audio %1", audiotracks - i);
        audioTrack.duration = 0;
        audioTrack.effectsList = EffectsList(true);
        tracks.append(audioTrack);

    }
    for (int i = 0; i < videotracks; ++i) {
        TrackInfo videoTrack;
        videoTrack.type = VideoTrack;
        videoTrack.isMute = false;
        videoTrack.isBlind = false;
        videoTrack.isLocked = false;
        videoTrack.trackName = i18n("Video %1", i + 1);
        videoTrack.duration = 0;
        videoTrack.effectsList = EffectsList(true);
        tracks.append(videoTrack);
    }
    return createEmptyDocument(tracks);
}

QDomDocument KdenliveDoc::createEmptyDocument(const QList <TrackInfo> &tracks)
{
    // Creating new document
    QDomDocument doc;
    QDomElement mlt = doc.createElement(QStringLiteral("mlt"));
    mlt.setAttribute(QStringLiteral("LC_NUMERIC"), QLatin1String(""));
    doc.appendChild(mlt);

    QDomElement blk = doc.createElement(QStringLiteral("producer"));
    blk.setAttribute(QStringLiteral("in"), 0);
    blk.setAttribute(QStringLiteral("out"), 500);
    blk.setAttribute(QStringLiteral("aspect_ratio"), 1);
    blk.setAttribute(QStringLiteral("set.test_audio"), 0);
    blk.setAttribute(QStringLiteral("id"), QStringLiteral("black"));

    QDomElement property = doc.createElement(QStringLiteral("property"));
    property.setAttribute(QStringLiteral("name"), QStringLiteral("mlt_type"));
    QDomText value = doc.createTextNode(QStringLiteral("producer"));
    property.appendChild(value);
    blk.appendChild(property);

    property = doc.createElement(QStringLiteral("property"));
    property.setAttribute(QStringLiteral("name"), QStringLiteral("aspect_ratio"));
    value = doc.createTextNode(QString::number(0));
    property.appendChild(value);
    blk.appendChild(property);

    property = doc.createElement(QStringLiteral("property"));
    property.setAttribute(QStringLiteral("name"), QStringLiteral("length"));
    value = doc.createTextNode(QString::number(15000));
    property.appendChild(value);
    blk.appendChild(property);

    property = doc.createElement(QStringLiteral("property"));
    property.setAttribute(QStringLiteral("name"), QStringLiteral("eof"));
    value = doc.createTextNode(QStringLiteral("pause"));
    property.appendChild(value);
    blk.appendChild(property);

    property = doc.createElement(QStringLiteral("property"));
    property.setAttribute(QStringLiteral("name"), QStringLiteral("resource"));
    value = doc.createTextNode(QStringLiteral("black"));
    property.appendChild(value);
    blk.appendChild(property);

    property = doc.createElement(QStringLiteral("property"));
    property.setAttribute(QStringLiteral("name"), QStringLiteral("mlt_service"));
    value = doc.createTextNode(QStringLiteral("colour"));
    property.appendChild(value);
    blk.appendChild(property);

    mlt.appendChild(blk);


    QDomElement tractor = doc.createElement(QStringLiteral("tractor"));
    tractor.setAttribute(QStringLiteral("id"), QStringLiteral("maintractor"));
    tractor.setAttribute(QStringLiteral("global_feed"), 1);
    //QDomElement multitrack = doc.createElement("multitrack");
    QDomElement playlist = doc.createElement(QStringLiteral("playlist"));
    playlist.setAttribute(QStringLiteral("id"), QStringLiteral("black_track"));
    mlt.appendChild(playlist);

    QDomElement blank0 = doc.createElement(QStringLiteral("entry"));
    blank0.setAttribute(QStringLiteral("in"), QStringLiteral("0"));
    blank0.setAttribute(QStringLiteral("out"), QStringLiteral("1"));
    blank0.setAttribute(QStringLiteral("producer"), QStringLiteral("black"));
    playlist.appendChild(blank0);

    // create playlists
    int total = tracks.count();
    // The lower video track will recieve composite transitions
    int lowestVideoTrack = -1;
    for (int i = 0; i < total; ++i) {
        QDomElement playlist = doc.createElement(QStringLiteral("playlist"));
        playlist.setAttribute(QStringLiteral("id"), "playlist" + QString::number(i+1));
        playlist.setAttribute(QStringLiteral("kdenlive:track_name"), tracks.at(i).trackName);
        if (tracks.at(i).type == AudioTrack) {
            playlist.setAttribute(QStringLiteral("kdenlive:audio_track"), 1);
        } else if (lowestVideoTrack == -1) {
            // Register first video track
            lowestVideoTrack = i + 1;
        }
        mlt.appendChild(playlist);
    }
    QString compositeService = TransitionHandler::compositeTransition();
    QDomElement track0 = doc.createElement(QStringLiteral("track"));
    track0.setAttribute(QStringLiteral("producer"), QStringLiteral("black_track"));
    tractor.appendChild(track0);

    // create audio and video tracks
    for (int i = 0; i < total; ++i) {
        QDomElement track = doc.createElement(QStringLiteral("track"));
        track.setAttribute(QStringLiteral("producer"), "playlist" + QString::number(i+1));
        if (tracks.at(i).type == AudioTrack) {
            track.setAttribute(QStringLiteral("hide"), QStringLiteral("video"));
        } else if (tracks.at(i).isBlind) {
            if (tracks.at(i).isMute) {
                track.setAttribute(QStringLiteral("hide"), QStringLiteral("all"));
            }
            else track.setAttribute(QStringLiteral("hide"), QStringLiteral("video"));
        }
        else if (tracks.at(i).isMute)
            track.setAttribute(QStringLiteral("hide"), QStringLiteral("audio"));
        tractor.appendChild(track);
    }

    // Transitions
    for (int i = 0; i <= total; i++) {
        if (i > 0) {
            QDomElement transition = doc.createElement(QStringLiteral("transition"));
            transition.setAttribute(QStringLiteral("always_active"), QStringLiteral("1"));

            QDomElement property = doc.createElement(QStringLiteral("property"));
            property.setAttribute(QStringLiteral("name"), QStringLiteral("mlt_service"));
            value = doc.createTextNode(QStringLiteral("mix"));
            property.appendChild(value);
            transition.appendChild(property);

            property = doc.createElement(QStringLiteral("property"));
            property.setAttribute(QStringLiteral("name"), QStringLiteral("a_track"));
            QDomText value = doc.createTextNode(QStringLiteral("0"));
            property.appendChild(value);
            transition.appendChild(property);

            property = doc.createElement(QStringLiteral("property"));
            property.setAttribute(QStringLiteral("name"), QStringLiteral("b_track"));
            value = doc.createTextNode(QString::number(i));
            property.appendChild(value);
            transition.appendChild(property);

            property = doc.createElement(QStringLiteral("property"));
            property.setAttribute(QStringLiteral("name"), QStringLiteral("combine"));
            value = doc.createTextNode(QStringLiteral("1"));
            property.appendChild(value);
            transition.appendChild(property);

            property = doc.createElement(QStringLiteral("property"));
            property.setAttribute(QStringLiteral("name"), QStringLiteral("internal_added"));
            value = doc.createTextNode(QStringLiteral("237"));
            property.appendChild(value);
            transition.appendChild(property);

            tractor.appendChild(transition);
        }
        if (i > 0 && tracks.at(i - 1).type == VideoTrack) {
            // Only add composite transition if both tracks are video
            QDomElement transition = doc.createElement(QStringLiteral("transition"));
            property = doc.createElement(QStringLiteral("property"));
            property.setAttribute(QStringLiteral("name"), QStringLiteral("mlt_service"));
            property.appendChild(doc.createTextNode(compositeService));
            transition.appendChild(property);

            property = doc.createElement(QStringLiteral("property"));
            property.setAttribute(QStringLiteral("name"), QStringLiteral("a_track"));
            property.appendChild(doc.createTextNode(QString::number(0)));
            transition.appendChild(property);

            property = doc.createElement(QStringLiteral("property"));
            property.setAttribute(QStringLiteral("name"), QStringLiteral("b_track"));
            property.appendChild(doc.createTextNode(QString::number(i)));
            transition.appendChild(property);

            property = doc.createElement(QStringLiteral("property"));
            property.setAttribute(QStringLiteral("name"), QStringLiteral("internal_added"));
            property.appendChild(doc.createTextNode(QStringLiteral("237")));
            transition.appendChild(property);

            tractor.appendChild(transition);
        }
    }
    mlt.appendChild(tractor);
    return doc;
}

bool KdenliveDoc::useProxy() const
{
    return m_documentProperties.value(QStringLiteral("enableproxy")).toInt();
}

bool KdenliveDoc::autoGenerateProxy(int width) const
{
    return m_documentProperties.value(QStringLiteral("generateproxy")).toInt() && width > m_documentProperties.value(QStringLiteral("proxyminsize")).toInt();
}

bool KdenliveDoc::autoGenerateImageProxy(int width) const
{
    return m_documentProperties.value(QStringLiteral("generateimageproxy")).toInt() && width > m_documentProperties.value(QStringLiteral("proxyimageminsize")).toInt();
}

void KdenliveDoc::slotAutoSave()
{
    if (m_render && m_autosave) {
        if (!m_autosave->isOpen() && !m_autosave->open(QIODevice::ReadWrite)) {
            // show error: could not open the autosave file
            qDebug() << "ERROR; CANNOT CREATE AUTOSAVE FILE";
        }
        //qDebug() << "// AUTOSAVE FILE: " << m_autosave->fileName();
        QDomDocument sceneList = xmlSceneList(m_render->sceneList());
        if (sceneList.isNull()) {
            //Make sure we don't save if scenelist is corrupted
            KMessageBox::error(QApplication::activeWindow(), i18n("Cannot write to file %1, scene list is corrupted.", m_autosave->fileName()));
            return;
        }
        m_autosave->resize(0); 
        m_autosave->write(sceneList.toString().toUtf8());
        m_autosave->flush();
    }
}

void KdenliveDoc::setZoom(int horizontal, int vertical)
{
    m_documentProperties[QStringLiteral("zoom")] = QString::number(horizontal);
    m_documentProperties[QStringLiteral("verticalzoom")] = QString::number(vertical);
}

QPoint KdenliveDoc::zoom() const
{
    return QPoint(m_documentProperties.value(QStringLiteral("zoom")).toInt(), m_documentProperties.value(QStringLiteral("verticalzoom")).toInt());
}

void KdenliveDoc::setZone(int start, int end)
{
    m_documentProperties[QStringLiteral("zonein")] = QString::number(start);
    m_documentProperties[QStringLiteral("zoneout")] = QString::number(end);
}

QPoint KdenliveDoc::zone() const
{
    return QPoint(m_documentProperties.value(QStringLiteral("zonein")).toInt(), m_documentProperties.value(QStringLiteral("zoneout")).toInt());
}

QDomDocument KdenliveDoc::xmlSceneList(const QString &scene)
{
    QDomDocument sceneList;
    sceneList.setContent(scene, true);
    QDomElement mlt = sceneList.firstChildElement(QStringLiteral("mlt"));
    if (mlt.isNull() || !mlt.hasChildNodes()) {
        //scenelist is corrupted
        return sceneList;
    }

    // Set playlist audio volume to 100%
    QDomElement tractor = mlt.firstChildElement(QStringLiteral("tractor"));
    if (!tractor.isNull()) {
        QDomNodeList props = tractor.elementsByTagName(QStringLiteral("property"));
        for (int i = 0; i < props.count(); ++i) {
            if (props.at(i).toElement().attribute(QStringLiteral("name")) == QLatin1String("meta.volume")) {
                props.at(i).firstChild().setNodeValue(QStringLiteral("1"));
                break;
            }
        }
    }
    QDomNodeList pls = mlt.elementsByTagName(QStringLiteral("playlist"));
    QDomElement mainPlaylist;
    for (int i = 0; i < pls.count(); ++i) {
        if (pls.at(i).toElement().attribute(QStringLiteral("id")) == pCore->binController()->binPlaylistId()) {
            mainPlaylist = pls.at(i).toElement();
            break;
        }
    }

    // check if project contains custom effects to embed them in project file
    QDomNodeList effects = mlt.elementsByTagName(QStringLiteral("filter"));
    int maxEffects = effects.count();
    //qDebug() << "// FOUD " << maxEffects << " EFFECTS+++++++++++++++++++++";
    QMap <QString, QString> effectIds;
    for (int i = 0; i < maxEffects; ++i) {
        QDomNode m = effects.at(i);
        QDomNodeList params = m.childNodes();
        QString id;
        QString tag;
        for (int j = 0; j < params.count(); ++j) {
            QDomElement e = params.item(j).toElement();
            if (e.attribute(QStringLiteral("name")) == QLatin1String("kdenlive_id")) {
                id = e.firstChild().nodeValue();
            }
            if (e.attribute(QStringLiteral("name")) == QLatin1String("tag")) {
                tag = e.firstChild().nodeValue();
            }
            if (!id.isEmpty() && !tag.isEmpty()) effectIds.insert(id, tag);
        }
    }
    //TODO: find a way to process this before rendering MLT scenelist to xml
    QDomDocument customeffects = initEffects::getUsedCustomEffects(effectIds);
    if (customeffects.documentElement().childNodes().count() > 0) {
	EffectsList::setProperty(mainPlaylist, QStringLiteral("kdenlive:customeffects"), customeffects.toString());
    }
    //addedXml.appendChild(sceneList.importNode(customeffects.documentElement(), true));

    //TODO: move metadata to previous step in saving process
    QDomElement docmetadata = sceneList.createElement(QStringLiteral("documentmetadata"));
    QMapIterator<QString, QString> j(m_documentMetadata);
    while (j.hasNext()) {
        j.next();
        docmetadata.setAttribute(j.key(), j.value());
    }
    //addedXml.appendChild(docmetadata);

    return sceneList;
}

QString KdenliveDoc::documentNotes() const
{
    QString text = m_notesWidget->toPlainText().simplified();
    if (text.isEmpty()) return QString();
    return m_notesWidget->toHtml();
}

bool KdenliveDoc::saveSceneList(const QString &path, const QString &scene)
{
    QDomDocument sceneList = xmlSceneList(scene);
    if (sceneList.isNull()) {
        //Make sure we don't save if scenelist is corrupted
        KMessageBox::error(QApplication::activeWindow(), i18n("Cannot write to file %1, scene list is corrupted.", path));
        return false;
    }

    // Backup current version
    backupLastSavedVersion(path);
    QFile file(path);

    if (!file.open(QIODevice::WriteOnly | QIODevice::Text)) {
        qWarning() << "//////  ERROR writing to file: " << path;
        KMessageBox::error(QApplication::activeWindow(), i18n("Cannot write to file %1", path));
        return false;
    }

    file.write(sceneList.toString().toUtf8());
    if (file.error() != QFile::NoError) {
        KMessageBox::error(QApplication::activeWindow(), i18n("Cannot write to file %1", path));
        file.close();
        return false;
    }
    file.close();
    cleanupBackupFiles();
    QFileInfo info(file);
    QString fileName = QUrl::fromLocalFile(path).fileName().section('.', 0, -2);
    fileName.append('-' + m_documentProperties.value(QStringLiteral("documentid")));
    fileName.append(info.lastModified().toString(QStringLiteral("-yyyy-MM-dd-hh-mm")));
    fileName.append(".kdenlive.png");
    QDir backupFolder(QStandardPaths::writableLocation(QStandardPaths::AppDataLocation) + QStringLiteral("/.backup"));
    emit saveTimelinePreview(backupFolder.absoluteFilePath(fileName));
    return true;
}

ClipManager *KdenliveDoc::clipManager()
{
    return m_clipManager;
}

QString KdenliveDoc::groupsXml() const
{
    return m_clipManager->groupsXml();
}

QUrl KdenliveDoc::projectFolder() const
{
    //if (m_projectFolder.isEmpty()) return QUrl(QStandardPaths::writableLocation(QStandardPaths::DataLocation) + "//projects/");
    return m_projectFolder;
}

void KdenliveDoc::setProjectFolder(QUrl url)
{
    if (url == m_projectFolder) return;
    setModified(true);
    QDir dir(url.toLocalFile());
    if (!dir.exists()) {
        dir.mkpath(dir.absolutePath());
    }
    dir.mkdir(QStringLiteral("titles"));
    if (KMessageBox::questionYesNo(QApplication::activeWindow(), i18n("You have changed the project folder. Do you want to copy the cached data from %1 to the new folder %2?", m_projectFolder.path(), url.path())) == KMessageBox::Yes) moveProjectData(url);
    m_projectFolder = url;

    updateProjectFolderPlacesEntry();
}

void KdenliveDoc::moveProjectData(const QUrl &url)
{
    QList <ClipController*> list = pCore->binController()->getControllerList();
    QList<QUrl> cacheUrls;
    for (int i = 0; i < list.count(); ++i) {
        ClipController *clip = list.at(i);
        if (clip->clipType() == Text) {
            // the image for title clip must be moved
            QUrl oldUrl = clip->clipUrl();
            QUrl newUrl = QUrl::fromLocalFile(url.toLocalFile() + QDir::separator() + "titles/" + oldUrl.fileName());
            KIO::Job *job = KIO::copy(oldUrl, newUrl);
            if (job->exec()) clip->setProperty(QStringLiteral("resource"), newUrl.path());
        }
        /*
        QString hash = clip->getClipHash();
        QUrl oldVideoThumbUrl = QUrl::fromLocalFile(m_projectFolder.path() + QDir::separator() + "thumbs/" + hash + ".png");
        if (QFile::exists(oldVideoThumbUrl.path())) {
            cacheUrls << oldVideoThumbUrl;
        }
        QUrl oldAudioThumbUrl = QUrl::fromLocalFile(m_projectFolder.path() + QDir::separator() + "thumbs/" + hash + ".thumb");
        if (QFile::exists(oldAudioThumbUrl.path())) {
            cacheUrls << oldAudioThumbUrl;
        }
        QUrl oldVideoProxyUrl = QUrl::fromLocalFile(m_projectFolder.path() + QDir::separator() + "proxy/" + hash + '.' + KdenliveSettings::proxyextension());
        if (QFile::exists(oldVideoProxyUrl.path())) {
            cacheUrls << oldVideoProxyUrl;
        }
        */
    }
    /*if (!cacheUrls.isEmpty()) {
        KIO::Job *job = KIO::copy(cacheUrls, QUrl::fromLocalFile(url.path() + QDir::separator() + "thumbs/"));
        KJobWidgets::setWindow(job, QApplication::activeWindow());
        job->exec();
    }*/
}

const QString &KdenliveDoc::profilePath() const
{
    return m_profile.path;
}

MltVideoProfile KdenliveDoc::mltProfile() const
{
    return m_profile;
}

bool KdenliveDoc::profileChanged(const QString &profile) const
{
    return m_profile.toList() != ProfilesDialog::getVideoProfile(profile).toList();
}

ProfileInfo KdenliveDoc::getProfileInfo() const
{
        ProfileInfo info;
        info.profileSize = getRenderSize();
        info.profileFps = fps();
        return info;
}

double KdenliveDoc::dar() const
{
    return (double) m_profile.display_aspect_num / m_profile.display_aspect_den;
}

DocUndoStack *KdenliveDoc::commandStack()
{
    return m_commandStack;
}

Render *KdenliveDoc::renderer()
{
    return m_render;
}

int KdenliveDoc::getFramePos(const QString &duration)
{
    return m_timecode.getFrameCount(duration);
}

QDomDocument KdenliveDoc::toXml()
{
    return m_document;
}

Timecode KdenliveDoc::timecode() const
{
    return m_timecode;
}

QDomNodeList KdenliveDoc::producersList()
{
    return m_document.elementsByTagName(QStringLiteral("producer"));
}

double KdenliveDoc::projectDuration() const
{
    if (m_render)
        return GenTime(m_render->getLength(), m_render->fps()).ms() / 1000;
    else
        return 0;
}

double KdenliveDoc::fps() const
{
    return m_render->fps();
}

int KdenliveDoc::width() const
{
    return m_width;
}

int KdenliveDoc::height() const
{
    return m_height;
}

QUrl KdenliveDoc::url() const
{
    return m_url;
}

void KdenliveDoc::setUrl(const QUrl &url)
{
    m_url = url;
}

void KdenliveDoc::slotModified()
{
    setModified(m_commandStack->isClean() == false);
}

void KdenliveDoc::setModified(bool mod)
{
    // fix mantis#3160: The document may have an empty URL if not saved yet, but should have a m_autosave in any case
    if (m_autosave && mod && KdenliveSettings::crashrecovery()) {
        emit startAutoSave();
    }
    if (mod == m_modified) return;
    m_modified = mod;
    emit docModified(m_modified);
}

bool KdenliveDoc::isModified() const
{
    return m_modified;
}

const QString KdenliveDoc::description() const
{
    if (!m_url.isValid())
        return i18n("Untitled") + "[*] / " + m_profile.description;
    else
        return m_url.fileName() + " [*]/ " + m_profile.description;
}

QString KdenliveDoc::searchFileRecursively(const QDir &dir, const QString &matchSize, const QString &matchHash) const
{
    QString foundFileName;
    QByteArray fileData;
    QByteArray fileHash;
    QStringList filesAndDirs = dir.entryList(QDir::Files | QDir::Readable);
    for (int i = 0; i < filesAndDirs.size() && foundFileName.isEmpty(); ++i) {
        QFile file(dir.absoluteFilePath(filesAndDirs.at(i)));
        if (file.open(QIODevice::ReadOnly)) {
            if (QString::number(file.size()) == matchSize) {
                /*
                * 1 MB = 1 second per 450 files (or faster)
                * 10 MB = 9 seconds per 450 files (or faster)
                */
                if (file.size() > 1000000 * 2) {
                    fileData = file.read(1000000);
                    if (file.seek(file.size() - 1000000))
                        fileData.append(file.readAll());
                } else
                    fileData = file.readAll();
                file.close();
                fileHash = QCryptographicHash::hash(fileData, QCryptographicHash::Md5);
                if (QString(fileHash.toHex()) == matchHash)
                    return file.fileName();
                else
                    qDebug() << filesAndDirs.at(i) << "size match but not hash";
            }
        }
        ////qDebug() << filesAndDirs.at(i) << file.size() << fileHash.toHex();
    }
    filesAndDirs = dir.entryList(QDir::Dirs | QDir::Readable | QDir::Executable | QDir::NoDotAndDotDot);
    for (int i = 0; i < filesAndDirs.size() && foundFileName.isEmpty(); ++i) {
        foundFileName = searchFileRecursively(dir.absoluteFilePath(filesAndDirs.at(i)), matchSize, matchHash);
        if (!foundFileName.isEmpty())
            break;
    }
    return foundFileName;
}

void KdenliveDoc::deleteClip(const QString &clipId, ClipType type, const QString url)
{
    pCore->binController()->removeBinClip(clipId);
    // Remove from file watch
    if (type != Color && type != SlideShow && type != QText && !url.isEmpty()) {
        m_fileWatcher.removeFile(url);
    }
}

ProjectClip *KdenliveDoc::getBinClip(const QString &clipId)
{
    return pCore->bin()->getBinClip(clipId);
}

QStringList KdenliveDoc::getBinFolderClipIds(const QString &folderId) const
{
    return pCore->bin()->getBinFolderClipIds(folderId);
}

ClipController *KdenliveDoc::getClipController(const QString &clipId)
{
    return pCore->binController()->getController(clipId);
}


void KdenliveDoc::slotCreateTextTemplateClip(const QString &group, const QString &groupId, QUrl path)
{
    QString titlesFolder = QDir::cleanPath(projectFolder().path() + QDir::separator() + "titles/");
    if (path.isEmpty()) {
        QPointer<QFileDialog> d = new QFileDialog(QApplication::activeWindow(),  i18n("Enter Template Path"), titlesFolder);
        d->setMimeTypeFilters(QStringList() << QStringLiteral("application/x-kdenlivetitle"));
        d->setFileMode(QFileDialog::ExistingFile);
        if (d->exec() == QDialog::Accepted && !d->selectedUrls().isEmpty()) {
            path = d->selectedUrls().first();
        }
        delete d;
    }

    if (path.isEmpty()) return;

    //TODO: rewrite with new title system (just set resource)
    m_clipManager->slotAddTextTemplateClip(i18n("Template title clip"), path, group, groupId);
    emit selectLastAddedClip(QString::number(m_clipManager->lastClipId()));
}

void KdenliveDoc::cacheImage(const QString &fileId, const QImage &img) const
{
    bool ok = false;
    QDir dir = getCacheDir(CacheThumbs, &ok);
    if (ok)
        img.save(dir.absoluteFilePath(fileId + ".png"));
}

void KdenliveDoc::setDocumentProperty(const QString &name, const QString &value)
{
    if (value.isEmpty()) {
        m_documentProperties.remove(name);
        return;
    }
    m_documentProperties[name] = value;
}

const QString KdenliveDoc::getDocumentProperty(const QString &name, const QString &defaultValue) const
{
    if (m_documentProperties.contains(name))
        return m_documentProperties.value(name);
    return defaultValue;
}

QMap <QString, QString> KdenliveDoc::getRenderProperties() const
{
    QMap <QString, QString> renderProperties;
    QMapIterator<QString, QString> i(m_documentProperties);
    while (i.hasNext()) {
        i.next();
        if (i.key().startsWith(QLatin1String("render"))) renderProperties.insert(i.key(), i.value());
    }
    return renderProperties;
}

void KdenliveDoc::saveCustomEffects(const QDomNodeList &customeffects)
{
    QDomElement e;
    QStringList importedEffects;
    int maxchild = customeffects.count();
    for (int i = 0; i < maxchild; ++i) {
        e = customeffects.at(i).toElement();
        const QString id = e.attribute(QStringLiteral("id"));
        const QString tag = e.attribute(QStringLiteral("tag"));
        if (!id.isEmpty()) {
            // Check if effect exists or save it
            if (MainWindow::customEffects.hasEffect(tag, id) == -1) {
                QDomDocument doc;
                doc.appendChild(doc.importNode(e, true));
                QString path = QStandardPaths::writableLocation(QStandardPaths::DataLocation) + "/effects";
                path += id + ".xml";
                if (!QFile::exists(path)) {
                    importedEffects << id;
                    QFile file(path);
                    if (file.open(QFile::WriteOnly | QFile::Truncate)) {
                        QTextStream out(&file);
                        out << doc.toString();
                    }
                }
            }
        }
    }
    if (!importedEffects.isEmpty())
        KMessageBox::informationList(QApplication::activeWindow(), i18n("The following effects were imported from the project:"), importedEffects);
    if (!importedEffects.isEmpty()) {
        emit reloadEffects();
    }
}

void KdenliveDoc::updateProjectFolderPlacesEntry()
{
    /*
     * For similar and more code have a look at kfileplacesmodel.cpp and the included files:
     * http://websvn.kde.org/trunk/KDE/kdelibs/kfile/kfileplacesmodel.cpp?view=markup
     */

    const QString file = QStandardPaths::writableLocation(QStandardPaths::GenericDataLocation) + "/user-places.xbel";
    KBookmarkManager *bookmarkManager = KBookmarkManager::managerForExternalFile(file);
    if (!bookmarkManager) return;
    KBookmarkGroup root = bookmarkManager->root();
    
    KBookmark bookmark = root.first();

    QString kdenliveName = QCoreApplication::applicationName();
    QUrl documentLocation = m_projectFolder;

    bool exists = false;

    while (!bookmark.isNull()) {
        // UDI not empty indicates a device
        QString udi = bookmark.metaDataItem(QStringLiteral("UDI"));
        QString appName = bookmark.metaDataItem(QStringLiteral("OnlyInApp"));

        if (udi.isEmpty() && appName == kdenliveName && bookmark.text() == i18n("Project Folder")) {
            if (bookmark.url() != documentLocation) {
                bookmark.setUrl(documentLocation);
                bookmarkManager->emitChanged(root);
            }
            exists = true;
            break;
        }

        bookmark = root.next(bookmark);
    }

    // if entry does not exist yet (was not found), well, create it then
    if (!exists) {
        bookmark = root.addBookmark(i18n("Project Folder"), documentLocation, QStringLiteral("folder-favorites"));
        // Make this user selectable ?
        bookmark.setMetaDataItem(QStringLiteral("OnlyInApp"), kdenliveName);
        bookmarkManager->emitChanged(root);
    }
}

const QSize KdenliveDoc::getRenderSize() const
{
    QSize size;
    if (m_render) {
	size.setWidth(m_render->frameRenderWidth());
	size.setHeight(m_render->renderHeight());
    }
    return size;
}
// static
double KdenliveDoc::getDisplayRatio(const QString &path)
{
    QFile file(path);
    QDomDocument doc;
    if (!file.open(QIODevice::ReadOnly)) {
        qWarning() << "ERROR, CANNOT READ: " << path;
        return 0;
    }
    if (!doc.setContent(&file)) {
        qWarning() << "ERROR, CANNOT READ: " << path;
        file.close();
        return 0;
    }
    file.close();
    QDomNodeList list = doc.elementsByTagName(QStringLiteral("profile"));
    if (list.isEmpty()) return 0;
    QDomElement profile = list.at(0).toElement();
    double den = profile.attribute(QStringLiteral("display_aspect_den")).toDouble();
    if (den > 0) return profile.attribute(QStringLiteral("display_aspect_num")).toDouble() / den;
    return 0;
}

void KdenliveDoc::backupLastSavedVersion(const QString &path)
{
    // Ensure backup folder exists
    if (path.isEmpty()) return;
    QFile file(path);
    QDir backupFolder(QStandardPaths::writableLocation(QStandardPaths::AppDataLocation) + QStringLiteral("/.backup"));
    QString fileName = QUrl::fromLocalFile(path).fileName().section('.', 0, -2);
    QFileInfo info(file);
    fileName.append('-' + m_documentProperties.value(QStringLiteral("documentid")));
    fileName.append(info.lastModified().toString(QStringLiteral("-yyyy-MM-dd-hh-mm")));
    fileName.append(".kdenlive");
    QString backupFile = backupFolder.absoluteFilePath(fileName);
    if (file.exists()) {
        // delete previous backup if it was done less than 60 seconds ago
        QFile::remove(backupFile);
        if (!QFile::copy(path, backupFile)) {
            KMessageBox::information(QApplication::activeWindow(), i18n("Cannot create backup copy:\n%1", backupFile));
        }
    }
}

void KdenliveDoc::cleanupBackupFiles()
{
    QDir backupFolder(QStandardPaths::writableLocation(QStandardPaths::AppDataLocation) + QStringLiteral("/.backup"));
    QString projectFile = url().fileName().section('.', 0, -2);
    projectFile.append('-' + m_documentProperties.value(QStringLiteral("documentid")));
    projectFile.append("-??");
    projectFile.append("??");
    projectFile.append("-??");
    projectFile.append("-??");
    projectFile.append("-??");
    projectFile.append("-??.kdenlive");

    QStringList filter;
    filter << projectFile;
    backupFolder.setNameFilters(filter);
    QFileInfoList resultList = backupFolder.entryInfoList(QDir::Files, QDir::Time);

    QDateTime d = QDateTime::currentDateTime();
    QStringList hourList;
    QStringList dayList;
    QStringList weekList;
    QStringList oldList;
    for (int i = 0; i < resultList.count(); ++i) {
        if (d.secsTo(resultList.at(i).lastModified()) < 3600) {
            // files created in the last hour
            hourList.append(resultList.at(i).absoluteFilePath());
        }
        else if (d.secsTo(resultList.at(i).lastModified()) < 43200) {
            // files created in the day
            dayList.append(resultList.at(i).absoluteFilePath());
        }
        else if (d.daysTo(resultList.at(i).lastModified()) < 8) {
            // files created in the week
            weekList.append(resultList.at(i).absoluteFilePath());
        }
        else {
            // older files
            oldList.append(resultList.at(i).absoluteFilePath());
        }
    }
    if (hourList.count() > 20) {
        int step = hourList.count() / 10;
        for (int i = 0; i < hourList.count(); i += step) {
            //qDebug()<<"REMOVE AT: "<<i<<", COUNT: "<<hourList.count();
            hourList.removeAt(i);
            --i;
        }
    } else hourList.clear();
    if (dayList.count() > 20) {
        int step = dayList.count() / 10;
        for (int i = 0; i < dayList.count(); i += step) {
            dayList.removeAt(i);
            --i;
        }
    } else dayList.clear();
    if (weekList.count() > 20) {
        int step = weekList.count() / 10;
        for (int i = 0; i < weekList.count(); i += step) {
            weekList.removeAt(i);
            --i;
        }
    } else weekList.clear();
    if (oldList.count() > 20) {
        int step = oldList.count() / 10;
        for (int i = 0; i < oldList.count(); i += step) {
            oldList.removeAt(i);
            --i;
        }
    } else oldList.clear();
    
    QString f;
    while (hourList.count() > 0) {
        f = hourList.takeFirst();
        QFile::remove(f);
        QFile::remove(f + ".png");
    }
    while (dayList.count() > 0) {
        f = dayList.takeFirst();
        QFile::remove(f);
        QFile::remove(f + ".png");
    }
    while (weekList.count() > 0) {
        f = weekList.takeFirst();
        QFile::remove(f);
        QFile::remove(f + ".png");
    }
    while (oldList.count() > 0) {
        f = oldList.takeFirst();
        QFile::remove(f);
        QFile::remove(f + ".png");
    }
}

const QMap <QString, QString> KdenliveDoc::metadata() const
{
    return m_documentMetadata;
}

void KdenliveDoc::setMetadata(const QMap<QString, QString> &meta)
{
    setModified(true);
    m_documentMetadata = meta;
}

void KdenliveDoc::slotProxyCurrentItem(bool doProxy, QList<ProjectClip *> clipList, bool force, QUndoCommand *masterCommand)
{
    if (clipList.isEmpty()) clipList = pCore->bin()->selectedClips();
    bool hasParent = true;
    if (masterCommand == NULL) {
        masterCommand = new QUndoCommand();
        if (doProxy) masterCommand->setText(i18np("Add proxy clip", "Add proxy clips", clipList.count()));
        else masterCommand->setText(i18np("Remove proxy clip", "Remove proxy clips", clipList.count()));
        hasParent = false;
    }

    // Make sure the proxy folder exists
    bool ok = false;
    QDir dir = getCacheDir(CacheProxy, &ok);
    if (!ok) {
        // Error
    }
    QString extension = QStringLiteral(".") + getDocumentProperty(QStringLiteral("proxyextension"));
    QString params = getDocumentProperty(QStringLiteral("proxyparams"));
    if (params.contains(QStringLiteral("-s "))) {
        QString proxySize = params.section(QStringLiteral("-s "), 1).section(QStringLiteral("x"), 0, 0);
        extension.prepend(QStringLiteral("-") + proxySize);
    }

    // Prepare updated properties
    QMap <QString, QString> newProps;
    QMap <QString, QString> oldProps;
    if (!doProxy) newProps.insert(QStringLiteral("kdenlive:proxy"), QStringLiteral("-"));

    // Parse clips
    for (int i = 0; i < clipList.count(); ++i) {
        ProjectClip *item = clipList.at(i);
        ClipType t = item->clipType();
        // Only allow proxy on some clip types
        if ((t == Video || t == AV || t == Unknown || t == Image || t == Playlist) && item->isReady()) {
	    if ((doProxy && !force && item->hasProxy()) || (!doProxy && !item->hasProxy() && pCore->binController()->hasClip(item->clipId()))) continue;
            if (pCore->producerQueue()->isProcessing(item->clipId())) {
                continue;
            }

            if (doProxy) {
                newProps.clear();
                QString path = dir.absoluteFilePath(item->hash() + (t == Image ? QStringLiteral(".png") : extension));
                // insert required duration for proxy
                newProps.insert(QStringLiteral("proxy_out"), item->getProducerProperty(QStringLiteral("out")));
                newProps.insert(QStringLiteral("kdenlive:proxy"), path);
            }
            else if (!pCore->binController()->hasClip(item->clipId())) {
                // Force clip reload
                newProps.insert(QStringLiteral("resource"), item->url().toLocalFile());
            }
            // We need to insert empty proxy so that undo will work
            //TODO: how to handle clip properties
            //oldProps = clip->currentProperties(newProps);
            if (doProxy) oldProps.insert(QStringLiteral("kdenlive:proxy"), QStringLiteral("-"));
            new EditClipCommand(pCore->bin(), item->clipId(), oldProps, newProps, true, masterCommand);
        }
    }
    if (!hasParent) {
        if (masterCommand->childCount() > 0) {
            m_commandStack->push(masterCommand);
        }
        else delete masterCommand;
    }
}


//TODO put all file watching stuff in own class
void KdenliveDoc::watchFile(const QUrl &url)
{
    m_fileWatcher.addFile(url.toLocalFile());
}

void KdenliveDoc::slotClipModified(const QString &path)
{
    QStringList ids = pCore->binController()->getBinIdsByResource(QUrl::fromLocalFile(path));
    foreach (const QString &id, ids) {
        if (!m_modifiedClips.contains(id)) {
            pCore->bin()->setWaitingStatus(id);
        }
        m_modifiedClips[id] = QTime::currentTime();
    }
    if (!m_modifiedTimer.isActive()) m_modifiedTimer.start();
}


void KdenliveDoc::slotClipMissing(const QString &path)
{
    qDebug() << "// CLIP: " << path << " WAS MISSING";
    QStringList ids = pCore->binController()->getBinIdsByResource(QUrl::fromLocalFile(path));
    //TODO handle missing clips by replacing producer with an invalid producer
    /*foreach (const QString &id, ids) {
        emit missingClip(id);
    }*/
}

void KdenliveDoc::slotProcessModifiedClips()
{
    if (!m_modifiedClips.isEmpty()) {
        QMapIterator<QString, QTime> i(m_modifiedClips);
        while (i.hasNext()) {
            i.next();
            if (QTime::currentTime().msecsTo(i.value()) <= -1500) {
                pCore->bin()->reloadClip(i.key());
                m_modifiedClips.remove(i.key());
                break;
            }
        }
        setModified(true);
    }
    if (m_modifiedClips.isEmpty()) m_modifiedTimer.stop();
}

QMap <QString, QString> KdenliveDoc::documentProperties()
{
    m_documentProperties.insert(QStringLiteral("version"), QString::number(DOCUMENTVERSION));
    m_documentProperties.insert(QStringLiteral("kdenliveversion"), QStringLiteral(KDENLIVE_VERSION));
    m_documentProperties.insert(QStringLiteral("projectfolder"), m_projectFolder.path());
    m_documentProperties.insert(QStringLiteral("profile"), profilePath());
    m_documentProperties.insert(QStringLiteral("position"), QString::number(m_render->seekPosition().frames(m_render->fps())));
    return m_documentProperties;
}

void KdenliveDoc::loadDocumentProperties()
{
    QDomNodeList list = m_document.elementsByTagName(QStringLiteral("playlist"));
    if (!list.isEmpty()) {
        QDomElement pl = list.at(0).toElement();
        if (pl.isNull()) return;
        QDomNodeList props = pl.elementsByTagName(QStringLiteral("property"));
        QString name;
        QDomElement e;
        for (int i = 0; i < props.count(); i++) {
            e = props.at(i).toElement();
            name = e.attribute(QStringLiteral("name"));
            if (name.startsWith(QLatin1String("kdenlive:docproperties."))) {
                name = name.section(QStringLiteral("."), 1);
                m_documentProperties.insert(name, e.firstChild().nodeValue());
            } else if (name.startsWith(QLatin1String("kdenlive:docmetadata."))) {
                name = name.section(QStringLiteral("."), 1);
                m_documentMetadata.insert(name, e.firstChild().nodeValue());
            }
        }
    }
    QString path = m_documentProperties.value(QStringLiteral("projectfolder"));
    if (!path.startsWith('/')) {
	QDir dir = QDir::home();
	path = dir.absoluteFilePath(path);
    }
    m_projectFolder = QUrl::fromLocalFile(path);
    QString profile = m_documentProperties.value(QStringLiteral("profile"));
    if (!profile.isEmpty())
        m_profile = ProfilesDialog::getVideoProfile(profile);
    if (!m_profile.isValid()) {
        // try to find matching profile from MLT profile properties
        list = m_document.elementsByTagName(QStringLiteral("profile"));
        if (!list.isEmpty()) {
            m_profile = ProfilesDialog::getVideoProfileFromXml(list.at(0).toElement());
        }
    }
    updateProjectProfile(false);
}

void KdenliveDoc::updateProjectProfile(bool reloadProducers)
{
    pCore->bin()->abortAudioThumbs();
    pCore->producerQueue()->abortOperations();
    KdenliveSettings::setProject_display_ratio((double) m_profile.display_aspect_num / m_profile.display_aspect_den);
    double fps = (double) m_profile.frame_rate_num / m_profile.frame_rate_den;
    KdenliveSettings::setProject_fps(fps);
    m_width = m_profile.width;
    m_height = m_profile.height;
    bool fpsChanged = m_timecode.fps() != fps;
    m_timecode.setFormat(fps);
    KdenliveSettings::setCurrent_profile(m_profile.path);
    pCore->monitorManager()->resetProfiles(m_profile, m_timecode);
    if (!reloadProducers) return;
    emit updateFps(fpsChanged);
    if (fpsChanged) {
        pCore->bin()->reloadAllProducers();
    }
}

void KdenliveDoc::resetProfile()
{
    m_profile = ProfilesDialog::getVideoProfile(KdenliveSettings::current_profile());
    updateProjectProfile(true);
    emit docModified(true);
}

void KdenliveDoc::slotSwitchProfile()
{
    QAction *action = qobject_cast<QAction *>(sender());
    if (!action) return;
    QVariantList data = action->data().toList();
    QString id = data.takeFirst().toString();
    if (!data.isEmpty()) {
        // we want a profile switch
        m_profile = MltVideoProfile(data);
        updateProjectProfile(true);
        emit docModified(true);
    }
}

void KdenliveDoc::switchProfile(MltVideoProfile profile, const QString &id, const QDomElement &xml)
{
    // Request profile update
    QString matchingProfile = ProfilesDialog::existingProfile(profile);
    if (matchingProfile.isEmpty() && (profile.width % 8 != 0)) {
      // Make sure profile width is a multiple of 8, required by some parts of mlt
        profile.adjustWidth();
        matchingProfile = ProfilesDialog::existingProfile(profile);
    }
    if (!matchingProfile.isEmpty()) {
        // We found a known matching profile, switch and inform user
        QMap< QString, QString > profileProperties = ProfilesDialog::getSettingsFromFile(matchingProfile);
        profile.path = matchingProfile;
        profile.description = profileProperties.value("description");
        
        if (KdenliveSettings::default_profile().isEmpty()) {
            // Default project format not yet confirmed, propose
            KMessageBox::ButtonCode answer = 
            KMessageBox::questionYesNoCancel(QApplication::activeWindow(), i18n("Your default project profile is %1, but your clip's profile is %2.\nDo you want to change default profile for future projects ?", m_profile.description, profile.description), i18n("Change default project profile"),  KGuiItem(i18n("Change default to %1",  profile.description)), KGuiItem(i18n("Keep current default %1",  m_profile.description)), KGuiItem(i18n("Ask me later")));
            
            switch(answer) {
                case KMessageBox::Yes :
                    KdenliveSettings::setDefault_profile(profile.path);
                    m_profile = profile;
                    updateProjectProfile(true);
                    emit docModified(true);
                    pCore->producerQueue()->getFileProperties(xml, id, 150, true);
                    return;
                    break;
                case KMessageBox::No :
                    KdenliveSettings::setDefault_profile(m_profile.path);
                    return;
                    break;
                default:
                    break;
            }
        }

        // Build actions for the info message (switch / cancel)
        QList <QAction*> list;
        QAction *ac = new QAction(KoIconUtils::themedIcon(QStringLiteral("dialog-ok")), i18n("Switch"), this);
        QVariantList params;
        connect(ac, SIGNAL(triggered(bool)), this, SLOT(slotSwitchProfile()));
        params << id << profile.toList();
        ac->setData(params);
        QAction *ac2 = new QAction(KoIconUtils::themedIcon(QStringLiteral("dialog-cancel")), i18n("Cancel"), this);
        QVariantList params2;
        params2 << id;
        ac2->setData(params2);
        connect(ac2, SIGNAL(triggered(bool)), this, SLOT(slotSwitchProfile()));
        list << ac << ac2;
        pCore->bin()->doDisplayMessage(i18n("Switch to clip profile %1?", profile.descriptiveString()), KMessageWidget::Information, list);
    } else {
        // No known profile, ask user if he wants to use clip profile anyway
        if (KMessageBox::warningContinueCancel(QApplication::activeWindow(), i18n("No profile found for your clip.\nCreate and switch to new profile (%1x%2, %3fps)?", profile.width, profile.height, QString::number((double)profile.frame_rate_num / profile.frame_rate_den, 'f', 2))) == KMessageBox::Continue) {
            m_profile = profile;
            m_profile.description = QString("%1x%2 %3fps").arg(profile.width).arg(profile.height).arg(QString::number((double)profile.frame_rate_num / profile.frame_rate_den, 'f', 2));
            ProfilesDialog::saveProfile(m_profile);
            updateProjectProfile(true);
            emit docModified(true);
            pCore->producerQueue()->getFileProperties(xml, id, 150, true);
        }
    }
}

void KdenliveDoc::forceProcessing(const QString &id)
{
    pCore->producerQueue()->forceProcessing(id);
}

void KdenliveDoc::getFileProperties(const QDomElement &xml, const QString &clipId, int imageHeight, bool replaceProducer)
{
    pCore->producerQueue()->getFileProperties(xml, clipId, imageHeight, replaceProducer);
}

void KdenliveDoc::doAddAction(const QString &name, QAction *a, QKeySequence shortcut)
{
    pCore->window()->actionCollection()->addAction(name, a);
    pCore->window()->actionCollection()->setDefaultShortcut(a, shortcut);
}

QAction *KdenliveDoc::getAction(const QString &name)
{
    return pCore->window()->actionCollection()->action(name);
}

void KdenliveDoc::previewProgress(int p)
{
    pCore->window()->setPreviewProgress(p);
}

void KdenliveDoc::displayMessage(const QString text, MessageType type, int timeOut)
{
    pCore->window()->displayMessage(text, type, timeOut);
}

void KdenliveDoc::selectPreviewProfile()
{
    // Read preview profiles and find the best match
    if (!KdenliveSettings::previewparams().isEmpty()) {
        setDocumentProperty(QStringLiteral("previewparameters"), KdenliveSettings::previewparams());
        setDocumentProperty(QStringLiteral("previewextension"), KdenliveSettings::previewextension());
        return;
    }
    KConfig conf(QStringLiteral("encodingprofiles.rc"), KConfig::CascadeConfig, QStandardPaths::DataLocation);
    KConfigGroup group(&conf, "timelinepreview");
    QMap< QString, QString > values = group.entryMap();
    QMapIterator<QString, QString> i(values);
    QStringList matchingProfiles;
    QStringList fallBackProfiles;
    QString profileSize = QString("%1x%2").arg(m_render->renderWidth()).arg(m_render->renderHeight());

    while (i.hasNext()) {
        i.next();
        // Check for frame rate
        QString params = i.value();
        QStringList data = i.value().split(" ");
        // Check for size mismatch
        if (params.contains(QStringLiteral("s="))) {
            QString paramSize = params.section(QStringLiteral("s="), 1).section(QStringLiteral(" "), 0, 0);
            if (paramSize != profileSize)
                continue;
        }
        bool rateFound = false;
        foreach(const QString arg, data) {
            if (arg.startsWith(QStringLiteral("r="))) {
                rateFound = true;
                double fps = arg.section(QStringLiteral("="), 1).toDouble();
                if (fps > 0) {
                    if (qAbs((int) (m_render->fps() * 100) - (fps * 100)) <= 1) {
                        matchingProfiles << i.value();
                        break;
                    }
                }
            }
        }
        if (!rateFound) {
            // Profile without fps, can be used as fallBack
            fallBackProfiles << i.value();
        }
    }
    QString bestMatch;
    if (!matchingProfiles.isEmpty()) {
        bestMatch = matchingProfiles.first();
    } else if (!fallBackProfiles.isEmpty()) {
        bestMatch = fallBackProfiles.first();
    }
    if (!bestMatch.isEmpty()) {
        setDocumentProperty(QStringLiteral("previewparameters"), bestMatch.section(";", 0, 0));
        setDocumentProperty(QStringLiteral("previewextension"), bestMatch.section(";", 1, 1));
    } else {
        setDocumentProperty(QStringLiteral("previewparameters"), QStringLiteral());
        setDocumentProperty(QStringLiteral("previewextension"), QStringLiteral());
    }
}

void KdenliveDoc::checkPreviewStack()
{
    // A command was pushed in the middle of the stack, remove all cached data from last undos
    emit removeInvalidUndo(m_commandStack->count());
}

void KdenliveDoc::saveMltPlaylist(const QString fileName)
{
    m_render->preparePreviewRendering(fileName);
}

void KdenliveDoc::initCacheDirs()
{
    bool ok = false;
    QString kdenliveCacheDir = QStandardPaths::writableLocation(QStandardPaths::CacheLocation);
    QString documentId = QDir::cleanPath(getDocumentProperty(QStringLiteral("documentid")));
    documentId.toLong(&ok);
    if (!ok || documentId.isEmpty() || kdenliveCacheDir.isEmpty()) {
        return;
    }
    QString basePath = kdenliveCacheDir + "/" + documentId;
    QDir dir(basePath);
    dir.mkpath(".");
    dir.mkdir("preview");
    dir.mkdir("audiothumbs");
    dir.mkdir("videothumbs");
    QDir cacheDir(kdenliveCacheDir);
    cacheDir.mkdir("proxy");
}

QDir KdenliveDoc::getCacheDir(CacheType type, bool *ok) const
{
    QString kdenliveCacheDir = QStandardPaths::writableLocation(QStandardPaths::CacheLocation);
    QString documentId = QDir::cleanPath(getDocumentProperty(QStringLiteral("documentid")));
    documentId.toLong(ok);
    if (!*ok || documentId.isEmpty() || kdenliveCacheDir.isEmpty()) {
        *ok = false;
        return QDir(kdenliveCacheDir);
    }
    QString basePath = kdenliveCacheDir + "/" + documentId;
    switch (type) {
        case CacheRoot:
            basePath = kdenliveCacheDir;
            break;
        case CachePreview:
            basePath.append(QStringLiteral("/preview"));
            break;
        case CacheProxy:
            basePath = kdenliveCacheDir;
            basePath.append(QStringLiteral("/proxy"));
            break;
        case CacheAudio:
            basePath.append(QStringLiteral("/audiothumbs"));
            break;
        case CacheThumbs:
            basePath.append(QStringLiteral("/videothumbs"));
            break;
        default:
            break;
    }
    QDir dir(basePath);
    if (!dir.exists()) {
        *ok = false;
    }
    return dir;
}

QStringList KdenliveDoc::getProxyHashList()
{
    return pCore->bin()->getProxyHashList();
}

//static
int KdenliveDoc::compositingMode()
{
    QString composite = TransitionHandler::compositeTransition();
    if (composite == QLatin1String("composite")) {
        // only simple preview compositing enabled
        return 0;
    }
    if (composite == QLatin1String("movit.overlay")) {
        // Movit compositing enabled
        return 2;
    }
    // Cairoblend or qtblend available
    return 1;
}
