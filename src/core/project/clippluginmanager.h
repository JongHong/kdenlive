/*
Copyright (C) 2012  Till Theato <root@ttill.de>
This file is part of Kdenlive. See www.kdenlive.org.

This program is free software: you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation, either version 3 of the License, or
(at your option) any later version.
*/

#ifndef CLIPPLUGINMANAGER_H
#define CLIPPLUGINMANAGER_H

#include <QObject>
#include <QHash>
#include <QStringList>
#include "kdenlivecore_export.h"

class AbstractClipPlugin;
class AbstractProjectClip;
class ProjectFolder;
class ProducerDescription;
class KUrl;
class QDomElement;
class QUndoCommand;
class QAction;

namespace Mlt
{
    class Properties;
}


/**
 * @class ClipPluginManager
 * @brief Loads the clip plugins and provides ways to open clips.
 */


class KDENLIVECORE_EXPORT ClipPluginManager : public QObject
{
    Q_OBJECT

public:
    /** @brief Loads the clip plugins and creates the "add clip" action. */
    explicit ClipPluginManager(QObject* parent = 0);
    ~ClipPluginManager();

    /**
     * @brief Finds out the clip plugin needed for the file provided and creates a AddClipCommand.
     * @param url url of the file which should be opened
     * @param folder folder the created clip should be parented to
     * @param parentCommand parent commmand
     * 
     * If no parent command is provided the created command will be pushed to the undo stack.
     */
    void createClip(const KUrl &url, ProjectFolder *folder, QUndoCommand *parentCommand = 0) const;
    const QString createClip(const QString &service, Mlt::Properties props, ProjectFolder *folder, QUndoCommand *parentCommand) const;
    /**
     * @brief Loads the clip described (only used when opening a project).
     * @param clipDescription element describing the clip
     * @param folder folder the created clip should be parented to
     *
     */
    AbstractProjectClip *loadClip(const QDomElement &clipDescription, ProjectFolder *folder) const;

    /**
     * @brief Adds support for the supplied mimetypes to the add clip file dialog.
     * @param mimetypes list of mimetype names
     */
    void addSupportedMimetypes(const QStringList &mimetypes);

    /** @brief Returns the clip plugin that is associated with the supplied producer type or NULL is no such plugin exists. */
    AbstractClipPlugin *clipPlugin(const QString &producerType) const;

    /** @brief Set some producer specific data. */
    ProducerDescription *filterDescription(Mlt::Properties props, ProducerDescription *description);

    /** @brief Returns true if this clip's plugin provider needs a clip reload when changing this property. */
    bool requiresClipReload(const QString &service, const QString &property);

public slots:
    /**
     * @brief Allows to add clips through the add clip dialog.
     * @param folder folder the created clips should be parented to
     */
    void execAddClipDialog(QAction *action = NULL, ProjectFolder *folder = 0) const;

private:
    QHash<QString, AbstractClipPlugin*> m_clipPlugins;
    QStringList m_supportedFileExtensions;
};

#endif