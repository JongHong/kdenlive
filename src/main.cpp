/***************************************************************************
 *   Copyright (C) 2007 by Marco Gittler (g.marco@freenet.de)              *
 *   Copyright (C) 2008 by Jean-Baptiste Mardelle (jb@kdenlive.org)        *
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

#include <config-kdenlive.h>
#include "mainwindow.h"

#include "kxmlgui_version.h"

#include <KAboutData>
#include <KCrash>
#include <KIconLoader>
#include <KSharedConfig>
#include <KConfigGroup>

#include <QDebug>
#include <QUrl> //new
#include <QApplication>
#include <klocalizedstring.h>
#include <KDBusService>
#include <QCommandLineParser>
#include <QCommandLineOption>
#include <QProcess>
#include <QIcon>

int main(int argc, char *argv[])
{
    // Force QDomDocument to use a deterministic XML attribute order
    extern Q_CORE_EXPORT QBasicAtomicInt qt_qhash_seed;
    qt_qhash_seed.store(0);

#if defined(Q_OS_UNIX) && !defined(Q_OS_MAC)
    QCoreApplication::setAttribute(Qt::AA_X11InitThreads);
#endif

    // Init application
    QApplication app(argc, argv);
    app.setApplicationName(QStringLiteral("kdenlive"));
    app.setWindowIcon(QIcon(":/pics/kdenlive.png"));
    KLocalizedString::setApplicationDomain("kdenlive");
    KSharedConfigPtr config = KSharedConfig::openConfig(QStringLiteral("kdenliverc"));
    KConfigGroup grp(config, "unmanaged");
    KConfigGroup initialGroup(config, "version");
    if (!initialGroup.exists()) {
        QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
        if (env.contains(QStringLiteral("XDG_CURRENT_DESKTOP"))) {
            if (env.value(QStringLiteral("XDG_CURRENT_DESKTOP")).toLower() != QLatin1String("kde")) {
                // We are not on a KDE desktop, force breeze icon theme
                grp.writeEntry("force_breeze", true);
                qDebug()<<"Non KDE Desktop detected, forcing Breeze icon theme";
            } else {
                qDebug()<<"KDE Desktop detected, using system icons";
            }
        }
    }

    bool forceBreeze = grp.readEntry("force_breeze", QVariant(false)).toBool();
    if (forceBreeze) {
        QIcon::setThemeName("breeze");
    }
    // Create KAboutData
    KAboutData aboutData(QByteArray("kdenlive"), 
                         i18n("Kdenlive"), KDENLIVE_VERSION,
                         i18n("An open source video editor."),
                         KAboutLicense::GPL,
                         i18n("Copyright © 2007–2016 Kdenlive authors"),
                         i18n("Please report bugs to http://bugs.kde.org"),
                         QStringLiteral("https://kdenlive.org"));
    aboutData.addAuthor(i18n("Jean-Baptiste Mardelle"), i18n("MLT and KDE SC 4 / KF5 port, main developer and maintainer"), QStringLiteral("jb@kdenlive.org"));
    aboutData.addAuthor(i18n("Vincent Pinon"), i18n("Interim maintainer, KF5 port, bugs fixing, minor functions, profiles updates, etc."), QStringLiteral("vpinon@april.org"));
    aboutData.addAuthor(i18n("Laurent Montel"), i18n("Bugs fixing, clean up code, optimization etc."), QStringLiteral("montel@kde.org"));
    aboutData.addAuthor(i18n("Marco Gittler"), i18n("MLT transitions and effects, timeline, audio thumbs"), QStringLiteral("g.marco@freenet.de"));
    aboutData.addAuthor(i18n("Dan Dennedy"), i18n("Bug fixing, etc."), QStringLiteral("dan@dennedy.org"));
    aboutData.addAuthor(i18n("Simon A. Eugster"), i18n("Color scopes, bug fixing, etc."), QStringLiteral("simon.eu@gmail.com"));
    aboutData.addAuthor(i18n("Till Theato"), i18n("Bug fixing, etc."), QStringLiteral("root@ttill.de"));
    aboutData.addAuthor(i18n("Alberto Villa"), i18n("Bug fixing, logo, etc."), QStringLiteral("avilla@FreeBSD.org"));
    aboutData.addAuthor(i18n("Jean-Michel Poure"), i18n("Rendering profiles customization"), QStringLiteral("jm@poure.com"));
    aboutData.addAuthor(i18n("Ray Lehtiniemi"), i18n("Bug fixing, etc."), QStringLiteral("rayl@mail.com"));
    aboutData.addAuthor(i18n("Steve Guilford"), i18n("Bug fixing, etc."), QStringLiteral("s.guilford@dbplugins.com"));
    aboutData.addAuthor(i18n("Jason Wood"), i18n("Original KDE 3 version author (not active anymore)"), QStringLiteral("jasonwood@blueyonder.co.uk"));
    aboutData.addCredit(i18n("Nara Oliveira and Farid Abdelnour | Estúdio Gunga"), i18n("Kdenlive 16.08 icon"));
    aboutData.setTranslator(i18n("NAME OF TRANSLATORS"), i18n("EMAIL OF TRANSLATORS"));
    aboutData.setOrganizationDomain(QByteArray("kde.org"));


    // Register about data
    KAboutData::setApplicationData(aboutData);

    // Add rcc stored icons to the search path so that we always find our icons
    KIconLoader *loader = KIconLoader::global();
    loader->reconfigure("kdenlive", QStringList() << QStringLiteral(":/pics"));

    // Set app stuff from about data
    app.setApplicationDisplayName(aboutData.displayName());
    app.setOrganizationDomain(aboutData.organizationDomain());
    app.setApplicationVersion(aboutData.version());

    // Create command line parser with options
    QCommandLineParser parser;
    aboutData.setupCommandLine(&parser);
    parser.setApplicationDescription(aboutData.shortDescription());
    parser.addVersionOption();
    parser.addHelpOption();

    parser.addOption(QCommandLineOption(QStringList() <<  QStringLiteral("mlt-path"), i18n("Set the path for MLT environment"), QStringLiteral("mlt-path")));
    parser.addOption(QCommandLineOption(QStringList() <<  QStringLiteral("i"), i18n("Comma separated list of clips to add"), QStringLiteral("clips")));
    parser.addPositionalArgument(QStringLiteral("file"), i18n("Document to open"));

    // Parse command line
    parser.process(app);
    aboutData.processCommandLine(&parser);

    // Register DBus service
    KDBusService programDBusService;
#if KXMLGUI_VERSION_MINOR > 14 || KXMLGUI_VERSION_MAJOR > 5
    KCrash::initialize();
#else
    KCrash::setCrashHandler(KCrash::defaultCrashHandler);
#endif

    // see if we are starting with session management
    if (qApp->isSessionRestored()){
	  int n = 1;
	  while (KMainWindow::canBeRestored(n)){
	      (new MainWindow())->restore(n);
	      n++;
	  }
    } else {
        QString clipsToLoad = parser.value(QStringLiteral("i"));
        QString mltPath = parser.value(QStringLiteral("mlt-path"));
        QUrl url;
        if (parser.positionalArguments().count()) {
            url = QUrl::fromLocalFile(parser.positionalArguments().at(0));
            // Make sure we get an absolute URL so that we can autosave correctly
            QString currentPath = QDir::currentPath();
            QUrl startup = QUrl::fromLocalFile(currentPath.endsWith(QDir::separator()) ? currentPath : currentPath + QDir::separator());
            url = startup.resolved(url);
        }
        MainWindow* window = new MainWindow(mltPath, url, clipsToLoad);
        window->show();
    }
    int result = app.exec();

    if (EXIT_RESTART == result) {
        qDebug() << "restarting app";
        QProcess* restart = new QProcess;
        restart->start(app.applicationFilePath(), QStringList());
        restart->waitForReadyRead();
        restart->waitForFinished(1000);
        result = EXIT_SUCCESS;
    }
    return result;
}
